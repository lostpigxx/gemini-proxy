// Thread-local freelist allocator for coroutine frames.
// Design: docs/design/io-and-coroutines.md §1 (do not bet on HALO).
#pragma once

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

  static void* allocate(std::size_t n);
  static void deallocate(void* p) noexcept;

  struct stats {
    std::uint64_t fresh = 0;   // served by ::operator new
    std::uint64_t reused = 0;  // served from a freelist
  };
  // Counters for the calling thread; test-only observability.
  [[nodiscard]] static stats thread_stats() noexcept;
};

}  // namespace vkp::io::detail
