#include "resp/value.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <utility>

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

// RESP3 doubles: from_chars general format covers digits, exponents and the
// "inf"/"-inf"/"nan" spellings valkey emits.
std::optional<double> parse_f64(std::string_view s) {
  double v = 0.0;
  const char* end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, v);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return v;
}

class tree_reader {
 public:
  tree_reader(std::string_view frame, const limits& lim) : frame_(frame), limits_(lim) {}

  std::expected<value, parse_errc> read() {
    auto root = read_value(0);
    if (root && pos_ != frame_.size()) {
      return std::unexpected(parse_errc::trailing_bytes);
    }
    return root;
  }

 private:
  using result = std::expected<value, parse_errc>;

  result read_value(std::uint32_t depth) {
    // An attribute may precede any value; it annotates the value that follows.
    std::vector<value> attributes;
    while (true) {
      auto line = read_line();
      if (!line) {
        return std::unexpected(line.error());
      }
      if (line->empty()) {
        return std::unexpected(parse_errc::unknown_type_byte);
      }
      const char type = line->front();
      const std::string_view rest = line->substr(1);
      if (type == '|') {
        if (depth >= limits_.max_nesting_depth) {
          return std::unexpected(parse_errc::nesting_too_deep);
        }
        auto pairs = read_children(rest, 2, depth + 1);
        if (!pairs) {
          return std::unexpected(pairs.error());
        }
        attributes = std::move(*pairs);
        continue;  // the annotated value follows
      }
      auto v = read_typed_value(type, rest, depth);
      if (v) {
        v->attributes = std::move(attributes);
      }
      return v;
    }
  }

  result read_typed_value(char type, std::string_view rest, std::uint32_t depth) {
    value v;
    switch (type) {
      case '+':
        v.type = value::kind::simple_string;
        v.text = rest;
        return v;
      case '-':
        v.type = value::kind::error;
        v.text = rest;
        return v;
      case ':': {
        const auto n = parse_i64(rest);
        if (!n) {
          return std::unexpected(parse_errc::bad_integer);
        }
        v.type = value::kind::integer;
        v.integer = *n;
        return v;
      }
      case ',': {
        const auto d = parse_f64(rest);
        if (!d) {
          return std::unexpected(parse_errc::bad_double);
        }
        v.type = value::kind::double_number;
        v.number = *d;
        return v;
      }
      case '#':
        if (rest != "t" && rest != "f") {
          return std::unexpected(parse_errc::invalid_boolean);
        }
        v.type = value::kind::boolean;
        v.boolean = rest == "t";
        return v;
      case '(':
        if (rest.empty()) {
          return std::unexpected(parse_errc::empty_scalar);
        }
        v.type = value::kind::big_number;
        v.text = rest;
        return v;
      case '_':
        if (!rest.empty()) {
          return std::unexpected(parse_errc::invalid_null);
        }
        v.type = value::kind::null;
        return v;
      case '$':
      case '=': {
        const auto n = parse_i64(rest);
        if (!n) {
          return std::unexpected(parse_errc::bad_integer);
        }
        if (*n == -1 && type == '$') {
          v.type = value::kind::null;
          return v;
        }
        if (*n < 0 || static_cast<std::uint64_t>(*n) > limits_.max_bulk_bytes) {
          return std::unexpected(parse_errc::length_out_of_range);
        }
        const auto length = static_cast<std::size_t>(*n);
        if (frame_.size() - pos_ < length + 2) {
          return std::unexpected(parse_errc::truncated_frame);
        }
        if (frame_[pos_ + length] != '\r' || frame_[pos_ + length + 1] != '\n') {
          return std::unexpected(parse_errc::missing_bulk_crlf);
        }
        v.type = type == '$' ? value::kind::bulk_string : value::kind::verbatim_string;
        v.text = frame_.substr(pos_, length);
        pos_ += length + 2;
        return v;
      }
      case '*':
      case '%':
      case '~':
      case '>': {
        if (type == '*' && rest == "-1") {
          v.type = value::kind::null;
          return v;
        }
        if (depth >= limits_.max_nesting_depth) {
          return std::unexpected(parse_errc::nesting_too_deep);
        }
        const std::uint64_t multiplier = type == '%' ? 2 : 1;
        auto children = read_children(rest, multiplier, depth + 1);
        if (!children) {
          return std::unexpected(children.error());
        }
        switch (type) {
          case '*':
            v.type = value::kind::array;
            break;
          case '%':
            v.type = value::kind::map;
            break;
          case '~':
            v.type = value::kind::set;
            break;
          default:
            v.type = value::kind::push;
            break;
        }
        v.elements = std::move(*children);
        return v;
      }
      default:
        return std::unexpected(parse_errc::unknown_type_byte);
    }
  }

  std::expected<std::vector<value>, parse_errc> read_children(std::string_view count_line,
                                                              std::uint64_t multiplier,
                                                              std::uint32_t depth) {
    if (count_line == "?") {
      return std::unexpected(parse_errc::streamed_not_supported);
    }
    const auto n = parse_i64(count_line);
    if (!n) {
      return std::unexpected(parse_errc::bad_integer);
    }
    if (*n < 0) {
      return std::unexpected(parse_errc::length_out_of_range);
    }
    const std::uint64_t total = static_cast<std::uint64_t>(*n) * multiplier;
    std::vector<value> children;
    // Every value is >= 3 bytes ("_\r\n"), so the frame size bounds any honest
    // count; a lying count fails on truncated_frame before allocating much.
    children.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(total, (frame_.size() - pos_) / 3 + 1)));
    for (std::uint64_t i = 0; i < total; ++i) {
      auto child = read_value(depth);
      if (!child) {
        return std::unexpected(child.error());
      }
      children.push_back(std::move(*child));
    }
    return children;
  }

  // Consumes one "...\r\n" line and returns its content (without CRLF).
  std::expected<std::string_view, parse_errc> read_line() {
    const std::size_t nl = frame_.find('\n', pos_);
    if (nl == std::string_view::npos) {
      return std::unexpected(parse_errc::truncated_frame);
    }
    if (nl == pos_ || frame_[nl - 1] != '\r') {
      return std::unexpected(parse_errc::bare_lf);
    }
    if (nl - 1 - pos_ > limits_.max_line_bytes) {
      return std::unexpected(parse_errc::line_too_long);
    }
    const std::string_view line = frame_.substr(pos_, nl - 1 - pos_);
    pos_ = nl + 1;
    return line;
  }

  std::string_view frame_;
  limits limits_;
  std::size_t pos_ = 0;
};

}  // namespace

std::expected<value, parse_errc> parse_tree(std::string_view frame, const limits& lim) {
  if (frame.size() > lim.max_message_bytes) {
    return std::unexpected(parse_errc::message_too_large);
  }
  return tree_reader(frame, lim).read();
}

}  // namespace vkp::resp
