// epoll reactor backend: readiness → nonblocking syscall → completion.
// First-class citizen: the mandatory fallback wherever io_uring is disabled
// (seccomp, kernel.io_uring_disabled, old kernels). Architecture decision 1.2.

#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <ctime>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "io/backend.hpp"

namespace vkp::io {

namespace {

constexpr int kMaxEvents = 256;

[[nodiscard]] bool is_read_side(const operation& op) noexcept {
  return op.op == opcode::accept || op.op == opcode::recv;
}

// Nonblocking attempt; true = final result in op.result, false = EAGAIN.
[[nodiscard]] bool try_syscall(operation& op, bool after_readiness) noexcept {
  switch (op.op) {
    case opcode::accept: {
      const int fd = ::accept4(op.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd >= 0) {
        op.result = fd;
        return true;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return false;
      }
      op.result = -errno;
      return true;
    }
    case opcode::recv: {
      const ssize_t n = ::recv(op.fd, op.rbuf, op.len, 0);
      if (n >= 0) {
        op.result = static_cast<std::int32_t>(n);
        return true;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return false;
      }
      op.result = -errno;
      return true;
    }
    case opcode::send: {
      const ssize_t n = ::send(op.fd, op.wbuf, op.len, MSG_NOSIGNAL);
      if (n >= 0) {
        op.result = static_cast<std::int32_t>(n);
        return true;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return false;
      }
      op.result = -errno;
      return true;
    }
    case opcode::connect: {
      if (after_readiness) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(op.fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
          err = errno;
        }
        op.result = err == 0 ? 0 : -err;
        return true;
      }
      if (::connect(op.fd, reinterpret_cast<const sockaddr*>(&op.addr), op.addr_len) == 0) {
        op.result = 0;
        return true;
      }
      if (errno == EINPROGRESS) {
        return false;
      }
      op.result = -errno;
      return true;
    }
    case opcode::nop:
    case opcode::timeout:
      break;
  }
  assert(false && "opcode not handled by backends");
  op.result = -EINVAL;
  return true;
}

class epoll_backend final : public backend {
 public:
  epoll_backend() : ep_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (ep_ < 0) {
      throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
  }
  ~epoll_backend() override {
    if (ep_ >= 0) {
      ::close(ep_);
    }
  }
  epoll_backend(const epoll_backend&) = delete;
  epoll_backend& operator=(const epoll_backend&) = delete;

  [[nodiscard]] std::string_view name() const noexcept override { return "epoll"; }

  void submit(operation& op, ready_queue& ready) override {
    if (try_syscall(op, /*after_readiness=*/false)) {
      ready.push(op);
      return;
    }
    fd_state& st = fds_[op.fd];  // node-based map: &st stays valid
    st.fd = op.fd;
    operation*& slot = is_read_side(op) ? st.reader : st.writer;
    assert(slot == nullptr && "one in-flight op per (fd, direction)");
    slot = &op;
    ++pending_count_;
    update_registration(st, ready);
  }

  bool cancel(operation& op, ready_queue& ready) override {
    const auto it = fds_.find(op.fd);
    if (it == fds_.end()) {
      return false;
    }
    fd_state& st = it->second;
    if (st.reader == &op) {
      st.reader = nullptr;
    } else if (st.writer == &op) {
      st.writer = nullptr;
    } else {
      return false;
    }
    --pending_count_;
    op.result = -ECANCELED;
    ready.push(op);
    update_registration(st, ready);  // may erase st
    return true;
  }

  void cancel_all(ready_queue& ready) override {
    std::vector<operation*> ops;
    ops.reserve(pending_count_);
    for (const auto& [fd, st] : fds_) {
      if (st.reader != nullptr) {
        ops.push_back(st.reader);
      }
      if (st.writer != nullptr) {
        ops.push_back(st.writer);
      }
    }
    for (operation* op : ops) {
      (void)cancel(*op, ready);
    }
  }

  void poll(std::optional<std::chrono::nanoseconds> timeout, ready_queue& ready) override {
    struct timespec ts{};
    const struct timespec* tsp = nullptr;
    if (timeout.has_value()) {
      const auto ns = std::max<std::chrono::nanoseconds::rep>(timeout->count(), 0);
      ts.tv_sec = static_cast<time_t>(ns / 1'000'000'000);
      ts.tv_nsec = static_cast<long>(ns % 1'000'000'000);
      tsp = &ts;
    }

    std::array<epoll_event, kMaxEvents> events{};
    const int n = ::epoll_pwait2(ep_, events.data(), kMaxEvents, tsp, nullptr);
    if (n < 0) {
      if (errno == EINTR) {
        return;
      }
      throw std::system_error(errno, std::generic_category(), "epoll_pwait2");
    }

    for (int i = 0; i < n; ++i) {
      // One event per fd per wait, and completions are only queued (never
      // resumed) inside this batch, so `st` cannot dangle mid-iteration.
      auto* st = static_cast<fd_state*>(events[i].data.ptr);
      const std::uint32_t evs = events[i].events;
      if ((evs & (EPOLLIN | EPOLLERR | EPOLLHUP)) != 0 && st->reader != nullptr) {
        complete_slot(st->reader, ready);
      }
      if ((evs & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0 && st->writer != nullptr) {
        complete_slot(st->writer, ready);
      }
      update_registration(*st, ready);  // may erase st; last use of it
    }
  }

  [[nodiscard]] std::size_t pending() const noexcept override { return pending_count_; }

 private:
  struct fd_state {
    int fd = -1;
    operation* reader = nullptr;
    operation* writer = nullptr;
    std::uint32_t armed = 0;  // currently registered epoll mask; 0 = absent
  };

  // Runs the syscall for a slot's op; clears the slot on final result,
  // leaves it armed on spurious readiness (level-triggered will refire).
  void complete_slot(operation*& slot, ready_queue& ready) {
    operation& op = *slot;
    if (try_syscall(op, /*after_readiness=*/true)) {
      slot = nullptr;
      --pending_count_;
      ready.push(op);
    }
  }

  // Syncs the epoll interest mask with the live slots; drops the fd_state
  // entry (invalidating `st`!) once both slots are empty.
  void update_registration(fd_state& st, ready_queue& ready) {
    const std::uint32_t want = (st.reader != nullptr ? EPOLLIN : 0U) |
                               (st.writer != nullptr ? EPOLLOUT : 0U);
    if (want == st.armed) {
      return;
    }
    if (want == 0) {
      (void)::epoll_ctl(ep_, EPOLL_CTL_DEL, st.fd, nullptr);
      fds_.erase(st.fd);
      return;
    }
    epoll_event ev{};
    ev.events = want;  // level-triggered
    ev.data.ptr = &st;
    const int rc =
        ::epoll_ctl(ep_, st.armed == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, st.fd, &ev);
    if (rc != 0) {
      const int err = errno;
      for (operation** slot : {&st.reader, &st.writer}) {
        if (*slot != nullptr) {
          (*slot)->result = -err;
          ready.push(**slot);
          *slot = nullptr;
          --pending_count_;
        }
      }
      if (st.armed != 0) {
        (void)::epoll_ctl(ep_, EPOLL_CTL_DEL, st.fd, nullptr);
      }
      fds_.erase(st.fd);
      return;
    }
    st.armed = want;
  }

  int ep_ = -1;
  std::unordered_map<int, fd_state> fds_;
  std::size_t pending_count_ = 0;
};

}  // namespace

std::unique_ptr<backend> make_epoll_backend() { return std::make_unique<epoll_backend>(); }

}  // namespace vkp::io
