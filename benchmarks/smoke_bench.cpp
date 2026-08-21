// M0 smoke benchmark: proves nanobench runs. Real baselines start in M1.
#include <string_view>

#include <fmt/format.h>
#include <nanobench.h>
#include <xxhash.h>

int main() {
  constexpr std::string_view key = "user:{1000}:profile";

  ankerl::nanobench::Bench()
      .title("M0 smoke")
      .run("xxh3_64 of a redis-style key",
           [&] { ankerl::nanobench::doNotOptimizeAway(XXH3_64bits(key.data(), key.size())); })
      .run("fmt::format small string", [&] {
        ankerl::nanobench::doNotOptimizeAway(fmt::format("slot for {} computed", key));
      });
}
