#include "proxy/server.hpp"

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "cluster/slot.hpp"
#include "core/buffer.hpp"
#include "core/byte_queue.hpp"
#include "core/log.hpp"
#include "core/version.hpp"
#include "io/wait_queue.hpp"
#include "proxy/command_table.hpp"
#include "proxy/router.hpp"
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

// PING/ECHO under cluster routing. Forwarding them would mean picking an
// arbitrary node for a command whose whole point is the round trip, so the
// proxy answers instead (design m4 §3, §7 spells out the consequence for
// `redis-benchmark -t ping`).
std::string cluster_local_reply(const resp::message_view& msg, const command_info& info) {
  if (info.name == "ECHO") {
    if (msg.args.size() != 2) {
      return "-ERR wrong number of arguments for 'echo' command\r\n";
    }
    std::string r;
    resp::append_bulk_string(r, msg.args[1]);
    return r;
  }
  // PING [message]
  if (msg.args.size() == 1) {
    return "+PONG\r\n";
  }
  if (msg.args.size() != 2) {
    return "-ERR wrong number of arguments for 'ping' command\r\n";
  }
  std::string r;
  resp::append_bulk_string(r, msg.args[1]);
  return r;
}

constexpr std::string_view kAsking = "*1\r\n$6\r\nASKING\r\n";

// A parsed "-MOVED 3999 127.0.0.1:6381\r\n" / "-ASK 3999 127.0.0.1:6381\r\n".
// `host` borrows from the frame, so it dies with the backend's read buffer —
// callers must resolve it before their next suspension point.
struct redirect {
  bool valid = false;
  bool moved = false;  // false = ASK: a migration-time hop, topology untouched
  std::uint16_t slot = 0;
  std::string_view host;
  std::uint16_t port = 0;
};

bool parse_u16(std::string_view text, unsigned& out) noexcept {
  const char* end = text.data() + text.size();
  const auto r = std::from_chars(text.data(), end, out);
  return r.ec == std::errc{} && r.ptr == end;
}

redirect parse_redirect(std::string_view frame) noexcept {
  redirect r;
  std::string_view rest;
  if (frame.starts_with("-MOVED ")) {
    r.moved = true;
    rest = frame.substr(7);
  } else if (frame.starts_with("-ASK ")) {
    rest = frame.substr(5);
  } else {
    return r;
  }
  if (const std::size_t cr = rest.find('\r'); cr != std::string_view::npos) {
    rest = rest.substr(0, cr);
  }
  const std::size_t space = rest.find(' ');
  if (space == std::string_view::npos) {
    return r;
  }
  unsigned slot = 0;
  if (!parse_u16(rest.substr(0, space), slot) || slot >= cluster::kSlotCount) {
    return r;
  }
  // A node with no known address answers "-MOVED 1 :6380". We cannot resolve
  // that from here (we do not know which node replied), so `colon == 0` leaves
  // the redirect invalid and the client sees the backend's own error.
  const std::string_view addr = rest.substr(space + 1);
  const std::size_t colon = addr.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 == addr.size()) {
    return r;
  }
  unsigned port = 0;
  if (!parse_u16(addr.substr(colon + 1), port) || port == 0 || port > 65535) {
    return r;
  }
  r.host = addr.substr(0, colon);
  if (r.host.size() >= 2 && r.host.front() == '[' && r.host.back() == ']') {
    r.host = r.host.substr(1, r.host.size() - 2);
  }
  r.slot = static_cast<std::uint16_t>(slot);
  r.port = static_cast<std::uint16_t>(port);
  r.valid = true;
  return r;
}

metrics::error_kind route_error_kind(routing::status st) noexcept {
  switch (st) {
    case routing::status::crossslot:
      return metrics::error_kind::crossslot;
    case routing::status::unsupported:
      return metrics::error_kind::unsupported;
    case routing::status::unavailable:
    case routing::status::ok:
      break;
  }
  return metrics::error_kind::no_topology;
}

std::string route_error_reply(routing::status st, const command_info* info) {
  switch (st) {
    case routing::status::crossslot:
      return std::string{kErrCrossSlot};
    case routing::status::unsupported:
      return fmt::format("-ERR proxy: unsupported in cluster mode: {}\r\n",
                         info != nullptr ? info->name : std::string_view{"unknown command"});
    case routing::status::unavailable:
    case routing::status::ok:
      break;
  }
  return std::string{kErrNoTopology};
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
  client_conn(io::event_loop& loop, io::unique_fd fd, router& rtr, metrics::worker_stats& stats,
              std::size_t outbuf_limit, std::size_t max_redirects)
      : loop_(loop),
        fd_(std::move(fd)),
        router_(rtr),
        stats_(stats),
        outbuf_limit_(outbuf_limit),
        max_redirects_(max_redirects),
        may_redirect_(rtr.may_redirect()),
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
      // owed before closing. Bounded by the request timeout (times the
      // redirect cap under cluster routing) — a dead backend fails the
      // queue, which drains us too.
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
    // Under cluster routing one client may have queued onto several nodes,
    // so every connection it touched needs the tombstone.
    for (const auto& c : used_) {
      c->detach(*this);
    }
  }

  void deliver(std::uint64_t token, std::string_view frame) override {
    if (aborting_) {
      return;
    }
    const auto index = static_cast<std::size_t>(token - head_token_);
    if (index >= slots_.size()) {
      return;  // already flushed or abandoned; nothing to fill
    }
    // MOVED/ASK never reach the client: the retry rides the same slot, so the
    // hop is invisible in the output order (design m4 §5).
    if (may_redirect_ && frame.starts_with('-')) {
      if (const redirect r = parse_redirect(frame); r.valid) {
        retry(index, r, frame);
        return;
      }
    }
    fill(index, frame);
  }

 private:
  struct reply_slot {
    bool filled = false;
    std::string reply;    // only populated when the reply arrived out of order
    std::string request;  // retry copy; cluster mode only (router::may_redirect)
    std::uint32_t redirects = 0;
    // From event_loop::now(), the clock the loop already read this iteration —
    // so the proxy-side histogram costs no per-request syscall (design m5 §2).
    std::chrono::steady_clock::time_point started{};
  };

  // What a finished reply counts as. Only error frames are inspected, so the
  // common path is one byte compare. Matching our canned errors by text means
  // a backend echoing them verbatim would be misfiled — it cannot happen from
  // a real valkey, and if it did, the classification would still be right.
  static metrics::outcome classify(std::string_view frame) noexcept {
    if (!frame.starts_with('-')) {
      return metrics::outcome::ok;
    }
    if (frame == kErrTimeout) {
      return metrics::outcome::timeout;
    }
    if (frame == kErrBackendUnavailable || frame == kErrBackendLost) {
      return metrics::outcome::unavailable;
    }
    return metrics::outcome::error;
  }

  // Puts `frame` in its place in the output order and flushes the contiguous
  // filled prefix.
  void fill(std::size_t index, std::string_view frame) {
    // Counted here rather than at flush time: this is the moment the request
    // got its answer, and it runs exactly once per slot in either order.
    stats_.bump_response(classify(frame));
    stats_.request_duration.observe(loop_.now() - slots_[index].started);
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
      stats_.bump_error(metrics::error_kind::outbuf_limit);
      abort_now();  // slow client: cut it loose instead of buffering forever
      return;
    }
    out_ready_.notify_one();
  }

  // Re-sends the slot's request to the node the redirect named. Runs inside
  // the old connection's driver, so it must not park: if the target has no
  // capacity we overshoot its watermark rather than fail the request (see
  // enqueue_forward), which is what keeps resharding under load error-free.
  void retry(std::size_t index, const redirect& r, std::string_view frame) {
    reply_slot& s = slots_[index];
    if (s.request.empty()) {
      fill(index, frame);  // nothing kept to re-send: pass the error through
      return;
    }
    if (s.redirects >= max_redirects_) {
      stats_.bump_error(metrics::error_kind::too_many_redirects);
      VKP_LOG_WARN_EVERY(
          1s, "redirect: giving up after {hops} hops, slot {slot} last pointed at {host}:{port}",
          max_redirects_, r.slot, r.host, r.port);
      fill(index, kErrTooManyRedirects);
      return;
    }
    ++s.redirects;
    stats_.bump_redirect(r.moved ? metrics::redirect_kind::moved : metrics::redirect_kind::ask);
    if (r.moved) {
      // Rate-limited hard: a resharding cluster emits MOVED by the thousand,
      // and one line per redirect would bury every other event in the log.
      VKP_LOG_WARN_EVERY(1s, "redirect: slot {slot} moved to {host}:{port}", r.slot, r.host,
                         r.port);
      // Repoint the slot now; the debounced refresh picks up the rest.
      router_.apply_moved(r.slot, r.host, r.port);
    }
    // Resolves the endpoint (numeric, so no DNS round trip) and creates the
    // pool entry if this node is new to us.
    const routing dest = router_.node_at(r.host, r.port);
    backend_conn* c = dest.get();
    if (dest.st != routing::status::ok || !c->available()) {
      stats_.bump_error(metrics::error_kind::backend_unavailable);
      fill(index, kErrBackendUnavailable);
      return;
    }
    remember(*dest.slot);
    if (!r.moved) {
      // ASKING must immediately precede the command on the same connection.
      // Single-threaded, so two consecutive pushes are adjacent by
      // construction; its +OK is discarded by the null sink.
      c->enqueue_internal(kAsking);
    }
    c->enqueue_forward(*this, head_token_ + index, s.request);
  }

  // Takes the next token and reserves its place in the output order.
  // Invariant: next_token_ == head_token_ + slots_.size().
  std::uint64_t new_slot() {
    slots_.emplace_back().started = loop_.now();
    return next_token_++;
  }

  void pop_head() noexcept {
    slots_.pop_front();
    ++head_token_;
  }

  // A reply the proxy produced itself. It takes a slot like any other so it
  // lands in the right place among pipelined requests.
  void reply_now(std::string_view reply) { deliver(new_slot(), reply); }

  // Keeps a connection alive for as long as this client might still be
  // delivered on, and records who needs a tombstone at teardown. Typically
  // one entry (standalone) and at most a handful under cluster routing, so a
  // linear scan beats a set.
  void remember(const std::shared_ptr<backend_conn>& c) {
    for (const auto& seen : used_) {
      if (seen.get() == c.get()) {
        return;
      }
    }
    used_.push_back(c);
  }

  io::task<void> reader() {
    read_buffer in;
    resp::parser parser;
    for (;;) {
      const frame_result fr = co_await read_frame(loop_, fd_.get(), in, parser, &recv_slot_);
      if (aborting_) {
        co_return;
      }
      if (fr.k == frame_result::kind::protocol_error) {
        stats_.bump_error(metrics::error_kind::protocol);
        reply_now(fmt::format("-ERR Protocol error: {}\r\n", resp::to_string(fr.perr)));
        co_return;
      }
      if (fr.k != frame_result::kind::frame) {
        co_return;  // eof / io error / cancelled
      }
      // Request frame bytes, not socket bytes: pipelined batches are already
      // covered and the number stays comparable with vkp_requests_total. A
      // frame that never parses is counted as an error instead.
      stats_.bump_bytes(metrics::byte_dir::in, fr.len);

      const resp::message_view& msg = parser.message();
      if (!msg.is_command) {
        stats_.bump_error(metrics::error_kind::protocol);
        reply_now("-ERR Protocol error: expected a command\r\n");
        co_return;
      }
      bool close_after = false;
      const command_info* info = find_command(msg.args[0]);
      // Unknown commands are `other`: the table is the only thing that knows
      // read from write, and guessing from the name is how you get a SET
      // counted as a read.
      stats_.bump_request(info != nullptr ? info->cls : metrics::cmd_class::other);
      const cmd_policy policy = info != nullptr ? info->policy : cmd_policy::forward;
      if (policy == cmd_policy::reject) {
        stats_.bump_error(metrics::error_kind::unsupported);
        reply_now(fmt::format("-ERR unsupported by proxy: {}\r\n", info->name));
      } else if (policy == cmd_policy::local) {
        reply_now(handle_local(msg, *info, close_after));
      } else if (router_.cluster_mode() && info != nullptr &&
                 info->cluster == cluster_policy::local) {
        reply_now(cluster_local_reply(msg, *info));
      } else {
        // Inline rather than a helper coroutine: this is the hot path and a
        // nested frame would be a heap allocation per request.
        bool stop = false;
        for (;;) {
          const routing r = router_.route(msg.args, info);
          if (r.st != routing::status::ok) {
            stats_.bump_error(route_error_kind(r.st));
            reply_now(route_error_reply(r.st, info));
            break;
          }
          backend_conn* c = r.get();
          if (!c->available()) {
            stats_.bump_error(metrics::error_kind::backend_unavailable);
            reply_now(kErrBackendUnavailable);
            break;
          }
          if (c->has_capacity()) {
            remember(*r.slot);
            const std::uint64_t token = new_slot();
            if (may_redirect_) {
              slots_.back().request.assign(msg.raw);  // MOVED/ASK may need it back
            }
            c->enqueue_forward(*this, token, msg.raw);
            break;
          }
          // Backpressure: wait for queue capacity; the client socket goes
          // unread meanwhile, so TCP pushes back upstream (design §2.5).
          // Re-route after waking: the topology may have moved on.
          if (co_await c->capacity_event().wait() < 0 || aborting_) {
            stop = true;
            break;
          }
        }
        if (stop) {
          co_return;
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
      stats_.bump_bytes(metrics::byte_dir::out, batch.size());
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
  router& router_;
  metrics::worker_stats& stats_;
  std::size_t outbuf_limit_;
  std::size_t max_redirects_;
  bool may_redirect_;  // standalone skips the per-request retry copy

  std::vector<std::shared_ptr<backend_conn>> used_;
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

namespace {

router_config make_router_config(const config& cfg) {
  return {
      .backend_host = cfg.backend_host,
      .backend_port = cfg.backend_port,
      .cluster_seeds = cfg.cluster_seeds,
      .refresh_interval = cfg.cluster_refresh,
      .conns_per_node = cfg.conns_per_backend > 0 ? cfg.conns_per_backend : 1,
      .backend = cfg.backend,
  };
}

}  // namespace

server::server(io::event_loop& loop, config cfg, metrics::worker_stats& stats)
    : loop_(loop),
      cfg_(std::move(cfg)),
      stats_(stats),
      router_(loop, make_router_config(cfg_), stats),
      listener_(io::listen_tcp(cfg_.listen_host, cfg_.listen_port, cfg_.backlog, cfg_.reuseport)),
      port_(io::local_port(listener_.get())) {}

void server::start() {
  router_.start();  // pool warmup: connecting begins now
  io::spawn(acceptor());
  VKP_LOG_DEBUG("worker listening on {host}:{port}", cfg_.listen_host, port_);
}

void server::begin_shutdown() {
  if (draining_) {
    VKP_LOG_WARN("second shutdown request: stopping now, {open} connection(s) still open", active_);
    loop_.stop();  // second request: no more grace
    return;
  }
  draining_ = true;
  VKP_LOG_INFO("draining: no longer accepting, {inflight} connection(s) in flight", active_);
  loop_.cancel(accept_cancel_);
  io::spawn(watchdog());
}

io::task<void> server::watchdog() {
  (void)co_await loop_.sleep_for(cfg_.shutdown_grace);
  if (active_ != 0) {
    VKP_LOG_WARN("shutdown grace of {grace_ms} ms expired with {open} connection(s) still open",
                 cfg_.shutdown_grace.count(), active_);
  }
  loop_.stop();  // idempotent; a clean drain already stopped the loop
}

io::task<void> server::acceptor() {
  for (;;) {
    const std::int32_t fd = co_await loop_.async_accept(listener_.get(), &accept_cancel_);
    if (fd == -ECANCELED) {
      break;
    }
    if (fd < 0) {
      // EMFILE and friends: pause briefly instead of spinning on the error.
      // Rate-limited because the condition that causes it (fd exhaustion)
      // persists, and one line per failed accept would bury everything else.
      VKP_LOG_WARN_EVERY(1s, "accept failed on port {port}: {error}", port_, std::strerror(-fd));
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
  stats_.client_connections.set(active_);
  stats_.client_connections_total.bump();
  // Debug level on purpose: at connection-churn rates this is the one control
  // -plane event frequent enough to cost something, so it stays off by default.
  VKP_LOG_DEBUG("client connected on port {port}, {open} open", port_, active_);
  {
    client_conn c{loop_,  std::move(client),        router_,
                  stats_, cfg_.client_outbuf_limit, cfg_.max_redirects};
    try {
      co_await c.run();
    } catch (const std::exception&) {
      // Never let an exception escape into spawn (it would terminate).
    }
  }
  --active_;
  stats_.client_connections.set(active_);
  VKP_LOG_DEBUG("client disconnected on port {port}, {open} open", port_, active_);
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
  router_.begin_drain();
  co_await router_.await_drained();
  VKP_LOG_INFO("drained cleanly: all clients served, backend pool closed");
  loop_.stop();
}

}  // namespace vkp::proxy
