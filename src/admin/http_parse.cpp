#include "admin/http_parse.hpp"

#include <algorithm>
#include <cstddef>

namespace vkp::admin {

parsed_request parse_request(std::string_view data) noexcept {
  using state = parsed_request::state;
  if (data.find("\r\n\r\n") == std::string_view::npos) {
    return {};  // headers not finished; the caller enforces the size cap
  }
  // The blank line exists, so a first CRLF does too, at or before it.
  const std::string_view line = data.substr(0, data.find("\r\n"));

  const std::size_t sp1 = line.find(' ');
  if (sp1 == std::string_view::npos) {
    return {.st = state::bad_request};
  }
  const std::size_t sp2 = line.find(' ', sp1 + 1);
  if (sp2 == std::string_view::npos) {
    return {.st = state::bad_request};  // HTTP/0.9 style; not worth supporting
  }
  const std::string_view method = line.substr(0, sp1);
  std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  const std::string_view version = line.substr(sp2 + 1);

  // Shape first, method second: a garbled line is a 400 and a well-formed
  // POST is a 405, and telling the two apart is the whole point of the order.
  if (!version.starts_with("HTTP/1.")) {
    return {.st = state::bad_request};
  }
  if (target.empty() || target.front() != '/') {
    return {.st = state::bad_request};  // absolute-form and CONNECT authority-form
  }
  if (method != "GET") {
    return {.st = state::not_get};
  }
  // min() of two npos is npos, so this also handles "neither present".
  target = target.substr(0, std::min(target.find('?'), target.find('#')));
  return {.st = state::ok, .path = target};
}

}  // namespace vkp::admin
