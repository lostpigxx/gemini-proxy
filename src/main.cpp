#include <csignal>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "admin/http_server.hpp"
#include "core/log.hpp"
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
  std::vector<std::string> cluster_seeds;
  std::uint32_t cluster_refresh_ms = 5000;
  std::size_t max_redirects = 5;
  app.add_option("-l,--listen", listen_ep, "Listen endpoint (host:port)")->capture_default_str();
  auto* backend_opt =
      app.add_option("-b,--backend", backend_ep, "Backend valkey endpoint (host:port)")
          ->capture_default_str();
  app.add_option("--cluster-seeds", cluster_seeds,
                 "Comma-separated cluster seed endpoints; enables cluster mode")
      ->delimiter(',')
      ->excludes(backend_opt);
  app.add_option("--cluster-refresh-ms", cluster_refresh_ms, "Topology refresh interval")
      ->capture_default_str();
  app.add_option("--max-redirects", max_redirects, "MOVED/ASK redirects allowed per request")
      ->check(CLI::Range(1, 64))
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

  vkp::log::options log_opt;
  std::string log_format = "text";
  app.add_option("--log-level", log_opt.level, "Log level")
      ->check(CLI::IsMember({"tracel3", "tracel2", "tracel1", "debug", "info", "warning", "error",
                             "critical", "none"}))
      ->capture_default_str();
  app.add_option("--log-format", log_format, "Log output format")
      ->check(CLI::IsMember({"text", "json"}))
      ->capture_default_str();
  app.add_option("--log-file", log_opt.file, "Log to this file instead of stdout");

  std::string admin_ep = "127.0.0.1:9180";
  app.add_option("--admin-listen", admin_ep,
                 "Admin HTTP endpoint (/metrics, /health, /topology); empty disables it")
      ->capture_default_str();

  CLI11_PARSE(app, argc, argv);

  log_opt.fmt = log_format == "json" ? vkp::log::format::json : vkp::log::format::text;
  try {
    vkp::log::init(log_opt);
  } catch (const std::exception& e) {
    // No logger yet, so this one genuinely has to go to stderr.
    fmt::print(stderr, "fatal: {}\n", e.what());
    return EXIT_FAILURE;
  }

  try {
    vkp::proxy::worker_pool::options opt;
    std::tie(opt.cfg.listen_host, opt.cfg.listen_port) = parse_endpoint(listen_ep);
    std::tie(opt.cfg.backend_host, opt.cfg.backend_port) = parse_endpoint(backend_ep);
    // Reject a malformed seed here rather than at the first bootstrap attempt,
    // where it would look like an unreachable node.
    for (const std::string& seed : cluster_seeds) {
      (void)parse_endpoint(seed);
    }
    opt.cfg.cluster_seeds = cluster_seeds;
    opt.cfg.cluster_refresh = std::chrono::milliseconds{cluster_refresh_ms};
    opt.cfg.max_redirects = max_redirects;
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

    // Same self-pipe protocol as the workers, so the handler stays a pure
    // write() fan-out: byte one drains (admin starts answering 503), byte two
    // stops. Constructed after the pool so a port clash on the data plane
    // fails first.
    std::unique_ptr<vkp::admin::http_server> admin;
    if (!admin_ep.empty()) {
      vkp::admin::http_config acfg;
      std::tie(acfg.listen_host, acfg.listen_port) = parse_endpoint(admin_ep);
      admin = std::make_unique<vkp::admin::http_server>(acfg, pool.stats());
    }

    // Signal plumbing before run(): SIGTERM/SIGINT drain, repeat forces.
    (void)std::signal(SIGPIPE, SIG_IGN);
    const auto& fds = pool.shutdown_fds();
    g_signal_fd_count = static_cast<int>(std::min<std::size_t>(fds.size(), kMaxWorkers - 1));
    for (int i = 0; i < g_signal_fd_count; ++i) {
      g_signal_fds[i] = fds[static_cast<std::size_t>(i)];
    }
    if (admin) {
      g_signal_fds[g_signal_fd_count++] = admin->shutdown_fd();
    }
    (void)std::signal(SIGTERM, on_signal);
    (void)std::signal(SIGINT, on_signal);

    if (admin) {
      admin->start();
    }

    const std::string upstream =
        cluster_seeds.empty()
            ? fmt::format("backend {}:{}", opt.cfg.backend_host, opt.cfg.backend_port)
            : fmt::format("cluster seeds {}", fmt::join(cluster_seeds, ","));
    VKP_LOG_INFO(
        "valkey-proxy {version} listening on {host}:{port} -> {upstream} "
        "({workers} worker(s), {conns_per_backend} conn(s)/backend, io: {io_backend})",
        vkp::kVersion, opt.cfg.listen_host, pool.port(), upstream, pool.workers(),
        conns_per_backend, pool.io_backend_name());

    pool.run();

    if (admin) {
      admin->stop();  // outlives the data plane so a scrape can catch the drain
    }
    VKP_LOG_INFO("shutdown complete");
  } catch (const std::exception& e) {
    VKP_LOG_ERROR("fatal: {}", e.what());
    vkp::log::shutdown();
    return EXIT_FAILURE;
  }

  vkp::log::shutdown();
  return EXIT_SUCCESS;
}
