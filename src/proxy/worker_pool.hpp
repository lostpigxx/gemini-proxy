// Thread-per-core worker pool: N threads × (event_loop + server), each with
// its own SO_REUSEPORT listener and self-pipe shutdown wakeup.
// Design: docs/design/m3-workers-pool-pipelining.md §1.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "io/backend.hpp"
#include "io/event_loop.hpp"
#include "proxy/server.hpp"

namespace vkp::proxy {

class worker_pool {
 public:
  struct options {
    config cfg;                                  // reuseport is set internally when workers > 1
    std::size_t workers = 1;                     // 0 = hardware_concurrency
    bool cpu_affinity = false;                   // Linux only; ignored elsewhere
    std::optional<io::backend_kind> io_backend;  // nullopt = auto probe
  };

  // Builds every loop and server up front (all listeners bound; with
  // listen_port 0, worker 0 picks the port and the rest join it). Throws on
  // any setup failure.
  explicit worker_pool(options opt);
  ~worker_pool();

  worker_pool(const worker_pool&) = delete;
  worker_pool& operator=(const worker_pool&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] std::size_t workers() const noexcept { return workers_.size(); }
  [[nodiscard]] std::string_view io_backend_name() const noexcept;

  // Launches one thread per worker and blocks until every loop exited.
  // Rethrows (as std::runtime_error) if any worker died on an exception.
  void run();

  // Nudges every worker toward graceful shutdown (drain); calling it again
  // forces an immediate stop. Callable from any thread.
  void request_shutdown() noexcept;

  // Write ends of the per-worker self-pipes, for async-signal-safe shutdown
  // from a signal handler (write one byte to each).
  [[nodiscard]] const std::vector<int>& shutdown_fds() const noexcept { return wake_write_fds_; }

 private:
  struct worker;

  std::vector<std::unique_ptr<worker>> workers_;
  std::vector<int> wake_write_fds_;
  bool cpu_affinity_ = false;
};

}  // namespace vkp::proxy
