#include "io/frame_pool.hpp"

#include <array>
#include <new>

namespace vkp::io::detail {

namespace {

struct free_node {
  free_node* next;
};

// Header tag: 0 = unpooled (size exceeded kMaxPooledSize), otherwise
// bucket index + 1.
struct pool_state {
  std::array<free_node*, frame_pool::kBucketCount> heads{};
  std::array<std::size_t, frame_pool::kBucketCount> counts{};
  frame_pool::stats stats{};

  ~pool_state() {
    for (free_node* node : heads) {
      while (node != nullptr) {
        free_node* next = node->next;
        ::operator delete(node);
        node = next;
      }
    }
  }
};

thread_local pool_state g_pool;

std::size_t* header_of(void* frame) noexcept {
  return reinterpret_cast<std::size_t*>(static_cast<char*>(frame) - frame_pool::kHeaderSize);
}

}  // namespace

void* frame_pool::allocate(std::size_t n) {
  const std::size_t total = n + kHeaderSize;
  if (total > kMaxPooledSize) {
    void* raw = ::operator new(total);
    *static_cast<std::size_t*>(raw) = 0;
    return static_cast<char*>(raw) + kHeaderSize;
  }

  const std::size_t bucket = (total + kGranularity - 1) / kGranularity - 1;
  if (free_node* node = g_pool.heads[bucket]; node != nullptr) {
    g_pool.heads[bucket] = node->next;
    --g_pool.counts[bucket];
    ++g_pool.stats.reused;
    void* raw = node;
    *static_cast<std::size_t*>(raw) = bucket + 1;
    return static_cast<char*>(raw) + kHeaderSize;
  }

  void* raw = ::operator new((bucket + 1) * kGranularity);
  ++g_pool.stats.fresh;
  *static_cast<std::size_t*>(raw) = bucket + 1;
  return static_cast<char*>(raw) + kHeaderSize;
}

void frame_pool::deallocate(void* p) noexcept {
  void* raw = static_cast<char*>(p) - kHeaderSize;
  const std::size_t tag = *header_of(p);
  if (tag == 0) {
    ::operator delete(raw);
    return;
  }

  const std::size_t bucket = tag - 1;
  if (g_pool.counts[bucket] >= kMaxFreePerBucket) {
    ::operator delete(raw);
    return;
  }
  auto* node = static_cast<free_node*>(raw);
  node->next = g_pool.heads[bucket];
  g_pool.heads[bucket] = node;
  ++g_pool.counts[bucket];
}

frame_pool::stats frame_pool::thread_stats() noexcept {
  return g_pool.stats;
}

}  // namespace vkp::io::detail
