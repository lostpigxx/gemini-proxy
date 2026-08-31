// Socket setup helpers and small coroutine utilities on top of event_loop.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>

#include "io/event_loop.hpp"
#include "io/task.hpp"

namespace vkp::io {

class unique_fd {
 public:
  unique_fd() noexcept = default;
  explicit unique_fd(int fd) noexcept : fd_(fd) {}
  unique_fd(unique_fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  unique_fd& operator=(unique_fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  unique_fd(const unique_fd&) = delete;
  unique_fd& operator=(const unique_fd&) = delete;
  ~unique_fd() { reset(); }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
  void reset() noexcept;
  explicit operator bool() const noexcept { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

struct resolved_addr {
  sockaddr_storage addr = {};
  socklen_t len = 0;
};

// Synchronous resolution — startup / connection-setup path only.
// Throws std::runtime_error on resolver failure.
[[nodiscard]] resolved_addr resolve_tcp(const std::string& host, std::uint16_t port);

// Nonblocking + cloexec listener with SO_REUSEADDR; port 0 picks an
// ephemeral port (see local_port). Throws std::system_error.
[[nodiscard]] unique_fd listen_tcp(const std::string& host, std::uint16_t port, int backlog = 1024);

[[nodiscard]] std::uint16_t local_port(int fd);  // throws std::system_error

// TCP_NODELAY for accepted sockets. Without it, serial small replies into a
// pipelining client stall on Nagle + delayed ACK (~40 ms per batch).
void set_tcp_nodelay(int fd) noexcept;

// Nonblocking connected TCP socket with TCP_NODELAY set.
// Throws std::system_error on failure (including -ECANCELED during stop()).
[[nodiscard]] task<unique_fd> connect_tcp(event_loop& loop, resolved_addr addr);

// Loops async_send until all of `data` is written. Returns 0 on success or
// -errno from the failing send.
[[nodiscard]] task<std::int32_t> send_all(event_loop& loop, int fd, std::string_view data);

}  // namespace vkp::io
