#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/buffer.hpp"
#include "resp/limits.hpp"
#include "resp/parser.hpp"

using vkp::resp::limits;
using vkp::resp::parse_errc;
using vkp::resp::parse_status;
using vkp::resp::parser;

namespace {

// Scans every truncation point: a prefix cut at any byte must yield
// need_more, and resuming with the full frame must complete with the exact
// boundary. Also feeds byte-by-byte through a single parser instance to
// exercise incremental resume at every position.
void check_boundary_at_every_cut(std::string_view frame) {
  for (std::size_t cut = 0; cut < frame.size(); ++cut) {
    CAPTURE(frame, cut);
    parser p;
    REQUIRE(p.parse(frame.substr(0, cut)) == parse_status::need_more);
    REQUIRE(p.parse(frame) == parse_status::complete);
    REQUIRE(p.message().raw == frame);
  }
  parser p;
  for (std::size_t n = 0; n < frame.size(); ++n) {
    CAPTURE(frame, n);
    REQUIRE(p.parse(frame.substr(0, n)) == parse_status::need_more);
  }
  REQUIRE(p.parse(frame) == parse_status::complete);
  REQUIRE(p.message().raw == frame);
}

parse_errc error_of(std::string_view input, const limits& lim = {}) {
  parser p(lim);
  REQUIRE(p.parse(input) == parse_status::protocol_error);
  return p.error();
}

}  // namespace

TEST_CASE("RESP2 types parse at every truncation point", "[resp][parser]") {
  check_boundary_at_every_cut("+OK\r\n");
  check_boundary_at_every_cut("-ERR unknown command 'foobar'\r\n");
  check_boundary_at_every_cut(":1000\r\n");
  check_boundary_at_every_cut(":-42\r\n");
  check_boundary_at_every_cut("$5\r\nhello\r\n");
  check_boundary_at_every_cut("$0\r\n\r\n");
  check_boundary_at_every_cut("$-1\r\n");
  check_boundary_at_every_cut("*-1\r\n");
  check_boundary_at_every_cut("*0\r\n");
  check_boundary_at_every_cut("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n");
  check_boundary_at_every_cut("*2\r\n*2\r\n:1\r\n:2\r\n*1\r\n+nested\r\n");
}

TEST_CASE("RESP3 types parse at every truncation point", "[resp][parser]") {
  check_boundary_at_every_cut("_\r\n");
  check_boundary_at_every_cut("#t\r\n");
  check_boundary_at_every_cut("#f\r\n");
  check_boundary_at_every_cut(",3.141\r\n");
  check_boundary_at_every_cut(",10\r\n");
  check_boundary_at_every_cut(",inf\r\n");
  check_boundary_at_every_cut(",-inf\r\n");
  check_boundary_at_every_cut(",nan\r\n");
  check_boundary_at_every_cut("(3492890328409238509324850943850943825024385\r\n");
  check_boundary_at_every_cut("=15\r\ntxt:Some string\r\n");
  check_boundary_at_every_cut("%2\r\n+first\r\n:1\r\n+second\r\n:2\r\n");
  check_boundary_at_every_cut("~3\r\n:1\r\n:2\r\n:3\r\n");
  check_boundary_at_every_cut(">3\r\n+message\r\n+channel\r\n+payload\r\n");
}

TEST_CASE("bulk payload may contain CR LF and type bytes", "[resp][parser]") {
  check_boundary_at_every_cut("$10\r\n+OK\r\n*2\r\n\r\r\n");
}

TEST_CASE("attribute binds to the following value as one message", "[resp][parser]") {
  const std::string_view frame = "|1\r\n+key-popularity\r\n,0.1923\r\n*2\r\n:1\r\n:2\r\n";
  check_boundary_at_every_cut(frame);

  SECTION("attribute alone is not a complete message") {
    parser p;
    REQUIRE(p.parse("|1\r\n+k\r\n+v\r\n") == parse_status::need_more);
    REQUIRE(p.parse("|1\r\n+k\r\n+v\r\n:7\r\n") == parse_status::complete);
  }
  SECTION("empty attribute still owes the annotated value") {
    parser p;
    REQUIRE(p.parse("|0\r\n") == parse_status::need_more);
    REQUIRE(p.parse("|0\r\n+OK\r\n") == parse_status::complete);
  }
  SECTION("nested attribute inside an aggregate") {
    check_boundary_at_every_cut("*2\r\n|1\r\n+k\r\n+v\r\n:1\r\n:2\r\n");
  }
}

TEST_CASE("command shape is detected with argument views", "[resp][parser]") {
  parser p;

  SECTION("SET command") {
    REQUIRE(p.parse("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n") == parse_status::complete);
    const auto& msg = p.message();
    REQUIRE(msg.is_command);
    REQUIRE(msg.args.size() == 3);
    REQUIRE(msg.args[0] == "SET");
    REQUIRE(msg.args[1] == "key");
    REQUIRE(msg.args[2] == "value");
  }
  SECTION("single-argument command") {
    REQUIRE(p.parse("*1\r\n$4\r\nPING\r\n") == parse_status::complete);
    REQUIRE(p.message().is_command);
    REQUIRE(p.message().args[0] == "PING");
  }
  SECTION("empty-payload argument is still an argument") {
    REQUIRE(p.parse("*2\r\n$3\r\nGET\r\n$0\r\n\r\n") == parse_status::complete);
    REQUIRE(p.message().is_command);
    REQUIRE(p.message().args[1].empty());
  }
}

TEST_CASE("non-command frames are complete but flagged", "[resp][parser]") {
  const auto expect_not_command = [](std::string_view frame) {
    CAPTURE(frame);
    parser p;
    REQUIRE(p.parse(frame) == parse_status::complete);
    REQUIRE_FALSE(p.message().is_command);
    REQUIRE(p.message().args.empty());
  };
  expect_not_command("+OK\r\n");
  expect_not_command("*0\r\n");
  expect_not_command("*-1\r\n");
  expect_not_command("*2\r\n$3\r\nfoo\r\n:1\r\n");     // integer element
  expect_not_command("*1\r\n$-1\r\n");                 // null bulk element
  expect_not_command("*1\r\n=5\r\ntxt:x\r\n");         // verbatim element
  expect_not_command("*1\r\n*1\r\n$4\r\nPING\r\n");    // nested array
  expect_not_command("|1\r\n+k\r\n+v\r\n*1\r\n$1\r\nx\r\n");  // attribute prefix
}

TEST_CASE("pipelined messages are delimited one at a time", "[resp][parser]") {
  const std::string stream = "*1\r\n$4\r\nPING\r\n*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\n+extra\r\n";
  std::string_view window = stream;
  parser p;

  REQUIRE(p.parse(window) == parse_status::complete);
  REQUIRE(p.message().raw == "*1\r\n$4\r\nPING\r\n");
  window.remove_prefix(p.message().raw.size());

  REQUIRE(p.parse(window) == parse_status::complete);
  REQUIRE(p.message().raw == "*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\n");
  REQUIRE(p.message().args[1] == "hi");
  window.remove_prefix(p.message().raw.size());

  REQUIRE(p.parse(window) == parse_status::complete);
  REQUIRE(p.message().raw == "+extra\r\n");
}

TEST_CASE("read_buffer + parser end-to-end with awkward chunking", "[resp][parser]") {
  // Simulates the proxy read loop: recv arbitrary chunks, parse, forward,
  // consume at message boundaries.
  const std::string stream =
      "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
      "+OK\r\n"
      "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";
  for (std::size_t chunk = 1; chunk <= 7; ++chunk) {
    CAPTURE(chunk);
    vkp::read_buffer buf(16);
    parser p;
    std::vector<std::string> messages;
    for (std::size_t off = 0; off < stream.size(); off += chunk) {
      const std::string_view piece = std::string_view(stream).substr(off, chunk);
      const auto w = buf.prepare(piece.size());
      std::memcpy(w.data(), piece.data(), piece.size());
      buf.commit(piece.size());
      while (true) {
        const auto st = p.parse(buf.readable());
        if (st != parse_status::complete) {
          REQUIRE(st == parse_status::need_more);
          break;
        }
        messages.emplace_back(p.message().raw);
        buf.consume(p.message().raw.size());
      }
    }
    REQUIRE(messages.size() == 3);
    REQUIRE(messages[0] == "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
    REQUIRE(messages[1] == "+OK\r\n");
    REQUIRE(messages[2] == "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
  }
}

TEST_CASE("malformed input yields protocol errors", "[resp][parser]") {
  REQUIRE(error_of("@bad\r\n") == parse_errc::unknown_type_byte);
  REQUIRE(error_of("PING\r\n") == parse_errc::unknown_type_byte);  // inline: TODO
  REQUIRE(error_of("\r\n") == parse_errc::unknown_type_byte);
  REQUIRE(error_of("+OK\n") == parse_errc::bare_lf);
  REQUIRE(error_of("\n") == parse_errc::bare_lf);
  REQUIRE(error_of(":\r\n") == parse_errc::bad_integer);
  REQUIRE(error_of(":12a\r\n") == parse_errc::bad_integer);
  REQUIRE(error_of(":+12\r\n") == parse_errc::bad_integer);
  REQUIRE(error_of("$abc\r\n") == parse_errc::bad_integer);
  REQUIRE(error_of("$\r\n") == parse_errc::bad_integer);
  REQUIRE(error_of("*1a\r\n") == parse_errc::bad_integer);
  REQUIRE(error_of("$-2\r\n") == parse_errc::length_out_of_range);
  REQUIRE(error_of("=-1\r\n") == parse_errc::length_out_of_range);
  REQUIRE(error_of("*-2\r\n") == parse_errc::length_out_of_range);
  REQUIRE(error_of("%-1\r\n") == parse_errc::length_out_of_range);
  REQUIRE(error_of("$3\r\nfooXY") == parse_errc::missing_bulk_crlf);
  REQUIRE(error_of("#x\r\n") == parse_errc::invalid_boolean);
  REQUIRE(error_of("#tt\r\n") == parse_errc::invalid_boolean);
  REQUIRE(error_of("_x\r\n") == parse_errc::invalid_null);
  REQUIRE(error_of(",\r\n") == parse_errc::empty_scalar);
  REQUIRE(error_of("(\r\n") == parse_errc::empty_scalar);
  REQUIRE(error_of("$?\r\n") == parse_errc::streamed_not_supported);
  REQUIRE(error_of("*?\r\n") == parse_errc::streamed_not_supported);
  REQUIRE(error_of("%?\r\n") == parse_errc::streamed_not_supported);
}

TEST_CASE("defensive limits trip", "[resp][parser]") {
  SECTION("nesting depth") {
    limits lim;
    lim.max_nesting_depth = 4;
    REQUIRE(error_of("*1\r\n*1\r\n*1\r\n*1\r\n*1\r\n:1\r\n", lim) == parse_errc::nesting_too_deep);
    // Exactly at the limit is fine.
    parser p(lim);
    REQUIRE(p.parse("*1\r\n*1\r\n*1\r\n*1\r\n:1\r\n") == parse_status::complete);
  }
  SECTION("single bulk size, rejected at the header") {
    limits lim;
    lim.max_bulk_bytes = 4;
    REQUIRE(error_of("$5\r\nhello\r\n", lim) == parse_errc::length_out_of_range);
    REQUIRE(error_of("$5\r\n", lim) == parse_errc::length_out_of_range);  // before payload
  }
  SECTION("total message size") {
    limits lim;
    lim.max_message_bytes = 16;
    REQUIRE(error_of("$100\r\n", lim) == parse_errc::message_too_large);
    REQUIRE(error_of("*9\r\n:1\r\n:2\r\n:3\r\n:4\r\n", lim) == parse_errc::message_too_large);
  }
  SECTION("line length, including a line that never terminates") {
    limits lim;
    lim.max_line_bytes = 8;
    REQUIRE(error_of("+123456789\r\n", lim) == parse_errc::line_too_long);
    const std::string unterminated = "+" + std::string(64, 'x');  // no CRLF at all
    REQUIRE(error_of(unterminated, lim) == parse_errc::line_too_long);
  }
  SECTION("argument overflow completes the frame but drops command shape") {
    limits lim;
    lim.max_command_args = 2;
    parser p(lim);
    REQUIRE(p.parse("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n") == parse_status::complete);
    REQUIRE_FALSE(p.message().is_command);
  }
}

TEST_CASE("protocol errors are sticky until reset", "[resp][parser]") {
  parser p;
  REQUIRE(p.parse("@\r\n") == parse_status::protocol_error);
  REQUIRE(p.parse("+OK\r\n") == parse_status::protocol_error);
  p.reset();
  REQUIRE(p.parse("+OK\r\n") == parse_status::complete);
}

TEST_CASE("huge declared aggregate count waits for data, then trips size cap",
          "[resp][parser]") {
  limits lim;
  lim.max_message_bytes = 64;
  parser p(lim);
  std::string input = "*99999999\r\n";
  REQUIRE(p.parse(input) == parse_status::need_more);  // no pre-allocation, just waits
  while (input.size() <= lim.max_message_bytes) {
    input += ":1\r\n";
  }
  REQUIRE(p.parse(input) == parse_status::protocol_error);
  REQUIRE(p.error() == parse_errc::message_too_large);
}
