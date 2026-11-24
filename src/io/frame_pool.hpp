// Thread-local freelist allocator for coroutine frames.
// Design: docs/design/io-and-coroutines.md §1 (do not bet on HALO).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vkp::io::detail {

// Frames are allocated with a 16-byte header recording their size bucket and
// recycled through per-thread, per-bucket freelists. Lock-free by design:
// under the shared-nothing model a frame is always allocated and freed on its
// owning worker thread.
class frame_pool {
 public:
  static constexpr std::size_t kGranularity = 64;
  static constexpr std::size_t kMaxPooledSize = 2048;  // header included
  static constexpr std::size_t kBucketCount = kMaxPooledSize / kGranularity;
  static constexpr std::size_t kMaxFreePerBucket = 64;
  static constexpr std::size_t kHeaderSize = 16;

  struct stats {
    std::uint64_t fresh = 0;   // served by ::operator new
    std::uint64_t reused = 0;  // served from a freelist
  };

  static void* allocate(std::size_t n);
  static void deallocate(void* p) noexcept;

  // Counters for the calling thread; test-only observability.
  [[nodiscard]] static stats thread_stats() noexcept;
};

// ---------------------------------------------------------------------------
// Implementation. It lives in the header so that the freelist hit — the case
// that runs several times per request — inlines into the coroutine's
// `operator new`. Out of line it was a real call whose prologue spilled six
// callee-saved register pairs and set up a 96-byte stack frame, all of it
// forced by the `::operator new` slow paths that the hot path never takes:
// thirteen instructions of frame management around eight of actual work
// (design m6 §9).
// ---------------------------------------------------------------------------

struct free_node {
  free_node* next;
};

// Trivially destructible on purpose. A `thread_local` whose type has a
// destructor makes the compiler emit a construct-and-register guard — a load
// and a conditional branch — at *every* access to it, twice per call here.
// The freelists are reaped instead by a separate object that is touched only
// on the cold path; see `pool_reaper` in the .cpp.
struct pool_state {
  std::array<free_node*, frame_pool::kBucketCount> heads;
  std::array<std::size_t, frame_pool::kBucketCount> counts;
  frame_pool::stats stats;
};

// One instance per thread, program-wide. Deliberately not in an anonymous
// namespace: each TU would get its own copy, and a frame allocated in one TU
// would be recycled into a different pool than the one it came from.
inline thread_local pool_state g_pool{};

// Cold halves, out of line so the hot path does not inherit their register
// pressure.
void* allocate_fresh(std::size_t bucket);
void* allocate_unpooled(std::size_t total);
void free_block(void* raw) noexcept;

inline void* frame_pool::allocate(std::size_t n) {
  const std::size_t total = n + kHeaderSize;
  if (total > kMaxPooledSize) [[unlikely]] {
    return allocate_unpooled(total);
  }
  const std::size_t bucket = ((total + kGranularity - 1) / kGranularity) - 1;
  free_node* const node = g_pool.heads[bucket];
  if (node == nullptr) [[unlikely]] {
    return allocate_fresh(bucket);
  }
  g_pool.heads[bucket] = node->next;
  --g_pool.counts[bucket];
  ++g_pool.stats.reused;
  auto* raw = reinterpret_cast<char*>(node);
  *reinterpret_cast<std::size_t*>(raw) = bucket + 1;
  return raw + kHeaderSize;
}

inline void frame_pool::deallocate(void* p) noexcept {
  auto* raw = static_cast<char*>(p) - kHeaderSize;
  // Header tag: 0 = unpooled (size exceeded kMaxPooledSize), otherwise
  // bucket index + 1.
  const std::size_t tag = *reinterpret_cast<std::size_t*>(raw);
  if (tag == 0) [[unlikely]] {
    free_block(raw);
    return;
  }
  const std::size_t bucket = tag - 1;
  if (g_pool.counts[bucket] >= kMaxFreePerBucket) [[unlikely]] {
    free_block(raw);
    return;
  }
  auto* node = reinterpret_cast<free_node*>(raw);
  node->next = g_pool.heads[bucket];
  g_pool.heads[bucket] = node;
  ++g_pool.counts[bucket];
}

}  // namespace vkp::io::detail
