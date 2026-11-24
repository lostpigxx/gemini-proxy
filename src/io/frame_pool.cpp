#include "io/frame_pool.hpp"

#include <new>

namespace vkp::io::detail {

namespace {

// Thread-exit cleanup, split out of `pool_state` on purpose. Giving
// `pool_state` itself a destructor costs a `__cxa_thread_atexit` guard — a
// load and a branch — on *every* access to the TLS object, twice per
// `allocate`. This object carries the destructor instead and is touched only
// from the cold path, so the hot path reaches the freelists with a bare
// `tpidr_el0 + offset`.
//
// `armed` is not decoration: a bare `(void)&g_reaper;` may be elided, and the
// thread_local would then never be registered for destruction. A store the
// destructor observably reads cannot be.
struct pool_reaper {
  bool armed = false;

  ~pool_reaper() {
    if (!armed) {
      return;
    }
    for (free_node* node : g_pool.heads) {
      while (node != nullptr) {
        free_node* next = node->next;
        ::operator delete(node);
        node = next;
      }
    }
  }
};

thread_local pool_reaper g_reaper;

}  // namespace

void* allocate_fresh(std::size_t bucket) {
  g_reaper.armed = true;
  void* raw = ::operator new((bucket + 1) * frame_pool::kGranularity);
  ++g_pool.stats.fresh;
  *static_cast<std::size_t*>(raw) = bucket + 1;
  return static_cast<char*>(raw) + frame_pool::kHeaderSize;
}

void* allocate_unpooled(std::size_t total) {
  void* raw = ::operator new(total);
  *static_cast<std::size_t*>(raw) = 0;
  return static_cast<char*>(raw) + frame_pool::kHeaderSize;
}

void free_block(void* raw) noexcept {
  ::operator delete(raw);
}

frame_pool::stats frame_pool::thread_stats() noexcept {
  return g_pool.stats;
}

}  // namespace vkp::io::detail
