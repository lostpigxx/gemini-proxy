// kqueue reactor backend: readiness → nonblocking syscall → completion.
// macOS development only; no performance ambitions (architecture decision 1.4).

#include <sys/event.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>
#include <unordered_set>

#include "io/backend.hpp"

namespace vkp::io {

namespace {

constexpr int kMaxEvents = 256;

// EVFILT_READ for accept/recv, EVFILT_WRITE for connect/send.
[[nodiscard]] std::int16_t filter_of(const operation& op) noexcept {
  return (op.op == opcode::accept || op.op == opcode::recv) ? EVFILT_READ : EVFILT_WRITE;
}

// Nonblocking attempt at the op's syscall. Returns true when the op reached
// a final result (op.result filled in); false means EAGAIN — wait for
// readiness. `after_readiness` distinguishes the connect completion check.
[[nodiscard]] bool try_syscall(operation& op, bool after_readiness) noexcept {
  switch (op.op) {
    case opcode::accept: {
      const int fd = ::accept(op.fd, nullptr, nullptr);
      if (fd >= 0) {
        // No accept4 on macOS; dev-only path, three extra syscalls are fine.
        (void)::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
        // macOS has no MSG_NOSIGNAL; suppress SIGPIPE at the socket level.
        const int one = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
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
      const ssize_t n = ::send(op.fd, op.wbuf, op.len, 0);
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
        // Writability after EINPROGRESS: fetch the final status.
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

class kqueue_backend final : public backend {
 public:
  kqueue_backend() : kq_(::kqueue()) {
    if (kq_ < 0) {
      throw std::system_error(errno, std::generic_category(), "kqueue");
    }
  }
  ~kqueue_backend() override {
    if (kq_ >= 0) {
      ::close(kq_);
    }
  }
  kqueue_backend(const kqueue_backend&) = delete;
  kqueue_backend& operator=(const kqueue_backend&) = delete;

  [[nodiscard]] std::string_view name() const noexcept override { return "kqueue"; }

  void submit(operation& op, ready_queue& ready) override {
    if (try_syscall(op, /*after_readiness=*/false)) {
      ready.push(op);
      return;
    }
    arm(op, ready);
  }

  bool cancel(operation& op, ready_queue& ready) override {
    if (pending_.erase(&op) == 0) {
      return false;
    }
    struct kevent ev{};
    EV_SET(&ev, static_cast<uintptr_t>(op.fd), filter_of(op), EV_DELETE, 0, 0, nullptr);
    (void)::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    op.result = -ECANCELED;
    ready.push(op);
    return true;
  }

  void cancel_all(ready_queue& ready) override {
    // cancel() mutates pending_; drain from a snapshot.
    const std::vector<operation*> snapshot(pending_.begin(), pending_.end());
    for (operation* op : snapshot) {
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

    std::array<struct kevent, kMaxEvents> events{};
    const int n = ::kevent(kq_, nullptr, 0, events.data(), kMaxEvents, tsp);
    if (n < 0) {
      if (errno == EINTR) {
        return;
      }
      throw std::system_error(errno, std::generic_category(), "kevent wait");
    }

    for (int i = 0; i < n; ++i) {
      auto* op = static_cast<operation*>(events[i].udata);
      // Safe: completions are queued, never resumed inside this batch, so no
      // coroutine can cancel/complete a sibling op mid-iteration.
      if ((events[i].flags & EV_ERROR) != 0) {
        pending_.erase(op);
        op->result = -static_cast<std::int32_t>(events[i].data);
        ready.push(*op);
        continue;
      }
      if (try_syscall(*op, /*after_readiness=*/true)) {
        pending_.erase(op);
        ready.push(*op);
      } else {
        // Spurious readiness: EV_ONESHOT already deleted the knote; re-arm.
        pending_.erase(op);
        arm(*op, ready);
      }
    }
  }

  [[nodiscard]] std::size_t pending() const noexcept override { return pending_.size(); }

 private:
  void arm(operation& op, ready_queue& ready) {
    struct kevent ev{};
    EV_SET(&ev, static_cast<uintptr_t>(op.fd), filter_of(op), EV_ADD | EV_ONESHOT, 0, 0, &op);
    if (::kevent(kq_, &ev, 1, nullptr, 0, nullptr) != 0) {
      op.result = -errno;
      ready.push(op);
      return;
    }
    pending_.insert(&op);
  }

  int kq_ = -1;
  std::unordered_set<operation*> pending_;
};

}  // namespace

std::unique_ptr<backend> make_kqueue_backend() { return std::make_unique<kqueue_backend>(); }

}  // namespace vkp::io
