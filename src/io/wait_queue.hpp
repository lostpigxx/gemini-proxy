// Single-threaded async condition variable. Design: docs/design/
// m3-workers-pool-pipelining.md §4.
//
// wait() parks the caller on an intrusive FIFO owned by this queue;
// notify_one/notify_all move parked waiters onto the event loop's ready
// queue. Every wait_queue registers with its loop so that (a) stop() and
// (b) the loop's "only parked waiters remain, nobody can ever notify them"
// idle rule can fail all waiters with -ECANCELED instead of leaking or
// deadlocking. Contract for callers: a negative wait() result means shut
// down — unwind, do not re-wait.
#pragma once

#include <coroutine>
#include <cstdint>

#include "io/event_loop.hpp"
#include "io/operation.hpp"

namespace vkp::io {

class wait_queue {
 public:
  explicit wait_queue(event_loop& loop) noexcept;
  ~wait_queue();

  wait_queue(const wait_queue&) = delete;
  wait_queue& operator=(const wait_queue&) = delete;

  class [[nodiscard]] wait_awaiter {
   public:
    explicit wait_awaiter(wait_queue& q) noexcept : queue_(q) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept {
      op_.op = opcode::nop;
      op_.continuation = h;
      queue_.park(op_);
    }
    [[nodiscard]] std::int32_t await_resume() const noexcept { return op_.result; }

   private:
    wait_queue& queue_;
    operation op_;
  };

  // Suspends until notify/cancel. Resumes with the notifier's result value
  // (0 by default) or -ECANCELED (loop stopping / queue destroyed / no
  // possible notifier left).
  [[nodiscard]] wait_awaiter wait() noexcept { return wait_awaiter{*this}; }

  // Wakes the oldest waiter (FIFO) / all waiters with `result`. Waiters
  // resume via the ready queue, never inline.
  void notify_one(std::int32_t result = 0) noexcept;
  void notify_all(std::int32_t result = 0) noexcept;

  [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

 private:
  friend class event_loop;

  void park(operation& op) noexcept;
  // Pops the oldest waiter or nullptr.
  operation* pop() noexcept;

  event_loop& loop_;
  operation* head_ = nullptr;  // intrusive FIFO via operation::next
  operation* tail_ = nullptr;

  // Intrusive registry links (owned by the loop).
  wait_queue* prev_ = nullptr;
  wait_queue* next_ = nullptr;
};

}  // namespace vkp::io
