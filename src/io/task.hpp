// Lazy stackless coroutine task with symmetric-transfer continuation.
// Design: docs/design/io-and-coroutines.md §1; rationale in
// docs/architecture-decisions.md 决策二.
#pragma once

#include <cassert>
#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "io/frame_pool.hpp"

namespace vkp::io {

template <typename T = void>
class task;

namespace detail {

class task_promise_base {
 public:
  // Frames go through the thread-local pool instead of raw operator new.
  static void* operator new(std::size_t n) { return frame_pool::allocate(n); }
  static void operator delete(void* p) noexcept { frame_pool::deallocate(p); }
  static void operator delete(void* p, std::size_t) noexcept { frame_pool::deallocate(p); }

  // Lazy: the body only runs once the task is co_awaited.
  [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }

  // On completion, resume whoever awaited us via symmetric transfer; a task
  // that was never awaited from a coroutine ends at the noop handle.
  struct final_awaiter {
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    template <typename Promise>
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
      auto& base = static_cast<task_promise_base&>(h.promise());
      return base.continuation_ ? base.continuation_ : std::noop_coroutine();
    }
    void await_resume() noexcept {}
  };
  [[nodiscard]] final_awaiter final_suspend() noexcept { return {}; }

  void set_continuation(std::coroutine_handle<> c) noexcept { continuation_ = c; }

 private:
  std::coroutine_handle<> continuation_;
};

template <typename T>
class task_promise final : public task_promise_base {
 public:
  task<T> get_return_object() noexcept;

  template <typename U = T>
    requires std::convertible_to<U&&, T>
  void return_value(U&& value) {
    result_.template emplace<1>(std::forward<U>(value));
  }
  void unhandled_exception() noexcept { result_.template emplace<2>(std::current_exception()); }

  T result() {
    if (result_.index() == 2) {
      std::rethrow_exception(std::get<2>(std::move(result_)));
    }
    assert(result_.index() == 1);
    return std::get<1>(std::move(result_));
  }

 private:
  std::variant<std::monostate, T, std::exception_ptr> result_;
};

template <>
class task_promise<void> final : public task_promise_base {
 public:
  task<void> get_return_object() noexcept;

  void return_void() noexcept {}
  void unhandled_exception() noexcept { error_ = std::current_exception(); }

  void result() {
    if (error_) {
      std::rethrow_exception(std::move(error_));
    }
  }

 private:
  std::exception_ptr error_;
};

}  // namespace detail

// Single-owner, single-await, lazy task. Destroying an unawaited task is
// legal and releases the (never started) frame.
template <typename T>
class [[nodiscard]] task {
 public:
  static_assert(!std::is_reference_v<T>, "task<T&> is not supported");
  using promise_type = detail::task_promise<T>;

  task() noexcept = default;
  explicit task(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}
  task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  task& operator=(task&& other) noexcept {
    if (this != &other) {
      destroy();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }
  task(const task&) = delete;
  task& operator=(const task&) = delete;
  ~task() { destroy(); }

  [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

  // rvalue-only await enforces single consumption. The awaited task object
  // must outlive completion (a temporary in the co_await full-expression
  // lives in the awaiting frame, which satisfies this).
  [[nodiscard]] auto operator co_await() && noexcept {
    struct awaiter {
      std::coroutine_handle<promise_type> handle;

      [[nodiscard]] bool await_ready() const noexcept { return false; }
      [[nodiscard]] std::coroutine_handle<> await_suspend(
          std::coroutine_handle<> continuation) noexcept {
        handle.promise().set_continuation(continuation);
        return handle;  // start the child; it resumes us from final_suspend
      }
      T await_resume() { return handle.promise().result(); }
    };
    assert(handle_ != nullptr);
    return awaiter{handle_};
  }

 private:
  void destroy() noexcept {
    if (handle_) {
      handle_.destroy();
      handle_ = {};
    }
  }

  std::coroutine_handle<promise_type> handle_;
};

namespace detail {

template <typename T>
task<T> task_promise<T>::get_return_object() noexcept {
  return task<T>{std::coroutine_handle<task_promise>::from_promise(*this)};
}

inline task<void> task_promise<void>::get_return_object() noexcept {
  return task<void>{std::coroutine_handle<task_promise>::from_promise(*this)};
}

// Self-destroying root coroutine: runs eagerly, frame gone at completion.
struct root_task {
  struct promise_type {
    static void* operator new(std::size_t n) { return frame_pool::allocate(n); }
    static void operator delete(void* p) noexcept { frame_pool::deallocate(p); }
    static void operator delete(void* p, std::size_t) noexcept { frame_pool::deallocate(p); }

    root_task get_return_object() noexcept { return {}; }
    [[nodiscard]] std::suspend_never initial_suspend() noexcept { return {}; }
    [[nodiscard]] std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    // Detached coroutines have nowhere to deliver an exception.
    void unhandled_exception() noexcept { std::terminate(); }
  };
};

template <typename T>
root_task sync_wait_run(task<T> t, std::optional<T>& out, std::exception_ptr& error, bool& done) {
  try {
    out.emplace(co_await std::move(t));
  } catch (...) {
    error = std::current_exception();
  }
  done = true;
}

inline root_task sync_wait_run(task<void> t, std::exception_ptr& error, bool& done) {
  try {
    co_await std::move(t);
  } catch (...) {
    error = std::current_exception();
  }
  done = true;
}

}  // namespace detail

// Starts a task and abandons the handle; the frame frees itself on
// completion. Exceptions escaping `t` terminate (catch inside the task).
inline void spawn(task<void> t) {
  [](task<void> inner) -> detail::root_task { co_await std::move(inner); }(std::move(t));
}

// Test helper: runs a task whose await chain completes synchronously (no IO,
// no scheduling) and returns its result. Aborts if the chain actually
// suspends — use event_loop for anything asynchronous.
template <typename T>
T sync_wait(task<T> t) {
  std::exception_ptr error;
  bool done = false;
  if constexpr (std::is_void_v<T>) {
    detail::sync_wait_run(std::move(t), error, done);
    if (!done) {
      std::terminate();
    }
    if (error) {
      std::rethrow_exception(error);
    }
  } else {
    std::optional<T> out;
    detail::sync_wait_run(std::move(t), out, error, done);
    if (!done) {
      std::terminate();
    }
    if (error) {
      std::rethrow_exception(error);
    }
    return std::move(*out);
  }
}

}  // namespace vkp::io
