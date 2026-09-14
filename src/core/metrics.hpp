// Prometheus metrics: per-worker counter blocks, aggregated at scrape time.
// Design: docs/design/m5-observability-config.md §1–§4.
//
// Threading contract, and the whole reason this is only ~two atomics deep:
// every counter has exactly ONE writer — the worker that owns its block. So a
// bump is a relaxed load plus a relaxed store (a plain LDR/STR on arm64, no
// LDADD, no barrier), while the admin thread reads with a relaxed load. The
// atomics buy defined semantics and a quiet TSan, not synchronisation.
//
// A scrape therefore is not an atomic snapshot across workers. Prometheus
// consumes these through rate(), so a few microseconds of skew between two
// workers' counters is invisible.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace vkp::metrics {

// ---------------------------------------------------------------- label sets
//
// Each of these indexes a fixed array of counters. Keeping them dense enums
// rather than strings is what lets a bump be an array offset on the hot path.

enum class cmd_class : std::uint8_t { read, write, admin, connection, other };
inline constexpr std::size_t kCmdClassCount = 5;

enum class outcome : std::uint8_t { ok, error, timeout, unavailable };
inline constexpr std::size_t kOutcomeCount = 4;

enum class error_kind : std::uint8_t {
  crossslot,
  unsupported,
  no_topology,
  too_many_redirects,
  backend_unavailable,
  protocol,
  outbuf_limit,
};
inline constexpr std::size_t kErrorKindCount = 7;

enum class redirect_kind : std::uint8_t { moved, ask };
inline constexpr std::size_t kRedirectKindCount = 2;

enum class conn_state : std::uint8_t { connected, connecting, down };
inline constexpr std::size_t kConnStateCount = 3;

enum class byte_dir : std::uint8_t { in, out };
inline constexpr std::size_t kByteDirCount = 2;

enum class refresh_result : std::uint8_t { ok, fail };
inline constexpr std::size_t kRefreshResultCount = 2;

// ------------------------------------------------------------------ counters

// Monotonic, single-writer. See the threading contract above.
class counter {
 public:
  void bump(std::uint64_t n = 1) noexcept {
    v_.store(v_.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t read() const noexcept { return v_.load(std::memory_order_relaxed); }

 private:
  std::atomic<std::uint64_t> v_{0};
};

// Same storage, different semantics: may go down. Kept as a distinct type so
// that a `# TYPE ... gauge` line and a `set()` call cannot drift apart.
class gauge {
 public:
  void set(std::uint64_t n) noexcept { v_.store(n, std::memory_order_relaxed); }
  void inc(std::uint64_t n = 1) noexcept {
    v_.store(v_.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
  }
  void dec(std::uint64_t n = 1) noexcept {
    const std::uint64_t cur = v_.load(std::memory_order_relaxed);
    v_.store(cur >= n ? cur - n : 0, std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t read() const noexcept { return v_.load(std::memory_order_relaxed); }

 private:
  std::atomic<std::uint64_t> v_{0};
};

// Fixed-bucket latency histogram, 100 µs (loopback floor) to 10 s (the request
// timeout ceiling). Buckets are stored non-cumulatively and accumulated at
// render time, which is what Prometheus's `le` wants.
class histogram {
 public:
  static constexpr std::size_t kFiniteBuckets = 16;
  static constexpr std::size_t kBucketCount = kFiniteBuckets + 1;  // + the +Inf bucket

  // Upper bounds in nanoseconds, ascending.
  static constexpr std::array<std::uint64_t, kFiniteBuckets> kBoundsNs = {
      100'000ULL,       250'000ULL,       500'000ULL,       1'000'000ULL,
      2'500'000ULL,     5'000'000ULL,     10'000'000ULL,    25'000'000ULL,
      50'000'000ULL,    100'000'000ULL,   250'000'000ULL,   500'000'000ULL,
      1'000'000'000ULL, 2'500'000'000ULL, 5'000'000'000ULL, 10'000'000'000ULL,
  };
  // The same bounds as Prometheus wants to see them printed. Kept as literals
  // rather than formatted from kBoundsNs so no float rounding can ever make
  // the `le` label disagree with the bucket it names.
  static constexpr std::array<std::string_view, kBucketCount> kBoundLabels = {
      "0.0001", "0.00025", "0.0005", "0.001", "0.0025", "0.005", "0.01", "0.025", "0.05",
      "0.1",    "0.25",    "0.5",    "1",     "2.5",    "5",     "10",   "+Inf",
  };

  void observe(std::chrono::nanoseconds d) noexcept {
    // A negative duration is possible by construction: the start stamp comes
    // from event_loop::now() and a caller may compare it against a fresher
    // real-clock reading. Floor it rather than wrapping into the +Inf bucket.
    const std::uint64_t ns = d.count() > 0 ? static_cast<std::uint64_t>(d.count()) : 0;
    // Linear from the bottom: normal traffic lands in the first two or three
    // comparisons, which beats a branchy binary search over 16 entries.
    std::size_t i = 0;
    while (i < kFiniteBuckets && ns > kBoundsNs[i]) {
      ++i;
    }
    buckets_[i].bump();
    count_.bump();
    sum_ns_.bump(ns);
  }

  [[nodiscard]] std::uint64_t bucket(std::size_t i) const noexcept { return buckets_[i].read(); }
  [[nodiscard]] std::uint64_t count() const noexcept { return count_.read(); }
  [[nodiscard]] std::uint64_t sum_ns() const noexcept { return sum_ns_.read(); }

 private:
  std::array<counter, kBucketCount> buckets_;
  counter count_;
  // Nanoseconds in a uint64 overflows after ~584 years of accumulated
  // latency; converted to seconds only at render time.
  counter sum_ns_;
};

// ---------------------------------------------------------------- per worker

// One block per worker, cache-line aligned so that two workers bumping their
// own counters never share a line. Within a block there is a single writer, so
// no padding between the individual counters is needed — and keeping the block
// compact means the hot path touches fewer lines.
class alignas(64) worker_stats {
 public:
  std::array<counter, kCmdClassCount> requests;
  std::array<counter, kOutcomeCount> responses;
  std::array<counter, kErrorKindCount> errors;
  std::array<counter, kRedirectKindCount> redirects;

  histogram request_duration;  // proxy side: frame read -> reply slot filled
  histogram backend_duration;  // backend side: enqueue -> FIFO pairing

  gauge client_connections;
  counter client_connections_total;
  std::array<counter, kByteDirCount> client_bytes;

  std::array<gauge, kConnStateCount> backend_connections;
  gauge backend_inflight;

  std::array<counter, kRefreshResultCount> topology_refresh;
  gauge topology_nodes;
  gauge topology_slots_assigned;

  void bump_request(cmd_class c) noexcept { requests[static_cast<std::size_t>(c)].bump(); }
  void bump_response(outcome o) noexcept { responses[static_cast<std::size_t>(o)].bump(); }
  void bump_error(error_kind k) noexcept { errors[static_cast<std::size_t>(k)].bump(); }
  void bump_redirect(redirect_kind k) noexcept { redirects[static_cast<std::size_t>(k)].bump(); }
  void bump_bytes(byte_dir d, std::uint64_t n) noexcept {
    client_bytes[static_cast<std::size_t>(d)].bump(n);
  }
  void bump_refresh(refresh_result r) noexcept {
    topology_refresh[static_cast<std::size_t>(r)].bump();
  }

  // The /topology snapshot. Each worker refreshes its own topology (m4 §4.2),
  // so the admin thread cannot read the live structure; the worker pre-renders
  // it instead. A plain mutex is enough — the worker writes at the refresh
  // interval (0.2 Hz) and the admin thread reads per scrape (0.067 Hz), and the
  // data plane never touches either. Design §5.
  void publish_topology(std::string text, std::chrono::steady_clock::time_point at);
  // Returns the snapshot and how old it is. Empty text = never published.
  [[nodiscard]] std::pair<std::string, std::chrono::steady_clock::duration> topology_snapshot(
      std::chrono::steady_clock::time_point now) const;

 private:
  mutable std::mutex topo_mu_;
  std::string topo_;
  std::chrono::steady_clock::time_point topo_at_{};
};

// ------------------------------------------------------------------ registry

// Owns every worker's block and renders the aggregate. Constructed once by
// worker_pool before the threads start, so the blocks never move.
class registry {
 public:
  explicit registry(std::size_t workers);

  [[nodiscard]] std::size_t workers() const noexcept { return blocks_.size(); }
  [[nodiscard]] worker_stats& worker(std::size_t i) noexcept { return *blocks_[i]; }
  [[nodiscard]] const worker_stats& worker(std::size_t i) const noexcept { return *blocks_[i]; }

  // Prometheus text exposition format 0.0.4. Appends; does not clear `out`.
  void render_prometheus(std::string& out) const;

  [[nodiscard]] std::chrono::steady_clock::duration uptime() const noexcept {
    return std::chrono::steady_clock::now() - start_;
  }

 private:
  // unique_ptr rather than a vector of values: worker_stats holds a mutex and
  // must never be relocated once a worker has a reference to it.
  std::vector<std::unique_ptr<worker_stats>> blocks_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

}  // namespace vkp::metrics
