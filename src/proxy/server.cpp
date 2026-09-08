#include "proxy/server.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "core/buffer.hpp"
#include "core/byte_queue.hpp"
#include "core/version.hpp"
#include "io/wait_queue.hpp"
#include "proxy/command_table.hpp"
#include "resp/parser.hpp"
#include "resp/serializer.hpp"

namespace vkp::proxy {

namespace {

using namespace std::chrono_literals;

bool ieq(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::toupper(static_cast<unsigned char>(a[i])) !=
        std::toupper(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::string hello_reply(int proto) {
  std::string r = (proto == 3) ? "%7\r\n" : "*14\r\n";
  const auto bulk = [&r](std::string_view s) { resp::append_bulk_string(r, s); };
  bulk("server");
  bulk("valkey-proxy");
  bulk("version");
  bulk(kVersion);
  bulk("proto");
  resp::append_integer(r, proto);
  bulk("id");
  resp::append_integer(r, 0);
  bulk("mode");
  bulk("standalone");
  bulk("role");
  bulk("master");
  bulk("modules");
  r += "*0\r\n";
  return r;
}

// HELLO/QUIT/SELECT/RESET: connection-state commands that must not touch the
// shared backend connection (design §5). Returns the canned reply.
std::string handle_local(const resp::message_view& msg, const command_info& info,
                         bool& close_after) {
  if (info.name == "QUIT") {
    close_after = true;
    return std::string{resp::kOk};
  }
  if (info.name == "RESET") {
    return "+RESET\r\n";  // proxy keeps no per-connection sticky state
  }
  if (info.name == "SELECT") {
    if (msg.args.size() != 2) {
      return "-ERR wrong number of arguments for 'select' command\r\n";
    }
    if (msg.args[1] == "0") {
      return std::string{resp::kOk};
    }
    return "-ERR proxy: SELECT is limited to db 0\r\n";
  }
  // HELLO [protover [AUTH ...] [SETNAME name]]. The backend pool is pinned
  // to RESP2; we answer the handshake ourselves and pass replies through
  // verbatim (RESP2 frames are valid RESP3 input).
  int proto = 2;
  if (msg.args.size() >= 2) {
    if (msg.args[1] == "2") {
      proto = 2;
    } else if (msg.args[1] == "3") {
      proto = 3;
    } else {
      return "-NOPROTO unsupported protocol version\r\n";
    }
  }
  for (std::size_t i = 2; i < msg.args.size(); ++i) {
    if (ieq(msg.args[i], "AUTH")) {
      return "-ERR proxy: AUTH not supported\r\n";
    }
    if (ieq(msg.args[i], "SETNAME")) {
      ++i;  // ignore the name
    }
  }
  return hello_reply(proto);
}

// One client connection: a reader (frames → command table → backend queue)
// and a writer (flushes the outbound byte queue). Replies arrive via
// deliver() from a backend connection's driver.
//
// This class is the sole ordering authority (design m4 §1). Every request
// read takes the next token and pushes a reply slot; a reply fills its slot
// and only the contiguous filled prefix is flushed. Proxy-generated replies
// fill their own slot the same way, so they need no help from the backend
// FIFO. Today all requests still go to one backend and arrive in order, so
// the deque is almost always length-1 at the head — but the invariant is what
// lets cluster routing fan a client out across nodes.
class client_conn final : public reply_sink {
 public:
  client_conn(io::event_loop& loop, io::unique_fd fd, backend_conn& conn, std::size_t outbuf_limit)
      : loop_(loop),
        fd_(std::move(fd)),
        conn_(conn),
        outbuf_limit_(outbuf_limit),
        out_ready_(loop),
        drained_(loop),
        writer_done_(loop) {}

  // Runs the whole lifecycle; when it returns, both coroutines have exited
  // and the connection is detached from the backend queue.
  io::task<void> run() {
    writer_live_ = true;
    io::spawn(writer_task());
    co_await reader();
    if (!aborting_) {
      // Client stopped sending (EOF/QUIT/error): deliver what it is still
      // owed before closing. Bounded by the request timeout — a dead
      // backend fails the queue, which drains us too.
      while (!slots_.empty() && !aborting_) {
        if (co_await drained_.wait() < 0) {
          break;
        }
      }
    }
    closing_ = true;
    out_ready_.notify_all();
    while (writer_live_) {
      (void)co_await writer_done_.wait();
    }
    conn_.detach(*this);
  }

  void deliver(std::uint64_t token, std::string_view frame) override {
    if (aborting_) {
      return;
    }
    const auto index = static_cast<std::size_t>(token - head_token_);
    if (index >= slots_.size()) {
      return;  // already flushed or abandoned; nothing to fill
    }
    if (index != 0) {
      // Out of order: park it and wait for the earlier slots.
      slots_[index].filled = true;
      slots_[index].reply.assign(frame);
      return;
    }
    // The head: goes straight out, skipping the intermediate string. Any
    // followers that arrived early become sendable with it.
    out_.append(frame);
    pop_head();
    while (!slots_.empty() && slots_.front().filled) {
      out_.append(slots_.front().reply);
      pop_head();
    }
    if (slots_.empty()) {
      drained_.notify_all();
    }
    if (out_.size() > outbuf_limit_) {
      abort_now();  // slow client: cut it loose instead of buffering forever
      return;
    }
    out_ready_.notify_one();
  }

 private:
  struct reply_slot {
    bool filled = false;
    std::string reply;  // only populated when the reply arrived out of order
  };

  // Takes the next token and reserves its place in the output order.
  // Invariant: next_token_ == head_token_ + slots_.size().
  std::uint64_t new_slot() {
    slots_.emplace_back();
    return next_token_++;
  }

  void pop_head() noexcept {
    slots_.pop_front();
    ++head_token_;
  }

  // A reply the proxy produced itself. It takes a slot like any other so it
  // lands in the right place among pipelined requests.
  void reply_now(std::string_view reply) { deliver(new_slot(), reply); }

  io::task<void> reader() {
    read_buffer in;
    resp::parser parser;
    for (;;) {
      const frame_result fr = co_await read_frame(loop_, fd_.get(), in, parser, &recv_slot_);
      if (aborting_) {
        co_return;
      }
      if (fr.k == frame_result::kind::protocol_error) {
        reply_now(fmt::format("-ERR Protocol error: {}\r\n", resp::to_string(fr.perr)));
        co_return;
      }
      if (fr.k != frame_result::kind::frame) {
        co_return;  // eof / io error / cancelled
      }

      const resp::message_view& msg = parser.message();
      if (!msg.is_command) {
        reply_now("-ERR Protocol error: expected a command\r\n");
        co_return;
      }
      bool close_after = false;
      const command_info* info = find_command(msg.args[0]);
      const cmd_policy policy = info != nullptr ? info->policy : cmd_policy::forward;
      if (policy == cmd_policy::reject) {
        reply_now(fmt::format("-ERR unsupported by proxy: {}\r\n", info->name));
      } else if (policy == cmd_policy::local) {
        reply_now(handle_local(msg, *info, close_after));
      } else {
        // Backpressure: wait for queue capacity; the client socket goes
        // unread meanwhile, so TCP pushes back upstream (design §2.5).
        for (;;) {
          if (!conn_.available()) {
            reply_now(kErrBackendUnavailable);
            break;
          }
          if (conn_.has_capacity()) {
            conn_.enqueue_forward(*this, new_slot(), msg.raw);
            break;
          }
          if (co_await conn_.capacity_event().wait() < 0 || aborting_) {
            co_return;
          }
        }
      }
      in.consume(fr.len);  // enqueue_forward copied the bytes
      if (close_after) {
        co_return;
      }
    }
  }

  io::task<void> writer_task() {
    for (;;) {
      if (aborting_) {
        break;
      }
      if (out_.empty()) {
        if (closing_) {
          break;
        }
        if (co_await out_ready_.wait() < 0) {
          break;
        }
        continue;
      }
      writer_sending_ = true;
      const std::string_view batch = out_.begin_send();
      const std::int32_t r = co_await io::send_all(loop_, fd_.get(), batch, &send_slot_);
      out_.end_send();
      writer_sending_ = false;
      send_slot_.reset();
      if (r < 0) {
        abort_now();
        break;
      }
    }
    writer_live_ = false;
    writer_done_.notify_all();
  }

  void abort_now() noexcept {
    if (aborting_) {
      return;
    }
    aborting_ = true;
    loop_.cancel(recv_slot_);
    if (writer_sending_) {
      loop_.cancel(send_slot_);
    }
    out_ready_.notify_all();
    drained_.notify_all();
  }

  io::event_loop& loop_;
  io::unique_fd fd_;
  backend_conn& conn_;
  std::size_t outbuf_limit_;

  byte_queue out_;
  std::deque<reply_slot> slots_;
  std::uint64_t next_token_ = 0;
  std::uint64_t head_token_ = 0;  // the token of slots_.front()
  bool aborting_ = false;
  bool closing_ = false;
  bool writer_live_ = false;
  bool writer_sending_ = false;

  io::wait_queue out_ready_;
  io::wait_queue drained_;
  io::wait_queue writer_done_;
  io::cancel_slot recv_slot_;
  io::cancel_slot send_slot_;
};

}  // namespace

server::server(io::event_loop& loop, config cfg)
    : loop_(loop),
      cfg_(std::move(cfg)),
      backend_addr_(io::resolve_tcp(cfg_.backend_host, cfg_.backend_port)),
      listener_(io::listen_tcp(cfg_.listen_host, cfg_.listen_port, cfg_.backlog, cfg_.reuseport)),
      port_(io::local_port(listener_.get())) {
  const std::size_t n = cfg_.conns_per_backend > 0 ? cfg_.conns_per_backend : 1;
  conns_.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    conns_.push_back(std::make_unique<backend_conn>(loop_, backend_addr_, cfg_.backend));
  }
}

void server::start() {
  for (const auto& c : conns_) {
    c->start();  // pool warmup: connecting begins now
  }
  io::spawn(acceptor());
}

void server::begin_shutdown() {
  if (draining_) {
    loop_.stop();  // second request: no more grace
    return;
  }
  draining_ = true;
  loop_.cancel(accept_cancel_);
  io::spawn(watchdog());
}

io::task<void> server::watchdog() {
  (void)co_await loop_.sleep_for(cfg_.shutdown_grace);
  loop_.stop();  // idempotent; a clean drain already stopped the loop
}

backend_conn& server::pick_conn() noexcept {
  backend_conn& c = *conns_[next_conn_];
  next_conn_ = (next_conn_ + 1) % conns_.size();
  return c;
}

io::task<void> server::acceptor() {
  for (;;) {
    const std::int32_t fd = co_await loop_.async_accept(listener_.get(), &accept_cancel_);
    if (fd == -ECANCELED) {
      break;
    }
    if (fd < 0) {
      // EMFILE and friends: pause briefly instead of spinning on the error.
      fmt::print(stderr, "accept failed: {}\n", std::strerror(-fd));
      (void)co_await loop_.sleep_for(100ms);
      continue;
    }
    io::setup_stream_socket(fd);
    io::spawn(connection(io::unique_fd{fd}));
  }
  listener_.reset();
  maybe_drain_backends();
}

io::task<void> server::connection(io::unique_fd client) {
  ++active_;
  {
    client_conn c{loop_, std::move(client), pick_conn(), cfg_.client_outbuf_limit};
    try {
      co_await c.run();
    } catch (const std::exception&) {
      // Never let an exception escape into spawn (it would terminate).
    }
  }
  --active_;
  maybe_drain_backends();
}

void server::maybe_drain_backends() {
  if (!draining_ || active_ != 0 || backend_drain_started_) {
    return;
  }
  backend_drain_started_ = true;
  io::spawn(drain_backends());
}

io::task<void> server::drain_backends() {
  for (const auto& c : conns_) {
    c->begin_drain();
  }
  for (const auto& c : conns_) {
    while (!c->finished()) {
      if (co_await c->finished_event().wait() < 0) {
        co_return;  // hard stop already underway
      }
    }
  }
  loop_.stop();
}

}  // namespace vkp::proxy
