#include "proxy/worker_pool.hpp"

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "core/buffer.hpp"
#include "io/event_loop.hpp"
#include "io/socket.hpp"
#include "io/task.hpp"
#include "resp/parser.hpp"

using namespace std::chrono_literals;
namespace io = vkp::io;
namespace proxy = vkp::proxy;

namespace {

// Minimal +PONG backend running on its own loop/thread; stopped via a
// self-pipe (stop() must be called from a coroutine on the owning loop).
io::task<void> pong_conn(io::event_loop& loop, io::unique_fd fd) {
  vkp::read_buffer buf;
  vkp::resp::parser parser;
  for (;;) {
    for (;;) {
      if (buf.readable_bytes() > 0) {
        const auto st = parser.parse(buf.readable());
        if (st == vkp::resp::parse_status::complete) {
          break;
        }
        if (st == vkp::resp::parse_status::protocol_error) {
          co_return;
        }
      }
      const auto w = buf.prepare(1024);
      const std::int32_t n = co_await loop.async_recv(fd.get(), w);
      if (n <= 0) {
        co_return;
      }
      buf.commit(static_cast<std::size_t>(n));
    }
    buf.consume(parser.message().raw.size());
    if (co_await io::send_all(loop, fd.get(), "+PONG\r\n") != 0) {
      co_return;
    }
  }
}

io::task<void> pong_acceptor(io::event_loop& loop, int listen_fd) {
  for (;;) {
    const std::int32_t fd = co_await loop.async_accept(listen_fd);
    if (fd < 0) {
      co_return;
    }
    io::spawn(pong_conn(loop, io::unique_fd{fd}));
  }
}

io::task<void> stopper(io::event_loop& loop, int fd) {
  char b = 0;
  (void)co_await loop.async_recv(fd, {&b, 1});
  loop.stop();
}

// Plain blocking client (this test exercises real cross-thread traffic).
std::string blocking_ping(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  timeval tv{.tv_sec = 3, .tv_usec = 0};
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  std::string out;
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
    constexpr std::string_view ping = "*1\r\n$4\r\nPING\r\n";
    (void)::send(fd, ping.data(), ping.size(), 0);
    char buf[64];
    while (out.size() < 7) {
      const auto n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        break;
      }
      out.append(buf, static_cast<std::size_t>(n));
    }
  }
  ::close(fd);
  return out;
}

}  // namespace

TEST_CASE("worker_pool serves one port from multiple workers", "[proxy][worker_pool]") {
  // Fake backend on its own thread.
  io::event_loop backend_loop;
  io::unique_fd backend_lst = io::listen_tcp("127.0.0.1", 0);
  const std::uint16_t backend_port = io::local_port(backend_lst.get());
  int stop_sv[2] = {-1, -1};
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, stop_sv) == 0);
  // event_loop contract: every fd it touches must be nonblocking.
  (void)::fcntl(stop_sv[0], F_SETFL, ::fcntl(stop_sv[0], F_GETFL, 0) | O_NONBLOCK);
  io::spawn(pong_acceptor(backend_loop, backend_lst.get()));
  io::spawn(stopper(backend_loop, stop_sv[0]));
  std::thread backend_thread([&backend_loop] { backend_loop.run(); });

  proxy::worker_pool::options opt;
  opt.cfg.listen_host = "127.0.0.1";
  opt.cfg.listen_port = 0;
  opt.cfg.backend_host = "127.0.0.1";
  opt.cfg.backend_port = backend_port;
  opt.cfg.shutdown_grace = 500ms;
  opt.cfg.backend.backoff_base = 10ms;
  opt.workers = 2;
  proxy::worker_pool pool{opt};
  CHECK(pool.workers() == 2);
  const std::uint16_t port = pool.port();
  CHECK(port != 0);

  std::thread pool_thread([&pool] { pool.run(); });

  int ok = 0;
  for (int i = 0; i < 8; ++i) {
    if (blocking_ping(port) == "+PONG\r\n") {
      ++ok;
    }
  }
  CHECK(ok == 8);

  pool.request_shutdown();
  pool_thread.join();

  const char byte = 1;
  (void)!::write(stop_sv[1], &byte, 1);
  backend_thread.join();
  ::close(stop_sv[0]);
  ::close(stop_sv[1]);
}
