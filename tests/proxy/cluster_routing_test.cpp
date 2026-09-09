// End-to-end cluster routing through a real proxy::server backed by fake
// cluster nodes. Design: docs/design/m4-cluster-routing.md §5.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "cluster/slot.hpp"
#include "core/buffer.hpp"
#include "io/event_loop.hpp"
#include "io/socket.hpp"
#include "io/task.hpp"
#include "proxy/router.hpp"
#include "proxy/server.hpp"
#include "resp/parser.hpp"

using namespace std::chrono_literals;
namespace io = vkp::io;
namespace proxy = vkp::proxy;
namespace cluster = vkp::cluster;

namespace {

// A stand-in cluster node.
//
// CLUSTER SHARDS is answered from `shards` while `shards_budget` lasts and
// with an error afterwards: the redirect tests want exactly one bootstrap and
// no further topology churn, since apply_moved() pokes the refresher.
// Data commands on `redirect_key` get `redirect` back while
// `redirect_budget` lasts, everything else gets the node's own tag. Served
// keys land in `seen` so a test can tell who handled what.
struct fake_node {
  explicit fake_node(io::event_loop& l)
      : loop(l),
        listener(io::listen_tcp("127.0.0.1", 0)),
        port(io::local_port(listener.get())),
        tag(fmt::format("+node{}\r\n", port)) {}

  void start() { io::spawn(acceptor()); }

  io::task<void> acceptor() {
    for (;;) {
      const std::int32_t fd = co_await loop.async_accept(listener.get());
      if (fd < 0) {
        co_return;  // -ECANCELED when the loop stops
      }
      io::setup_stream_socket(fd);
      io::spawn(session(io::unique_fd{fd}));
    }
  }

  io::task<void> session(io::unique_fd fd) {
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
        const auto w = buf.prepare(4096);
        const std::int32_t n = co_await loop.async_recv(fd.get(), w);
        if (n <= 0) {
          co_return;
        }
        buf.commit(static_cast<std::size_t>(n));
      }
      const auto& msg = parser.message();
      const std::string_view key = msg.args.size() >= 2 ? msg.args[1] : std::string_view{};
      std::string reply;
      if (msg.args[0] == "CLUSTER") {
        // Always a reply, even for a node with no canned topology: a silent
        // node would desync this connection's FIFO pairing.
        reply =
            (!shards.empty() && shards_budget-- > 0) ? shards : "-ERR cluster support disabled\r\n";
      } else if (msg.args[0] == "PING") {
        reply = "+PONG\r\n";
      } else if (msg.args[0] == "ASKING") {
        ++asking;
        reply = "+OK\r\n";
      } else {
        seen.emplace_back(key);
        reply = (key == redirect_key && redirect_budget-- > 0) ? redirect : tag;
      }
      buf.consume(msg.raw.size());
      if (co_await io::send_all(loop, fd.get(), reply) != 0) {
        co_return;
      }
    }
  }

  io::event_loop& loop;
  io::unique_fd listener;
  std::uint16_t port;
  std::string tag;
  std::string shards;
  int shards_budget = 1;
  std::string redirect_key;
  std::string redirect;
  int redirect_budget = 0;
  int asking = 0;
  std::vector<std::string> seen;
};

std::ptrdiff_t served(const fake_node& n, std::string_view key) {
  return std::ranges::count(n.seen, key);
}

// CLUSTER SHARDS reply builders (RESP2 shape, as redis 7.0 emits).
std::string bulk(std::string_view s) {
  return fmt::format("${}\r\n{}\r\n", s.size(), s);
}
std::string num(long long n) {
  return fmt::format(":{}\r\n", n);
}
std::string arr(std::initializer_list<std::string> items) {
  std::string out = fmt::format("*{}\r\n", items.size());
  for (const auto& i : items) {
    out += i;
  }
  return out;
}

struct range {
  int first;
  int last;
  std::uint16_t port;
};

std::string shards_frame(std::initializer_list<range> ranges) {
  std::string out = fmt::format("*{}\r\n", ranges.size());
  for (const range& r : ranges) {
    out += arr({bulk("slots"), arr({num(r.first), num(r.last)}), bulk("nodes"),
                arr({arr({bulk("id"), bulk(fmt::format("id{}", r.port)), bulk("port"), num(r.port),
                          bulk("ip"), bulk("127.0.0.1"), bulk("endpoint"), bulk("127.0.0.1"),
                          bulk("role"), bulk("master"), bulk("health"), bulk("online")})})});
  }
  return out;
}

std::string get_cmd(std::string_view key) {
  return fmt::format("*2\r\n$3\r\nGET\r\n${}\r\n{}\r\n", key.size(), key);
}

proxy::config cluster_config(std::uint16_t seed_port) {
  proxy::config cfg;
  cfg.listen_port = 0;  // ephemeral
  cfg.cluster_seeds = {fmt::format("127.0.0.1:{}", seed_port)};
  cfg.cluster_refresh = 10s;  // long: only the redirect paths may move slots
  cfg.shutdown_grace = 1s;    // bounds the damage if a test path hangs
  cfg.backend.backoff_base = 10ms;
  cfg.backend.backoff_max = 50ms;
  return cfg;
}

// Reads until `sink` holds `want` bytes or the peer goes away.
io::task<void> collect(io::event_loop& loop, int fd, std::size_t want, std::string& sink) {
  char buf[8192];
  while (sink.size() < want) {
    const std::int32_t n = co_await loop.async_recv(fd, buf);
    if (n <= 0) {
      co_return;
    }
    sink.append(buf, static_cast<std::size_t>(n));
  }
}

// Waits out the bootstrap fetch (requests before it lands are refused, by
// design), then runs `steps` one at a time, each {command, expected reply
// bytes}. Strictly sequential: pipelining the next command would route it on
// the topology as it was *before* the previous redirect landed, which is
// correct behaviour but not what these tests are measuring.
io::task<void> client_script(io::event_loop& loop, proxy::server& srv,
                             std::vector<std::pair<std::string, std::size_t>> steps,
                             std::string& out) {
  try {
    io::unique_fd fd = co_await io::connect_tcp(loop, io::resolve_tcp("127.0.0.1", srv.port()));
    for (int attempt = 0; attempt < 400; ++attempt) {
      (void)co_await io::send_all(loop, fd.get(), get_cmd("warmup"));
      std::string probe;
      co_await collect(loop, fd.get(), 1, probe);
      if (probe.empty() || probe.find("topology unavailable") == std::string_view::npos) {
        break;
      }
      (void)co_await loop.sleep_for(5ms);
    }
    for (const auto& [command, want] : steps) {
      const std::size_t before = out.size();
      (void)co_await io::send_all(loop, fd.get(), command);
      co_await collect(loop, fd.get(), before + want, out);
    }
  } catch (const std::exception&) {
  }
  srv.begin_shutdown();
}

}  // namespace

TEST_CASE("MOVED is retried on the new node and updates the topology", "[cluster][redirect]") {
  io::event_loop loop;
  fake_node n0{loop};
  fake_node n1{loop};
  n0.shards = shards_frame({{0, 16383, n0.port}});  // n0 claims everything...
  n0.redirect_key = "mykey";                        // ...but hands this one key over, once
  n0.redirect = fmt::format("-MOVED {} 127.0.0.1:{}\r\n", cluster::key_slot("mykey"), n1.port);
  n0.redirect_budget = 1;
  n0.start();
  n1.start();

  proxy::server srv{loop, cluster_config(n0.port)};
  srv.start();

  // Two GETs of the same key: the first eats the MOVED, the second must go
  // straight to n1 because apply_moved() already repointed the slot.
  std::string out;
  io::spawn(client_script(
      loop, srv, {{get_cmd("mykey"), n1.tag.size()}, {get_cmd("mykey"), n1.tag.size()}}, out));
  loop.run();

  CHECK(out == n1.tag + n1.tag);  // the redirect never reaches the client
  CHECK(served(n1, "mykey") == 2);
  CHECK(served(n0, "mykey") == 1);  // only the attempt that got redirected
  CHECK(n1.asking == 0);            // MOVED is permanent: no ASKING
}

TEST_CASE("ASK sends ASKING first and leaves the topology alone", "[cluster][redirect]") {
  io::event_loop loop;
  fake_node n0{loop};
  fake_node n1{loop};
  n0.shards = shards_frame({{0, 16383, n0.port}});
  n0.redirect_key = "mykey";
  n0.redirect = fmt::format("-ASK {} 127.0.0.1:{}\r\n", cluster::key_slot("mykey"), n1.port);
  n0.redirect_budget = 1;
  n0.start();
  n1.start();

  proxy::server srv{loop, cluster_config(n0.port)};
  srv.start();

  std::string out;
  io::spawn(client_script(
      loop, srv, {{get_cmd("mykey"), n1.tag.size()}, {get_cmd("mykey"), n0.tag.size()}}, out));
  loop.run();

  // First reply from n1 (asked), second from n0 again: an ASK is a one-shot
  // hop, so the slot must still point at n0 afterwards.
  CHECK(out == n1.tag + n0.tag);
  CHECK(n1.asking == 1);
  CHECK(served(n1, "mykey") == 1);
  CHECK(served(n0, "mykey") == 2);
}

TEST_CASE("a redirect loop is capped instead of spinning forever", "[cluster][redirect]") {
  io::event_loop loop;
  fake_node n0{loop};
  fake_node n1{loop};
  const std::uint16_t slot = cluster::key_slot("mykey");
  n0.shards = shards_frame({{0, 16383, n0.port}});
  // The two nodes bounce the key back and forth forever.
  n0.redirect_key = "mykey";
  n1.redirect_key = "mykey";
  n0.redirect = fmt::format("-MOVED {} 127.0.0.1:{}\r\n", slot, n1.port);
  n1.redirect = fmt::format("-MOVED {} 127.0.0.1:{}\r\n", slot, n0.port);
  n0.redirect_budget = 1000;
  n1.redirect_budget = 1000;
  n0.start();
  n1.start();

  proxy::config cfg = cluster_config(n0.port);
  cfg.max_redirects = 3;
  proxy::server srv{loop, cfg};
  srv.start();

  std::string out;
  io::spawn(
      client_script(loop, srv, {{get_cmd("mykey"), proxy::kErrTooManyRedirects.size()}}, out));
  loop.run();

  CHECK(out == proxy::kErrTooManyRedirects);
  // The original attempt plus exactly max_redirects retries, no more.
  CHECK(served(n0, "mykey") + served(n1, "mykey") == 4);
}

TEST_CASE("standalone passes a redirect through instead of chasing it", "[cluster][redirect]") {
  io::event_loop loop;
  fake_node backend{loop};
  // A standalone backend has no business emitting MOVED, but if it does the
  // proxy must hand it to the client verbatim: may_redirect() is false, so no
  // retry copy was kept and nothing tries to route on it.
  backend.redirect_key = "mykey";
  backend.redirect = "-MOVED 0 127.0.0.1:1\r\n";
  backend.redirect_budget = 1;
  backend.start();

  proxy::config cfg;
  cfg.listen_port = 0;
  cfg.backend_port = backend.port;
  cfg.shutdown_grace = 1s;
  proxy::server srv{loop, cfg};
  srv.start();

  std::string out;
  io::spawn([](io::event_loop& l, proxy::server& s, std::string& sink) -> io::task<void> {
    io::unique_fd fd = co_await io::connect_tcp(l, io::resolve_tcp("127.0.0.1", s.port()));
    (void)co_await io::send_all(l, fd.get(), get_cmd("mykey"));
    co_await collect(l, fd.get(), std::string_view{"-MOVED 0 127.0.0.1:1\r\n"}.size(), sink);
    s.begin_shutdown();
  }(loop, srv, out));
  loop.run();

  CHECK(out == "-MOVED 0 127.0.0.1:1\r\n");
}
