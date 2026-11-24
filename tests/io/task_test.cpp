#include "io/task.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "io/frame_pool.hpp"

namespace {

using vkp::io::spawn;
using vkp::io::sync_wait;
using vkp::io::task;

task<int> forty_two() {
  co_return 42;
}

task<std::string> concat(std::string a, std::string b) {
  co_return a + b;
}

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
  }  // destroying the unawaited task must not leak (ASan-checked)
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
#if defined(__GNUC__) && !defined(__clang__) && !defined(__OPTIMIZE__)
  // GCC only emits the symmetric-transfer tail call with
  // -foptimize-sibling-calls (-O2+, PR 100897); an unoptimized deep chain
  // genuinely overflows. Clang guarantees the tail call at every level
  // (one reason production builds are Clang).
  constexpr int kDepth = 1'000;
#else
  constexpr int kDepth = 100'000;
#endif
  CHECK(sync_wait(chain(kDepth)) == kDepth);
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

TEST_CASE("frame pool falls back to operator new for oversized frames", "[io][task][frame_pool]") {
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

TEST_CASE("frame pool caps how much each bucket retains", "[io][task][frame_pool]") {
  using vkp::io::detail::frame_pool;

  // Driven through the raw API: the cap only trips when more frames of one
  // size are live simultaneously than the bucket will keep, which coroutines
  // in a test would make needlessly indirect.
  constexpr std::size_t kSize = 300;  // one bucket, well under kMaxPooledSize
  constexpr std::size_t kCount = frame_pool::kMaxFreePerBucket * 2;

  std::vector<void*> blocks;
  blocks.reserve(kCount);
  for (std::size_t i = 0; i < kCount; ++i) {
    void* p = frame_pool::allocate(kSize);
    std::memset(p, 0xab, kSize);  // the payload must be usable past the header
    blocks.push_back(p);
  }
  for (void* p : blocks) {  // all live at once, so none was served from a list
    frame_pool::deallocate(p);
  }

  // Exactly kMaxFreePerBucket survived the release; the rest went back to
  // ::operator delete and have to be allocated fresh again.
  const auto before = frame_pool::thread_stats();
  blocks.clear();
  for (std::size_t i = 0; i < kCount; ++i) {
    blocks.push_back(frame_pool::allocate(kSize));
  }
  const auto after = frame_pool::thread_stats();
  CHECK(after.reused - before.reused == frame_pool::kMaxFreePerBucket);
  CHECK(after.fresh - before.fresh == kCount - frame_pool::kMaxFreePerBucket);

  for (void* p : blocks) {
    frame_pool::deallocate(p);
  }
}

TEST_CASE("frame pool is per-thread and reaped at thread exit", "[io][task][frame_pool]") {
  using vkp::io::detail::frame_pool;

  // The pool backs a shared-nothing runtime, so a frame must never migrate
  // between threads' freelists. This thread's pool is already warm by now; a
  // fresh thread must not see any of it.
  frame_pool::stats on_entry{};
  std::uint64_t reused_after_release = 0;
  std::thread t([&] {
    on_entry = frame_pool::thread_stats();

    frame_pool::deallocate(frame_pool::allocate(128));
    const auto before = frame_pool::thread_stats();
    void* again = frame_pool::allocate(128);
    reused_after_release = frame_pool::thread_stats().reused - before.reused;
    frame_pool::deallocate(again);

    // Deliberately left sitting on this thread's freelists at exit: the
    // reaper has to free them, which LeakSanitizer checks for us.
    frame_pool::deallocate(frame_pool::allocate(256));
    frame_pool::deallocate(frame_pool::allocate(1024));
  });
  t.join();

  CHECK(on_entry.fresh == 0);
  CHECK(on_entry.reused == 0);
  CHECK(reused_after_release == 1);  // its own release, not this thread's
}
