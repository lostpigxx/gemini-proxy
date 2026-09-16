#include "admin/http_server.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <iterator>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>

#include <fmt/format.h>

#include "core/log.hpp"

namespace vkp::admin {

namespace {

using sink = std::back_insert_iterator<std::string>;

constexpr std::string_view kMetricsType = "text/plain; version=0.0.4; charset=utf-8";
constexpr std::string_view kPlainType = "text/plain; charset=utf-8";
constexpr std::string_view kJsonType = "application/json";

// Fixed-point seconds, same reasoning as metrics.cpp: a double would round,
// and an age that disagrees with itself between two scrapes is confusing.
void seconds(std::string& out, std::chrono::steady_clock::duration d) {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
  const auto v = static_cast<std::uint64_t>(ns > 0 ? ns : 0);
  fmt::format_to(sink{out}, "{}.{:03}", v / 1'000'000'000ULL,
                 (v % 1'000'000'000ULL) / 1'000'000ULL);
}

std::string http_response(int code, std::string_view reason, std::string_view ctype,
                          std::string_view body) {
  // Content-Length plus Connection: close — no chunking, no keep-alive.
  return fmt::format(
      "HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
      code, reason, ctype, body.size(), body);
}

void set_nonblocking(int fd) noexcept {
  (void)::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

}  // namespace

void render_health(std::string& out, const metrics::registry& stats, bool draining) {
  fmt::format_to(sink{out}, "{{\"status\":\"{}\",\"workers\":{},\"uptime_seconds\":",
                 draining ? "draining" : "ok", stats.workers());
  seconds(out, stats.uptime());
  out += "}\n";
}

void render_topology(std::string& out, const metrics::registry& stats) {
  const auto now = std::chrono::steady_clock::now();
  if (stats.workers() == 0) {
    out += "(no workers)\n";
    return;
  }

  // Worker 0's full map. Every worker refreshes independently (m4 §4.2), so
  // there is no single truth to print — one full map plus a per-worker
  // summary makes a disagreement visible instead of hiding it behind a
  // "representative" view.
  const std::string text = stats.worker(0).topology_snapshot(now).first;
  out += "# worker 0 slot map\n";
  if (text.empty()) {
    out += "(no topology yet)\n";
  } else {
    out += text;
  }

  out += "\n# per-worker summary\n# worker nodes slots snapshot_age_seconds\n";
  for (std::size_t w = 0; w < stats.workers(); ++w) {
    const metrics::worker_stats& s = stats.worker(w);
    const auto [t, a] = s.topology_snapshot(now);
    fmt::format_to(sink{out}, "{} {} {} ", w, s.topology_nodes.read(),
                   s.topology_slots_assigned.read());
    if (t.empty()) {
      out += "never";
    } else {
      seconds(out, a);
    }
    out.push_back('\n');
  }
}

http_server::http_server(http_config cfg, const metrics::registry& stats)
    : cfg_(std::move(cfg)),
      stats_(stats),
      loop_(std::make_unique<io::event_loop>()),
      listener_(io::listen_tcp(cfg_.listen_host, cfg_.listen_port, cfg_.backlog)),
      port_(io::local_port(listener_.get())) {
  int sv[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    throw std::system_error(errno, std::generic_category(), "socketpair");
  }
  set_nonblocking(sv[0]);
  set_nonblocking(sv[1]);
  wake_read_fd_ = sv[0];
  wake_write_fd_ = sv[1];
}

http_server::~http_server() {
  stop();
  if (wake_read_fd_ >= 0) {
    ::close(wake_read_fd_);
  }
  if (wake_write_fd_ >= 0) {
    ::close(wake_write_fd_);
  }
}

void http_server::start() {
  thread_ = std::thread{[this] {
    try {
      // Spawned here, not in the constructor: the first suspension registers
      // with the loop, and that must happen on the thread that runs it.
      io::spawn(acceptor());
      io::spawn(shutdown_watcher());
      loop_->run();
    } catch (const std::exception& e) {
      error_ = e.what();
    }
  }};
}

void http_server::stop() noexcept {
  if (!thread_.joinable()) {
    return;
  }
  // Two bytes: see shutdown_watcher. stop() means stop even if no signal ever
  // delivered the first byte (a worker died on an exception, say).
  const char bytes[2] = {1, 1};
  (void)!::write(wake_write_fd_, bytes, 2);
  thread_.join();
  if (!error_.empty()) {
    VKP_LOG_ERROR("admin: server thread failed: {error}", error_);
  }
}

io::task<void> http_server::shutdown_watcher() {
  char buf[16];
  int bytes = 0;
  for (;;) {
    const std::int32_t n = co_await loop_->async_recv(wake_read_fd_, buf);
    if (n <= 0) {
      co_return;
    }
    bytes += n;
    // Same self-pipe protocol as the workers, so the signal handler stays a
    // pure write() fan-out: the first byte means the process started
    // draining — keep serving, a load balancer needs to see the 503 — and
    // only the second actually takes the admin surface down.
    if (bytes == 1) {
      begin_drain();
      VKP_LOG_INFO("admin: draining, /health now reports 503");
      continue;
    }
    break;
  }
  VKP_LOG_INFO("admin: stopping");
  loop_->stop();
}

std::string http_server::respond(std::string_view path) const {
  std::string body;
  if (path == "/metrics") {
    body.reserve(4096);
    stats_.render_prometheus(body);
    return http_response(200, "OK", kMetricsType, body);
  }
  if (path == "/health") {
    const bool draining = draining_.load(std::memory_order_relaxed);
    render_health(body, stats_, draining);
    // 503 while draining so a load balancer pulls traffic before the listener
    // goes away. A dead backend is deliberately still 200: every proxy
    // instance shares one backend cluster, so taking them all out of rotation
    // achieves nothing (design §5).
    return draining ? http_response(503, "Service Unavailable", kJsonType, body)
                    : http_response(200, "OK", kJsonType, body);
  }
  if (path == "/topology") {
    render_topology(body, stats_);
    return http_response(200, "OK", kPlainType, body);
  }
  return http_response(404, "Not Found", kPlainType, "not found\n");
}

io::task<void> http_server::session(io::unique_fd fd) {
  std::string buf;
  std::string reply;
  for (;;) {
    const parsed_request req = parse_request(buf);
    if (req.st == parsed_request::state::ok) {
      reply = respond(req.path);
      break;
    }
    if (req.st == parsed_request::state::not_get) {
      reply = http_response(405, "Method Not Allowed", kPlainType, "only GET is supported\n");
      break;
    }
    if (req.st == parsed_request::state::bad_request) {
      reply = http_response(400, "Bad Request", kPlainType, "malformed request\n");
      break;
    }
    if (buf.size() >= cfg_.max_request_bytes) {
      reply = http_response(431, "Request Header Fields Too Large", kPlainType, "too large\n");
      break;
    }
    char tmp[1024];
    const std::int32_t n = co_await loop_->async_recv(fd.get(), tmp);
    if (n <= 0) {
      co_return;  // client vanished, or the loop is stopping
    }
    buf.append(tmp, static_cast<std::size_t>(n));
  }
  (void)co_await io::send_all(*loop_, fd.get(), reply);
  // Connection: close — dropping fd here is the close.
}

io::task<void> http_server::acceptor() {
  VKP_LOG_INFO("admin: listening on {host}:{port}", cfg_.listen_host, port_);
  for (;;) {
    const std::int32_t fd = co_await loop_->async_accept(listener_.get());
    if (fd < 0) {
      co_return;  // -ECANCELED on stop
    }
    io::setup_stream_socket(fd);
    io::spawn(session(io::unique_fd{fd}));
  }
}

}  // namespace vkp::admin
