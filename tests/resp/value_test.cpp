#include "resp/value.hpp"

#include <cmath>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "resp/limits.hpp"
#include "resp/parser.hpp"

using vkp::resp::limits;
using vkp::resp::parse_errc;
using vkp::resp::parse_tree;
using vkp::resp::value;

TEST_CASE("deep parse of scalar types", "[resp][value]") {
  SECTION("simple string") {
    const auto v = parse_tree("+OK\r\n");
    REQUIRE(v);
    REQUIRE(v->type == value::kind::simple_string);
    REQUIRE(v->text == "OK");
  }
  SECTION("error") {
    const auto v = parse_tree("-ERR nope\r\n");
    REQUIRE(v);
    REQUIRE(v->type == value::kind::error);
    REQUIRE(v->text == "ERR nope");
  }
  SECTION("integer") {
    const auto v = parse_tree(":-1234\r\n");
    REQUIRE(v);
    REQUIRE(v->type == value::kind::integer);
    REQUIRE(v->integer == -1234);
  }
  SECTION("double") {
    REQUIRE(parse_tree(",3.5\r\n")->number == 3.5);
    REQUIRE(parse_tree(",10\r\n")->number == 10.0);
    REQUIRE(parse_tree(",inf\r\n")->number == std::numeric_limits<double>::infinity());
    REQUIRE(parse_tree(",-inf\r\n")->number == -std::numeric_limits<double>::infinity());
    REQUIRE(std::isnan(parse_tree(",nan\r\n")->number));
  }
  SECTION("boolean") {
    REQUIRE(parse_tree("#t\r\n")->boolean);
    REQUIRE_FALSE(parse_tree("#f\r\n")->boolean);
  }
  SECTION("big number") {
    const auto v = parse_tree("(3492890328409238509324850943850943825024385\r\n");
    REQUIRE(v);
    REQUIRE(v->type == value::kind::big_number);
    REQUIRE(v->text == "3492890328409238509324850943850943825024385");
  }
  SECTION("nulls, all three encodings") {
    REQUIRE(parse_tree("_\r\n")->type == value::kind::null);
    REQUIRE(parse_tree("$-1\r\n")->type == value::kind::null);
    REQUIRE(parse_tree("*-1\r\n")->type == value::kind::null);
  }
  SECTION("non-canonical -01 lengths agree with the shallow parser (fuzzer regression)") {
    // The shallow parser judges null-ness by parsed value, so "-01" == -1;
    // the deep parser must not diverge by comparing the raw text.
    REQUIRE(parse_tree("*-01\r\n")->type == value::kind::null);
    REQUIRE(parse_tree("$-01\r\n")->type == value::kind::null);
  }
  SECTION("bulk and verbatim") {
    const auto b = parse_tree("$5\r\nhello\r\n");
    REQUIRE(b->type == value::kind::bulk_string);
    REQUIRE(b->text == "hello");
    const auto vb = parse_tree("=15\r\ntxt:Some string\r\n");
    REQUIRE(vb->type == value::kind::verbatim_string);
    REQUIRE(vb->text == "txt:Some string");
  }
}

TEST_CASE("deep parse of aggregates", "[resp][value]") {
  SECTION("array") {
    const auto v = parse_tree("*3\r\n:1\r\n+two\r\n$5\r\nthree\r\n");
    REQUIRE(v);
    REQUIRE(v->type == value::kind::array);
    REQUIRE(v->elements.size() == 3);
    REQUIRE(v->elements[0].integer == 1);
    REQUIRE(v->elements[1].text == "two");
    REQUIRE(v->elements[2].text == "three");
  }
  SECTION("map is flat key,value pairs") {
    const auto v = parse_tree("%2\r\n+a\r\n:1\r\n+b\r\n:2\r\n");
    REQUIRE(v);
    REQUIRE(v->type == value::kind::map);
    REQUIRE(v->elements.size() == 4);
    REQUIRE(v->elements[2].text == "b");
    REQUIRE(v->elements[3].integer == 2);
  }
  SECTION("set and push") {
    REQUIRE(parse_tree("~2\r\n:1\r\n:2\r\n")->type == value::kind::set);
    REQUIRE(parse_tree(">2\r\n+message\r\n+hi\r\n")->type == value::kind::push);
  }
  SECTION("nested") {
    const auto v = parse_tree("*2\r\n*2\r\n:1\r\n:2\r\n%1\r\n+k\r\n#t\r\n");
    REQUIRE(v);
    REQUIRE(v->elements[0].elements[1].integer == 2);
    REQUIRE(v->elements[1].type == value::kind::map);
    REQUIRE(v->elements[1].elements[1].boolean);
  }
  SECTION("empty aggregates") {
    REQUIRE(parse_tree("*0\r\n")->elements.empty());
    REQUIRE(parse_tree("%0\r\n")->elements.empty());
  }
}

TEST_CASE("deep parse attaches attributes to the annotated value", "[resp][value]") {
  const auto v = parse_tree("|1\r\n+key-popularity\r\n,0.1923\r\n*2\r\n:1\r\n:2\r\n");
  REQUIRE(v);
  REQUIRE(v->type == value::kind::array);
  REQUIRE(v->elements.size() == 2);
  REQUIRE(v->attributes.size() == 2);
  REQUIRE(v->attributes[0].text == "key-popularity");
  REQUIRE(v->attributes[1].number == 0.1923);

  const auto nested = parse_tree("*1\r\n|1\r\n+k\r\n+v\r\n:7\r\n");
  REQUIRE(nested);
  REQUIRE(nested->elements[0].integer == 7);
  REQUIRE(nested->elements[0].attributes[0].text == "k");
}

TEST_CASE("deep parse rejects bad frames", "[resp][value]") {
  REQUIRE(parse_tree("+OK\r\n+extra\r\n").error() == parse_errc::trailing_bytes);
  REQUIRE(parse_tree("*2\r\n:1\r\n").error() == parse_errc::truncated_frame);
  REQUIRE(parse_tree("$5\r\nhel\r\n").error() == parse_errc::truncated_frame);
  REQUIRE(parse_tree("$3\r\nhello\r\n").error() == parse_errc::missing_bulk_crlf);
  REQUIRE(parse_tree(",abc\r\n").error() == parse_errc::bad_double);
  REQUIRE(parse_tree("*?\r\n").error() == parse_errc::streamed_not_supported);
  REQUIRE(parse_tree("@\r\n").error() == parse_errc::unknown_type_byte);
  // A lying element count never allocates unboundedly.
  REQUIRE(parse_tree("*99999999\r\n:1\r\n").error() == parse_errc::truncated_frame);

  limits lim;
  lim.max_nesting_depth = 2;
  REQUIRE(parse_tree("*1\r\n*1\r\n*1\r\n:1\r\n", lim).error() == parse_errc::nesting_too_deep);
}

TEST_CASE("empty aggregates are exempt from the depth limit (fuzzer regression)", "[resp][value]") {
  // The depth limit guards recursion; empty aggregates never descend, and the
  // shallow parser accepts them at any depth. Both parsers must agree.
  limits lim;
  lim.max_nesting_depth = 2;
  REQUIRE(parse_tree("*1\r\n*1\r\n*0\r\n", lim));
  REQUIRE(parse_tree("*1\r\n*1\r\n%0\r\n", lim));
  REQUIRE(parse_tree("*1\r\n*1\r\n|0\r\n:1\r\n", lim));
  // ...but a non-empty aggregate at the same position still trips it.
  REQUIRE(parse_tree("*1\r\n*1\r\n*1\r\n:1\r\n", lim).error() == parse_errc::nesting_too_deep);
  REQUIRE(parse_tree("*1\r\n*1\r\n|1\r\n+k\r\n+v\r\n:1\r\n", lim).error() ==
          parse_errc::nesting_too_deep);
}
