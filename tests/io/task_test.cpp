#include "io/task.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>
#include <string>

#include "io/frame_pool.hpp"

namespace {

using vkp::io::spawn;
using vkp::io::sync_wait;
using vkp::io::task;

task<int> forty_two() { co_return 42; }

task<std::string> concat(std::string a, std::string b) { co_return a + b; }

task<int> add_nested(int a, int b) {
  int x = co_await forty_two();
  co_return a + b + x - 42;
}

task<void> set_flag(bool& flag) {
  flag = true;
  co_return;
}

task<int> throws_immediately() {
  throw std::runtime_error("boom");
  co_return 0;  // unreachable
}

task<int> rethrow_chain() {
  // The exception must cross one co_await boundary.
  co_return co_await throws_immediately();
}

task<int> chain(int n) {
  if (n == 0) {
    co_return 0;
  }
  co_return 1 + co_await chain(n - 1);
}

}  // namespace

TEST_CASE("task returns a value through sync_wait", "[io][task]") {
  CHECK(sync_wait(forty_two()) == 42);
  CHECK(sync_wait(concat("foo", "bar")) == "foobar");
}

TEST_CASE("task<void> completes", "[io][task]") {
  bool flag = false;
  sync_wait(set_flag(flag));
  CHECK(flag);
}

TEST_CASE("nested co_await propagates values", "[io][task]") {
  CHECK(sync_wait(add_nested(1, 2)) == 3);
}

TEST_CASE("task is lazy until awaited", "[io][task]") {
  bool flag = false;
  {
    auto t = set_flag(flag);
    CHECK(t.valid());
    CHECK_FALSE(flag);  // body has not started
  }                     // destroying the unawaited task must not leak (ASan-checked)
  CHECK_FALSE(flag);
}

TEST_CASE("exceptions propagate across co_await and sync_wait", "[io][task]") {
  CHECK_THROWS_AS(sync_wait(throws_immediately()), std::runtime_error);
  CHECK_THROWS_AS(sync_wait(rethrow_chain()), std::runtime_error);
}

TEST_CASE("move semantics transfer ownership", "[io][task]") {
  auto a = forty_two();
  task<int> b = std::move(a);
  CHECK_FALSE(a.valid());  // NOLINT(bugprone-use-after-move): intentional check
  CHECK(b.valid());
  CHECK(sync_wait(std::move(b)) == 42);
}

TEST_CASE("deep recursive chain does not overflow the stack", "[io][task]") {
  // 100k frames alive at once; unwinding relies on symmetric transfer being
  // a genuine tail call at every optimization level.
  CHECK(sync_wait(chain(100'000)) == 100'000);
}

TEST_CASE("spawn runs a detached task to completion", "[io][task]") {
  bool flag = false;
  spawn(set_flag(flag));
  CHECK(flag);  // synchronous chain: completed before spawn returned
}

TEST_CASE("frame pool recycles coroutine frames", "[io][task][frame_pool]") {
  using vkp::io::detail::frame_pool;

  // Warm up: the first iterations populate the freelist.
  for (int i = 0; i < 8; ++i) {
    (void)sync_wait(forty_two());
  }
  const auto before = frame_pool::thread_stats();
  for (int i = 0; i < 64; ++i) {
    (void)sync_wait(forty_two());
  }
  const auto after = frame_pool::thread_stats();

  // Identical coroutines allocated sequentially must be served from the pool.
  CHECK(after.reused - before.reused >= 64);
  CHECK(after.fresh == before.fresh);
}

TEST_CASE("frame pool falls back to operator new for oversized frames",
          "[io][task][frame_pool]") {
  using vkp::io::detail::frame_pool;

  // A frame holding a 4 KiB array exceeds kMaxPooledSize and must take the
  // unpooled path (tag 0) — exercised here mainly so ASan/UBSan see it.
  auto big = []() -> task<int> {
    char scratch[4096] = {};
    scratch[0] = 1;
    scratch[4095] = 2;
    co_return scratch[0] + scratch[4095];
  };
  CHECK(sync_wait(big()) == 3);
}
