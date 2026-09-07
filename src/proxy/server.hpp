// M3 proxy server: one instance per worker. Clients bind to one pooled
// backend connection at accept time; requests pipeline onto it with FIFO
// pairing. Design: docs/design/m3-workers-pool-pipelining.md.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "io/event_loop.hpp"
#include "io/socket.hpp"
#include "io/task.hpp"
#include "proxy/backend_conn.hpp"

namespace vkp::proxy {

struct config {
  std::string listen_host = "127.0.0.1";
  std::uint16_t listen_port = 6380;
  std::string backend_host = "127.0.0.1";
  std::uint16_t backend_port = 6379;
  std::chrono::milliseconds shutdown_grace{5000};
  int backlog = 1024;
  bool reuseport = false;  // multi-worker: every worker binds its own listener

  std::size_t conns_per_backend = 1;
  std::size_t client_outbuf_limit = 8U << 20;  // slow-client disconnect watermark
  backend_conn_config backend;
};

class server {
 public:
  // Resolves the backend address and binds the listener immediately (throws
  // std::system_error / std::runtime_error); coroutines start on start().
  server(io::event_loop& loop, config cfg);

  server(const server&) = delete;
  server& operator=(const server&) = delete;

  void start();  // spawns the acceptor and the backend pool onto the loop

  // Actual listen port (after an ephemeral bind with listen_port = 0).
  [[nodiscard]] std::uint16_t port() const { return port_; }

  // First call: stop accepting, let in-flight connections drain, then drain
  // the backend pool and stop the loop (or after shutdown_grace). Second
  // call: stop immediately.
  void begin_shutdown();

  [[nodiscard]] std::size_t active_connections() const noexcept { return active_; }

 private:
  io::task<void> acceptor();
  io::task<void> connection(io::unique_fd client);
  io::task<void> watchdog();
  io::task<void> drain_backends();
  void maybe_drain_backends();
  [[nodiscard]] backend_conn& pick_conn() noexcept;

  io::event_loop& loop_;
  config cfg_;
  io::resolved_addr backend_addr_;
  std::vector<std::unique_ptr<backend_conn>> conns_;
  io::unique_fd listener_;
  std::uint16_t port_ = 0;
  io::cancel_slot accept_cancel_;
  std::size_t active_ = 0;
  std::size_t next_conn_ = 0;
  bool draining_ = false;
  bool backend_drain_started_ = false;
};

}  // namespace vkp::proxy
