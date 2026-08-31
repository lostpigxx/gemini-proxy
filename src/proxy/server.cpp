#include "proxy/server.hpp"

#include <cerrno>
#include <cstring>
#include <string_view>

#include <fmt/format.h>

#include "core/buffer.hpp"
#include "resp/parser.hpp"

namespace vkp::proxy {

namespace {

using namespace std::chrono_literals;

constexpr std::string_view kBackendUnavailable = "-ERR proxy: backend unavailable\r\n";
constexpr std::string_view kBackendLost = "-ERR proxy: backend connection lost\r\n";

struct frame_result {
  enum class kind : std::uint8_t { frame, eof, io_error, protocol_error };
  kind k = kind::frame;
  std::size_t len = 0;                             // frame bytes at the buffer front
  std::int32_t err = 0;                            // -errno for io_error
  resp::parse_errc perr = resp::parse_errc::none;  // for protocol_error
};

// Reads until one complete RESP frame sits at the front of `buf`. The
// incremental parser never rescans bytes; the read_buffer contract keeps the
// window anchored at the current message start across prepare()/commit().
io::task<frame_result> read_frame(io::event_loop& loop, int fd, read_buffer& buf,
                                  resp::parser& parser) {
  for (;;) {
    if (buf.readable_bytes() > 0) {
      switch (parser.parse(buf.readable())) {
        case resp::parse_status::complete:
          co_return frame_result{.k = frame_result::kind::frame,
                                 .len = parser.message().raw.size()};
        case resp::parse_status::protocol_error:
          co_return frame_result{.k = frame_result::kind::protocol_error, .perr = parser.error()};
        case resp::parse_status::need_more:
          break;
      }
    }
    const std::span<char> w = buf.prepare(4096);
    const std::int32_t n = co_await loop.async_recv(fd, w);
    if (n == 0) {
      co_return frame_result{.k = frame_result::kind::eof};
    }
    if (n < 0) {
      co_return frame_result{.k = frame_result::kind::io_error, .err = n};
    }
    buf.commit(static_cast<std::size_t>(n));
  }
}

}  // namespace

server::server(io::event_loop& loop, config cfg)
    : loop_(loop),
      cfg_(std::move(cfg)),
      backend_addr_(io::resolve_tcp(cfg_.backend_host, cfg_.backend_port)),
      listener_(io::listen_tcp(cfg_.listen_host, cfg_.listen_port, cfg_.backlog)),
      port_(io::local_port(listener_.get())) {}

void server::start() {
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
  loop_.stop();  // idempotent; already-drained shutdown cancelled this sleep
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
    io::set_tcp_nodelay(fd);
    io::spawn(connection(io::unique_fd{fd}));
  }
  listener_.reset();
  if (draining_ && active_ == 0) {
    loop_.stop();
  }
}

io::task<void> server::connection(io::unique_fd client) {
  ++active_;
  io::unique_fd backend;
  try {
    backend = co_await io::connect_tcp(loop_, backend_addr_);
  } catch (const std::exception&) {
    // co_await is illegal inside a catch handler; reply after unwinding.
  }
  if (!backend) {
    (void)co_await io::send_all(loop_, client.get(), kBackendUnavailable);
  } else {
    try {
      co_await relay(client.get(), backend.get());
    } catch (const std::exception&) {
      // Never let an exception escape into spawn (it would terminate).
    }
  }
  --active_;
  if (draining_ && active_ == 0) {
    loop_.stop();
  }
}

io::task<void> server::relay(int client_fd, int backend_fd) {
  read_buffer cbuf;  // client → backend direction
  read_buffer bbuf;  // backend → client direction
  resp::parser req_parser;
  resp::parser rsp_parser;

  for (;;) {
    // 1. One complete request frame from the client.
    const frame_result req = co_await read_frame(loop_, client_fd, cbuf, req_parser);
    if (req.k == frame_result::kind::protocol_error) {
      const std::string err = fmt::format("-ERR Protocol error: {}\r\n", resp::to_string(req.perr));
      (void)co_await io::send_all(loop_, client_fd, err);
      break;
    }
    if (req.k != frame_result::kind::frame) {
      break;  // eof or io_error (including -ECANCELED during stop())
    }

    // 2. Forward the raw bytes; consume only after the send completed (the
    //    frame view points into cbuf).
    const std::string_view request = cbuf.readable().substr(0, req.len);
    if (co_await io::send_all(loop_, backend_fd, request) != 0) {
      (void)co_await io::send_all(loop_, client_fd, kBackendLost);
      break;
    }
    cbuf.consume(req.len);

    // 3. One complete response frame from the backend (strictly serial in
    //    M2; the parser counts attribute frames as part of their value, so
    //    FIFO pairing stays intact).
    const frame_result rsp = co_await read_frame(loop_, backend_fd, bbuf, rsp_parser);
    if (rsp.k != frame_result::kind::frame) {
      (void)co_await io::send_all(loop_, client_fd, kBackendLost);
      break;
    }

    // 4. Relay the reply verbatim.
    const std::string_view response = bbuf.readable().substr(0, rsp.len);
    if (co_await io::send_all(loop_, client_fd, response) != 0) {
      break;
    }
    bbuf.consume(rsp.len);
  }
}

}  // namespace vkp::proxy
