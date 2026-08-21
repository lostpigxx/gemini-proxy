// M0 smoke test: proves the test framework runs and key dependencies link.
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <toml++/toml.hpp>
#include <xxhash.h>

#include "core/version.hpp"

TEST_CASE("version string is set", "[core]") {
  REQUIRE_FALSE(vkp::kVersion.empty());
  REQUIRE(vkp::kVersion.find('.') != std::string_view::npos);
}

TEST_CASE("toml++ parses a config snippet", "[deps]") {
  constexpr std::string_view doc = R"(
    [proxy]
    listen = "0.0.0.0:6380"
    workers = 4
  )";
  const auto tbl = toml::parse(doc);
  REQUIRE(tbl["proxy"]["workers"].value<int>() == 4);
  REQUIRE(tbl["proxy"]["listen"].value<std::string_view>() == "0.0.0.0:6380");
}

TEST_CASE("xxh3 is deterministic", "[deps]") {
  constexpr std::string_view key = "user:{1000}:profile";
  const auto h1 = XXH3_64bits(key.data(), key.size());
  const auto h2 = XXH3_64bits(key.data(), key.size());
  REQUIRE(h1 == h2);
  REQUIRE(h1 != 0);
}
