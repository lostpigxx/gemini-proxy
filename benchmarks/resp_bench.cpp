// M1 baseline: shallow-parse throughput for typical proxy traffic shapes.
// Recorded in docs/development-plan.md (M1 实施记录).
#include <cstddef>
#include <string>
#include <string_view>

#include <nanobench.h>

#include "resp/parser.hpp"
#include "resp/serializer.hpp"
#include "resp/value.hpp"

namespace {

std::string make_command(std::initializer_list<std::string_view> args) {
  std::string out;
  vkp::resp::append_array_header(out, args.size());
  for (const auto arg : args) {
    vkp::resp::append_bulk_string(out, arg);
  }
  return out;
}

std::string make_mget(std::size_t keys) {
  std::string out;
  vkp::resp::append_array_header(out, keys + 1);
  vkp::resp::append_bulk_string(out, "MGET");
  for (std::size_t i = 0; i < keys; ++i) {
    vkp::resp::append_bulk_string(out, "user:profile:" + std::to_string(100000 + i));
  }
  return out;
}

void bench_shallow(ankerl::nanobench::Bench& bench, std::string_view name,
                   std::string_view frame) {
  vkp::resp::parser parser;
  const std::string label = std::string(name) + " (" + std::to_string(frame.size()) + " B)";
  bench.batch(frame.size()).unit("byte").run(label, [&] {
    const auto st = parser.parse(frame);
    ankerl::nanobench::doNotOptimizeAway(st == vkp::resp::parse_status::complete);
    ankerl::nanobench::doNotOptimizeAway(parser.message().raw.size());
  });
}

}  // namespace

int main() {
  const std::string get = make_command({"GET", "user:profile:100042"});
  const std::string set_64 = make_command({"SET", "user:profile:100042", std::string(64, 'v')});
  const std::string set_1k = make_command({"SET", "user:profile:100042", std::string(1024, 'v')});
  const std::string mget_10 = make_mget(10);
  const std::string mget_100 = make_mget(100);
  const std::string bulk_reply_1k = [] {
    std::string out;
    vkp::resp::append_bulk_string(out, std::string(1024, 'v'));
    return out;
  }();

  ankerl::nanobench::Bench bench;
  bench.title("RESP shallow parse (bytes/s is the headline number)").minEpochIterations(50000);

  bench_shallow(bench, "GET", get);
  bench_shallow(bench, "SET 64B value", set_64);
  bench_shallow(bench, "SET 1KiB value", set_1k);
  bench_shallow(bench, "MGET 10 keys", mget_10);
  bench_shallow(bench, "MGET 100 keys", mget_100);
  bench_shallow(bench, "bulk reply 1KiB", bulk_reply_1k);

  // Deep parse is control-plane only; tracked to keep an eye on the gap.
  vkp::resp::limits lim;
  bench.minEpochIterations(10000)
      .batch(mget_100.size())
      .run("deep parse MGET 100 keys (control plane)", [&] {
        auto tree = vkp::resp::parse_tree(mget_100, lim);
        ankerl::nanobench::doNotOptimizeAway(tree);
      });
}
