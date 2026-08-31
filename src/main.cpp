#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/sinks/ConsoleSink.h>

#include "core/version.hpp"
#include "io/backend.hpp"
#include "io/event_loop.hpp"
#include "io/socket.hpp"
#include "io/task.hpp"
#include "proxy/server.hpp"

namespace {

// Self-pipe via socketpair: write(2) is async-signal-safe, and recv works on
// AF_UNIX sockets under every backend (io_uring's RECV is socket-only).
int g_signal_fd = -1;

extern "C" void on_signal(int /*signo*/) {
  const char byte = 1;
  (void)!::write(g_signal_fd, &byte, 1);
}

vkp::io::task<void> signal_watcher(vkp::io::event_loop& loop, vkp::proxy::server& srv, int fd,
                                   quill::Logger* logger) {
  char buf[16];
  for (;;) {
    const std::int32_t n = co_await loop.async_recv(fd, buf);
    if (n <= 0) {
      co_return;  // -ECANCELED once the loop stops
    }
    if (srv.active_connections() > 0) {
      LOG_INFO(logger,
               "shutdown requested: draining {} connection(s), grace 5s "
               "(signal again to force)",
               srv.active_connections());
    } else {
      LOG_INFO(logger, "shutdown requested");
    }
    srv.begin_shutdown();
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
  app.add_option("-l,--listen", listen_ep, "Listen endpoint (host:port)")->capture_default_str();
  app.add_option("-b,--backend", backend_ep, "Backend valkey endpoint (host:port)")
      ->capture_default_str();
  app.add_option("--io-backend", io_backend, "IO backend")
      ->check(CLI::IsMember({"auto", "io_uring", "epoll", "kqueue"}))
      ->capture_default_str();

  CLI11_PARSE(app, argc, argv);

  quill::Backend::start();
  auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console");
  auto* logger = quill::Frontend::create_or_get_logger("root", std::move(sink));

  try {
    auto loop_backend = [&] {
      if (io_backend == "io_uring") {
        return vkp::io::make_backend(vkp::io::backend_kind::io_uring);
      }
      if (io_backend == "epoll") {
        return vkp::io::make_backend(vkp::io::backend_kind::epoll);
      }
      if (io_backend == "kqueue") {
        return vkp::io::make_backend(vkp::io::backend_kind::kqueue);
      }
      return vkp::io::make_backend();
    }();
    vkp::io::event_loop loop{std::move(loop_backend)};

    vkp::proxy::config cfg;
    std::tie(cfg.listen_host, cfg.listen_port) = parse_endpoint(listen_ep);
    std::tie(cfg.backend_host, cfg.backend_port) = parse_endpoint(backend_ep);

    vkp::proxy::server server{loop, cfg};

    // Signal plumbing before start(): SIGTERM/SIGINT drain, repeat forces.
    (void)std::signal(SIGPIPE, SIG_IGN);
    int sv[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
      throw std::system_error(errno, std::generic_category(), "socketpair");
    }
    g_signal_fd = sv[1];
    (void)::fcntl(sv[0], F_SETFL, ::fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
    (void)::fcntl(sv[1], F_SETFL, ::fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);
    (void)std::signal(SIGTERM, on_signal);
    (void)std::signal(SIGINT, on_signal);

    server.start();
    vkp::io::spawn(signal_watcher(loop, server, sv[0], logger));

    LOG_INFO(logger, "valkey-proxy {} listening on {}:{} -> backend {}:{} (io: {})", vkp::kVersion,
             cfg.listen_host, server.port(), cfg.backend_host, cfg.backend_port,
             loop.backend_name());

    loop.run();

    ::close(sv[0]);
    ::close(sv[1]);
    LOG_INFO(logger, "shutdown complete");
  } catch (const std::exception& e) {
    LOG_ERROR(logger, "fatal: {}", e.what());
    quill::Backend::stop();
    return EXIT_FAILURE;
  }

  quill::Backend::stop();
  return EXIT_SUCCESS;
}
