#include "resp/serializer.hpp"

#include <cstdint>
#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>

using namespace vkp::resp;

TEST_CASE("serializer emits exact wire bytes", "[resp][serializer]") {
  std::string out;

  SECTION("simple string and canned replies") {
    append_simple_string(out, "OK");
    REQUIRE(out == "+OK\r\n");
    REQUIRE(out == kOk);
    REQUIRE(kPong == "+PONG\r\n");
  }
  SECTION("error") {
    append_error(out, "ERR unsupported by proxy");
    REQUIRE(out == "-ERR unsupported by proxy\r\n");
  }
  SECTION("integer") {
    append_integer(out, 0);
    append_integer(out, -42);
    append_integer(out, std::numeric_limits<std::int64_t>::min());
    REQUIRE(out == ":0\r\n:-42\r\n:-9223372036854775808\r\n");
  }
  SECTION("bulk string") {
    append_bulk_string(out, "hello");
    REQUIRE(out == "$5\r\nhello\r\n");
    out.clear();
    append_bulk_string(out, "");
    REQUIRE(out == "$0\r\n\r\n");
    out.clear();
    append_bulk_string(out, std::string_view{"a\r\nb", 4});  // binary-safe
    REQUIRE(out == "$4\r\na\r\nb\r\n");
  }
  SECTION("array header") {
    append_array_header(out, 3);
    REQUIRE(out == "*3\r\n");
  }
  SECTION("null per protocol version") {
    append_null(out, protocol::resp2);
    REQUIRE(out == "$-1\r\n");
    out.clear();
    append_null(out, protocol::resp3);
    REQUIRE(out == "_\r\n");
  }
  SECTION("composed reply round-trips through the shallow contract") {
    append_array_header(out, 2);
    append_bulk_string(out, "GET");
    append_bulk_string(out, "key");
    REQUIRE(out == "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n");
  }
}
