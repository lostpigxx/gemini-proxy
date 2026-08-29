#include "resp/serializer.hpp"

#include <cassert>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace vkp::resp {

namespace {

constexpr std::string_view kCrlf = "\r\n";

void append_i64(std::string& out, std::int64_t v) {
  char digits[20];  // -9223372036854775808 is 20 chars
  const auto [ptr, ec] = std::to_chars(digits, digits + sizeof(digits), v);
  assert(ec == std::errc{});
  static_cast<void>(ec);  // assert-only in release builds
  out.append(digits, ptr);
}

// assert-only: unused (and warned about) in release builds.
[[maybe_unused]] bool has_crlf(std::string_view s) {
  return s.find('\r') != std::string_view::npos || s.find('\n') != std::string_view::npos;
}

}  // namespace

void append_simple_string(std::string& out, std::string_view s) {
  assert(!has_crlf(s));
  out += '+';
  out += s;
  out += kCrlf;
}

void append_error(std::string& out, std::string_view message) {
  assert(!has_crlf(message));
  out += '-';
  out += message;
  out += kCrlf;
}

void append_integer(std::string& out, std::int64_t v) {
  out += ':';
  append_i64(out, v);
  out += kCrlf;
}

void append_bulk_string(std::string& out, std::string_view payload) {
  out += '$';
  append_i64(out, static_cast<std::int64_t>(payload.size()));
  out += kCrlf;
  out += payload;
  out += kCrlf;
}

void append_array_header(std::string& out, std::size_t count) {
  out += '*';
  append_i64(out, static_cast<std::int64_t>(count));
  out += kCrlf;
}

void append_null(std::string& out, protocol p) {
  out += p == protocol::resp2 ? std::string_view{"$-1\r\n"} : std::string_view{"_\r\n"};
}

}  // namespace vkp::resp
