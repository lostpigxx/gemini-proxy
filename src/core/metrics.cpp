#include "core/metrics.hpp"

#include <iterator>
#include <utility>

#include <fmt/format.h>

#include "core/version.hpp"

namespace vkp::metrics {

namespace {

constexpr std::array<std::string_view, kCmdClassCount> kCmdClassNames = {
    "read", "write", "admin", "connection", "other",
};
constexpr std::array<std::string_view, kOutcomeCount> kOutcomeNames = {
    "ok",
    "error",
    "timeout",
    "unavailable",
};
constexpr std::array<std::string_view, kErrorKindCount> kErrorKindNames = {
    "crossslot",           "unsupported", "no_topology",  "too_many_redirects",
    "backend_unavailable", "protocol",    "outbuf_limit",
};
constexpr std::array<std::string_view, kRedirectKindCount> kRedirectKindNames = {"moved", "ask"};
constexpr std::array<std::string_view, kConnStateCount> kConnStateNames = {
    "connected",
    "connecting",
    "down",
};
constexpr std::array<std::string_view, kByteDirCount> kByteDirNames = {"in", "out"};
constexpr std::array<std::string_view, kRefreshResultCount> kRefreshResultNames = {"ok", "fail"};

using sink = std::back_insert_iterator<std::string>;

void header(std::string& out, std::string_view name, std::string_view type, std::string_view help) {
  fmt::format_to(sink{out}, "# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
}

// Exact fixed-point seconds from a nanosecond count. Going through a double
// would round, and a `_sum` that disagrees with the observations it summarises
// is the kind of thing nobody notices until a dashboard is wrong.
void seconds(std::string& out, std::uint64_t ns) {
  fmt::format_to(sink{out}, "{}.{:09}", ns / 1'000'000'000ULL, ns % 1'000'000'000ULL);
}

// One labelled counter family, summed across workers.
template <std::size_t N, typename Pick>
void render_labelled(std::string& out, const registry& reg, std::string_view name,
                     std::string_view type, std::string_view help, std::string_view label,
                     const std::array<std::string_view, N>& values, Pick pick) {
  header(out, name, type, help);
  for (std::size_t i = 0; i < N; ++i) {
    std::uint64_t total = 0;
    for (std::size_t w = 0; w < reg.workers(); ++w) {
      total += pick(reg.worker(w), i);
    }
    fmt::format_to(sink{out}, "{}{{{}=\"{}\"}} {}\n", name, label, values[i], total);
  }
}

void render_histogram(std::string& out, const registry& reg, std::string_view name,
                      std::string_view help, const histogram& (*pick)(const worker_stats&)) {
  header(out, name, "histogram", help);
  std::uint64_t cumulative = 0;
  for (std::size_t b = 0; b < histogram::kBucketCount; ++b) {
    for (std::size_t w = 0; w < reg.workers(); ++w) {
      cumulative += pick(reg.worker(w)).bucket(b);
    }
    fmt::format_to(sink{out}, "{}_bucket{{le=\"{}\"}} {}\n", name, histogram::kBoundLabels[b],
                   cumulative);
  }
  std::uint64_t sum_ns = 0;
  std::uint64_t count = 0;
  for (std::size_t w = 0; w < reg.workers(); ++w) {
    sum_ns += pick(reg.worker(w)).sum_ns();
    count += pick(reg.worker(w)).count();
  }
  fmt::format_to(sink{out}, "{}_sum ", name);
  seconds(out, sum_ns);
  fmt::format_to(sink{out}, "\n{}_count {}\n", name, count);
}

}  // namespace

void worker_stats::publish_topology(std::string text, std::chrono::steady_clock::time_point at) {
  const std::lock_guard<std::mutex> lock(topo_mu_);
  topo_ = std::move(text);
  topo_at_ = at;
}

std::pair<std::string, std::chrono::steady_clock::duration> worker_stats::topology_snapshot(
    std::chrono::steady_clock::time_point now) const {
  const std::lock_guard<std::mutex> lock(topo_mu_);
  if (topo_.empty()) {
    return {std::string{}, std::chrono::steady_clock::duration::zero()};
  }
  return {topo_, now - topo_at_};
}

registry::registry(std::size_t workers) {
  blocks_.reserve(workers);
  for (std::size_t i = 0; i < workers; ++i) {
    blocks_.push_back(std::make_unique<worker_stats>());
  }
}

void registry::render_prometheus(std::string& out) const {
  render_labelled(out, *this, "vkp_requests_total", "counter",
                  "Client requests accepted, by command class", "class", kCmdClassNames,
                  [](const worker_stats& w, std::size_t i) { return w.requests[i].read(); });
  render_labelled(out, *this, "vkp_responses_total", "counter",
                  "Replies handed back to clients, by outcome", "outcome", kOutcomeNames,
                  [](const worker_stats& w, std::size_t i) { return w.responses[i].read(); });
  render_labelled(out, *this, "vkp_errors_total", "counter",
                  "Proxy-generated error replies, by cause", "kind", kErrorKindNames,
                  [](const worker_stats& w, std::size_t i) { return w.errors[i].read(); });
  render_labelled(out, *this, "vkp_redirects_total", "counter",
                  "Cluster redirects followed, by kind", "kind", kRedirectKindNames,
                  [](const worker_stats& w, std::size_t i) { return w.redirects[i].read(); });

  render_histogram(out, *this, "vkp_request_duration_seconds",
                   "End-to-end time from reading a request frame to filling its reply slot",
                   [](const worker_stats& w) -> const histogram& { return w.request_duration; });
  render_histogram(out, *this, "vkp_backend_duration_seconds",
                   "Time from enqueueing a request on a backend to pairing its response",
                   [](const worker_stats& w) -> const histogram& { return w.backend_duration; });

  std::uint64_t conns = 0;
  std::uint64_t conns_total = 0;
  std::uint64_t inflight = 0;
  std::uint64_t nodes = 0;
  std::uint64_t slots = 0;
  for (std::size_t w = 0; w < workers(); ++w) {
    const worker_stats& s = worker(w);
    conns += s.client_connections.read();
    conns_total += s.client_connections_total.read();
    inflight += s.backend_inflight.read();
    // Not summed: every worker holds its own copy of the same topology, so the
    // fleet-wide truth is one worker's view. Disagreement shows up in
    // /topology, which prints every worker's summary side by side.
    nodes = std::max(nodes, s.topology_nodes.read());
    slots = std::max(slots, s.topology_slots_assigned.read());
  }

  header(out, "vkp_client_connections", "gauge", "Client connections currently open");
  fmt::format_to(sink{out}, "vkp_client_connections {}\n", conns);
  header(out, "vkp_client_connections_total", "counter", "Client connections accepted since start");
  fmt::format_to(sink{out}, "vkp_client_connections_total {}\n", conns_total);

  render_labelled(out, *this, "vkp_client_bytes_total", "counter",
                  "Bytes read from and written to clients", "dir", kByteDirNames,
                  [](const worker_stats& w, std::size_t i) { return w.client_bytes[i].read(); });
  render_labelled(
      out, *this, "vkp_backend_connections", "gauge", "Pooled backend connections, by state",
      "state", kConnStateNames,
      [](const worker_stats& w, std::size_t i) { return w.backend_connections[i].read(); });

  header(out, "vkp_backend_inflight", "gauge", "Requests sent to backends awaiting a response");
  fmt::format_to(sink{out}, "vkp_backend_inflight {}\n", inflight);

  render_labelled(
      out, *this, "vkp_topology_refresh_total", "counter",
      "CLUSTER SHARDS refresh attempts, by result", "result", kRefreshResultNames,
      [](const worker_stats& w, std::size_t i) { return w.topology_refresh[i].read(); });
  header(out, "vkp_topology_nodes", "gauge", "Master nodes in the current topology");
  fmt::format_to(sink{out}, "vkp_topology_nodes {}\n", nodes);
  header(out, "vkp_topology_slots_assigned", "gauge", "Hash slots with a known owner (of 16384)");
  fmt::format_to(sink{out}, "vkp_topology_slots_assigned {}\n", slots);

  // Per-worker series, deliberately only two of them. Thread-per-core with
  // SO_REUSEPORT degrades silently when the kernel stops spreading connections
  // evenly, and these are the two numbers that make it visible. Everything
  // else stays aggregated so the series count does not scale with worker count.
  header(out, "vkp_worker_requests_total", "counter", "Requests handled, per worker");
  for (std::size_t w = 0; w < workers(); ++w) {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < kCmdClassCount; ++i) {
      total += worker(w).requests[i].read();
    }
    fmt::format_to(sink{out}, "vkp_worker_requests_total{{worker=\"{}\"}} {}\n", w, total);
  }
  header(out, "vkp_worker_client_connections", "gauge", "Client connections open, per worker");
  for (std::size_t w = 0; w < workers(); ++w) {
    fmt::format_to(sink{out}, "vkp_worker_client_connections{{worker=\"{}\"}} {}\n", w,
                   worker(w).client_connections.read());
  }

  header(out, "vkp_uptime_seconds", "gauge", "Seconds since the proxy started");
  fmt::format_to(sink{out}, "vkp_uptime_seconds ");
  seconds(out, static_cast<std::uint64_t>(
                   std::chrono::duration_cast<std::chrono::nanoseconds>(uptime()).count()));
  out.push_back('\n');

  header(out, "vkp_build_info", "gauge", "Build metadata; the value is always 1");
  fmt::format_to(sink{out}, "vkp_build_info{{version=\"{}\"}} 1\n", kVersion);
}

}  // namespace vkp::metrics
