#pragma once

#include <cstddef>
#include <cstdint>

namespace vkp::resp {

// Defensive limits shared by the shallow and deep parsers.
// Rationale for each default: docs/design/resp-buffer-and-parser.md §6.
struct limits {
  std::size_t max_message_bytes = 512u * 1024 * 1024;
  std::size_t max_bulk_bytes = 512u * 1024 * 1024;
  std::size_t max_line_bytes = 64u * 1024;
  std::uint32_t max_nesting_depth = 32;
  // Exceeding this does not fail the frame; it only disables command-argument
  // recording (giant top-level arrays are legal in backend replies).
  std::uint32_t max_command_args = 1u * 1024 * 1024;
};

}  // namespace vkp::resp
