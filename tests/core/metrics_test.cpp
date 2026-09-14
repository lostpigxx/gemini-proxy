#include "core/metrics.hpp"

#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;
using vkp::metrics::byte_dir;
using vkp::metrics::cmd_class;
using vkp::metrics::counter;
using vkp::metrics::error_kind;
using vkp::metrics::gauge;
using vkp::metrics::histogram;
using vkp::metrics::outcome;
using vkp::metrics::redirect_kind;
using vkp::metrics::registry;

namespace {

// Pulls one line out of a rendered exposition body. Returns "" when absent,
// which makes a missing series fail as loudly as a wrong value.
std::string line_for(const std::string& body, std::string_view prefix) {
  std::size_t pos = 0;
  while (pos < body.size()) {
    const std::size_t end = body.find('\n', pos);
    const std::string_view l{body.data() + pos,
                             (end == std::string::npos ? body.size() : end) - pos};
    if (l.starts_with(prefix) &&
        (l.size() == prefix.size() || l[prefix.size()] == ' ' || l[prefix.size()] == '{')) {
      return std::string{l};
    }
    if (end == std::string::npos) {
      break;
    }
    pos = end + 1;
  }
  return {};
}

}  // namespace

TEST_CASE("counter and gauge are plain single-writer accumulators") {
  counter c;
  REQUIRE(c.read() == 0);
  c.bump();
  c.bump(41);
  REQUIRE(c.read() == 42);

  gauge g;
  g.set(10);
  g.inc();
  g.dec(3);
  REQUIRE(g.read() == 8);
  // Connection accounting must not wrap when a decrement outruns an increment
  // during teardown; 0 is the only sane floor for a count of live things.
  g.dec(100);
  REQUIRE(g.read() == 0);
}

TEST_CASE("histogram buckets are le, not lt") {
  histogram h;
  // Exactly on a boundary belongs *in* that bucket: Prometheus `le` is
  // inclusive. Off-by-one here is invisible until a p99 is quietly wrong.
  h.observe(100us);  // == the first bound
  REQUIRE(h.bucket(0) == 1);

  h.observe(100us + 1ns);  // just over it
  REQUIRE(h.bucket(0) == 1);
  REQUIRE(h.bucket(1) == 1);

  h.observe(10s);  // == the last finite bound
  REQUIRE(h.bucket(histogram::kFiniteBuckets - 1) == 1);
  REQUIRE(h.bucket(histogram::kBucketCount - 1) == 0);

  h.observe(10s + 1ns);  // overflows into +Inf
  REQUIRE(h.bucket(histogram::kBucketCount - 1) == 1);

  REQUIRE(h.count() == 4);
  REQUIRE(h.sum_ns() == (100'000ULL + 100'001ULL + 10'000'000'000ULL + 10'000'000'001ULL));
}

TEST_CASE("histogram floors a negative duration instead of wrapping") {
  histogram h;
  // event_loop::now() is cached, so a caller comparing it against a fresher
  // real-clock stamp can produce a negative span. Unsigned-wrapping that into
  // the +Inf bucket would fake a 500-year request.
  h.observe(-5ms);
  REQUIRE(h.bucket(0) == 1);
  REQUIRE(h.bucket(histogram::kBucketCount - 1) == 0);
  REQUIRE(h.sum_ns() == 0);
}

TEST_CASE("registry aggregates across workers") {
  registry reg{3};
  REQUIRE(reg.workers() == 3);

  reg.worker(0).bump_request(cmd_class::read);
  reg.worker(1).bump_request(cmd_class::read);
  reg.worker(1).bump_request(cmd_class::write);
  reg.worker(2).bump_error(error_kind::crossslot);
  reg.worker(0).bump_redirect(redirect_kind::moved);
  reg.worker(0).bump_response(outcome::ok);
  reg.worker(0).bump_bytes(byte_dir::in, 100);
  reg.worker(1).bump_bytes(byte_dir::in, 23);
  reg.worker(0).client_connections.set(2);
  reg.worker(1).client_connections.set(5);

  std::string body;
  reg.render_prometheus(body);

  REQUIRE(line_for(body, "vkp_requests_total{class=\"read\"}") ==
          "vkp_requests_total{class=\"read\"} 2");
  REQUIRE(line_for(body, "vkp_requests_total{class=\"write\"}") ==
          "vkp_requests_total{class=\"write\"} 1");
  REQUIRE(line_for(body, "vkp_requests_total{class=\"admin\"}") ==
          "vkp_requests_total{class=\"admin\"} 0");
  REQUIRE(line_for(body, "vkp_errors_total{kind=\"crossslot\"}") ==
          "vkp_errors_total{kind=\"crossslot\"} 1");
  REQUIRE(line_for(body, "vkp_redirects_total{kind=\"moved\"}") ==
          "vkp_redirects_total{kind=\"moved\"} 1");
  REQUIRE(line_for(body, "vkp_client_bytes_total{dir=\"in\"}") ==
          "vkp_client_bytes_total{dir=\"in\"} 123");
  REQUIRE(line_for(body, "vkp_client_connections") == "vkp_client_connections 7");

  // The per-worker series exist for every worker, including the idle one —
  // an absent series and a zero series read very differently on a dashboard.
  REQUIRE(line_for(body, "vkp_worker_requests_total{worker=\"0\"}") ==
          "vkp_worker_requests_total{worker=\"0\"} 1");
  REQUIRE(line_for(body, "vkp_worker_requests_total{worker=\"1\"}") ==
          "vkp_worker_requests_total{worker=\"1\"} 2");
  REQUIRE(line_for(body, "vkp_worker_requests_total{worker=\"2\"}") ==
          "vkp_worker_requests_total{worker=\"2\"} 0");
}

TEST_CASE("rendered histogram buckets are cumulative and the sum is exact") {
  registry reg{2};
  reg.worker(0).request_duration.observe(50us);   // bucket 0
  reg.worker(1).request_duration.observe(300us);  // bucket 2 (<= 500us)
  reg.worker(1).request_duration.observe(3s);     // bucket 13 (<= 2.5s? no -> <= 5s = 14)

  std::string body;
  reg.render_prometheus(body);

  REQUIRE(line_for(body, "vkp_request_duration_seconds_bucket{le=\"0.0001\"}") ==
          "vkp_request_duration_seconds_bucket{le=\"0.0001\"} 1");
  // Cumulative: the 300 µs observation is counted here on top of the 50 µs one.
  REQUIRE(line_for(body, "vkp_request_duration_seconds_bucket{le=\"0.0005\"}") ==
          "vkp_request_duration_seconds_bucket{le=\"0.0005\"} 2");
  REQUIRE(line_for(body, "vkp_request_duration_seconds_bucket{le=\"2.5\"}") ==
          "vkp_request_duration_seconds_bucket{le=\"2.5\"} 2");
  REQUIRE(line_for(body, "vkp_request_duration_seconds_bucket{le=\"5\"}") ==
          "vkp_request_duration_seconds_bucket{le=\"5\"} 3");
  // +Inf must equal _count, or the exposition is malformed.
  REQUIRE(line_for(body, "vkp_request_duration_seconds_bucket{le=\"+Inf\"}") ==
          "vkp_request_duration_seconds_bucket{le=\"+Inf\"} 3");
  REQUIRE(line_for(body, "vkp_request_duration_seconds_count") ==
          "vkp_request_duration_seconds_count 3");
  // 50us + 300us + 3s, in exact fixed point rather than via a double.
  REQUIRE(line_for(body, "vkp_request_duration_seconds_sum") ==
          "vkp_request_duration_seconds_sum 3.000350000");
}

TEST_CASE("every family carries a HELP and a TYPE line") {
  registry reg{1};
  std::string body;
  reg.render_prometheus(body);

  // promtool rejects a sample whose family was never declared, so this is the
  // cheap local stand-in for the container-side `promtool check metrics` run.
  for (std::string_view name :
       {"vkp_requests_total", "vkp_responses_total", "vkp_errors_total", "vkp_redirects_total",
        "vkp_request_duration_seconds", "vkp_backend_duration_seconds", "vkp_client_connections",
        "vkp_client_connections_total", "vkp_client_bytes_total", "vkp_backend_connections",
        "vkp_backend_inflight", "vkp_topology_refresh_total", "vkp_topology_nodes",
        "vkp_topology_slots_assigned", "vkp_worker_requests_total", "vkp_worker_client_connections",
        "vkp_uptime_seconds", "vkp_build_info"}) {
    INFO("family " << name);
    REQUIRE(body.find(std::string{"# HELP "} + std::string{name} + " ") != std::string::npos);
    REQUIRE(body.find(std::string{"# TYPE "} + std::string{name} + " ") != std::string::npos);
  }
}

TEST_CASE("topology snapshot crosses threads under its own lock") {
  registry reg{1};
  const auto t0 = std::chrono::steady_clock::now();

  auto [empty, age] = reg.worker(0).topology_snapshot(t0);
  REQUIRE(empty.empty());

  // The real writer is a worker thread and the real reader is the admin
  // thread; run it that way so TSan has something to look at.
  std::thread writer{[&] { reg.worker(0).publish_topology("slots 0-16383 -> a:1\n", t0); }};
  writer.join();

  auto [text, age2] = reg.worker(0).topology_snapshot(t0 + 3s);
  REQUIRE(text == "slots 0-16383 -> a:1\n");
  REQUIRE(age2 == 3s);
}
