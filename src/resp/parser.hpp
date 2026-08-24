// Incremental shallow RESP2/3 parser: finds message boundaries and command
// argument positions without materializing a message tree.
// Design: docs/design/resp-buffer-and-parser.md
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "resp/limits.hpp"

namespace vkp::resp {

enum class parse_status : std::uint8_t {
  complete,        // one full message scanned; see parser::message()
  need_more,       // window ends mid-message; call again with more bytes
  protocol_error,  // malformed or limit-violating input; see parser::error()
};

enum class parse_errc : std::uint8_t {
  none = 0,
  unknown_type_byte,        // unrecognized type byte (includes inline commands, TODO)
  bare_lf,                  // '\n' not preceded by '\r'
  bad_integer,              // malformed integer / length line
  length_out_of_range,      // negative length where not allowed, or over limit
  line_too_long,            // header line exceeds max_line_bytes
  message_too_large,        // message exceeds max_message_bytes
  nesting_too_deep,         // aggregate nesting exceeds max_nesting_depth
  missing_bulk_crlf,        // bulk payload not terminated by CRLF
  invalid_boolean,          // '#' line other than "t"/"f"
  invalid_null,             // '_' line not empty
  empty_scalar,             // ','/'(' with empty content
  streamed_not_supported,   // RESP3 streamed/chunked types
  // Deep parser (resp::parse_tree) only:
  trailing_bytes,   // bytes left over after the single expected value
  truncated_frame,  // frame ended mid-value (caller violated the contract)
  bad_double,       // ',' line is not a valid RESP3 double
};

[[nodiscard]] std::string_view to_string(parse_errc e) noexcept;

// Result views point into the window passed to parse(); they are valid only
// until the underlying buffer is next mutated.
struct message_view {
  std::string_view raw;  // the full frame, for passthrough

  // True when the message has command shape: top-level array of >= 1 bulk
  // strings. args[0] is the command name.
  bool is_command = false;
  std::span<const std::string_view> args;
};

class parser {
 public:
  parser() = default;
  explicit parser(const limits& lim) : limits_(lim) {}

  // `window` must start at the first byte of the message being parsed and,
  // across calls for the same message, may only grow by appending (the old
  // window must be a prefix of the new one). Already-scanned bytes are never
  // re-examined. After `complete`, the next call starts a fresh message.
  [[nodiscard]] parse_status parse(std::string_view window);

  // Valid after parse() returned `complete`, until the next parse()/reset().
  [[nodiscard]] const message_view& message() const noexcept { return message_; }

  // Valid after parse() returned `protocol_error`.
  [[nodiscard]] parse_errc error() const noexcept { return error_; }

  // Discards all in-progress state; the next parse() starts a fresh message.
  void reset() noexcept;

 private:
  enum class state : std::uint8_t { header, bulk_body, done, failed };

  struct aggregate {
    std::uint64_t remaining;  // sub-values still owed
    bool is_attribute;        // attributes do not complete their parent's slot
  };

  parse_status fail(parse_errc e) noexcept;
  [[nodiscard]] parse_status dispatch_header(std::string_view window, std::size_t line_end);
  // Returns true when the whole message is complete.
  [[nodiscard]] bool on_value_complete();
  [[nodiscard]] parse_status finish(std::string_view window);
  void record_arg(std::size_t offset, std::size_t length);
  [[nodiscard]] parse_status open_aggregate(std::uint64_t children, bool is_attribute);

  limits limits_{};
  state state_ = state::header;
  std::size_t pos_ = 0;          // next unexamined byte (offset into window)
  std::size_t token_start_ = 0;  // offset where the current header line began
  std::size_t bulk_start_ = 0;   // payload offset while in bulk_body
  std::size_t bulk_length_ = 0;  // payload length while in bulk_body
  bool bulk_records_arg_ = false;
  std::vector<aggregate> stack_;
  parse_errc error_ = parse_errc::none;

  // Command-shape tracking (top-level array of bulks).
  struct arg_position {
    std::size_t offset;
    std::size_t length;
  };
  bool command_candidate_ = false;
  bool args_overflowed_ = false;
  std::vector<arg_position> arg_positions_;
  std::vector<std::string_view> arg_views_;
  message_view message_{};
};

}  // namespace vkp::resp
