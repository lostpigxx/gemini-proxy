#include "io/wait_queue.hpp"

#include <cerrno>

namespace vkp::io {

wait_queue::wait_queue(event_loop& loop) noexcept : loop_(loop) {
  next_ = loop_.wait_queues_;
  if (next_ != nullptr) {
    next_->prev_ = this;
  }
  loop_.wait_queues_ = this;
}

wait_queue::~wait_queue() {
  notify_all(-ECANCELED);
  if (prev_ != nullptr) {
    prev_->next_ = next_;
  } else {
    loop_.wait_queues_ = next_;
  }
  if (next_ != nullptr) {
    next_->prev_ = prev_;
  }
}

void wait_queue::park(operation& op) noexcept {
  if (loop_.stopping_) {
    op.result = -ECANCELED;
    loop_.post(op);
    return;
  }
  op.next = nullptr;
  if (tail_ != nullptr) {
    tail_->next = &op;
  } else {
    head_ = &op;
  }
  tail_ = &op;
  ++loop_.parked_waiters_;
}

operation* wait_queue::pop() noexcept {
  operation* op = head_;
  if (op != nullptr) {
    head_ = op->next;
    if (head_ == nullptr) {
      tail_ = nullptr;
    }
    op->next = nullptr;
    --loop_.parked_waiters_;
  }
  return op;
}

void wait_queue::notify_one(std::int32_t result) noexcept {
  if (operation* op = pop()) {
    op->result = result;
    loop_.post(*op);
  }
}

void wait_queue::notify_all(std::int32_t result) noexcept {
  while (operation* op = pop()) {
    op->result = result;
    loop_.post(*op);
  }
}

std::size_t event_loop::cancel_parked_waiters() noexcept {
  std::size_t n = 0;
  for (wait_queue* q = wait_queues_; q != nullptr; q = q->next_) {
    while (operation* op = q->pop()) {
      op->result = -ECANCELED;
      ready_.push(*op);
      ++n;
    }
  }
  return n;
}

}  // namespace vkp::io
