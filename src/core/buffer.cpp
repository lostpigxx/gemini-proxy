#include "core/buffer.hpp"

#include <bit>
#include <cassert>
#include <cstring>

namespace vkp {

read_buffer::read_buffer(std::size_t initial_capacity)
    : data_(new char[initial_capacity]), capacity_(initial_capacity) {}

std::span<char> read_buffer::prepare(std::size_t min_free) {
  if (capacity_ - tail_ < min_free) {
    const std::size_t used = tail_ - head_;
    if (capacity_ - used >= min_free) {
      // Compaction alone frees enough space: move unconsumed data to the front.
      std::memmove(data_.get(), data_.get() + head_, used);
    } else {
      const std::size_t needed = used + min_free;
      const std::size_t new_capacity = std::bit_ceil(needed);
      auto new_data = std::make_unique_for_overwrite<char[]>(new_capacity);
      std::memcpy(new_data.get(), data_.get() + head_, used);
      data_ = std::move(new_data);
      capacity_ = new_capacity;
    }
    head_ = 0;
    tail_ = used;
  }
  return {data_.get() + tail_, capacity_ - tail_};
}

void read_buffer::commit(std::size_t n) noexcept {
  assert(n <= capacity_ - tail_);
  tail_ += n;
}

void read_buffer::consume(std::size_t n) noexcept {
  assert(n <= tail_ - head_);
  head_ += n;
  if (head_ == tail_) {
    head_ = 0;
    tail_ = 0;
  }
}

}  // namespace vkp
