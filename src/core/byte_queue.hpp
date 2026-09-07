// Double-buffered outbound byte queue. Design: docs/design/
// m3-workers-pool-pipelining.md §2.4.
//
// A writer coroutine holds a view into the sending half across co_await
// while other coroutines keep appending to the pending half — appends can
// therefore never move bytes under an in-flight send. Exactly one consumer:
// begin_send()/end_send() must not be nested.
#pragma once

#include <cassert>
#include <cstddef>
#include <string_view>
#include <vector>

namespace vkp {

class byte_queue {
 public:
  // Appends to the pending half; always safe, never invalidates the view
  // returned by begin_send().
  void append(std::string_view data) { pending_.insert(pending_.end(), data.begin(), data.end()); }

  // True when there is nothing pending and no send batch is in progress.
  [[nodiscard]] bool empty() const noexcept { return pending_.empty() && !sending_active_; }

  // Total buffered bytes (pending + the batch currently being sent).
  [[nodiscard]] std::size_t size() const noexcept {
    return pending_.size() + (sending_active_ ? sending_.size() : 0);
  }

  // Swaps pending into the (idle) sending half and returns it as one batch.
  // The view is stable until end_send(). Call only when !empty().
  [[nodiscard]] std::string_view begin_send() noexcept {
    assert(!sending_active_);
    sending_.clear();
    sending_.swap(pending_);
    sending_active_ = true;
    return {sending_.data(), sending_.size()};
  }

  // Marks the current batch fully sent (or abandoned on error).
  void end_send() noexcept {
    assert(sending_active_);
    sending_active_ = false;
    sending_.clear();
  }

  // Drops all buffered bytes. Only legal with no send batch in progress
  // (connection teardown after the writer has parked).
  void clear() noexcept {
    assert(!sending_active_);
    pending_.clear();
    sending_.clear();
  }

 private:
  std::vector<char> pending_;
  std::vector<char> sending_;
  bool sending_active_ = false;
};

}  // namespace vkp
