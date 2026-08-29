#include "io/event_loop.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace vkp::io {

event_loop::event_loop() : backend_(make_backend()) {}
event_loop::event_loop(std::unique_ptr<backend> b) : backend_(std::move(b)) {}
event_loop::~event_loop() = default;

event_loop::io_awaiter event_loop::async_accept(int listen_fd, cancel_slot* slot) noexcept {
  operation op;
  op.op = opcode::accept;
  op.fd = listen_fd;
  return io_awaiter{*this, op, slot};
}

event_loop::io_awaiter event_loop::async_recv(int fd, std::span<char> buf,
                                              cancel_slot* slot) noexcept {
  operation op;
  op.op = opcode::recv;
  op.fd = fd;
  op.rbuf = buf.data();
  op.len = buf.size();
  return io_awaiter{*this, op, slot};
}

event_loop::io_awaiter event_loop::async_send(int fd, std::span<const char> buf,
                                              cancel_slot* slot) noexcept {
  operation op;
  op.op = opcode::send;
  op.fd = fd;
  op.wbuf = buf.data();
  op.len = buf.size();
  return io_awaiter{*this, op, slot};
}

event_loop::io_awaiter event_loop::async_connect(int fd, const sockaddr* addr,
                                                 socklen_t addr_len,
                                                 cancel_slot* slot) noexcept {
  operation op;
  op.op = opcode::connect;
  op.fd = fd;
  std::memcpy(&op.addr, addr, addr_len);
  op.addr_len = addr_len;
  return io_awaiter{*this, op, slot};
}

event_loop::io_awaiter event_loop::sleep_for(std::chrono::nanoseconds d,
                                             cancel_slot* slot) noexcept {
  operation op;
  op.op = opcode::timeout;
  op.duration = d;
  return io_awaiter{*this, op, slot};
}

event_loop::io_awaiter event_loop::schedule() noexcept {
  operation op;
  op.op = opcode::nop;
  return io_awaiter{*this, op, nullptr};
}

void event_loop::submit(operation& op, cancel_slot* slot) {
  if (slot != nullptr) {
    if (slot->requested_) {
      op.result = -ECANCELED;
      ready_.push(op);
      return;
    }
    slot->pending_ = &op;
  }
  if (stopping_ && op.op != opcode::nop) {
    op.result = -ECANCELED;
    ready_.push(op);
    return;
  }

  switch (op.op) {
    case opcode::nop:
      op.result = 0;
      ready_.push(op);
      break;
    case opcode::timeout:
      timers_.push_back({std::chrono::steady_clock::now() + op.duration, &op});
      std::push_heap(timers_.begin(), timers_.end());
      break;
    default:
      backend_->submit(op, ready_);
      break;
  }
}

void event_loop::cancel(cancel_slot& slot) noexcept {
  slot.requested_ = true;
  if (slot.pending_ != nullptr) {
    cancel_op(*slot.pending_);
  }
}

void event_loop::cancel_op(operation& op) noexcept {
  if (backend_->cancel(op, ready_)) {
    return;
  }
  const auto it = std::find_if(timers_.begin(), timers_.end(),
                               [&op](const timer_entry& t) { return t.op == &op; });
  if (it != timers_.end()) {
    timers_.erase(it);
    std::make_heap(timers_.begin(), timers_.end());
    op.result = -ECANCELED;
    ready_.push(op);
  }
  // Not found: the op already completed and sits in the ready queue.
}

std::optional<std::chrono::nanoseconds> event_loop::expire_timers() {
  const auto now = std::chrono::steady_clock::now();
  while (!timers_.empty() && timers_.front().deadline <= now) {
    std::pop_heap(timers_.begin(), timers_.end());
    operation* op = timers_.back().op;
    timers_.pop_back();
    op->result = 0;
    ready_.push(*op);
  }
  if (timers_.empty()) {
    return std::nullopt;
  }
  return timers_.front().deadline - now;
}

void event_loop::run() {
  for (;;) {
    while (operation* op = ready_.pop()) {
      op->continuation.resume();
    }
    const auto next_deadline = expire_timers();
    if (!ready_.empty()) {
      continue;  // due timers fired (or a resume queued work): drain first
    }
    if (backend_->pending() == 0 && timers_.empty()) {
      break;  // nothing can ever complete again
    }
    backend_->poll(next_deadline, ready_);
  }
}

void event_loop::stop() noexcept {
  if (stopping_) {
    return;
  }
  stopping_ = true;
  backend_->cancel_all(ready_);
  for (const timer_entry& t : timers_) {
    t.op->result = -ECANCELED;
    ready_.push(*t.op);
  }
  timers_.clear();
}

}  // namespace vkp::io
