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

constexpr std::string_view kPing = "*1\r\n$4\r\nPING\r\n";

// Minimal +PONG backend running on its own loop/thread; stopped via a
// self-pipe (stop() must be called from a coroutine on the owning loop).
// `delay` holds each reply back, which is how a test gets a request that is
// genuinely still in flight when the proxy starts draining.
io::task<void> pong_conn(io::event_loop& loop, io::unique_fd fd, std::chrono::milliseconds delay) {
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
    if (delay > 0ms) {
      (void)co_await loop.sleep_for(delay);
    }
    if (co_await io::send_all(loop, fd.get(), "+PONG\r\n") != 0) {
      co_return;
    }
  }
}

io::task<void> pong_acceptor(io::event_loop& loop, int listen_fd, std::chrono::milliseconds delay) {
  for (;;) {
    const std::int32_t fd = co_await loop.async_accept(listen_fd);
    if (fd < 0) {
      co_return;
    }
    io::spawn(pong_conn(loop, io::unique_fd{fd}, delay));
  }
}

io::task<void> stopper(io::event_loop& loop, int fd) {
  char b = 0;
  (void)co_await loop.async_recv(fd, {&b, 1});
  loop.stop();
}

// The fake backend plus the thread and self-pipe it needs, so a test that
// wants one reads as one line.
class fake_backend {
 public:
  explicit fake_backend(std::chrono::milliseconds delay = 0ms) {
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, stop_) == 0);
    // event_loop contract: every fd it touches must be nonblocking.
    (void)::fcntl(stop_[0], F_SETFL, ::fcntl(stop_[0], F_GETFL, 0) | O_NONBLOCK);
    // Spawned before the thread starts, so the eager start happens with
    // nobody else on the loop.
    io::spawn(pong_acceptor(loop_, lst_.get(), delay));
    io::spawn(stopper(loop_, stop_[0]));
    thread_ = std::thread([this] { loop_.run(); });
  }

  ~fake_backend() {
    const char byte = 1;
    (void)!::write(stop_[1], &byte, 1);
    thread_.join();
    ::close(stop_[0]);
    ::close(stop_[1]);
  }

  fake_backend(const fake_backend&) = delete;
  fake_backend& operator=(const fake_backend&) = delete;

  [[nodiscard]] std::uint16_t port() const { return io::local_port(lst_.get()); }

 private:
  io::event_loop loop_;
  io::unique_fd lst_ = io::listen_tcp("127.0.0.1", 0);
  int stop_[2] = {-1, -1};
  std::thread thread_;
};

// Plain blocking client. These tests are about what a client on the other end
// of the wire actually observes, which a coroutine client sharing the proxy's
// loop cannot show: loop.stop() cancels its pending read before it can see the
// bytes already sitting in its socket buffer.
class blocking_client {
 public:
  explicit blocking_client(std::uint16_t port) : fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
    REQUIRE(fd_ >= 0);
    const timeval tv{.tv_sec = 3, .tv_usec = 0};  // a bug must not hang the suite
    (void)::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connected_ = ::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
  }

  ~blocking_client() { ::close(fd_); }

  blocking_client(const blocking_client&) = delete;
  blocking_client& operator=(const blocking_client&) = delete;

  [[nodiscard]] bool connected() const { return connected_; }

  void send(std::string_view s) const { (void)::send(fd_, s.data(), s.size(), 0); }

  // Reads until `n` bytes have arrived; returns short on close or timeout.
  [[nodiscard]] std::string read_n(std::size_t n) const {
    std::string out;
    char buf[256];
    while (out.size() < n) {
      const auto r = ::recv(fd_, buf, sizeof(buf), 0);
      if (r <= 0) {
        break;
      }
      out.append(buf, static_cast<std::size_t>(r));
    }
    return out;
  }

  // One more read: true only for a clean FIN. A reset (which is what closing
  // on top of unread data would produce) fails with ECONNRESET instead, and
  // that distinction is the whole point of the drain.
  [[nodiscard]] bool at_eof() const {
    char b = 0;
    return ::recv(fd_, &b, 1, 0) == 0;
  }

 private:
  int fd_;
  bool connected_ = false;
};

std::string blocking_ping(std::uint16_t port) {
  const blocking_client c{port};
  if (!c.connected()) {
    return {};
  }
  c.send(kPing);
  return c.read_n(7);
}

proxy::worker_pool::options pool_options(std::uint16_t backend_port, std::size_t workers) {
  proxy::worker_pool::options opt;
  opt.cfg.listen_host = "127.0.0.1";
  opt.cfg.listen_port = 0;
  opt.cfg.backend_host = "127.0.0.1";
  opt.cfg.backend_port = backend_port;
  opt.cfg.shutdown_grace = 500ms;
  opt.cfg.backend.backoff_base = 10ms;
  opt.workers = workers;
  return opt;
}

}  // namespace

TEST_CASE("worker_pool serves one port from multiple workers", "[proxy][worker_pool]") {
  const fake_backend backend;

  proxy::worker_pool pool{pool_options(backend.port(), 2)};
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
}

TEST_CASE("drain closes an idle client instead of waiting out the grace",
          "[proxy][worker_pool][shutdown]") {
  const fake_backend backend;

  proxy::worker_pool::options opt = pool_options(backend.port(), 1);
  opt.cfg.shutdown_grace = 5s;  // long enough that waiting it out is unmistakable
  proxy::worker_pool pool{opt};
  std::thread pool_thread([&pool] { pool.run(); });

  blocking_client c{pool.port()};
  REQUIRE(c.connected());
  c.send(kPing);
  REQUIRE(c.read_n(7) == "+PONG\r\n");  // now idle, and nothing will wake it

  const auto t0 = std::chrono::steady_clock::now();
  pool.request_shutdown();
  CHECK(c.at_eof());  // closed where a correct client can simply reconnect
  pool_thread.join();
  // The point of the drain: an idle pooled connection no longer holds the
  // whole shutdown open until the grace timer reaps it.
  CHECK(std::chrono::steady_clock::now() - t0 < 2s);
}

TEST_CASE("drain lets an in-flight request finish before closing",
          "[proxy][worker_pool][shutdown]") {
  const fake_backend backend{200ms};  // its reply is still in flight when drain starts

  proxy::worker_pool::options opt = pool_options(backend.port(), 1);
  opt.cfg.shutdown_grace = 5s;
  proxy::worker_pool pool{opt};
  std::thread pool_thread([&pool] { pool.run(); });

  blocking_client c{pool.port()};
  REQUIRE(c.connected());
  c.send(kPing);
  std::this_thread::sleep_for(50ms);  // long enough that the proxy has forwarded it

  const auto t0 = std::chrono::steady_clock::now();
  pool.request_shutdown();
  CHECK(c.read_n(7) == "+PONG\r\n");  // served, not dropped
  CHECK(c.at_eof());
  pool_thread.join();
  // It waited for the reply (~150 ms more), not for the 5 s grace.
  CHECK(std::chrono::steady_clock::now() - t0 < 2s);
}
