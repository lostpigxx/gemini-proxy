// Command metadata table: name → policy + key positions, plus slot routing.
// Design: docs/design/m3-workers-pool-pipelining.md §5, m4-cluster-routing.md §3.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace vkp::proxy {

// Standalone-mode disposition. Unchanged since M3; cluster mode consults
// cluster_policy instead for the rows that reach routing.
enum class cmd_policy : std::uint8_t {
  forward,  // pass through to the backend
  reject,   // connection-sticky / blocking / would poison a shared conn
  local,    // the proxy answers by itself (HELLO/QUIT/SELECT/RESET)
};

// Cluster-mode disposition. Only consulted for cmd_policy::forward rows —
// reject/local are handled before routing and behave the same in both modes.
enum class cluster_policy : std::uint8_t {
  by_key,       // route to the slot owner; keys must agree or -CROSSSLOT
  any_node,     // keyless admin command: any master will do
  local,        // the proxy answers by itself (PING/ECHO)
  unsupported,  // reject: single-node semantics, or keys we refuse to guess
};

struct command_info {
  std::string_view name;  // uppercase
  cmd_policy policy = cmd_policy::forward;
  cluster_policy cluster = cluster_policy::unsupported;
  // Key spec, redis convention: 1-based argv indices (argv[0] is the command
  // name), last_key -1 means "through the last argument", key_step is the
  // distance between consecutive keys. first_key 0 = no range spec.
  std::int8_t first_key = 0;
  std::int8_t last_key = 0;
  std::int8_t key_step = 0;
  // argv index holding the key count for `... numkeys key [key ...]` shapes
  // (0 = none). Combines with the range spec above: ZUNIONSTORE has both a
  // destination key at argv[1] and a numkeys-counted list at argv[2].
  std::int8_t numkeys_idx = 0;
};

// Case-insensitive lookup; nullptr when the command is not in the table.
[[nodiscard]] const command_info* find_command(std::string_view name) noexcept;

struct route_result {
  enum class kind : std::uint8_t {
    by_slot,    // route to the owner of `slot`
    any_node,   // no keys in this invocation: any master will do
    crossslot,  // keys span several slots; reply -CROSSSLOT
  };
  kind k = kind::any_node;
  std::uint16_t slot = 0;
};

// Hashes the command's keys and decides where it goes. Argv shapes that
// contradict the key spec (too few arguments, unparsable numkeys) yield
// any_node so the backend produces the authoritative arity error.
[[nodiscard]] route_result route_slot(std::span<const std::string_view> args,
                                      const command_info& info) noexcept;

// The keys route_slot() would hash, in argv order. Allocates — diagnostics
// and tests only, never the hot path.
[[nodiscard]] std::vector<std::string_view> collect_keys(std::span<const std::string_view> args,
                                                         const command_info& info);

}  // namespace vkp::proxy
