// Admin HTTP surface: /metrics, /health, /topology.
// Design: docs/design/m5-observability-config.md §5.
//
// Runs on its own thread with its own event_loop rather than borrowing a
// worker's. Rendering /metrics builds a few KB of string; doing that on a data
// -plane loop injects a periodic stall into exactly the p99 M6 means to
// measure. The thread costs one idle fd wait.
//
// The HTTP it speaks is deliberately the minimum: request line only, headers
// discarded, GET only, no keep-alive. Prometheus scrapes at 15 s intervals —
// the saved handshake is not worth a second state machine.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "admin/http_parse.hpp"
#include "core/metrics.hpp"
#include "io/event_loop.hpp"
#include "io/socket.hpp"
#include "io/task.hpp"

namespace vkp::admin {

struct http_config {
  std::string listen_host = "127.0.0.1";
  std::uint16_t listen_port = 9180;
  int backlog = 16;
  // A request the proxy will never legitimately see is a request it should
  // stop buffering. Only the request line matters and it is a fixed path.
  std::size_t max_request_bytes = 8U << 10;
};

// Body builders, exposed so tests can assert on the content without a socket.
void render_topology(std::string& out, const metrics::registry& stats);
void render_health(std::string& out, const metrics::registry& stats, bool draining);

class http_server {
 public:
  // Binds immediately (throws std::system_error) so a port clash is a startup
  // failure, not a surprise once the data plane is already serving.
  http_server(http_config cfg, const metrics::registry& stats);
  ~http_server();

  http_server(const http_server&) = delete;
  http_server& operator=(const http_server&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  void start();  // launches the thread
  // Flips /health to 503 without stopping the server: a load balancer needs
  // to see the proxy leave rotation while it is still draining requests.
  // Async-signal-safe.
  void begin_drain() noexcept { draining_.store(true, std::memory_order_relaxed); }
  // Stops the loop and joins. Safe to call from any thread, and from a signal
  // handler only via shutdown_fd().
  void stop() noexcept;
  // Write end of the self-pipe, for the signal fan-out in main.
  [[nodiscard]] int shutdown_fd() const noexcept { return wake_write_fd_; }

 private:
  io::task<void> acceptor();
  io::task<void> session(io::unique_fd fd);
  io::task<void> shutdown_watcher();
  [[nodiscard]] std::string respond(std::string_view path) const;

  http_config cfg_;
  const metrics::registry& stats_;
  std::unique_ptr<io::event_loop> loop_;
  io::unique_fd listener_;
  std::uint16_t port_ = 0;
  int wake_read_fd_ = -1;
  int wake_write_fd_ = -1;
  std::atomic<bool> draining_{false};
  std::thread thread_;
  std::string error_;  // first exception out of the thread; logged on stop()
};

}  // namespace vkp::admin
