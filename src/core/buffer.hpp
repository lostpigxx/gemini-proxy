// Connection read buffer. Ownership and stability model:
// docs/design/resp-buffer-and-parser.md
#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace vkp {

// Single contiguous growable buffer with head/tail cursors.
// Contract: consume() is only called at message boundaries, so readable()
// always starts at the first byte of the message currently being parsed.
class read_buffer {
 public:
  static constexpr std::size_t kDefaultInitialCapacity = 8 * 1024;

  explicit read_buffer(std::size_t initial_capacity = kDefaultInitialCapacity);

  read_buffer(const read_buffer&) = delete;
  read_buffer& operator=(const read_buffer&) = delete;
  read_buffer(read_buffer&&) = default;
  read_buffer& operator=(read_buffer&&) = default;

  // Ensures at least `min_free` writable bytes at the tail (compacting or
  // growing as needed) and returns the writable region. Invalidates all views.
  std::span<char> prepare(std::size_t min_free);

  // Marks `n` bytes of the region returned by prepare() as filled.
  void commit(std::size_t n) noexcept;

  // The unconsumed bytes. Views remain valid until the next prepare/consume.
  [[nodiscard]] std::string_view readable() const noexcept {
    return {data_.get() + head_, tail_ - head_};
  }

  // Discards `n` readable bytes from the front. Only legal at message
  // boundaries (see class contract). Invalidates all views.
  void consume(std::size_t n) noexcept;

  [[nodiscard]] std::size_t readable_bytes() const noexcept { return tail_ - head_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  std::unique_ptr<char[]> data_;
  std::size_t capacity_ = 0;
  std::size_t head_ = 0;  // first unconsumed byte
  std::size_t tail_ = 0;  // one past the last filled byte
};

}  // namespace vkp
