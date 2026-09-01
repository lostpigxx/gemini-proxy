// Single-threaded proactor event loop: ready queue + timer heap + backend.
// Design: docs/design/io-and-coroutines.md §1, §4.
#pragma once

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "io/backend.hpp"
#include "io/operation.hpp"

namespace vkp::io {

class event_loop;

// Per-operation cancellation (asio per-op semantics). Bind one slot to at
// most one in-flight op. reset() re-arms a fired slot for reuse.
class cancel_slot {
 public:
  [[nodiscard]] bool requested() const noexcept { return requested_; }

  // Clears a previous cancel request. Only legal once the cancelled op has
  // completed (no op may be pending on this slot).
  void reset() noexcept {
    assert(pending_ == nullptr);
    requested_ = false;
  }

 private:
  friend class event_loop;
  bool requested_ = false;
  operation* pending_ = nullptr;
};

class wait_queue;

class event_loop {
 public:
  event_loop();  // best available backend
  explicit event_loop(std::unique_ptr<backend> b);
  ~event_loop();

  event_loop(const event_loop&) = delete;
  event_loop& operator=(const event_loop&) = delete;

  class [[nodiscard]] io_awaiter {
   public:
    io_awaiter(event_loop& loop, const operation& op, cancel_slot* slot) noexcept
        : loop_(loop), op_(op), slot_(slot) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
      op_.continuation = h;
      loop_.submit(op_, slot_);
    }
    [[nodiscard]] std::int32_t await_resume() noexcept {
      if (slot_ != nullptr && slot_->pending_ == &op_) {
        slot_->pending_ = nullptr;
      }
      return op_.result;
    }

   private:
    event_loop& loop_;
    operation op_;
    cancel_slot* slot_;
  };

  // All fds must be nonblocking. Results follow the operation convention
  // (>= 0 payload, < 0 is -errno); see operation.hpp.
  [[nodiscard]] io_awaiter async_accept(int listen_fd, cancel_slot* slot = nullptr) noexcept;
  [[nodiscard]] io_awaiter async_recv(int fd, std::span<char> buf,
                                      cancel_slot* slot = nullptr) noexcept;
  [[nodiscard]] io_awaiter async_send(int fd, std::span<const char> buf,
                                      cancel_slot* slot = nullptr) noexcept;
  [[nodiscard]] io_awaiter async_connect(int fd, const sockaddr* addr, socklen_t addr_len,
                                         cancel_slot* slot = nullptr) noexcept;
  [[nodiscard]] io_awaiter sleep_for(std::chrono::nanoseconds d,
                                     cancel_slot* slot = nullptr) noexcept;

  // Reschedules the awaiting coroutine at the back of the ready queue.
  [[nodiscard]] io_awaiter schedule() noexcept;

  // Requests cancellation of the op bound to `slot` (if any) and makes every
  // future submit through `slot` complete instantly with -ECANCELED (until
  // slot.reset()).
  void cancel(cancel_slot& slot) noexcept;

  // Pushes an op whose result is already set onto the ready queue; its
  // continuation resumes on the next drain. Used by wait_queue::notify.
  void post(operation& op) noexcept { ready_.push(op); }

  // Runs until there is no pending work (ops, timers, ready) — or, after
  // stop(), until all cancelled coroutines have unwound. Coroutines still
  // suspended when the loop is destroyed leak; always let run() finish.
  void run();

  // Cancels everything; new submissions complete with -ECANCELED. run()
  // returns once the coroutines have unwound. Single-threaded: call it from
  // a coroutine on this loop (e.g. the signal watcher).
  void stop() noexcept;

  [[nodiscard]] bool stopping() const noexcept { return stopping_; }
  [[nodiscard]] std::string_view backend_name() const noexcept { return backend_->name(); }

 private:
  friend class io_awaiter;
  friend class wait_queue;

  struct timer_entry {
    std::chrono::steady_clock::time_point deadline;
    operation* op;
    // std::push_heap builds a max-heap; invert to get the earliest deadline
    // at the front.
    [[nodiscard]] bool operator<(const timer_entry& other) const noexcept {
      return deadline > other.deadline;
    }
  };

  void submit(operation& op, cancel_slot* slot);
  void cancel_op(operation& op) noexcept;
  // Moves due timers to ready; returns the next deadline if any remain.
  std::optional<std::chrono::nanoseconds> expire_timers();

  // Cancels every waiter parked in registered wait_queues; returns how many.
  std::size_t cancel_parked_waiters() noexcept;

  std::unique_ptr<backend> backend_;
  ready_queue ready_;
  std::vector<timer_entry> timers_;    // heap via std::push_heap/pop_heap
  wait_queue* wait_queues_ = nullptr;  // intrusive registry (see wait_queue)
  std::size_t parked_waiters_ = 0;
  bool stopping_ = false;
};

}  // namespace vkp::io
