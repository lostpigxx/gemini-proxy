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
#include "config/settings.hpp"
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

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{fmt::format("valkey-proxy {}", vkp::kVersion)};
  app.set_version_flag("--version", std::string{vkp::kVersion});

  // These hold what the command line said; whether they are used at all is
  // decided after the file is read, by Option::count(). The defaults below are
  // only what --help prints — vkp::config::settings owns the real ones.
  std::string config_path;
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
  std::string log_level = "info";
  std::string log_format = "text";
  std::string log_file;
  std::string admin_ep = "127.0.0.1:9180";
  std::uint32_t shutdown_grace_ms = 5000;

  app.add_option("-c,--config", config_path, "TOML configuration file")->check(CLI::ExistingFile);
  auto* o_listen = app.add_option("-l,--listen", listen_ep, "Listen endpoint (host:port)")
                       ->capture_default_str();
  auto* o_backend =
      app.add_option("-b,--backend", backend_ep, "Backend valkey endpoint (host:port)")
          ->capture_default_str();
  auto* o_seeds = app.add_option("--cluster-seeds", cluster_seeds,
                                 "Comma-separated cluster seed endpoints; enables cluster mode")
                      ->delimiter(',')
                      ->excludes(o_backend);
  auto* o_refresh =
      app.add_option("--cluster-refresh-ms", cluster_refresh_ms, "Topology refresh interval")
          ->capture_default_str();
  auto* o_redirects =
      app.add_option("--max-redirects", max_redirects, "MOVED/ASK redirects allowed per request")
          ->check(CLI::Range(1, 64))
          ->capture_default_str();
  auto* o_io = app.add_option("--io-backend", io_backend, "IO backend")
                   ->check(CLI::IsMember({"auto", "io_uring", "epoll", "kqueue"}))
                   ->capture_default_str();
  auto* o_workers =
      app.add_option("-w,--workers", workers, "Worker threads (0 = one per hardware thread)")
          ->capture_default_str();
  auto* o_affinity =
      app.add_flag("--cpu-affinity", cpu_affinity, "Pin workers to CPUs (Linux only)");
  auto* o_conns = app.add_option("--conns-per-backend", conns_per_backend,
                                 "Backend connections per worker (1-2 recommended)")
                      ->check(CLI::Range(1, 8))
                      ->capture_default_str();
  auto* o_timeout = app.add_option("--request-timeout-ms", request_timeout_ms,
                                   "Per-request timeout, enqueue to response")
                        ->capture_default_str();
  auto* o_level = app.add_option("--log-level", log_level, "Log level")
                      ->check(CLI::IsMember({"tracel3", "tracel2", "tracel1", "debug", "info",
                                             "warning", "error", "critical", "none"}))
                      ->capture_default_str();
  auto* o_logfmt = app.add_option("--log-format", log_format, "Log output format")
                       ->check(CLI::IsMember({"text", "json"}))
                       ->capture_default_str();
  auto* o_logfile = app.add_option("--log-file", log_file, "Log to this file instead of stdout");
  auto* o_admin =
      app.add_option("--admin-listen", admin_ep,
                     "Admin HTTP endpoint (/metrics, /health, /topology); empty disables it")
          ->capture_default_str();
  auto* o_grace = app.add_option("--shutdown-grace-ms", shutdown_grace_ms,
                                 "Hard stop this long after SIGTERM, even with requests in flight")
                      ->capture_default_str();

  CLI11_PARSE(app, argc, argv);

  vkp::config::settings cfg;
  try {
    // Precedence: defaults → TOML → explicit command line. "Explicit" is
    // count() > 0 rather than "differs from the default", because the latter
    // cannot tell "not given" from "given a value equal to the default" — and
    // the whole point of the file is to move the effective default.
    if (!config_path.empty()) {
      cfg = vkp::config::load_toml(config_path);
    }
    const auto given = [](const CLI::Option* o) { return o->count() > 0; };
    if (given(o_listen)) {
      std::tie(cfg.proxy.listen_host, cfg.proxy.listen_port) =
          vkp::config::parse_endpoint(listen_ep);
    }
    if (given(o_backend)) {
      std::tie(cfg.proxy.backend_host, cfg.proxy.backend_port) =
          vkp::config::parse_endpoint(backend_ep);
    }
    if (given(o_seeds)) {
      cfg.proxy.cluster_seeds = cluster_seeds;
    }
    if (given(o_refresh)) {
      cfg.proxy.cluster_refresh = std::chrono::milliseconds{cluster_refresh_ms};
    }
    if (given(o_redirects)) {
      cfg.proxy.max_redirects = max_redirects;
    }
    if (given(o_io)) {
      cfg.io_backend = io_backend;
    }
    if (given(o_workers)) {
      cfg.workers = workers;
    }
    if (given(o_affinity)) {
      cfg.cpu_affinity = cpu_affinity;
    }
    if (given(o_conns)) {
      cfg.proxy.conns_per_backend = conns_per_backend;
    }
    if (given(o_timeout)) {
      cfg.proxy.backend.request_timeout = std::chrono::milliseconds{request_timeout_ms};
    }
    if (given(o_level)) {
      cfg.log.level = log_level;
    }
    if (given(o_logfmt)) {
      cfg.log_format = log_format;
    }
    if (given(o_logfile)) {
      cfg.log.file = log_file;
    }
    if (given(o_admin)) {
      cfg.admin_endpoint = admin_ep;
    }
    if (given(o_grace)) {
      cfg.proxy.shutdown_grace = std::chrono::milliseconds{shutdown_grace_ms};
    }
    vkp::config::validate(cfg);
  } catch (const std::exception& e) {
    // No logger yet — this one genuinely has to go to stderr.
    fmt::print(stderr, "fatal: {}\n", e.what());
    return EXIT_FAILURE;
  }

  cfg.log.fmt = cfg.log_format == "json" ? vkp::log::format::json : vkp::log::format::text;
  try {
    vkp::log::init(cfg.log);
  } catch (const std::exception& e) {
    fmt::print(stderr, "fatal: {}\n", e.what());
    return EXIT_FAILURE;
  }

  try {
    vkp::proxy::worker_pool::options opt;
    opt.cfg = cfg.proxy;
    opt.workers = cfg.workers;
    opt.cpu_affinity = cfg.cpu_affinity;
    if (cfg.io_backend == "io_uring") {
      opt.io_backend = vkp::io::backend_kind::io_uring;
    } else if (cfg.io_backend == "epoll") {
      opt.io_backend = vkp::io::backend_kind::epoll;
    } else if (cfg.io_backend == "kqueue") {
      opt.io_backend = vkp::io::backend_kind::kqueue;
    }

    // Logged before anything binds, so a postmortem's first question — "did
    // the config take effect?" — is answered without guessing.
    VKP_LOG_INFO("effective configuration:\n{config}", vkp::config::to_string(cfg));

    vkp::proxy::worker_pool pool{opt};

    // Same self-pipe protocol as the workers, so the handler stays a pure
    // write() fan-out: byte one drains (admin starts answering 503), byte two
    // stops. Constructed after the pool so a port clash on the data plane
    // fails first.
    std::unique_ptr<vkp::admin::http_server> admin;
    if (!cfg.admin_endpoint.empty()) {
      vkp::admin::http_config acfg;
      std::tie(acfg.listen_host, acfg.listen_port) =
          vkp::config::parse_endpoint(cfg.admin_endpoint);
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
        cfg.proxy.cluster_seeds.empty()
            ? fmt::format("backend {}:{}", cfg.proxy.backend_host, cfg.proxy.backend_port)
            : fmt::format("cluster seeds {}", fmt::join(cfg.proxy.cluster_seeds, ","));
    VKP_LOG_INFO(
        "valkey-proxy {version} listening on {host}:{port} -> {upstream} "
        "({workers} worker(s), {conns_per_backend} conn(s)/backend, io: {io_backend})",
        vkp::kVersion, cfg.proxy.listen_host, pool.port(), upstream, pool.workers(),
        cfg.proxy.conns_per_backend, pool.io_backend_name());

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
