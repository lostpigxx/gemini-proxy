#include "cluster/topology.hpp"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "resp/value.hpp"

using vkp::cluster::kSlotCount;
using vkp::cluster::parse_cluster_shards;
using vkp::cluster::topology;
using vkp::cluster::topology_errc;

namespace {

std::string bulk(std::string_view s) {
  return "$" + std::to_string(s.size()) + "\r\n" + std::string{s} + "\r\n";
}

std::string num(long long n) {
  return ":" + std::to_string(n) + "\r\n";
}

std::string arr(std::initializer_list<std::string> items) {
  std::string out = "*" + std::to_string(items.size()) + "\r\n";
  for (const auto& item : items) {
    out += item;
  }
  return out;
}

// RESP3 map: the count is pairs, not elements.
std::string map(std::initializer_list<std::string> kv) {
  std::string out = "%" + std::to_string(kv.size() / 2) + "\r\n";
  for (const auto& item : kv) {
    out += item;
  }
  return out;
}

std::string node_entry(std::string_view id, std::string_view host, int port,
                       std::string_view role = "master", std::string_view health = "online") {
  return arr({bulk("id"), bulk(id), bulk("port"), num(port), bulk("ip"), bulk(host),
              bulk("endpoint"), bulk(host), bulk("role"), bulk(role), bulk("replication-offset"),
              num(72), bulk("health"), bulk(health)});
}

std::string shard(std::string slots, std::initializer_list<std::string> nodes) {
  return arr({bulk("slots"), std::move(slots), bulk("nodes"), arr(nodes)});
}

// The reply the container's 3-master cluster actually produces.
std::string three_masters() {
  return arr({
      shard(arr({num(0), num(5460)}), {node_entry("aaa", "127.0.0.1", 7000),
                                       node_entry("ddd", "127.0.0.1", 7003, "replica")}),
      shard(arr({num(5461), num(10922)}), {node_entry("bbb", "127.0.0.1", 7001)}),
      shard(arr({num(10923), num(16383)}), {node_entry("ccc", "127.0.0.1", 7002)}),
  });
}

// Parses `frame` and hands the tree to parse_cluster_shards. The tree's views
// point into `frame`, but the resulting topology owns its strings, so the
// result outlives the frame safely.
vkp::cluster::shards_result parse(const std::string& frame) {
  const auto tree = vkp::resp::parse_tree(frame);
  REQUIRE(static_cast<bool>(tree));
  return parse_cluster_shards(*tree);
}

}  // namespace

TEST_CASE("a healthy three-master reply covers every slot", "[cluster][topology]") {
  const auto result = parse(three_masters());
  REQUIRE(result);
  const topology& t = result.value;

  REQUIRE(t.nodes().size() == 3);
  CHECK(t.nodes()[0].address() == "127.0.0.1:7000");
  CHECK(t.nodes()[0].id == "aaa");
  CHECK(t.nodes()[1].address() == "127.0.0.1:7001");
  CHECK(t.nodes()[2].address() == "127.0.0.1:7002");

  CHECK(t.assigned_slots() == kSlotCount);
  CHECK(t.owner_of(0) == 0);
  CHECK(t.owner_of(5460) == 0);
  CHECK(t.owner_of(5461) == 1);
  CHECK(t.owner_of(10922) == 1);
  CHECK(t.owner_of(10923) == 2);
  CHECK(t.owner_of(16383) == 2);
  CHECK(t.owner_of(kSlotCount) == topology::kUnassigned);  // out of range
}

TEST_CASE("RESP3 maps parse to the same topology as RESP2 arrays", "[cluster][topology]") {
  const std::string resp3 = arr({
      map({bulk("slots"), arr({num(0), num(8191)}), bulk("nodes"),
           arr({map({bulk("id"), bulk("aaa"), bulk("port"), num(7000), bulk("endpoint"),
                     bulk("10.0.0.1"), bulk("role"), bulk("master"), bulk("health"),
                     bulk("online")})})}),
      map({bulk("slots"), arr({num(8192), num(16383)}), bulk("nodes"),
           arr({map({bulk("id"), bulk("bbb"), bulk("port"), num(7001), bulk("endpoint"),
                     bulk("10.0.0.2"), bulk("role"), bulk("master"), bulk("health"),
                     bulk("online")})})}),
  });
  const auto result = parse(resp3);
  REQUIRE(result);
  REQUIRE(result.value.nodes().size() == 2);
  CHECK(result.value.nodes()[0].address() == "10.0.0.1:7000");
  CHECK(result.value.nodes()[1].address() == "10.0.0.2:7001");
  CHECK(result.value.assigned_slots() == kSlotCount);
  CHECK(result.value.owner_of(8191) == 0);
  CHECK(result.value.owner_of(8192) == 1);
}

TEST_CASE("several slot ranges in one shard all map to its master", "[cluster][topology]") {
  const auto result = parse(arr({
      shard(arr({num(0), num(99), num(1000), num(1099)}), {node_entry("aaa", "h1", 7000)}),
      shard(arr({num(100), num(999)}), {node_entry("bbb", "h2", 7001)}),
  }));
  REQUIRE(result);
  CHECK(result.value.owner_of(0) == 0);
  CHECK(result.value.owner_of(99) == 0);
  CHECK(result.value.owner_of(100) == 1);
  CHECK(result.value.owner_of(1000) == 0);
  CHECK(result.value.owner_of(1099) == 0);
  CHECK(result.value.owner_of(1100) == topology::kUnassigned);
  CHECK(result.value.assigned_slots() == 1100);
}

TEST_CASE("a shard whose master is down leaves a hole instead of failing", "[cluster][topology]") {
  const auto result = parse(arr({
      shard(arr({num(0), num(99)}), {node_entry("aaa", "h1", 7000)}),
      shard(arr({num(100), num(199)}), {node_entry("bbb", "h2", 7001, "master", "failed"),
                                        node_entry("eee", "h5", 7004, "replica")}),
  }));
  REQUIRE(result);
  REQUIRE(result.value.nodes().size() == 1);  // the failed master is not dialed
  CHECK(result.value.owner_of(0) == 0);
  CHECK(result.value.owner_of(150) == topology::kUnassigned);
  CHECK(result.value.assigned_slots() == 100);
}

TEST_CASE("replicas and empty masters never enter the node list", "[cluster][topology]") {
  const auto result = parse(arr({
      shard(arr({num(0), num(16383)}),
            {node_entry("aaa", "h1", 7000), node_entry("ddd", "h4", 7003, "replica")}),
      shard(arr({}), {node_entry("bbb", "h2", 7001)}),  // freshly added, owns nothing
  }));
  REQUIRE(result);
  REQUIRE(result.value.nodes().size() == 1);
  CHECK(result.value.nodes()[0].address() == "h1:7000");
}

TEST_CASE("endpoint falls back to ip when it is empty or a placeholder", "[cluster][topology]") {
  const auto with_placeholder =
      arr({arr({bulk("slots"), arr({num(0), num(16383)}), bulk("nodes"),
                arr({arr({bulk("port"), num(7000), bulk("ip"), bulk("10.9.9.9"), bulk("endpoint"),
                          bulk("?"), bulk("role"), bulk("master")})})})});
  const auto result = parse(with_placeholder);
  REQUIRE(result);
  REQUIRE(result.value.nodes().size() == 1);
  CHECK(result.value.nodes()[0].address() == "10.9.9.9:7000");
  CHECK(result.value.nodes()[0].id.empty());  // the reply carried no id
}

TEST_CASE("malformed replies are rejected rather than half-applied", "[cluster][topology]") {
  const auto err_of = [](const std::string& frame) { return parse(frame).err; };

  CHECK(err_of(bulk("nope")) == topology_errc::not_an_array);
  CHECK(err_of(arr({bulk("shard")})) == topology_errc::bad_shard);
  CHECK(err_of(arr({arr({bulk("slots")})})) == topology_errc::bad_shard);           // odd length
  CHECK(err_of(arr({arr({bulk("nodes"), arr({})})})) == topology_errc::bad_shard);  // no slots

  // Slot ranges: odd count, out of range, inverted.
  CHECK(err_of(arr({shard(arr({num(0)}), {node_entry("a", "h", 7000)})})) ==
        topology_errc::bad_slots);
  CHECK(err_of(arr({shard(arr({num(0), num(16384)}), {node_entry("a", "h", 7000)})})) ==
        topology_errc::bad_slots);
  CHECK(err_of(arr({shard(arr({num(99), num(0)}), {node_entry("a", "h", 7000)})})) ==
        topology_errc::bad_slots);
  CHECK(err_of(arr({shard(bulk("0-16383"), {node_entry("a", "h", 7000)})})) ==
        topology_errc::bad_slots);

  // Node entries: missing role, missing address, missing/absurd port.
  const auto one_shard = [](std::string node) {
    return arr(
        {arr({bulk("slots"), arr({num(0), num(9)}), bulk("nodes"), arr({std::move(node)})})});
  };
  CHECK(err_of(one_shard(arr({bulk("port"), num(7000), bulk("ip"), bulk("h")}))) ==
        topology_errc::bad_node);
  CHECK(err_of(one_shard(arr({bulk("role"), bulk("master"), bulk("port"), num(7000)}))) ==
        topology_errc::bad_node);
  CHECK(err_of(one_shard(arr({bulk("role"), bulk("master"), bulk("ip"), bulk("h")}))) ==
        topology_errc::bad_node);
  CHECK(err_of(one_shard(arr({bulk("role"), bulk("master"), bulk("ip"), bulk("h"), bulk("port"),
                              num(0)}))) == topology_errc::bad_node);
  CHECK(err_of(one_shard(bulk("node"))) == topology_errc::bad_node);
}

TEST_CASE("a reply that serves no slot at all is an error", "[cluster][topology]") {
  CHECK(parse(arr({})).err == topology_errc::no_slot_owner);
  CHECK(
      parse(arr({shard(arr({num(0), num(99)}), {node_entry("a", "h", 7000, "master", "failed")})}))
          .err == topology_errc::no_slot_owner);
}

TEST_CASE("a failed parse yields an empty topology, never a partial one", "[cluster][topology]") {
  const auto result = parse(arr({
      shard(arr({num(0), num(99)}), {node_entry("aaa", "h1", 7000)}),
      shard(arr({num(100), num(99999)}), {node_entry("bbb", "h2", 7001)}),  // bad
  }));
  CHECK_FALSE(result);
  CHECK(result.value.empty());
  CHECK(result.value.assigned_slots() == 0);
}

TEST_CASE("topology mutators dedupe nodes and clamp ranges", "[cluster][topology]") {
  topology t;
  CHECK(t.empty());
  CHECK(t.owner_of(0) == topology::kUnassigned);

  const std::int16_t a = t.add_node("id-a", "h1", 7000);
  const std::int16_t b = t.add_node("id-b", "h2", 7001);
  CHECK(a == 0);
  CHECK(b == 1);
  CHECK(t.add_node("id-a", "h1", 7000) == a);  // same address: same slot in the pool
  CHECK(t.add_node("id-a", "h1", 7002) == 2);  // different port: a different node
  CHECK(t.nodes().size() == 3);

  t.assign(10, 12, b);
  CHECK(t.owner_of(9) == topology::kUnassigned);
  CHECK(t.owner_of(10) == b);
  CHECK(t.owner_of(12) == b);
  CHECK(t.owner_of(13) == topology::kUnassigned);

  t.assign(16380, 60000, a);  // clamped at the last slot
  CHECK(t.owner_of(16383) == a);
  CHECK(t.assigned_slots() == 7);

  t.assign(100, 50, a);  // inverted: ignored
  CHECK(t.owner_of(50) == topology::kUnassigned);
}
