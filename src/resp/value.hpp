// Deep RESP parser: materializes a complete frame into a value tree.
// Control-plane only (e.g. CLUSTER SHARDS replies); the data-plane hot path
// uses the shallow resp::parser. Design: docs/design/resp-buffer-and-parser.md
#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "resp/limits.hpp"
#include "resp/parser.hpp"

namespace vkp::resp {

struct value {
  enum class kind : std::uint8_t {
    simple_string,
    error,
    integer,
    double_number,
    boolean,
    big_number,
    bulk_string,
    verbatim_string,
    null,
    array,
    map,
    set,
    push,
  };

  kind type = kind::null;
  std::string_view text;  // simple_string/error/big_number/bulk/verbatim payload
  std::int64_t integer = 0;
  double number = 0.0;
  bool boolean = false;
  std::vector<value> elements;    // array/set/push; map: flat key,value pairs
  std::vector<value> attributes;  // flat key,value pairs from a preceding '|'

  [[nodiscard]] bool is_aggregate() const noexcept {
    return type == kind::array || type == kind::map || type == kind::set || type == kind::push;
  }
};

// Minimal expected-like result. Not std::expected: Ubuntu 24.04's clang-18
// defines __cpp_concepts as 201907L, so libstdc++ guards <expected> out, and
// the project's stated compiler baseline is Clang 18 (see
// docs/architecture-decisions.md §3.3). One tiny type beats a toolchain bump.
class tree_result {
 public:
  // NOLINTNEXTLINE(google-explicit-constructor): mirrors std::expected
  tree_result(value v) noexcept : value_(std::move(v)) {}
  // NOLINTNEXTLINE(google-explicit-constructor): mirrors std::expected
  tree_result(parse_errc e) noexcept : error_(e) {}

  explicit operator bool() const noexcept { return error_ == parse_errc::none; }
  [[nodiscard]] value& operator*() noexcept { return value_; }
  [[nodiscard]] const value& operator*() const noexcept { return value_; }
  [[nodiscard]] value* operator->() noexcept { return &value_; }
  [[nodiscard]] const value* operator->() const noexcept { return &value_; }
  [[nodiscard]] parse_errc error() const noexcept { return error_; }

 private:
  value value_{};
  parse_errc error_ = parse_errc::none;
};

// `frame` must be exactly one complete RESP message (use the shallow parser
// to find the boundary first). Views in the result point into `frame` and
// share its lifetime.
[[nodiscard]] tree_result parse_tree(std::string_view frame, const limits& lim = {});

}  // namespace vkp::resp
