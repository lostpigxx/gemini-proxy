#include "io/socket.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <system_error>
#include <unistd.h>

#include <fmt/format.h>

namespace vkp::io {

namespace {

void set_nonblocking_cloexec(int fd) {
  if (::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != 0 ||
      ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
    throw std::system_error(errno, std::generic_category(), "fcntl");
  }
}

void set_common_options(int fd) {
  const int one = 1;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#if defined(__APPLE__)
  // macOS has no MSG_NOSIGNAL; suppress SIGPIPE per socket instead.
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

}  // namespace

void unique_fd::reset() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

resolved_addr resolve_tcp(const std::string& host, std::uint16_t port) {
  addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* list = nullptr;
  const std::string service = std::to_string(port);
  if (const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &list); rc != 0) {
    throw std::runtime_error(
        fmt::format("resolve {}:{}: {}", host, port, ::gai_strerror(rc)));
  }

  resolved_addr out;
  std::memcpy(&out.addr, list->ai_addr, list->ai_addrlen);
  out.len = static_cast<socklen_t>(list->ai_addrlen);
  ::freeaddrinfo(list);
  return out;
}

unique_fd listen_tcp(const std::string& host, std::uint16_t port, int backlog) {
  const resolved_addr target = resolve_tcp(host, port);

  unique_fd fd{::socket(target.addr.ss_family, SOCK_STREAM, IPPROTO_TCP)};
  if (!fd) {
    throw std::system_error(errno, std::generic_category(), "socket");
  }
  set_nonblocking_cloexec(fd.get());

  const int one = 1;
  (void)::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&target.addr), target.len) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            fmt::format("bind {}:{}", host, port));
  }
  if (::listen(fd.get(), backlog) != 0) {
    throw std::system_error(errno, std::generic_category(), "listen");
  }
  return fd;
}

std::uint16_t local_port(int fd) {
  sockaddr_storage addr = {};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    throw std::system_error(errno, std::generic_category(), "getsockname");
  }
  if (addr.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
  }
  return ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port);
}

task<unique_fd> connect_tcp(event_loop& loop, resolved_addr addr) {
  unique_fd fd{::socket(addr.addr.ss_family, SOCK_STREAM, IPPROTO_TCP)};
  if (!fd) {
    throw std::system_error(errno, std::generic_category(), "socket");
  }
  set_nonblocking_cloexec(fd.get());
  set_common_options(fd.get());

  const std::int32_t rc = co_await loop.async_connect(
      fd.get(), reinterpret_cast<const sockaddr*>(&addr.addr), addr.len);
  if (rc < 0) {
    throw std::system_error(-rc, std::generic_category(), "connect");
  }
  co_return fd;
}

task<std::int32_t> send_all(event_loop& loop, int fd, std::string_view data) {
  std::size_t off = 0;
  while (off < data.size()) {
    const std::int32_t n = co_await loop.async_send(fd, {data.data() + off, data.size() - off});
    if (n < 0) {
      co_return n;
    }
    if (n == 0) {
      co_return -EIO;  // send(2) should never return 0 for nonzero lengths
    }
    off += static_cast<std::size_t>(n);
  }
  co_return 0;
}

}  // namespace vkp::io
