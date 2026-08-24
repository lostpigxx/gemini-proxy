#include "resp/parser.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vkp::resp {

namespace {

// Strict int64 line: optional '-', digits, full consumption. RESP never uses
// a leading '+' or whitespace in length/integer lines.
std::optional<std::int64_t> parse_i64(std::string_view s) {
  std::int64_t v = 0;
  const char* end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, v);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return v;
}

}  // namespace

std::string_view to_string(parse_errc e) noexcept {
  switch (e) {
    case parse_errc::none:
      return "none";
    case parse_errc::unknown_type_byte:
      return "unknown type byte";
    case parse_errc::bare_lf:
      return "LF not preceded by CR";
    case parse_errc::bad_integer:
      return "malformed integer or length";
    case parse_errc::length_out_of_range:
      return "length out of range";
    case parse_errc::line_too_long:
      return "line too long";
    case parse_errc::message_too_large:
      return "message too large";
    case parse_errc::nesting_too_deep:
      return "nesting too deep";
    case parse_errc::missing_bulk_crlf:
      return "bulk payload not CRLF-terminated";
    case parse_errc::invalid_boolean:
      return "invalid boolean";
    case parse_errc::invalid_null:
      return "invalid null";
    case parse_errc::empty_scalar:
      return "empty scalar line";
    case parse_errc::streamed_not_supported:
      return "streamed types not supported";
    case parse_errc::trailing_bytes:
      return "trailing bytes after value";
    case parse_errc::truncated_frame:
      return "truncated frame";
    case parse_errc::bad_double:
      return "malformed double";
  }
  return "unknown error";
}

void parser::reset() noexcept {
  state_ = state::header;
  pos_ = 0;
  token_start_ = 0;
  bulk_start_ = 0;
  bulk_length_ = 0;
  bulk_records_arg_ = false;
  stack_.clear();
  error_ = parse_errc::none;
  command_candidate_ = false;
  args_overflowed_ = false;
  arg_positions_.clear();
  arg_views_.clear();
  message_ = {};
}

parse_status parser::fail(parse_errc e) noexcept {
  state_ = state::failed;
  error_ = e;
  return parse_status::protocol_error;
}

bool parser::on_value_complete() {
  while (!stack_.empty()) {
    aggregate& top = stack_.back();
    if (--top.remaining > 0) {
      return false;
    }
    const bool was_attribute = top.is_attribute;
    stack_.pop_back();
    if (was_attribute) {
      // The attribute's slot still owes the annotated value.
      return false;
    }
  }
  return true;
}

void parser::record_arg(std::size_t offset, std::size_t length) {
  if (arg_positions_.size() >= limits_.max_command_args) {
    args_overflowed_ = true;
    return;
  }
  arg_positions_.push_back({offset, length});
}

parse_status parser::open_aggregate(std::uint64_t children, bool is_attribute) {
  if (children == 0) {
    if (is_attribute) {
      // Empty attribute: nothing to scan, but the slot still owes a value.
      return parse_status::need_more;
    }
    return on_value_complete() ? parse_status::complete : parse_status::need_more;
  }
  if (stack_.size() >= limits_.max_nesting_depth) {
    return fail(parse_errc::nesting_too_deep);
  }
  stack_.push_back({children, is_attribute});
  return parse_status::need_more;
}

// Interprets one complete header line [token_start_, line_end). Returns
// `complete` if this finished the whole message, `need_more` to keep
// scanning, `protocol_error` on failure.
parse_status parser::dispatch_header(std::string_view window, std::size_t line_end) {
  const std::size_t line_start = token_start_;
  const char type = window[line_start];
  const std::string_view rest = window.substr(line_start + 1, line_end - line_start - 1);
  token_start_ = pos_;

  // Command shape requires every direct child of the top-level array to be a
  // real bulk string; anything else disqualifies the frame.
  const bool top_level_child = command_candidate_ && stack_.size() == 1;
  if (top_level_child && type != '$') {
    command_candidate_ = false;
  }

  const auto value_done = [this] {
    return on_value_complete() ? parse_status::complete : parse_status::need_more;
  };

  switch (type) {
    case '+':  // simple string
    case '-':  // simple error
      return value_done();

    case ':':  // integer
      if (!parse_i64(rest)) {
        return fail(parse_errc::bad_integer);
      }
      return value_done();

    case ',':  // double
    case '(':  // big number
      // Passthrough-only fields: loose validation by design (see design doc §7).
      if (rest.empty()) {
        return fail(parse_errc::empty_scalar);
      }
      return value_done();

    case '#':  // boolean
      if (rest != "t" && rest != "f") {
        return fail(parse_errc::invalid_boolean);
      }
      return value_done();

    case '_':  // RESP3 null
      if (!rest.empty()) {
        return fail(parse_errc::invalid_null);
      }
      return value_done();

    case '$':    // bulk string
    case '=': {  // verbatim string
      if (rest == "?") {
        return fail(parse_errc::streamed_not_supported);
      }
      const auto n = parse_i64(rest);
      if (!n) {
        return fail(parse_errc::bad_integer);
      }
      if (*n == -1 && type == '$') {
        // RESP2 null bulk. Not a real bulk: disqualifies command shape.
        if (top_level_child) {
          command_candidate_ = false;
        }
        return value_done();
      }
      if (*n < 0 || static_cast<std::uint64_t>(*n) > limits_.max_bulk_bytes) {
        return fail(parse_errc::length_out_of_range);
      }
      const auto length = static_cast<std::size_t>(*n);
      if (pos_ + length + 2 > limits_.max_message_bytes) {
        return fail(parse_errc::message_too_large);
      }
      bulk_start_ = pos_;
      bulk_length_ = length;
      bulk_records_arg_ = top_level_child && type == '$' && command_candidate_;
      state_ = state::bulk_body;
      return parse_status::need_more;
    }

    case '*':    // array
    case '%':    // map
    case '~':    // set
    case '>':    // push
    case '|': {  // attribute
      if (rest == "?") {
        return fail(parse_errc::streamed_not_supported);
      }
      const auto n = parse_i64(rest);
      if (!n) {
        return fail(parse_errc::bad_integer);
      }
      if (*n == -1 && type == '*') {
        return value_done();  // RESP2 null array
      }
      if (*n < 0) {
        return fail(parse_errc::length_out_of_range);
      }
      if (type == '*' && line_start == 0 && *n >= 1) {
        // The message opens with a non-empty array: candidate command shape.
        command_candidate_ = true;
      }
      const std::uint64_t multiplier = (type == '%' || type == '|') ? 2 : 1;
      return open_aggregate(static_cast<std::uint64_t>(*n) * multiplier, type == '|');
    }

    default:
      // Also hit by inline commands (bare text line); support is a deliberate
      // TODO (valkey-cli never sends them).
      return fail(parse_errc::unknown_type_byte);
  }
}

parse_status parser::finish(std::string_view window) {
  state_ = state::done;
  message_.raw = window.substr(0, pos_);
  const bool is_command = command_candidate_ && !args_overflowed_;
  message_.is_command = is_command;
  arg_views_.clear();
  if (is_command) {
    arg_views_.reserve(arg_positions_.size());
    for (const arg_position& p : arg_positions_) {
      arg_views_.push_back(window.substr(p.offset, p.length));
    }
  }
  message_.args = arg_views_;
  return parse_status::complete;
}

parse_status parser::parse(std::string_view window) {
  if (state_ == state::done) {
    reset();
  }
  if (state_ == state::failed) {
    return parse_status::protocol_error;  // sticky: the stream is unrecoverable
  }

  while (true) {
    switch (state_) {
      case state::header: {
        const std::size_t nl = window.find('\n', pos_);
        if (nl == std::string_view::npos) {
          pos_ = window.size();
          if (pos_ - token_start_ > limits_.max_line_bytes + 2) {
            return fail(parse_errc::line_too_long);
          }
          if (pos_ > limits_.max_message_bytes) {
            return fail(parse_errc::message_too_large);
          }
          return parse_status::need_more;
        }
        if (nl == token_start_ || window[nl - 1] != '\r') {
          return fail(parse_errc::bare_lf);
        }
        if (nl - 1 - token_start_ > limits_.max_line_bytes) {
          return fail(parse_errc::line_too_long);
        }
        pos_ = nl + 1;
        if (pos_ > limits_.max_message_bytes) {
          return fail(parse_errc::message_too_large);
        }
        const parse_status st = dispatch_header(window, nl - 1);
        if (st == parse_status::complete) {
          return finish(window);
        }
        if (st == parse_status::protocol_error) {
          return st;
        }
        break;  // keep scanning
      }

      case state::bulk_body: {
        const std::size_t end = bulk_start_ + bulk_length_ + 2;
        if (window.size() < end) {
          // Payload bytes are not scanned; nothing to remember but progress.
          pos_ = window.size();
          return parse_status::need_more;
        }
        if (window[end - 2] != '\r' || window[end - 1] != '\n') {
          return fail(parse_errc::missing_bulk_crlf);
        }
        pos_ = end;
        token_start_ = pos_;
        if (bulk_records_arg_) {
          record_arg(bulk_start_, bulk_length_);
          bulk_records_arg_ = false;
        }
        state_ = state::header;
        if (on_value_complete()) {
          return finish(window);
        }
        break;
      }

      case state::done:
      case state::failed:
        return parse_status::protocol_error;  // unreachable by construction
    }
  }
}

}  // namespace vkp::resp
