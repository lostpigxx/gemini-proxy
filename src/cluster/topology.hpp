// Cluster topology: which node owns which slot.
// Pure data + pure functions — no IO, so it unit-tests against canned
// CLUSTER SHARDS replies. Design: docs/design/m4-cluster-routing.md §4.1.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cluster/slot.hpp"

namespace vkp::resp {
struct value;
}  // namespace vkp::resp

namespace vkp::cluster {

struct node {
  std::string id;    // the cluster's node id; empty when the reply omitted it
  std::string host;  // `endpoint`, falling back to `ip`
  std::uint16_t port = 0;

  [[nodiscard]] std::string address() const;  // "host:port", the node-pool key
  [[nodiscard]] bool operator==(const node& other) const noexcept {
    return port == other.port && host == other.host;
  }
};

// A snapshot of the slot map. Each worker owns its own copy and refreshes it
// independently — no cross-thread sharing anywhere (§4.2).
class topology {
 public:
  // Index into nodes() stored per slot; -1 means the slot is not served.
  static constexpr std::int16_t kUnassigned = -1;

  topology() { slots_.fill(kUnassigned); }

  [[nodiscard]] const std::vector<node>& nodes() const noexcept { return nodes_; }
  [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }
  [[nodiscard]] std::int16_t owner_of(std::uint16_t slot) const noexcept {
    return slot < kSlotCount ? slots_[slot] : kUnassigned;
  }
  [[nodiscard]] std::size_t assigned_slots() const noexcept;

  // Returns the index of the node at host:port, adding it when it is new.
  // Returns kUnassigned if the table is full (int16 indices).
  std::int16_t add_node(std::string id, std::string host, std::uint16_t port);
  // Inclusive range; out-of-range bounds are clamped away.
  void assign(std::uint16_t first, std::uint16_t last, std::int16_t index) noexcept;

 private:
  std::vector<node> nodes_;
  std::array<std::int16_t, kSlotCount> slots_{};
};

enum class topology_errc : std::uint8_t {
  none,
  not_an_array,   // the reply is not an array of shards
  bad_shard,      // a shard is not an even-length key/value aggregate
  bad_slots,      // the `slots` field is malformed or out of range
  bad_node,       // a node entry is malformed
  no_slot_owner,  // parsed fine, but not a single slot ended up served
};

[[nodiscard]] std::string_view describe(topology_errc err) noexcept;

struct shards_result {
  topology_errc err = topology_errc::none;
  topology value;

  explicit operator bool() const noexcept { return err == topology_errc::none; }
};

// Builds a topology from a `CLUSTER SHARDS` reply (RESP2 arrays and RESP3
// maps are both accepted — the value tree flattens both to key/value pairs).
//
// Only `role == master && health == online` nodes are taken; replicas are a
// backlog item. Shards that own no slot are skipped entirely, so an empty
// master never costs a connection. A shard whose master is down leaves its
// slots unassigned rather than failing the whole parse — during a failover a
// partial map beats no map at all.
[[nodiscard]] shards_result parse_cluster_shards(const resp::value& reply);

}  // namespace vkp::cluster
