#include <chrono>
#include <functional>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "core/buffer.hpp"
#include "io/backend.hpp"
#include "io/event_loop.hpp"
#include "io/socket.hpp"
#include "io/task.hpp"
#include "proxy/backend_conn.hpp"
#include "proxy/server.hpp"
#include "resp/parser.hpp"

using namespace std::chrono_literals;
namespace io = vkp::io;
namespace proxy = vkp::proxy;

namespace {

constexpr std::string_view kPing = "*1\r\n$4\r\nPING\r\n";

std::string echo_cmd(std::string_view v) {
  return fmt::format("*2\r\n$4\r\nECHO\r\n${}\r\n{}\r\n", v.size(), v);
}
std::string echo_reply(std::string_view v) {
  return fmt::format("${}\r\n{}\r\n", v.size(), v);
}

// Scriptable fake backend. echo: ECHO → bulk arg, PING → +PONG, else +OK.
// silent: reads frames, never replies. first_then_close: replies to the
// first frame, closes cleanly after reading the second.
struct fake_behavior {
  enum class mode : std::uint8_t { echo, silent, first_then_close };
  mode m = mode::echo;
  int* ping_count = nullptr;
};

io::task<void> fake_conn(io::event_loop& loop, io::unique_fd fd, fake_behavior fb) {
  vkp::read_buffer buf;
  vkp::resp::parser parser;
  int seen = 0;
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
      const auto w = buf.prepare(4096);
      const std::int32_t n = co_await loop.async_recv(fd.get(), w);
      if (n <= 0) {
        co_return;
      }
      buf.commit(static_cast<std::size_t>(n));
    }
    const auto& msg = parser.message();
    std::string reply = "+OK\r\n";
    if (msg.is_command && msg.args[0] == "PING") {
      if (fb.ping_count != nullptr) {
        ++*fb.ping_count;
      }
      reply = "+PONG\r\n";
    } else if (msg.is_command && msg.args.size() >= 2) {
      reply = echo_reply(msg.args[1]);
    }
    buf.consume(msg.raw.size());
    ++seen;

    switch (fb.m) {
      case fake_behavior::mode::echo:
        if (co_await io::send_all(loop, fd.get(), reply) != 0) {
          co_return;
        }
        break;
      case fake_behavior::mode::silent:
        break;  // swallow
      case fake_behavior::mode::first_then_close:
        if (seen == 1) {
          if (co_await io::send_all(loop, fd.get(), reply) != 0) {
            co_return;
          }
        } else {
          co_return;  // clean FIN, nothing unread
        }
        break;
    }
  }
}

io::task<void> fake_acceptor(io::event_loop& loop, int listen_fd, fake_behavior fb) {
  for (;;) {
    const std::int32_t fd = co_await loop.async_accept(listen_fd);
    if (fd < 0) {
      co_return;  // -ECANCELED when the loop stops
    }
    io::spawn(fake_conn(loop, io::unique_fd{fd}, fb));
  }
}

// Sends `payload`, collects replies until `expect_len` bytes (or peer close
// when expect_eof), then reports done.
io::task<void> client_script(io::event_loop& loop, std::uint16_t port, std::string payload,
                             std::size_t expect_len, bool expect_eof, std::string& out,
                             std::function<void()> done) {
  try {
    io::unique_fd fd = co_await io::connect_tcp(loop, io::resolve_tcp("127.0.0.1", port));
    (void)co_await io::send_all(loop, fd.get(), payload);
    char buf[8192];
    while (expect_eof || out.size() < expect_len) {
      const std::int32_t n = co_await loop.async_recv(fd.get(), buf);
      if (n <= 0) {
        break;
      }
      out.append(buf, static_cast<std::size_t>(n));
    }
  } catch (const std::exception&) {
  }
  done();
}

proxy::config test_config(std::uint16_t backend_port) {
  proxy::config cfg;
  cfg.listen_port = 0;  // ephemeral
  cfg.backend_port = backend_port;
  cfg.shutdown_grace = 500ms;  // bounds the damage if a test path hangs
  cfg.backend.backoff_base = 10ms;
  cfg.backend.backoff_max = 50ms;
  return cfg;
}

// Shutdown trigger shared by N clients.
struct countdown {
  int n;
  proxy::server* srv;
  void done() {
    if (--n == 0) {
      srv->begin_shutdown();
    }
  }
};

std::uint16_t grab_free_port() {
  io::unique_fd tmp = io::listen_tcp("127.0.0.1", 0);
  return io::local_port(tmp.get());  // freed when tmp closes
}

}  // namespace

TEST_CASE("proxy relays frames per backend", "[proxy]") {
  for (const auto kind : io::available_backends()) {
    DYNAMIC_SECTION("backend=" << io::to_string(kind)) {
      io::event_loop loop{io::make_backend(kind)};

      io::unique_fd fake_lst = io::listen_tcp("127.0.0.1", 0);
      const std::uint16_t fake_port = io::local_port(fake_lst.get());
      io::spawn(fake_acceptor(loop, fake_lst.get(), {}));

      proxy::server srv{loop, test_config(fake_port)};
      srv.start();
      countdown cd{.n = 1, .srv = &srv};

      SECTION("two pipelined PINGs come back as two PONGs, in order") {
        std::string out;
        io::spawn(client_script(loop, srv.port(), std::string{kPing} + std::string{kPing}, 14,
                                false, out, [&cd] { cd.done(); }));
        loop.run();
        CHECK(out == "+PONG\r\n+PONG\r\n");
        CHECK(srv.active_connections() == 0);
      }

      SECTION("protocol garbage gets an -ERR reply and a close") {
        std::string out;
        io::spawn(
            client_script(loop, srv.port(), "!!!bad\r\n", 0, true, out, [&cd] { cd.done(); }));
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

TEST_CASE("interleaved pipelines from two clients keep per-client order", "[proxy]") {
  io::event_loop loop;
  io::unique_fd fake_lst = io::listen_tcp("127.0.0.1", 0);
  io::spawn(fake_acceptor(loop, fake_lst.get(), {}));

  proxy::server srv{loop, test_config(io::local_port(fake_lst.get()))};
  srv.start();
  countdown cd{.n = 2, .srv = &srv};

  constexpr int kN = 20;
  std::string payload_a;
  std::string expect_a;
  std::string payload_b;
  std::string expect_b;
  for (int i = 0; i < kN; ++i) {
    payload_a += echo_cmd(fmt::format("a{}", i));
    expect_a += echo_reply(fmt::format("a{}", i));
    payload_b += echo_cmd(fmt::format("b{}", i));
    expect_b += echo_reply(fmt::format("b{}", i));
  }

  std::string out_a;
  std::string out_b;
  io::spawn(client_script(loop, srv.port(), payload_a, expect_a.size(), false, out_a,
                          [&cd] { cd.done(); }));
  io::spawn(client_script(loop, srv.port(), payload_b, expect_b.size(), false, out_b,
                          [&cd] { cd.done(); }));
  loop.run();
  CHECK(out_a == expect_a);
  CHECK(out_b == expect_b);
}

TEST_CASE("rejected command replies stay in pipeline order", "[proxy]") {
  io::event_loop loop;
  io::unique_fd fake_lst = io::listen_tcp("127.0.0.1", 0);
  io::spawn(fake_acceptor(loop, fake_lst.get(), {}));

  proxy::server srv{loop, test_config(io::local_port(fake_lst.get()))};
  srv.start();
  countdown cd{.n = 1, .srv = &srv};

  const std::string payload =
      echo_cmd("a") + "*2\r\n$9\r\nSUBSCRIBE\r\n$1\r\nx\r\n" + echo_cmd("b");
  const std::string expect =
      echo_reply("a") + "-ERR unsupported by proxy: SUBSCRIBE\r\n" + echo_reply("b");

  std::string out;
  io::spawn(
      client_script(loop, srv.port(), payload, expect.size(), false, out, [&cd] { cd.done(); }));
  loop.run();
  CHECK(out == expect);
}

TEST_CASE("connection-state commands are answered locally", "[proxy]") {
  io::event_loop loop;
  io::unique_fd fake_lst = io::listen_tcp("127.0.0.1", 0);
  io::spawn(fake_acceptor(loop, fake_lst.get(), {}));

  proxy::server srv{loop, test_config(io::local_port(fake_lst.get()))};
  srv.start();
  countdown cd{.n = 1, .srv = &srv};

  SECTION("HELLO variants, SELECT, RESET") {
    const std::string payload =
        std::string{"*1\r\n$5\r\nHELLO\r\n"} + "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n" +
        "*2\r\n$5\r\nHELLO\r\n$1\r\n9\r\n" + "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n" +
        "*2\r\n$6\r\nSELECT\r\n$1\r\n2\r\n" + "*1\r\n$5\r\nRESET\r\n";
    std::string out;
    // Parse count: read until the final +RESET shows up.
    io::spawn(client_script(loop, srv.port(), payload, SIZE_MAX, true, out, [&cd] { cd.done(); }));
    io::spawn([](io::event_loop& l, proxy::server& s) -> io::task<void> {
      (void)co_await l.sleep_for(200ms);  // give replies time, then close via drain
      s.begin_shutdown();
      s.begin_shutdown();  // force: client reads to EOF
    }(loop, srv));
    loop.run();
    CHECK(out.starts_with("*14\r\n"));                          // HELLO → RESP2 shape
    CHECK(out.find("valkey-proxy") != std::string::npos);       // self-description
    CHECK(out.find("%7\r\n") != std::string::npos);             // HELLO 3 → RESP3 map
    CHECK(out.find("-NOPROTO") != std::string::npos);           // HELLO 9
    CHECK(out.find("+OK\r\n") != std::string::npos);            // SELECT 0
    CHECK(out.find("SELECT is limited") != std::string::npos);  // SELECT 2
    CHECK(out.find("+RESET\r\n") != std::string::npos);         // RESET
  }

  SECTION("QUIT flushes earlier replies, then +OK, then close") {
    const std::string payload = echo_cmd("a") + "*1\r\n$4\r\nQUIT\r\n";
    const std::string expect = echo_reply("a") + "+OK\r\n";
    std::string out;
    io::spawn(client_script(loop, srv.port(), payload, 0, true, out, [&cd] { cd.done(); }));
    loop.run();
    CHECK(out == expect);
  }
}

TEST_CASE("proxy reports an unreachable backend and keeps the client open", "[proxy]") {
  io::event_loop loop;
  const std::uint16_t dead_port = grab_free_port();

  proxy::server srv{loop, test_config(dead_port)};
  srv.start();
  countdown cd{.n = 1, .srv = &srv};

  std::string out;
  io::spawn(client_script(loop, srv.port(), std::string{kPing},
                          proxy::kErrBackendUnavailable.size(), false, out, [&cd] { cd.done(); }));
  loop.run();
  CHECK(out == proxy::kErrBackendUnavailable);
}

TEST_CASE("request timeout fails the request and rebuilds the connection", "[proxy]") {
  io::event_loop loop;
  io::unique_fd fake_lst = io::listen_tcp("127.0.0.1", 0);
  io::spawn(fake_acceptor(loop, fake_lst.get(), {.m = fake_behavior::mode::silent}));

  proxy::config cfg = test_config(io::local_port(fake_lst.get()));
  cfg.backend.request_timeout = 80ms;
  proxy::server srv{loop, cfg};
  srv.start();
  countdown cd{.n = 1, .srv = &srv};

  std::string out;
  const auto t0 = std::chrono::steady_clock::now();
  io::spawn(client_script(loop, srv.port(), echo_cmd("x"), proxy::kErrTimeout.size(), false, out,
                          [&cd] { cd.done(); }));
  loop.run();
  CHECK(out == proxy::kErrTimeout);
  CHECK(std::chrono::steady_clock::now() - t0 < 400ms);  // 80ms timeout, not the grace
}

TEST_CASE("backend dying mid-pipeline fails the remaining requests", "[proxy]") {
  io::event_loop loop;
  io::unique_fd fake_lst = io::listen_tcp("127.0.0.1", 0);
  io::spawn(fake_acceptor(loop, fake_lst.get(), {.m = fake_behavior::mode::first_then_close}));

  proxy::server srv{loop, test_config(io::local_port(fake_lst.get()))};
  srv.start();
  countdown cd{.n = 1, .srv = &srv};

  const std::string expect = echo_reply("r1") + std::string{proxy::kErrBackendLost};
  std::string out;
  io::spawn(client_script(loop, srv.port(), echo_cmd("r1") + echo_cmd("r2"), expect.size(), false,
                          out, [&cd] { cd.done(); }));
  loop.run();
  CHECK(out == expect);
}

TEST_CASE("proxy reconnects after the backend comes back", "[proxy]") {
  io::event_loop loop;
  const std::uint16_t port = grab_free_port();

  proxy::server srv{loop, test_config(port)};  // nothing listens yet
  srv.start();

  io::spawn([](io::event_loop& l, proxy::server& s, std::uint16_t backend_port) -> io::task<void> {
    // Let the first connect attempt fail, then bring the backend up.
    (void)co_await l.sleep_for(50ms);
    io::unique_fd lst = io::listen_tcp("127.0.0.1", backend_port);
    io::spawn(fake_acceptor(l, lst.get(), {}));

    // Retry until the proxy answers through the revived backend.
    std::string ok;
    for (int i = 0; i < 100 && ok.empty(); ++i) {
      try {
        io::unique_fd fd = co_await io::connect_tcp(l, io::resolve_tcp("127.0.0.1", s.port()));
        (void)co_await io::send_all(l, fd.get(), kPing);
        char buf[256];
        const std::int32_t n = co_await l.async_recv(fd.get(), buf);
        if (n > 0 && buf[0] == '+') {
          ok.assign(buf, static_cast<std::size_t>(n));
        }
      } catch (const std::exception&) {
      }
      if (ok.empty()) {
        (void)co_await l.sleep_for(20ms);
      }
    }
    CHECK(ok == "+PONG\r\n");
    s.begin_shutdown();
    (void)lst.release();  // acceptor coroutine still holds the raw fd; loop stop reaps it
  }(loop, srv, port));

  loop.run();
}

TEST_CASE("deep pipeline under max_inflight=1 stays correct", "[proxy]") {
  io::event_loop loop;
  io::unique_fd fake_lst = io::listen_tcp("127.0.0.1", 0);
  io::spawn(fake_acceptor(loop, fake_lst.get(), {}));

  proxy::config cfg = test_config(io::local_port(fake_lst.get()));
  cfg.backend.max_inflight = 1;  // every request waits for the previous one
  proxy::server srv{loop, cfg};
  srv.start();
  countdown cd{.n = 1, .srv = &srv};

  std::string payload;
  std::string expect;
  for (int i = 0; i < 10; ++i) {
    payload += echo_cmd(fmt::format("v{}", i));
    expect += echo_reply(fmt::format("v{}", i));
  }
  std::string out;
  io::spawn(
      client_script(loop, srv.port(), payload, expect.size(), false, out, [&cd] { cd.done(); }));
  loop.run();
  CHECK(out == expect);
}

TEST_CASE("idle backend connections get health-check PINGs", "[proxy]") {
  io::event_loop loop;
  io::unique_fd fake_lst = io::listen_tcp("127.0.0.1", 0);
  int pings = 0;
  io::spawn(fake_acceptor(loop, fake_lst.get(), {.ping_count = &pings}));

  proxy::config cfg = test_config(io::local_port(fake_lst.get()));
  cfg.backend.health_interval = 30ms;
  proxy::server srv{loop, cfg};
  srv.start();

  io::spawn([](io::event_loop& l, proxy::server& s) -> io::task<void> {
    (void)co_await l.sleep_for(200ms);
    s.begin_shutdown();
  }(loop, srv));
  loop.run();
  CHECK(pings >= 2);
}
