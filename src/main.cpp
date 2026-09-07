#include <csignal>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/sinks/ConsoleSink.h>

#include "core/version.hpp"
#include "io/backend.hpp"
#include "proxy/server.hpp"
#include "proxy/worker_pool.hpp"

namespace {

// Async-signal-safe shutdown fan-out: one byte to every worker's self-pipe.
constexpr int kMaxWorkers = 256;
int g_signal_fds[kMaxWorkers];
int g_signal_fd_count = 0;

extern "C" void on_signal(int /*signo*/) {
  const char byte = 1;
  for (int i = 0; i < g_signal_fd_count; ++i) {
    (void)!::write(g_signal_fds[i], &byte, 1);
  }
}

// "host:port" (host may be empty for 0.0.0.0); IPv6 literals use [addr]:port.
std::pair<std::string, std::uint16_t> parse_endpoint(const std::string& ep) {
  const std::size_t colon = ep.rfind(':');
  if (colon == std::string::npos) {
    throw std::runtime_error(fmt::format("invalid endpoint '{}': expected host:port", ep));
  }
  std::string host = ep.substr(0, colon);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  if (host.empty()) {
    host = "0.0.0.0";
  }
  const int port = std::stoi(ep.substr(colon + 1));
  if (port < 0 || port > 65535) {
    throw std::runtime_error(fmt::format("invalid port in '{}'", ep));
  }
  return {host, static_cast<std::uint16_t>(port)};
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{fmt::format("valkey-proxy {}", vkp::kVersion)};
  app.set_version_flag("--version", std::string{vkp::kVersion});

  std::string listen_ep = "127.0.0.1:6380";
  std::string backend_ep = "127.0.0.1:6379";
  std::string io_backend = "auto";
  std::size_t workers = 1;
  bool cpu_affinity = false;
  std::size_t conns_per_backend = 1;
  std::uint32_t request_timeout_ms = 1000;
  app.add_option("-l,--listen", listen_ep, "Listen endpoint (host:port)")->capture_default_str();
  app.add_option("-b,--backend", backend_ep, "Backend valkey endpoint (host:port)")
      ->capture_default_str();
  app.add_option("--io-backend", io_backend, "IO backend")
      ->check(CLI::IsMember({"auto", "io_uring", "epoll", "kqueue"}))
      ->capture_default_str();
  app.add_option("-w,--workers", workers, "Worker threads (0 = one per hardware thread)")
      ->capture_default_str();
  app.add_flag("--cpu-affinity", cpu_affinity, "Pin workers to CPUs (Linux only)");
  app.add_option("--conns-per-backend", conns_per_backend,
                 "Backend connections per worker (1-2 recommended)")
      ->check(CLI::Range(1, 8))
      ->capture_default_str();
  app.add_option("--request-timeout-ms", request_timeout_ms,
                 "Per-request timeout, enqueue to response")
      ->capture_default_str();

  CLI11_PARSE(app, argc, argv);

  quill::Backend::start();
  auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console");
  auto* logger = quill::Frontend::create_or_get_logger("root", std::move(sink));

  try {
    vkp::proxy::worker_pool::options opt;
    std::tie(opt.cfg.listen_host, opt.cfg.listen_port) = parse_endpoint(listen_ep);
    std::tie(opt.cfg.backend_host, opt.cfg.backend_port) = parse_endpoint(backend_ep);
    opt.cfg.conns_per_backend = conns_per_backend;
    opt.cfg.backend.request_timeout = std::chrono::milliseconds{request_timeout_ms};
    opt.workers = workers;
    opt.cpu_affinity = cpu_affinity;
    if (io_backend == "io_uring") {
      opt.io_backend = vkp::io::backend_kind::io_uring;
    } else if (io_backend == "epoll") {
      opt.io_backend = vkp::io::backend_kind::epoll;
    } else if (io_backend == "kqueue") {
      opt.io_backend = vkp::io::backend_kind::kqueue;
    }

    vkp::proxy::worker_pool pool{opt};

    // Signal plumbing before run(): SIGTERM/SIGINT drain, repeat forces.
    (void)std::signal(SIGPIPE, SIG_IGN);
    const auto& fds = pool.shutdown_fds();
    g_signal_fd_count = static_cast<int>(std::min<std::size_t>(fds.size(), kMaxWorkers));
    for (int i = 0; i < g_signal_fd_count; ++i) {
      g_signal_fds[i] = fds[static_cast<std::size_t>(i)];
    }
    (void)std::signal(SIGTERM, on_signal);
    (void)std::signal(SIGINT, on_signal);

    LOG_INFO(logger,
             "valkey-proxy {} listening on {}:{} -> backend {}:{} "
             "({} worker(s), {} conn(s)/backend, io: {})",
             vkp::kVersion, opt.cfg.listen_host, pool.port(), opt.cfg.backend_host,
             opt.cfg.backend_port, pool.workers(), conns_per_backend, pool.io_backend_name());

    pool.run();

    LOG_INFO(logger, "shutdown complete");
  } catch (const std::exception& e) {
    LOG_ERROR(logger, "fatal: {}", e.what());
    quill::Backend::stop();
    return EXIT_FAILURE;
  }

  quill::Backend::stop();
  return EXIT_SUCCESS;
}
