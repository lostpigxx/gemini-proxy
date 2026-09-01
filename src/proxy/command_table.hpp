// Command metadata table: name → policy + key positions.
// Design: docs/design/m3-workers-pool-pipelining.md §5.
//
// M3 uses the policy (forward / reject / local); the key-position fields are
// laid down now for M4 slot routing. Unknown commands are forwarded with no
// key info — the backend produces the authoritative error.
#pragma once

#include <cstdint>
#include <string_view>

namespace vkp::proxy {

enum class cmd_policy : std::uint8_t {
  forward,  // pass through to the backend
  reject,   // connection-sticky / blocking / would poison a shared conn
  local,    // the proxy answers by itself (HELLO/QUIT/SELECT/RESET)
};

struct command_info {
  std::string_view name;  // uppercase
  cmd_policy policy = cmd_policy::forward;
  // Key spec, redis convention: 1-based argv indices (argv[0] is the command
  // name), last_key -1 means "through the last argument", key_step is the
  // distance between consecutive keys. first_key 0 = no keys.
  std::int8_t first_key = 0;
  std::int8_t last_key = 0;
  std::int8_t key_step = 0;
};

// Case-insensitive lookup; nullptr when the command is not in the table.
[[nodiscard]] const command_info* find_command(std::string_view name) noexcept;

}  // namespace vkp::proxy
