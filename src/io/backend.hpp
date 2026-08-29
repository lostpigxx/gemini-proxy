// Completion-model (proactor) backend interface: io_uring | epoll | kqueue.
// Design: docs/design/io-and-coroutines.md §1, §3.
#pragma once

#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "io/operation.hpp"

namespace vkp::io {

// Intrusive FIFO of completed operations awaiting resumption.
class ready_queue {
 public:
  void push(operation& op) noexcept {
    op.next = nullptr;
    if (tail_ != nullptr) {
      tail_->next = &op;
    } else {
      head_ = &op;
    }
    tail_ = &op;
  }

  [[nodiscard]] operation* pop() noexcept {
    operation* op = head_;
    if (op != nullptr) {
      head_ = op->next;
      if (head_ == nullptr) {
        tail_ = nullptr;
      }
      op->next = nullptr;
    }
    return op;
  }

  [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

 private:
  operation* head_ = nullptr;
  operation* tail_ = nullptr;
};

// Single-threaded. Completions are only ever delivered by pushing onto a
// ready_queue (never by resuming a coroutine inline); the event loop resumes
// them between poll rounds. Contract: at most one in-flight operation per
// (fd, direction) — one reader, one writer.
class backend {
 public:
  virtual ~backend() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  // Takes ownership of the op until completion. May complete it eagerly
  // (nonblocking syscall succeeded right away) by pushing to `ready`.
  virtual void submit(operation& op, ready_queue& ready) = 0;

  // Requests withdrawal of a pending op. The op then completes through the
  // ready queue / a later poll (normally with -ECANCELED; a genuine result
  // wins the race on io_uring). Returns false if the op is not pending here.
  virtual bool cancel(operation& op, ready_queue& ready) = 0;

  // Requests cancellation of every pending op (shutdown path).
  virtual void cancel_all(ready_queue& ready) = 0;

  // Waits up to `timeout` (nullopt = indefinitely) for completions and
  // pushes them to `ready`.
  virtual void poll(std::optional<std::chrono::nanoseconds> timeout, ready_queue& ready) = 0;

  // Ops currently owned by the backend.
  [[nodiscard]] virtual std::size_t pending() const noexcept = 0;
};

enum class backend_kind : std::uint8_t { io_uring, epoll, kqueue };

[[nodiscard]] std::string_view to_string(backend_kind k) noexcept;

// Best available backend for this platform: io_uring if the runtime probe
// succeeds (seccomp / io_uring_disabled / old kernels fail it), else epoll;
// kqueue on macOS. Throws std::system_error when nothing works.
[[nodiscard]] std::unique_ptr<backend> make_backend();

// Forces a specific backend; throws std::system_error if unsupported here.
[[nodiscard]] std::unique_ptr<backend> make_backend(backend_kind kind);

// Kinds usable on this machine right now (probe included); test helper.
[[nodiscard]] std::vector<backend_kind> available_backends();

}  // namespace vkp::io
