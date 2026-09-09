#include "proxy/router.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "cluster/slot.hpp"
#include "core/buffer.hpp"
#include "io/event_loop.hpp"
#include "io/socket.hpp"
#include "io/task.hpp"
#include "proxy/command_table.hpp"
#include "resp/parser.hpp"

using namespace std::chrono_literals;
namespace io = vkp::io;
namespace proxy = vkp::proxy;
namespace cluster = vkp::cluster;

namespace {

std::vector<std::string_view> argv(std::initializer_list<std::string_view> args) {
  return {args};
}

// A stand-in cluster node. Answers CLUSTER SHARDS with whatever the test put
// in `shards`, PING with +PONG, and every other command with its own tag so
// a test can tell which node served a request.
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
      std::string reply;
      if (msg.is_command && msg.args[0] == "CLUSTER") {
        ++shards_requests;
        reply = shards.empty() ? "-ERR This instance has cluster support disabled\r\n" : shards;
      } else if (msg.is_command && msg.args[0] == "PING") {
        reply = "+PONG\r\n";
      } else {
        ++data_requests;
        reply = tag;
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
  int shards_requests = 0;
  int data_requests = 0;
};

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

// Polls `pred` until it holds or the budget runs out (so a broken router
// fails the assertion instead of hanging the suite).
io::task<void> until(io::event_loop& loop, const std::function<bool()>& pred,
                     std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!pred() && std::chrono::steady_clock::now() < deadline) {
    if (co_await loop.sleep_for(5ms) < 0) {
      co_return;
    }
  }
}

}  // namespace

TEST_CASE("parse_endpoint splits host and port", "[router]") {
  CHECK(proxy::parse_endpoint("127.0.0.1:6379").host == "127.0.0.1");
  CHECK(proxy::parse_endpoint("127.0.0.1:6379").port == 6379);
  CHECK(proxy::parse_endpoint("example.com:1").port == 1);
  CHECK(proxy::parse_endpoint("[::1]:6380").host == "::1");  // brackets stripped
  CHECK(proxy::parse_endpoint("::1:6380").host == "::1");    // rightmost colon wins

  CHECK_THROWS(proxy::parse_endpoint("127.0.0.1"));
  CHECK_THROWS(proxy::parse_endpoint(":6379"));
  CHECK_THROWS(proxy::parse_endpoint("127.0.0.1:"));
  CHECK_THROWS(proxy::parse_endpoint("127.0.0.1:0"));
  CHECK_THROWS(proxy::parse_endpoint("127.0.0.1:65536"));
  CHECK_THROWS(proxy::parse_endpoint("127.0.0.1:80x"));
}

TEST_CASE("standalone is one node owning every slot", "[router]") {
  io::event_loop loop;
  proxy::router_config cfg;  // defaults are standalone against 127.0.0.1:6379
  proxy::router r{loop, cfg};

  CHECK_FALSE(r.cluster_mode());
  CHECK_FALSE(r.may_redirect());  // no MOVED possible: clients skip the retry copy
  CHECK(r.node_count() == 1);
  CHECK(r.topology().assigned_slots() == cluster::kSlotCount);

  // Keys, keyless admin commands and unknown commands all take the same
  // route — the standalone path never consults cluster_policy or hashes.
  const auto get = r.route(argv({"GET", "k"}), proxy::find_command("GET"));
  REQUIRE(get.st == proxy::routing::status::ok);
  REQUIRE(get.get() != nullptr);
  CHECK(r.route(argv({"INFO"}), proxy::find_command("INFO")).get() == get.get());
  CHECK(r.route(argv({"SCAN", "0"}), proxy::find_command("SCAN")).get() == get.get());
  CHECK(r.route(argv({"NOSUCHCOMMAND"}), nullptr).get() == get.get());
  CHECK(r.route(argv({"MSET", "a", "1", "b", "2"}), proxy::find_command("MSET")).get() ==
        get.get());  // would be CROSSSLOT in cluster mode
}

TEST_CASE("a node's connections are handed out round-robin", "[router]") {
  io::event_loop loop;
  proxy::router_config cfg;
  cfg.conns_per_node = 2;
  proxy::router r{loop, cfg};

  const auto first = r.route(argv({"GET", "k"}), proxy::find_command("GET"));
  const auto second = r.route(argv({"GET", "k"}), proxy::find_command("GET"));
  const auto third = r.route(argv({"GET", "k"}), proxy::find_command("GET"));
  CHECK(first.get() != second.get());
  CHECK(third.get() == first.get());
}

TEST_CASE("cluster mode without a topology refuses instead of queueing", "[router]") {
  io::event_loop loop;
  proxy::router_config cfg;
  cfg.cluster_seeds = {"127.0.0.1:7000"};
  proxy::router r{loop, cfg};

  CHECK(r.cluster_mode());
  CHECK(r.may_redirect());
  CHECK(r.node_count() == 0);
  CHECK(r.route(argv({"GET", "k"}), proxy::find_command("GET")).st ==
        proxy::routing::status::unavailable);
  CHECK(r.route(argv({"INFO"}), proxy::find_command("INFO")).st ==
        proxy::routing::status::unavailable);
}

TEST_CASE("cluster mode classifies commands before it needs a topology", "[router]") {
  io::event_loop loop;
  proxy::router_config cfg;
  cfg.cluster_seeds = {"127.0.0.1:7000"};
  proxy::router r{loop, cfg};

  // Refused outright, whatever the topology says.
  CHECK(r.route(argv({"SCAN", "0"}), proxy::find_command("SCAN")).st ==
        proxy::routing::status::unsupported);
  CHECK(r.route(argv({"NOSUCHCOMMAND"}), nullptr).st == proxy::routing::status::unsupported);
  // Cross-slot is decided from the argv alone, before any node is picked.
  CHECK(r.route(argv({"MSET", "a", "1", "b", "2"}), proxy::find_command("MSET")).st ==
        proxy::routing::status::crossslot);
  CHECK(r.route(argv({"MSET", "{t}a", "1", "{t}b", "2"}), proxy::find_command("MSET")).st ==
        proxy::routing::status::unavailable);  // same slot, but nobody owns it yet
}

TEST_CASE("the router bootstraps from a seed and routes by slot", "[router]") {
  io::event_loop loop;
  fake_node n0{loop};
  fake_node n1{loop};
  n0.shards = shards_frame({{0, 8191, n0.port}, {8192, 16383, n1.port}});
  n1.shards = n0.shards;
  n0.start();
  n1.start();

  proxy::router_config cfg;
  cfg.cluster_seeds = {fmt::format("127.0.0.1:{}", n0.port)};
  cfg.refresh_interval = 30ms;
  proxy::router r{loop, cfg};
  r.start();

  io::spawn([](io::event_loop& l, proxy::router& rtr, const fake_node& a,
               const fake_node& b) -> io::task<void> {
    co_await until(l, [&rtr] { return rtr.node_count() == 2; }, 2s);
    CHECK(rtr.node_count() == 2);
    CHECK(rtr.topology().assigned_slots() == cluster::kSlotCount);

    // Every slot resolves to the node the reply said owns it.
    proxy::backend_conn* first = rtr.node_at("127.0.0.1", a.port).get();
    proxy::backend_conn* second = rtr.node_at("127.0.0.1", b.port).get();
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first != second);

    const auto owner = [&rtr](std::string_view key) {
      return rtr.route(argv({"GET", key}), proxy::find_command("GET")).get();
    };
    // Pick keys on either side of the split rather than trusting a guess.
    bool saw_low = false;
    bool saw_high = false;
    for (int i = 0; i < 64; ++i) {
      const std::string key = fmt::format("k{}", i);
      const std::uint16_t slot = cluster::key_slot(key);
      CHECK(owner(key) == (slot <= 8191 ? first : second));
      (slot <= 8191 ? saw_low : saw_high) = true;
    }
    CHECK(saw_low);
    CHECK(saw_high);

    // Keyless admin commands spread over the masters instead of piling up.
    proxy::backend_conn* any1 = rtr.route(argv({"INFO"}), proxy::find_command("INFO")).get();
    proxy::backend_conn* any2 = rtr.route(argv({"INFO"}), proxy::find_command("INFO")).get();
    CHECK(any1 != any2);

    rtr.begin_drain();
    co_await rtr.await_drained();
    l.stop();
  }(loop, r, n0, n1));

  loop.run();
  CHECK(n0.shards_requests >= 1);
}

TEST_CASE("a node that leaves the topology is retired, not dropped", "[router]") {
  io::event_loop loop;
  fake_node n0{loop};
  fake_node n1{loop};
  n0.shards = shards_frame({{0, 8191, n0.port}, {8192, 16383, n1.port}});
  n1.shards = n0.shards;
  n0.start();
  n1.start();

  proxy::router_config cfg;
  cfg.cluster_seeds = {fmt::format("127.0.0.1:{}", n0.port)};
  cfg.refresh_interval = 20ms;
  proxy::router r{loop, cfg};
  r.start();

  io::spawn([](io::event_loop& l, proxy::router& rtr, fake_node& a,
               const fake_node& b) -> io::task<void> {
    co_await until(l, [&rtr] { return rtr.node_count() == 2; }, 2s);
    REQUIRE(rtr.node_count() == 2);

    // n1 hands its slots back to n0; the next refresh should notice.
    a.shards = shards_frame({{0, 16383, a.port}});
    co_await until(l, [&rtr] { return rtr.node_count() == 1; }, 2s);
    CHECK(rtr.node_count() == 1);
    CHECK(rtr.retired_count() == 1);  // draining, still alive for its last owner
    CHECK(rtr.topology().assigned_slots() == cluster::kSlotCount);
    CHECK(rtr.route(argv({"GET", "k"}), proxy::find_command("GET")).get() ==
          rtr.node_at("127.0.0.1", a.port).get());
    CHECK(b.port != a.port);

    rtr.begin_drain();
    co_await rtr.await_drained();
    l.stop();
  }(loop, r, n0, n1));

  loop.run();
}

TEST_CASE("MOVED repoints a single slot right away", "[router]") {
  io::event_loop loop;
  fake_node n0{loop};
  fake_node n1{loop};
  n0.shards = shards_frame({{0, 16383, n0.port}});
  n0.start();
  n1.start();

  proxy::router_config cfg;
  cfg.cluster_seeds = {fmt::format("127.0.0.1:{}", n0.port)};
  cfg.refresh_interval = 10s;  // long: only the MOVED path may change anything
  proxy::router r{loop, cfg};
  r.start();

  io::spawn([](io::event_loop& l, proxy::router& rtr, const fake_node& a,
               const fake_node& b) -> io::task<void> {
    co_await until(l, [&rtr] { return rtr.node_count() == 1; }, 2s);
    REQUIRE(rtr.node_count() == 1);

    const std::uint16_t slot = cluster::key_slot("mykey");
    proxy::backend_conn* before =
        rtr.route(argv({"GET", "mykey"}), proxy::find_command("GET")).get();
    CHECK(before == rtr.node_at("127.0.0.1", a.port).get());

    rtr.apply_moved(slot, "127.0.0.1", b.port);
    CHECK(rtr.node_count() == 2);  // the new node joined the pool
    CHECK(rtr.route(argv({"GET", "mykey"}), proxy::find_command("GET")).get() ==
          rtr.node_at("127.0.0.1", b.port).get());
    // Only that one slot moved; its neighbours still point at the old owner.
    CHECK(rtr.topology().owner_of(static_cast<std::uint16_t>(slot == 0 ? 1 : slot - 1)) !=
          rtr.topology().owner_of(slot));

    rtr.begin_drain();
    co_await rtr.await_drained();
    l.stop();
  }(loop, r, n0, n1));

  loop.run();
}
