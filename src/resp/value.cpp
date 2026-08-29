#include "resp/value.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace vkp::resp {

namespace {

std::optional<std::int64_t> parse_i64(std::string_view s) {
  std::int64_t v = 0;
  const char* end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, v);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return v;
}

// RESP3 doubles: digits/exponents plus the "inf"/"-inf"/"nan" spellings
// valkey emits. strtod instead of from_chars: libc++ only grew floating-point
// from_chars in LLVM 20, and CI's Apple Clang is older. Cold path, so the
// bounce through a NUL-terminated buffer costs nothing that matters.
std::optional<double> parse_f64(std::string_view s) {
  char buf[64];  // real RESP3 double lines are far shorter
  if (s.empty() || s.size() >= sizeof(buf)) {
    return std::nullopt;
  }
  // strtod skips leading whitespace; RESP forbids it, so reject up front.
  if (std::isspace(static_cast<unsigned char>(s.front())) != 0) {
    return std::nullopt;
  }
  std::memcpy(buf, s.data(), s.size());
  buf[s.size()] = '\0';
  char* end = nullptr;
  const double v = std::strtod(buf, &end);
  if (end != buf + s.size()) {
    return std::nullopt;
  }
  return v;
}

// Error-code style throughout (fill the out-param, return parse_errc::none on
// success); the public API wraps the root into tree_result.
class tree_reader {
 public:
  tree_reader(std::string_view frame, const limits& lim) : frame_(frame), limits_(lim) {}

  parse_errc read(value& out) {
    const parse_errc err = read_value(0, out);
    if (err == parse_errc::none && pos_ != frame_.size()) {
      return parse_errc::trailing_bytes;
    }
    return err;
  }

 private:
  parse_errc read_value(std::uint32_t depth, value& out) {
    // An attribute may precede any value; it annotates the value that follows.
    std::vector<value> attributes;
    while (true) {
      std::string_view line;
      if (const parse_errc err = read_line(line); err != parse_errc::none) {
        return err;
      }
      if (line.empty()) {
        return parse_errc::unknown_type_byte;
      }
      const char type = line.front();
      const std::string_view rest = line.substr(1);
      if (type == '|') {
        if (rest == "?") {
          return parse_errc::streamed_not_supported;
        }
        const auto n = parse_i64(rest);
        if (!n) {
          return parse_errc::bad_integer;
        }
        if (*n < 0) {
          return parse_errc::length_out_of_range;
        }
        // The depth limit only guards actual descent; empty aggregates never
        // recurse and the shallow parser accepts them at any depth (the two
        // parsers must agree on every frame, fuzzer-found divergence #2).
        if (*n > 0 && depth >= limits_.max_nesting_depth) {
          return parse_errc::nesting_too_deep;
        }
        if (const parse_errc err =
                read_children(static_cast<std::uint64_t>(*n) * 2, depth + 1, attributes);
            err != parse_errc::none) {
          return err;
        }
        continue;  // the annotated value follows
      }
      const parse_errc err = read_typed_value(type, rest, depth, out);
      if (err == parse_errc::none) {
        out.attributes = std::move(attributes);
      }
      return err;
    }
  }

  parse_errc read_typed_value(char type, std::string_view rest, std::uint32_t depth, value& out) {
    switch (type) {
      case '+':
        out.type = value::kind::simple_string;
        out.text = rest;
        return parse_errc::none;
      case '-':
        out.type = value::kind::error;
        out.text = rest;
        return parse_errc::none;
      case ':': {
        const auto n = parse_i64(rest);
        if (!n) {
          return parse_errc::bad_integer;
        }
        out.type = value::kind::integer;
        out.integer = *n;
        return parse_errc::none;
      }
      case ',': {
        const auto d = parse_f64(rest);
        if (!d) {
          return parse_errc::bad_double;
        }
        out.type = value::kind::double_number;
        out.number = *d;
        return parse_errc::none;
      }
      case '#':
        if (rest != "t" && rest != "f") {
          return parse_errc::invalid_boolean;
        }
        out.type = value::kind::boolean;
        out.boolean = rest == "t";
        return parse_errc::none;
      case '(':
        if (rest.empty()) {
          return parse_errc::empty_scalar;
        }
        out.type = value::kind::big_number;
        out.text = rest;
        return parse_errc::none;
      case '_':
        if (!rest.empty()) {
          return parse_errc::invalid_null;
        }
        out.type = value::kind::null;
        return parse_errc::none;
      case '$':
      case '=': {
        const auto n = parse_i64(rest);
        if (!n) {
          return parse_errc::bad_integer;
        }
        if (*n == -1 && type == '$') {
          out.type = value::kind::null;
          return parse_errc::none;
        }
        if (*n < 0 || static_cast<std::uint64_t>(*n) > limits_.max_bulk_bytes) {
          return parse_errc::length_out_of_range;
        }
        const auto length = static_cast<std::size_t>(*n);
        if (frame_.size() - pos_ < length + 2) {
          return parse_errc::truncated_frame;
        }
        if (frame_[pos_ + length] != '\r' || frame_[pos_ + length + 1] != '\n') {
          return parse_errc::missing_bulk_crlf;
        }
        out.type = type == '$' ? value::kind::bulk_string : value::kind::verbatim_string;
        out.text = frame_.substr(pos_, length);
        pos_ += length + 2;
        return parse_errc::none;
      }
      case '*':
      case '%':
      case '~':
      case '>': {
        if (rest == "?") {
          return parse_errc::streamed_not_supported;
        }
        const auto n = parse_i64(rest);
        if (!n) {
          return parse_errc::bad_integer;
        }
        // Compare the parsed value, not the text: the shallow parser accepts
        // non-canonical spellings like "*-01" as a null array, and the two
        // parsers must agree on every frame (fuzzer-found divergence).
        if (type == '*' && *n == -1) {
          out.type = value::kind::null;
          return parse_errc::none;
        }
        if (*n < 0) {
          return parse_errc::length_out_of_range;
        }
        const std::uint64_t multiplier = type == '%' ? 2 : 1;
        const std::uint64_t total = static_cast<std::uint64_t>(*n) * multiplier;
        // Empty aggregates never recurse: see the '|' branch above.
        if (total > 0 && depth >= limits_.max_nesting_depth) {
          return parse_errc::nesting_too_deep;
        }
        if (const parse_errc err = read_children(total, depth + 1, out.elements);
            err != parse_errc::none) {
          return err;
        }
        switch (type) {
          case '*':
            out.type = value::kind::array;
            break;
          case '%':
            out.type = value::kind::map;
            break;
          case '~':
            out.type = value::kind::set;
            break;
          default:
            out.type = value::kind::push;
            break;
        }
        return parse_errc::none;
      }
      default:
        return parse_errc::unknown_type_byte;
    }
  }

  parse_errc read_children(std::uint64_t total, std::uint32_t depth, std::vector<value>& out) {
    // Every value is >= 3 bytes ("_\r\n"), so the frame size bounds any honest
    // count; a lying count fails on truncated_frame before allocating much.
    out.reserve(
        static_cast<std::size_t>(std::min<std::uint64_t>(total, (frame_.size() - pos_) / 3 + 1)));
    for (std::uint64_t i = 0; i < total; ++i) {
      value child;
      if (const parse_errc err = read_value(depth, child); err != parse_errc::none) {
        return err;
      }
      out.push_back(std::move(child));
    }
    return parse_errc::none;
  }

  // Consumes one "...\r\n" line and returns its content (without CRLF).
  parse_errc read_line(std::string_view& out) {
    const std::size_t nl = frame_.find('\n', pos_);
    if (nl == std::string_view::npos) {
      return parse_errc::truncated_frame;
    }
    if (nl == pos_ || frame_[nl - 1] != '\r') {
      return parse_errc::bare_lf;
    }
    if (nl - 1 - pos_ > limits_.max_line_bytes) {
      return parse_errc::line_too_long;
    }
    out = frame_.substr(pos_, nl - 1 - pos_);
    pos_ = nl + 1;
    return parse_errc::none;
  }

  std::string_view frame_;
  limits limits_;
  std::size_t pos_ = 0;
};

}  // namespace

tree_result parse_tree(std::string_view frame, const limits& lim) {
  if (frame.size() > lim.max_message_bytes) {
    return parse_errc::message_too_large;
  }
  value root;
  if (const parse_errc err = tree_reader(frame, lim).read(root); err != parse_errc::none) {
    return err;
  }
  return root;
}

}  // namespace vkp::resp
