// io_uring proactor backend — the primary Linux backend.
// M2 uses single-shot ops throughout; multishot recv requires provided
// buffer rings (IORING_RECV_MULTISHOT implies IOSQE_BUFFER_SELECT), so both
// move to M6 together. See docs/design/io-and-coroutines.md §3.

#include <liburing.h>
#include <sys/socket.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <system_error>
#include <unordered_set>

#include "io/backend.hpp"

namespace vkp::io {

namespace {

constexpr unsigned kQueueDepth = 256;

class uring_backend final : public backend {
 public:
  // Constructing IS the runtime probe: io_uring_setup fails under Docker's
  // default seccomp profile, kernel.io_uring_disabled, or pre-5.19 kernels.
  uring_backend() {
    const int rc = io_uring_queue_init(kQueueDepth, &ring_, 0);
    if (rc < 0) {
      throw std::system_error(-rc, std::generic_category(), "io_uring_queue_init");
    }
  }
  ~uring_backend() override { io_uring_queue_exit(&ring_); }
  uring_backend(const uring_backend&) = delete;
  uring_backend& operator=(const uring_backend&) = delete;

  [[nodiscard]] std::string_view name() const noexcept override { return "io_uring"; }

  void submit(operation& op, ready_queue& ready) override {
    io_uring_sqe* sqe = get_sqe();
    if (sqe == nullptr) {
      op.result = -EBUSY;  // SQ full even after a flush; should not happen
      ready.push(op);
      return;
    }
    switch (op.op) {
      case opcode::accept:
        io_uring_prep_accept(sqe, op.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        break;
      case opcode::recv:
        io_uring_prep_recv(sqe, op.fd, op.rbuf, op.len, 0);
        break;
      case opcode::send:
        io_uring_prep_send(sqe, op.fd, op.wbuf, op.len, MSG_NOSIGNAL);
        break;
      case opcode::connect:
        io_uring_prep_connect(sqe, op.fd, reinterpret_cast<const sockaddr*>(&op.addr),
                              op.addr_len);
        break;
      case opcode::nop:
      case opcode::timeout:
        assert(false && "opcode not handled by backends");
        io_uring_prep_nop(sqe);
        break;
    }
    io_uring_sqe_set_data(sqe, &op);
    inflight_.insert(&op);
    // No io_uring_submit here: poll() flushes the whole batch in one syscall.
  }

  bool cancel(operation& op, ready_queue& /*ready*/) override {
    if (!inflight_.contains(&op)) {
      return false;
    }
    // Asynchronous: the op's own CQE (usually -ECANCELED; a real result wins
    // the race) still arrives through poll(). The cancel SQE's CQE carries
    // null user_data and is skipped.
    io_uring_sqe* sqe = get_sqe();
    if (sqe == nullptr) {
      return false;  // retryable via stop()/cancel_all; effectively can't happen
    }
    io_uring_prep_cancel(sqe, &op, 0);
    io_uring_sqe_set_data(sqe, nullptr);
    return true;
  }

  void cancel_all(ready_queue& /*ready*/) override {
    if (inflight_.empty()) {
      return;
    }
    io_uring_sqe* sqe = get_sqe();
    if (sqe == nullptr) {
      return;
    }
    io_uring_prep_cancel(sqe, nullptr, IORING_ASYNC_CANCEL_ANY);
    io_uring_sqe_set_data(sqe, nullptr);
  }

  void poll(std::optional<std::chrono::nanoseconds> timeout, ready_queue& ready) override {
    int rc = 0;
    if (timeout.has_value()) {
      const auto ns = std::max<std::chrono::nanoseconds::rep>(timeout->count(), 0);
      __kernel_timespec ts{};
      ts.tv_sec = ns / 1'000'000'000;
      ts.tv_nsec = ns % 1'000'000'000;
      io_uring_cqe* cqe = nullptr;
      rc = io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, &ts, nullptr);
    } else {
      rc = io_uring_submit_and_wait(&ring_, 1);
    }
    if (rc < 0 && rc != -ETIME && rc != -EINTR) {
      throw std::system_error(-rc, std::generic_category(), "io_uring submit_and_wait");
    }

    unsigned head = 0;
    unsigned count = 0;
    io_uring_cqe* cqe = nullptr;
    io_uring_for_each_cqe(&ring_, head, cqe) {
      ++count;
      auto* op = static_cast<operation*>(io_uring_cqe_get_data(cqe));
      if (op == nullptr) {
        continue;  // completion of a cancel SQE
      }
      inflight_.erase(op);
      op->result = cqe->res;
      ready.push(*op);
    }
    io_uring_cq_advance(&ring_, count);
  }

  [[nodiscard]] std::size_t pending() const noexcept override { return inflight_.size(); }

 private:
  io_uring_sqe* get_sqe() {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
      (void)io_uring_submit(&ring_);  // flush to make room
      sqe = io_uring_get_sqe(&ring_);
    }
    return sqe;
  }

  io_uring ring_{};
  std::unordered_set<operation*> inflight_;
};

}  // namespace

std::unique_ptr<backend> make_uring_backend() { return std::make_unique<uring_backend>(); }

}  // namespace vkp::io
