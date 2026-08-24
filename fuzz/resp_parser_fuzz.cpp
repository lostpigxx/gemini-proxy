// libFuzzer target for the RESP parsers.
//
// Input layout: byte 0 seeds the chunk size; the rest is a RESP byte stream.
// For each limits config the harness cross-checks three implementations that
// must agree on message boundaries:
//   1. one-shot shallow parse over the full window (reference)
//   2. incremental shallow parse fed `chunk` bytes at a time via read_buffer
//   3. deep parse of every complete frame (must accept, modulo its stricter
//      double validation, and must never disagree on the boundary)
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/buffer.hpp"
#include "resp/limits.hpp"
#include "resp/parser.hpp"
#include "resp/value.hpp"

namespace {

using vkp::resp::limits;
using vkp::resp::parse_errc;
using vkp::resp::parse_status;
using vkp::resp::parser;

struct stream_result {
  std::vector<std::string> frames;
  parse_status final_status = parse_status::need_more;
  parse_errc error = parse_errc::none;
};

[[noreturn]] void die(const char* what) {
  std::fprintf(stderr, "consistency violation: %s\n", what);
  std::abort();
}

stream_result parse_one_shot(std::string_view stream, const limits& lim) {
  stream_result out;
  std::string_view window = stream;
  parser p(lim);
  while (true) {
    const parse_status st = p.parse(window);
    if (st != parse_status::complete) {
      out.final_status = st;
      out.error = p.error();
      return out;
    }
    out.frames.emplace_back(p.message().raw);
    window.remove_prefix(p.message().raw.size());
  }
}

stream_result parse_chunked(std::string_view stream, const limits& lim, std::size_t chunk) {
  stream_result out;
  vkp::read_buffer buf(16);
  parser p(lim);
  out.final_status = parse_status::need_more;
  for (std::size_t off = 0; off < stream.size(); off += chunk) {
    if (out.final_status == parse_status::protocol_error) {
      break;  // a real connection stops reading here
    }
    const std::string_view piece = stream.substr(off, chunk);
    const auto w = buf.prepare(piece.size());
    std::memcpy(w.data(), piece.data(), piece.size());
    buf.commit(piece.size());
    while (true) {
      const parse_status st = p.parse(buf.readable());
      out.final_status = st;
      if (st != parse_status::complete) {
        out.error = p.error();
        break;
      }
      out.frames.emplace_back(p.message().raw);
      buf.consume(p.message().raw.size());
    }
  }
  return out;
}

void check_stream(std::string_view stream, const limits& lim, std::size_t chunk) {
  const stream_result reference = parse_one_shot(stream, lim);
  const stream_result chunked = parse_chunked(stream, lim, chunk);

  if (reference.frames != chunked.frames) {
    die("one-shot and chunked parses disagree on frames");
  }
  // Final status may legitimately differ only in one direction: the chunked
  // parse stops feeding after an error, and trailing garbage after the last
  // complete frame can turn need_more into protocol_error at different
  // points. Both must agree on error-vs-not for the same consumed input.
  if (reference.final_status == parse_status::protocol_error &&
      chunked.final_status != parse_status::protocol_error) {
    die("one-shot errored where chunked did not");
  }

  for (const std::string& frame : reference.frames) {
    if (frame.size() > lim.max_message_bytes) {
      die("shallow parser emitted an oversized frame");
    }
    const auto tree = vkp::resp::parse_tree(frame, lim);
    if (!tree && tree.error() != parse_errc::bad_double) {
      // The deep parser is stricter only about doubles; any boundary or
      // structural disagreement with the shallow parser is a bug.
      die("deep parse rejected a frame the shallow parser completed");
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 1) {
    return 0;
  }
  const std::size_t chunk = static_cast<std::size_t>(data[0] % 37) + 1;
  const std::string_view stream(reinterpret_cast<const char*>(data + 1), size - 1);

  check_stream(stream, limits{}, chunk);

  limits tiny;
  tiny.max_message_bytes = 1024;
  tiny.max_bulk_bytes = 128;
  tiny.max_line_bytes = 64;
  tiny.max_nesting_depth = 4;
  tiny.max_command_args = 8;
  check_stream(stream, tiny, chunk);

  return 0;
}
