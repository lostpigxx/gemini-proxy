#include "proxy/server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>

#include "core/buffer.hpp"
#include "io/backend.hpp"
#include "io/event_loop.hpp"
#include "io/socket.hpp"
#include "io/task.hpp"
#include "resp/parser.hpp"

using namespace std::chrono_literals;
namespace io = vkp::io;

namespace {

constexpr std::string_view kPing = "*1\r\n$4\r\nPING\r\n";

// Minimal RESP backend: replies "+PONG\r\n" to every complete frame.
io::task<void> fake_backend_conn(io::event_loop& loop, io::unique_fd fd) {
  vkp::read_buffer buf;
  vkp::resp::parser parser;
  for (;;) {
    for (;;) {  // accumulate one full frame
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

io::task<void> fake_backend_acceptor(io::event_loop& loop, int listen_fd) {
  for (;;) {
    const std::int32_t fd = co_await loop.async_accept(listen_fd);
    if (fd < 0) {
      co_return;  // -ECANCELED when the loop stops
    }
    io::spawn(fake_backend_conn(loop, io::unique_fd{fd}));
  }
}

// Sends `payload`, collects replies until `expect_len` bytes or peer close,
// then initiates proxy shutdown so loop.run() terminates.
io::task<void> client_script(io::event_loop& loop, std::uint16_t port, std::string payload,
                             std::size_t expect_len, std::string& out,
                             vkp::proxy::server& srv) {
  try {
    io::unique_fd fd = co_await io::connect_tcp(loop, io::resolve_tcp("127.0.0.1", port));
    (void)co_await io::send_all(loop, fd.get(), payload);
    char buf[4096];
    while (out.size() < expect_len) {
      const std::int32_t n = co_await loop.async_recv(fd.get(), buf);
      if (n <= 0) {
        break;
      }
      out.append(buf, static_cast<std::size_t>(n));
    }
  } catch (const std::exception&) {
  }
  srv.begin_shutdown();
}

vkp::proxy::config test_config(std::uint16_t backend_port) {
  vkp::proxy::config cfg;
  cfg.listen_port = 0;  // ephemeral
  cfg.backend_port = backend_port;
  cfg.shutdown_grace = 500ms;  // bounds the damage if a test path hangs
  return cfg;
}

}  // namespace

TEST_CASE("proxy relays frames per backend", "[proxy]") {
  for (const auto kind : io::available_backends()) {
    DYNAMIC_SECTION("backend=" << io::to_string(kind)) {
      {
        io::event_loop loop{io::make_backend(kind)};

        io::unique_fd fake_lst = io::listen_tcp("127.0.0.1", 0);
        const std::uint16_t fake_port = io::local_port(fake_lst.get());
        io::spawn(fake_backend_acceptor(loop, fake_lst.get()));

        vkp::proxy::server srv{loop, test_config(fake_port)};
        srv.start();

        SECTION("two pipelined PINGs come back as two PONGs, in order") {
          std::string out;
          io::spawn(client_script(loop, srv.port(),
                                  std::string{kPing} + std::string{kPing}, 14, out, srv));
          loop.run();
          CHECK(out == "+PONG\r\n+PONG\r\n");
          CHECK(srv.active_connections() == 0);
        }

        SECTION("protocol garbage gets an -ERR reply and a close") {
          std::string out;
          io::spawn(client_script(loop, srv.port(), "!!!bad\r\n", SIZE_MAX, out, srv));
          loop.run();
          CHECK(out.starts_with("-ERR Protocol error:"));
        }

        SECTION("immediate shutdown with no connections") {
          srv.begin_shutdown();
          const auto t0 = std::chrono::steady_clock::now();
          loop.run();
          CHECK(std::chrono::steady_clock::now() - t0 < 400ms);
        }
      }
    }
  }
}

TEST_CASE("proxy reports an unreachable backend", "[proxy]") {
  io::event_loop loop;

  std::uint16_t dead_port = 0;
  {
    io::unique_fd tmp = io::listen_tcp("127.0.0.1", 0);
    dead_port = io::local_port(tmp.get());
  }

  vkp::proxy::server srv{loop, test_config(dead_port)};
  srv.start();

  std::string out;
  io::spawn(client_script(loop, srv.port(), std::string{kPing}, SIZE_MAX, out, srv));
  loop.run();
  CHECK(out == "-ERR proxy: backend unavailable\r\n");
}
