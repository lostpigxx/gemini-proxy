// M2 minimal proxy: single thread, single backend, whole-frame passthrough.
// One coroutine per client; strictly serial request/response per connection
// (pipelining/backend pooling arrive in M3). Design doc §5.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "io/event_loop.hpp"
#include "io/socket.hpp"
#include "io/task.hpp"

namespace vkp::proxy {

struct config {
  std::string listen_host = "127.0.0.1";
  std::uint16_t listen_port = 6380;
  std::string backend_host = "127.0.0.1";
  std::uint16_t backend_port = 6379;
  std::chrono::milliseconds shutdown_grace{5000};
  int backlog = 1024;
};

class server {
 public:
  // Resolves the backend address and binds the listener immediately (throws
  // std::system_error / std::runtime_error); coroutines start on start().
  server(io::event_loop& loop, config cfg);

  server(const server&) = delete;
  server& operator=(const server&) = delete;

  void start();  // spawns the acceptor onto the loop

  // Actual listen port (after an ephemeral bind with listen_port = 0).
  [[nodiscard]] std::uint16_t port() const { return port_; }

  // First call: stop accepting, let in-flight connections drain, stop the
  // loop when idle or after shutdown_grace. Second call: stop immediately.
  void begin_shutdown();

  [[nodiscard]] std::size_t active_connections() const noexcept { return active_; }

 private:
  io::task<void> acceptor();
  io::task<void> connection(io::unique_fd client);
  io::task<void> relay(int client_fd, int backend_fd);
  io::task<void> watchdog();

  io::event_loop& loop_;
  config cfg_;
  io::resolved_addr backend_addr_;
  io::unique_fd listener_;
  std::uint16_t port_ = 0;
  io::cancel_slot accept_cancel_;
  std::size_t active_ = 0;
  bool draining_ = false;
};

}  // namespace vkp::proxy
