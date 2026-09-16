#include "config/settings.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <toml++/toml.hpp>

namespace vkp::config {

namespace {

constexpr std::string_view kLogLevels[] = {"tracel3", "tracel2", "tracel1",  "debug", "info",
                                           "warning", "error",   "critical", "none"};
constexpr std::string_view kIoBackends[] = {"auto", "io_uring", "epoll", "kqueue"};

// The signal fan-out array in main() is fixed at 256 entries and the admin
// server takes one of them.
constexpr std::size_t kMaxWorkers = 255;

bool one_of(std::string_view v, std::initializer_list<std::string_view> set) {
  return std::find(set.begin(), set.end(), v) != set.end();
}

template <std::size_t N>
bool one_of(std::string_view v, const std::string_view (&set)[N]) {
  return std::find(std::begin(set), std::end(set), v) != std::end(set);
}

// Every diagnostic from this file carries path:line:col. That is the whole
// reason the file is parsed with toml++ instead of by hand.
struct reader {
  std::string_view file;

  [[noreturn]] void fail(const toml::node& n, const std::string& what) const {
    const toml::source_position p = n.source().begin;
    throw std::runtime_error(fmt::format("{}:{}:{}: {}", file, p.line, p.column, what));
  }

  [[nodiscard]] const toml::table* section(const toml::table& root, std::string_view name) const {
    const toml::node* n = root.get(name);
    if (n == nullptr) {
      return nullptr;
    }
    const toml::table* t = n->as_table();
    if (t == nullptr) {
      fail(*n, fmt::format("[{}] must be a table", name));
    }
    return t;
  }

  // An unknown key is an error, not a warning: a typo that is silently ignored
  // is a config bug that only surfaces in production, under load, as "the
  // setting I changed did nothing".
  void known(const toml::table& t, std::string_view sect,
             std::initializer_list<std::string_view> keys) const {
    for (const auto& [k, v] : t) {
      if (one_of(k.str(), keys)) {
        continue;
      }
      fail(v, sect.empty() ? fmt::format("unknown top-level key '{}'", k.str())
                           : fmt::format("unknown key '{}' in [{}]", k.str(), sect));
    }
  }

  void str(const toml::table& t, std::string_view key, std::string& out) const {
    const toml::node* n = t.get(key);
    if (n == nullptr) {
      return;
    }
    const std::optional<std::string> v = n->value<std::string>();
    if (!v) {
      fail(*n, fmt::format("'{}' must be a string", key));
    }
    out = *v;
  }

  void flag(const toml::table& t, std::string_view key, bool& out) const {
    const toml::node* n = t.get(key);
    if (n == nullptr) {
      return;
    }
    const std::optional<bool> v = n->value<bool>();
    if (!v) {
      fail(*n, fmt::format("'{}' must be true or false", key));
    }
    out = *v;
  }

  template <typename T>
  void num(const toml::table& t, std::string_view key, T& out, std::int64_t lo,
           std::int64_t hi) const {
    const toml::node* n = t.get(key);
    if (n == nullptr) {
      return;
    }
    const std::optional<std::int64_t> v = n->value<std::int64_t>();
    if (!v) {
      fail(*n, fmt::format("'{}' must be an integer", key));
    }
    if (*v < lo || *v > hi) {
      fail(*n, fmt::format("'{}' must be between {} and {}, got {}", key, lo, hi, *v));
    }
    out = static_cast<T>(*v);
  }

  void millis(const toml::table& t, std::string_view key, std::chrono::milliseconds& out,
              std::int64_t lo, std::int64_t hi) const {
    std::int64_t v = out.count();
    num(t, key, v, lo, hi);
    out = std::chrono::milliseconds{v};
  }

  void strings(const toml::table& t, std::string_view key, std::vector<std::string>& out) const {
    const toml::node* n = t.get(key);
    if (n == nullptr) {
      return;
    }
    const toml::array* a = n->as_array();
    if (a == nullptr) {
      fail(*n, fmt::format("'{}' must be an array of strings", key));
    }
    out.clear();
    for (const toml::node& e : *a) {
      const std::optional<std::string> v = e.value<std::string>();
      if (!v) {
        fail(e, fmt::format("'{}' must contain only strings", key));
      }
      out.push_back(*v);
    }
  }

  // Parsed here rather than in validate() so that a malformed endpoint points
  // at the line that holds it.
  void endpoint(const toml::table& t, std::string_view key, std::string& host,
                std::uint16_t& port) const {
    const toml::node* n = t.get(key);
    if (n == nullptr) {
      return;
    }
    const std::optional<std::string> v = n->value<std::string>();
    if (!v) {
      fail(*n, fmt::format("'{}' must be a string", key));
    }
    try {
      std::tie(host, port) = parse_endpoint(*v);
    } catch (const std::exception& e) {
      fail(*n, e.what());
    }
  }
};

}  // namespace

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
  const std::string digits = ep.substr(colon + 1);
  if (digits.empty() ||
      digits.find_first_not_of("0123456789") != std::string::npos) {  // stoi would take " 12x"
    throw std::runtime_error(fmt::format("invalid port in '{}': expected a number", ep));
  }
  const long port = std::stol(digits);
  if (port > 65535) {
    throw std::runtime_error(fmt::format("invalid port in '{}': out of range", ep));
  }
  return {host, static_cast<std::uint16_t>(port)};
}

settings load_toml(const std::string& path) {
  if (!std::ifstream{path}.good()) {
    throw std::runtime_error(fmt::format("cannot open config file '{}'", path));
  }

  toml::table root;
  try {
    root = toml::parse_file(path);
  } catch (const toml::parse_error& e) {
    const toml::source_position p = e.source().begin;
    throw std::runtime_error(fmt::format("{}:{}:{}: {}", path, p.line, p.column, e.description()));
  }

  settings s;
  const reader r{path};
  r.known(root, "",
          {"listen", "backend", "cluster", "worker", "limits", "log", "admin", "shutdown"});

  if (const toml::table* t = r.section(root, "listen")) {
    r.known(*t, "listen", {"endpoint", "backlog"});
    r.endpoint(*t, "endpoint", s.proxy.listen_host, s.proxy.listen_port);
    r.num(*t, "backlog", s.proxy.backlog, 1, 65535);
  }

  if (const toml::table* t = r.section(root, "backend")) {
    r.known(*t, "backend",
            {"endpoint", "conns_per_worker", "connect_timeout_ms", "request_timeout_ms",
             "health_interval_ms", "backoff_base_ms", "backoff_max_ms"});
    r.endpoint(*t, "endpoint", s.proxy.backend_host, s.proxy.backend_port);
    r.num(*t, "conns_per_worker", s.proxy.conns_per_backend, 1, 8);
    r.millis(*t, "connect_timeout_ms", s.proxy.backend.connect_timeout, 1, 600'000);
    r.millis(*t, "request_timeout_ms", s.proxy.backend.request_timeout, 1, 600'000);
    r.millis(*t, "health_interval_ms", s.proxy.backend.health_interval, 1, 600'000);
    r.millis(*t, "backoff_base_ms", s.proxy.backend.backoff_base, 1, 600'000);
    r.millis(*t, "backoff_max_ms", s.proxy.backend.backoff_max, 1, 600'000);
  }

  if (const toml::table* t = r.section(root, "cluster")) {
    r.known(*t, "cluster", {"seeds", "refresh_ms", "max_redirects"});
    r.strings(*t, "seeds", s.proxy.cluster_seeds);
    r.millis(*t, "refresh_ms", s.proxy.cluster_refresh, 100, 3'600'000);
    r.num(*t, "max_redirects", s.proxy.max_redirects, 1, 64);
  }

  if (const toml::table* t = r.section(root, "worker")) {
    r.known(*t, "worker", {"threads", "cpu_affinity", "io_backend"});
    r.num(*t, "threads", s.workers, 0, static_cast<std::int64_t>(kMaxWorkers));
    r.flag(*t, "cpu_affinity", s.cpu_affinity);
    r.str(*t, "io_backend", s.io_backend);
  }

  if (const toml::table* t = r.section(root, "limits")) {
    r.known(*t, "limits", {"max_inflight", "backend_outbuf_bytes", "client_outbuf_bytes"});
    r.num(*t, "max_inflight", s.proxy.backend.max_inflight, 1, 1'000'000);
    r.num(*t, "backend_outbuf_bytes", s.proxy.backend.max_outbuf, 4096, 1LL << 32);
    r.num(*t, "client_outbuf_bytes", s.proxy.client_outbuf_limit, 4096, 1LL << 32);
  }

  if (const toml::table* t = r.section(root, "log")) {
    r.known(*t, "log", {"level", "format", "file"});
    r.str(*t, "level", s.log.level);
    r.str(*t, "format", s.log_format);
    r.str(*t, "file", s.log.file);
  }

  if (const toml::table* t = r.section(root, "admin")) {
    r.known(*t, "admin", {"endpoint"});
    r.str(*t, "endpoint", s.admin_endpoint);
  }

  if (const toml::table* t = r.section(root, "shutdown")) {
    r.known(*t, "shutdown", {"grace_ms"});
    r.millis(*t, "grace_ms", s.proxy.shutdown_grace, 0, 600'000);
  }

  return s;
}

void validate(const settings& s) {
  if (!one_of(s.io_backend, kIoBackends)) {
    throw std::runtime_error(fmt::format("unknown io_backend '{}': expected one of {}",
                                         s.io_backend, fmt::join(kIoBackends, ", ")));
  }
  if (!one_of(s.log_format, {"text", "json"})) {
    throw std::runtime_error(
        fmt::format("unknown log format '{}': expected text or json", s.log_format));
  }
  if (!one_of(s.log.level, kLogLevels)) {
    throw std::runtime_error(fmt::format("unknown log level '{}': expected one of {}", s.log.level,
                                         fmt::join(kLogLevels, ", ")));
  }
  if (s.workers > kMaxWorkers) {
    throw std::runtime_error(
        fmt::format("worker threads {} exceeds the maximum of {}", s.workers, kMaxWorkers));
  }
  if (s.proxy.conns_per_backend < 1 || s.proxy.conns_per_backend > 8) {
    throw std::runtime_error(fmt::format("backend connections per worker must be 1-8, got {}",
                                         s.proxy.conns_per_backend));
  }
  if (s.proxy.max_redirects < 1 || s.proxy.max_redirects > 64) {
    throw std::runtime_error(
        fmt::format("max_redirects must be 1-64, got {}", s.proxy.max_redirects));
  }
  if (s.proxy.backend.backoff_max < s.proxy.backend.backoff_base) {
    throw std::runtime_error(fmt::format("backoff_max_ms ({}) is below backoff_base_ms ({})",
                                         s.proxy.backend.backoff_max.count(),
                                         s.proxy.backend.backoff_base.count()));
  }
  // Seeds reach the network at bootstrap; rejecting them here means a typo is
  // a startup failure rather than a node that looks permanently unreachable.
  for (const std::string& seed : s.proxy.cluster_seeds) {
    (void)parse_endpoint(seed);
  }
  if (!s.admin_endpoint.empty()) {
    (void)parse_endpoint(s.admin_endpoint);
  }
}

std::string to_string(const settings& s) {
  std::string seeds;
  for (const std::string& v : s.proxy.cluster_seeds) {
    if (!seeds.empty()) {
      seeds += ", ";
    }
    seeds += fmt::format("\"{}\"", v);
  }

  std::string out;
  fmt::format_to(std::back_inserter(out),
                 "[listen]\n"
                 "endpoint = \"{}:{}\"\n"
                 "backlog = {}\n\n"
                 "[backend]\n"
                 "endpoint = \"{}:{}\"\n"
                 "conns_per_worker = {}\n"
                 "connect_timeout_ms = {}\n"
                 "request_timeout_ms = {}\n"
                 "health_interval_ms = {}\n"
                 "backoff_base_ms = {}\n"
                 "backoff_max_ms = {}\n\n",
                 s.proxy.listen_host, s.proxy.listen_port, s.proxy.backlog, s.proxy.backend_host,
                 s.proxy.backend_port, s.proxy.conns_per_backend,
                 s.proxy.backend.connect_timeout.count(), s.proxy.backend.request_timeout.count(),
                 s.proxy.backend.health_interval.count(), s.proxy.backend.backoff_base.count(),
                 s.proxy.backend.backoff_max.count());
  fmt::format_to(std::back_inserter(out),
                 "[cluster]\n"
                 "seeds = [{}]\n"
                 "refresh_ms = {}\n"
                 "max_redirects = {}\n\n"
                 "[worker]\n"
                 "threads = {}\n"
                 "cpu_affinity = {}\n"
                 "io_backend = \"{}\"\n\n",
                 seeds, s.proxy.cluster_refresh.count(), s.proxy.max_redirects, s.workers,
                 s.cpu_affinity, s.io_backend);
  fmt::format_to(std::back_inserter(out),
                 "[limits]\n"
                 "max_inflight = {}\n"
                 "backend_outbuf_bytes = {}\n"
                 "client_outbuf_bytes = {}\n\n"
                 "[log]\n"
                 "level = \"{}\"\n"
                 "format = \"{}\"\n"
                 "file = \"{}\"\n\n"
                 "[admin]\n"
                 "endpoint = \"{}\"\n\n"
                 "[shutdown]\n"
                 "grace_ms = {}\n",
                 s.proxy.backend.max_inflight, s.proxy.backend.max_outbuf,
                 s.proxy.client_outbuf_limit, s.log.level, s.log_format, s.log.file,
                 s.admin_endpoint, s.proxy.shutdown_grace.count());
  return out;
}

}  // namespace vkp::config
