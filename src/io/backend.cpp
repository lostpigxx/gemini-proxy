#include "io/backend.hpp"

#include <system_error>

namespace vkp::io {

// Implemented in the per-platform translation units.
#if defined(__linux__)
std::unique_ptr<backend> make_uring_backend();  // throws if the probe fails
std::unique_ptr<backend> make_epoll_backend();
#elif defined(__APPLE__)
std::unique_ptr<backend> make_kqueue_backend();
#endif

std::string_view to_string(backend_kind k) noexcept {
  switch (k) {
    case backend_kind::io_uring:
      return "io_uring";
    case backend_kind::epoll:
      return "epoll";
    case backend_kind::kqueue:
      return "kqueue";
  }
  return "?";
}

std::unique_ptr<backend> make_backend() {
#if defined(__linux__)
  try {
    return make_uring_backend();
  } catch (const std::system_error&) {
    // seccomp / kernel.io_uring_disabled / pre-5.19 kernel: mandatory
    // fallback (docs/architecture-decisions.md §1.2).
  }
  return make_epoll_backend();
#elif defined(__APPLE__)
  return make_kqueue_backend();
#else
#error "unsupported platform"
#endif
}

std::unique_ptr<backend> make_backend(backend_kind kind) {
  switch (kind) {
    case backend_kind::io_uring:
#if defined(__linux__)
      return make_uring_backend();
#else
      break;
#endif
    case backend_kind::epoll:
#if defined(__linux__)
      return make_epoll_backend();
#else
      break;
#endif
    case backend_kind::kqueue:
#if defined(__APPLE__)
      return make_kqueue_backend();
#else
      break;
#endif
  }
  throw std::system_error(
      ENOTSUP, std::generic_category(),
      std::string("backend unavailable on this platform: ") + std::string(to_string(kind)));
}

std::vector<backend_kind> available_backends() {
  std::vector<backend_kind> kinds;
#if defined(__linux__)
  try {
    (void)make_uring_backend();
    kinds.push_back(backend_kind::io_uring);
  } catch (const std::system_error&) {
  }
  kinds.push_back(backend_kind::epoll);
#elif defined(__APPLE__)
  kinds.push_back(backend_kind::kqueue);
#endif
  return kinds;
}

}  // namespace vkp::io
