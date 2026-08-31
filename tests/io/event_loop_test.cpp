#include "io/event_loop.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "io/backend.hpp"
#include "io/socket.hpp"
#include "io/task.hpp"

using namespace std::chrono_literals;
using vkp::io::available_backends;
using vkp::io::backend_kind;
using vkp::io::cancel_slot;
using vkp::io::connect_tcp;
using vkp::io::event_loop;
using vkp::io::listen_tcp;
using vkp::io::local_port;
using vkp::io::make_backend;
using vkp::io::resolve_tcp;
using vkp::io::send_all;
using vkp::io::spawn;
using vkp::io::task;
using vkp::io::unique_fd;

namespace {

// Coroutines report through out-parameters; Catch2 assertions stay in the
// test body (a failing REQUIRE inside a detached coroutine would terminate).

task<void> serve_echo_once(event_loop& loop, int listen_fd, std::string& seen) {
  const std::int32_t cfd = co_await loop.async_accept(listen_fd);
  if (cfd < 0) {
    co_return;
  }
  unique_fd conn{cfd};
  char buf[16384];
  for (;;) {
    const std::int32_t n = co_await loop.async_recv(conn.get(), buf);
    if (n <= 0) {
      break;
    }
    seen.append(buf, static_cast<std::size_t>(n));
    if (co_await send_all(loop, conn.get(), {buf, static_cast<std::size_t>(n)}) != 0) {
      break;
    }
  }
}

task<void> client_bulk(event_loop& loop, std::uint16_t port, const std::string& payload,
                       std::string& reply, std::int32_t& send_status) {
  unique_fd fd = co_await connect_tcp(loop, resolve_tcp("127.0.0.1", port));
  const int raw = fd.get();

  spawn([](event_loop& l, int wfd, const std::string& p, std::int32_t& st) -> task<void> {
    st = co_await send_all(l, wfd, p);
    ::shutdown(wfd, SHUT_WR);  // half-close: server sees EOF once drained
  }(loop, raw, payload, send_status));

  char buf[16384];
  for (;;) {
    const std::int32_t n = co_await loop.async_recv(raw, buf);
    if (n <= 0) {
      break;
    }
    reply.append(buf, static_cast<std::size_t>(n));
  }
}

}  // namespace

TEST_CASE("event_loop per-backend behavior", "[io][event_loop]") {
  for (const backend_kind kind : available_backends()) {
    DYNAMIC_SECTION("backend=" << vkp::io::to_string(kind)) {
      event_loop loop{make_backend(kind)};

      SECTION("sleep_for waits and run() returns when idle") {
        bool fired = false;
        const auto t0 = std::chrono::steady_clock::now();
        spawn([](event_loop& l, bool& f) -> task<void> {
          (void)co_await l.sleep_for(50ms);
          f = true;
        }(loop, fired));
        loop.run();
        CHECK(fired);
        CHECK(std::chrono::steady_clock::now() - t0 >= 50ms);
      }

      SECTION("schedule yields FIFO between two tasks") {
        std::string order;
        auto worker = [](event_loop& l, std::string& out, char tag) -> task<void> {
          for (int i = 0; i < 3; ++i) {
            out.push_back(tag);
            (void)co_await l.schedule();
          }
        };
        spawn(worker(loop, order, 'a'));
        spawn(worker(loop, order, 'b'));
        loop.run();
        CHECK(order == "ababab");
      }

      SECTION("TCP echo roundtrip over loopback") {
        unique_fd lst = listen_tcp("127.0.0.1", 0);
        const std::uint16_t port = local_port(lst.get());

        std::string seen;
        std::string reply;
        std::int32_t send_status = -1;
        const std::string payload = "hello proxy";

        spawn(serve_echo_once(loop, lst.get(), seen));
        spawn(client_bulk(loop, port, payload, reply, send_status));
        loop.run();

        CHECK(send_status == 0);
        CHECK(seen == payload);
        CHECK(reply == payload);
      }

      SECTION("bulk transfer exercises partial sends and EAGAIN re-arm") {
        unique_fd lst = listen_tcp("127.0.0.1", 0);
        const std::uint16_t port = local_port(lst.get());

        std::string payload(1 << 20, '\0');  // 1 MiB >> socket buffers
        for (std::size_t i = 0; i < payload.size(); ++i) {
          payload[i] = static_cast<char>('a' + i % 26);
        }
        std::string seen;
        std::string reply;
        std::int32_t send_status = -1;

        spawn(serve_echo_once(loop, lst.get(), seen));
        spawn(client_bulk(loop, port, payload, reply, send_status));
        loop.run();

        CHECK(send_status == 0);
        CHECK(seen.size() == payload.size());
        CHECK(reply == payload);
      }

      SECTION("connect to a dead port fails with ECONNREFUSED") {
        std::uint16_t dead_port = 0;
        {
          unique_fd lst = listen_tcp("127.0.0.1", 0);
          dead_port = local_port(lst.get());
        }  // listener closed: nobody home

        int ec = 0;
        spawn([](event_loop& l, std::uint16_t port, int& out) -> task<void> {
          try {
            unique_fd fd = co_await connect_tcp(l, resolve_tcp("127.0.0.1", port));
          } catch (const std::system_error& e) {
            out = e.code().value();
          }
        }(loop, dead_port, ec));
        loop.run();
        CHECK(ec == ECONNREFUSED);
      }

      SECTION("cancel_slot aborts a pending accept") {
        unique_fd lst = listen_tcp("127.0.0.1", 0);
        cancel_slot slot;
        std::int32_t res = 1;

        spawn([](event_loop& l, int fd, cancel_slot& s, std::int32_t& out) -> task<void> {
          out = co_await l.async_accept(fd, &s);
        }(loop, lst.get(), slot, res));
        spawn([](event_loop& l, cancel_slot& s) -> task<void> {
          (void)co_await l.sleep_for(10ms);
          l.cancel(s);
        }(loop, slot));
        loop.run();

        CHECK(res == -ECANCELED);
        CHECK(slot.requested());
      }

      SECTION("submitting through a cancelled slot completes immediately") {
        cancel_slot slot;
        loop.cancel(slot);
        std::int32_t res = 1;
        spawn([](event_loop& l, cancel_slot& s, std::int32_t& out) -> task<void> {
          out = co_await l.sleep_for(10s, &s);  // would hang the test if submitted
        }(loop, slot, res));
        loop.run();
        CHECK(res == -ECANCELED);
      }

      SECTION("stop() cancels everything and run() unwinds cleanly") {
        unique_fd lst = listen_tcp("127.0.0.1", 0);

        int sv[2] = {-1, -1};
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        unique_fd a{sv[0]};
        unique_fd b{sv[1]};
        (void)::fcntl(a.get(), F_SETFL, ::fcntl(a.get(), F_GETFL, 0) | O_NONBLOCK);

        std::int32_t accept_res = 1;
        std::int32_t recv_res = 1;
        std::int32_t sleep_res = 1;

        spawn([](event_loop& l, int fd, std::int32_t& out) -> task<void> {
          out = co_await l.async_accept(fd);
        }(loop, lst.get(), accept_res));
        spawn([](event_loop& l, int fd, std::int32_t& out) -> task<void> {
          char buf[16];
          out = co_await l.async_recv(fd, buf);
        }(loop, a.get(), recv_res));
        spawn([](event_loop& l, std::int32_t& out) -> task<void> {
          out = co_await l.sleep_for(10s, nullptr);
        }(loop, sleep_res));
        spawn([](event_loop& l) -> task<void> {
          (void)co_await l.sleep_for(10ms);
          l.stop();
        }(loop));

        const auto t0 = std::chrono::steady_clock::now();
        loop.run();  // must return long before the 10s sleeps
        CHECK(std::chrono::steady_clock::now() - t0 < 5s);
        CHECK(accept_res == -ECANCELED);
        CHECK(recv_res == -ECANCELED);
        CHECK(sleep_res == -ECANCELED);
      }
    }
  }
}
