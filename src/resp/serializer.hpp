// RESP serializer for the proxy's own replies (errors, +OK, admin responses).
// The forwarding hot path never serializes: it passes bytes through verbatim.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vkp::resp {

enum class protocol : std::uint8_t { resp2, resp3 };

inline constexpr std::string_view kOk = "+OK\r\n";
inline constexpr std::string_view kPong = "+PONG\r\n";

// `s` / `message` must not contain CR or LF (asserted in debug builds).
void append_simple_string(std::string& out, std::string_view s);
void append_error(std::string& out, std::string_view message);

void append_integer(std::string& out, std::int64_t v);
void append_bulk_string(std::string& out, std::string_view payload);
void append_array_header(std::string& out, std::size_t count);
void append_null(std::string& out, protocol p);

}  // namespace vkp::resp
