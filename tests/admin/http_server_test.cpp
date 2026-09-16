#include "admin/http_server.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "core/metrics.hpp"

using namespace std::chrono_literals;
namespace admin = vkp::admin;
namespace metrics = vkp::metrics;

namespace {

using state = admin::parsed_request::state;

// Plain blocking client: the server runs on its own thread, so the test
// thread can simply speak HTTP at it.
std::string http_get(std::uint16_t port, std::string_view request) {
  // The server answers oversized requests and closes while the client may
  // still be writing; without this the test dies of SIGPIPE instead.
  (void)std::signal(SIGPIPE, SIG_IGN);
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  const timeval tv{.tv_sec = 3, .tv_usec = 0};
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  std::string out;
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
    (void)::send(fd, request.data(), request.size(), 0);
    char buf[4096];
    for (;;) {  // Connection: close, so read to EOF
      const auto n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        break;
      }
      out.append(buf, static_cast<std::size_t>(n));
    }
  }
  ::close(fd);
  return out;
}

std::string get(std::uint16_t port, std::string_view path) {
  return http_get(port, std::string{"GET "} + std::string{path} + " HTTP/1.1\r\nHost: x\r\n\r\n");
}

// Body after the blank line.
std::string_view body_of(std::string_view response) {
  const std::size_t at = response.find("\r\n\r\n");
  return at == std::string_view::npos ? std::string_view{} : response.substr(at + 4);
}

}  // namespace

TEST_CASE("the request-line parser separates incomplete, bad and unsupported", "[admin]") {
  CHECK(admin::parse_request("").st == state::need_more);
  CHECK(admin::parse_request("GET /metrics HTTP/1.1\r\n").st == state::need_more);
  CHECK(admin::parse_request("GET /metrics HTTP/1.1\r\nHost: x\r\n").st == state::need_more);

  const auto ok = admin::parse_request("GET /metrics HTTP/1.1\r\n\r\n");
  CHECK(ok.st == state::ok);
  CHECK(ok.path == "/metrics");

  // Query and fragment are stripped; Prometheus appends neither, but a human
  // with a browser will.
  CHECK(admin::parse_request("GET /metrics?debug=1 HTTP/1.1\r\n\r\n").path == "/metrics");
  CHECK(admin::parse_request("GET /metrics#frag HTTP/1.1\r\n\r\n").path == "/metrics");
  CHECK(admin::parse_request("GET / HTTP/1.0\r\n\r\n").path == "/");

  // Well formed but not GET: the caller owes a 405, not a 400.
  CHECK(admin::parse_request("POST /metrics HTTP/1.1\r\n\r\n").st == state::not_get);
  CHECK(admin::parse_request("DELETE /x HTTP/1.1\r\n\r\n").st == state::not_get);

  CHECK(admin::parse_request("\r\n\r\n").st == state::bad_request);
  CHECK(admin::parse_request("GET\r\n\r\n").st == state::bad_request);
  CHECK(admin::parse_request("GET /metrics\r\n\r\n").st == state::bad_request);
  CHECK(admin::parse_request("GET /metrics HTTP/2\r\n\r\n").st == state::bad_request);
  CHECK(admin::parse_request("GET metrics HTTP/1.1\r\n\r\n").st == state::bad_request);
  CHECK(admin::parse_request("GET http://x/y HTTP/1.1\r\n\r\n").st == state::bad_request);
  // Binary garbage, NULs and all: bad_request, not a crash.
  CHECK(admin::parse_request(std::string_view{"\0\0\0 \0 \0\r\n\r\n", 11}).st ==
        state::bad_request);
}

TEST_CASE("the admin server answers its three routes", "[admin]") {
  metrics::registry reg{2};
  reg.worker(0).bump_request(metrics::cmd_class::read);
  reg.worker(0).publish_topology("0-16383 127.0.0.1:6379\n", std::chrono::steady_clock::now());

  admin::http_config cfg;
  cfg.listen_port = 0;  // ephemeral
  admin::http_server srv{cfg, reg};
  REQUIRE(srv.port() != 0);
  srv.start();

  SECTION("/metrics renders the exposition format") {
    const std::string r = get(srv.port(), "/metrics");
    CHECK(r.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(r.find("Content-Type: text/plain; version=0.0.4") != std::string::npos);
    CHECK(body_of(r).find("vkp_requests_total{class=\"read\"} 1\n") != std::string_view::npos);
    CHECK(body_of(r).find("vkp_build_info{") != std::string_view::npos);
  }

  SECTION("/health is 200 until the drain starts, then 503") {
    const std::string before = get(srv.port(), "/health");
    CHECK(before.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(body_of(before).find("\"status\":\"ok\"") != std::string_view::npos);
    CHECK(body_of(before).find("\"workers\":2") != std::string_view::npos);

    srv.begin_drain();
    const std::string after = get(srv.port(), "/health");
    CHECK(after.starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
    CHECK(body_of(after).find("\"status\":\"draining\"") != std::string_view::npos);
  }

  SECTION("/topology shows worker 0's map and every worker's summary") {
    // Named, not a temporary: body_of() returns a view into it.
    const std::string raw = get(srv.port(), "/topology");
    const std::string_view b = body_of(raw);
    CHECK(b.find("0-16383 127.0.0.1:6379\n") != std::string_view::npos);
    // Worker 1 never published: said so rather than shown as a stale age.
    CHECK(b.find("1 0 0 never\n") != std::string_view::npos);
  }

  SECTION("unknown paths 404 and non-GET methods 405") {
    CHECK(get(srv.port(), "/nope").starts_with("HTTP/1.1 404 Not Found\r\n"));
    CHECK(http_get(srv.port(), "POST /metrics HTTP/1.1\r\n\r\n")
              .starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    CHECK(http_get(srv.port(), "nonsense\r\n\r\n").starts_with("HTTP/1.1 400 Bad Request\r\n"));
  }

  SECTION("a request that never ends is cut off at the size cap") {
    // Headers that never terminate: the server must answer rather than buffer
    // until the process dies.
    std::string request = "GET /metrics HTTP/1.1\r\n";
    while (request.size() < 9U << 10) {
      request += "X-Pad: 0123456789012345678901234567890123456789\r\n";
    }
    CHECK(http_get(srv.port(), request)
              .starts_with("HTTP/1.1 431 Request Header Fields Too Large\r\n"));
  }

  srv.stop();
}
