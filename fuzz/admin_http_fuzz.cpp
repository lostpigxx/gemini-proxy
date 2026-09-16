// Fuzzes the admin HTTP request-line parser — the second parser in this
// project that eats input from the network, so by project convention it gets
// a target of its own.
//
// The parser is pure and allocation-free, so the target is just "feed it
// bytes and assert the contract holds". What it is really checking is that no
// input reaches a substr/find with an out-of-range index: every branch in
// there computes offsets from find() results.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "admin/http_parse.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input{reinterpret_cast<const char*>(data), size};
  const vkp::admin::parsed_request r = vkp::admin::parse_request(input);

  using state = vkp::admin::parsed_request::state;
  if (r.st == state::ok) {
    // The path must be a view into the input, and a well-formed absolute path.
    if (r.path.empty() || r.path.front() != '/') {
      __builtin_trap();
    }
    if (r.path.data() < input.data() ||
        r.path.data() + r.path.size() > input.data() + input.size()) {
      __builtin_trap();
    }
    // Stripping the query string must not leave a separator behind.
    if (r.path.find('?') != std::string_view::npos || r.path.find('#') != std::string_view::npos) {
      __builtin_trap();
    }
  } else if (!r.path.empty()) {
    __builtin_trap();  // no path unless the request parsed
  }

  // Feeding the same bytes one prefix at a time must never report a complete
  // request before the blank line arrives — that is what bounds the buffer
  // the server is willing to hold.
  if (input.find("\r\n\r\n") == std::string_view::npos && r.st != state::need_more) {
    __builtin_trap();
  }
  return 0;
}
