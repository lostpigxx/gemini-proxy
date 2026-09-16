// The admin server's HTTP request-line parser, kept in its own translation
// unit because it is the only part of the admin surface that eats bytes off
// the network — so it is what the fuzz target compiles and instruments,
// without dragging in the event loop (fuzz/admin_http_fuzz.cpp).
//
// It is deliberately the minimum HTTP/1.x a Prometheus scrape needs: the
// request line, then everything up to the blank line thrown away.
#pragma once

#include <cstdint>
#include <string_view>

namespace vkp::admin {

// What `parse_request` made of the bytes so far. The path points into the
// caller's buffer.
struct parsed_request {
  enum class state : std::uint8_t {
    need_more,    // no blank line yet
    ok,           // complete GET; `path` is set
    bad_request,  // not something that looks like HTTP/1.x at all
    not_get,      // well formed, but a method this server does not serve
  };
  state st = state::need_more;
  std::string_view path;  // query string and fragment stripped
};

// Pure and allocation-free. Never reads past `data`.
[[nodiscard]] parsed_request parse_request(std::string_view data) noexcept;

}  // namespace vkp::admin
