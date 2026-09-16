// Effective configuration: defaults → TOML file → explicit command line.
// Design: docs/design/m5-observability-config.md §6.
//
// `settings` is deliberately the union of the structs the rest of the tree
// already takes (proxy::config, log::options, plus the handful of scalars
// worker_pool and the admin server need), not a parallel schema. A new tunable
// gets one line here and one line in config/proxy.toml; nothing has to be kept
// in sync twice.
//
// io_backend and admin_endpoint stay strings rather than their resolved types
// so that every spelling mistake — in the file or on the command line — is
// reported by the same validate() with the same wording.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "core/log.hpp"
#include "proxy/server.hpp"

namespace vkp::config {

struct settings {
  proxy::config proxy;
  std::size_t workers = 1;  // 0 = one per hardware thread
  bool cpu_affinity = false;
  std::string io_backend = "auto";  // auto | io_uring | epoll | kqueue
  log::options log;
  std::string log_format = "text";                // text | json; mirrors log.fmt
  std::string admin_endpoint = "127.0.0.1:9180";  // empty disables the admin server
};

// "host:port" (an empty host means 0.0.0.0); IPv6 literals use [addr]:port.
// Throws std::runtime_error naming the offending string.
std::pair<std::string, std::uint16_t> parse_endpoint(const std::string& ep);

// Parses `path` and returns defaults overlaid with whatever it sets. Throws
// std::runtime_error on a missing/unreadable file, a syntax error, a wrongly
// typed or out-of-range value, or an unknown key — all carrying
// `path:line:col`. Unknown keys are an error on purpose: a silently ignored
// typo in a config file is the kind of bug that only shows up in production.
settings load_toml(const std::string& path);

// Cross-field and enumeration checks that parsing alone cannot make (and that
// the command line needs too, since CLI values bypass load_toml). Throws
// std::runtime_error.
void validate(const settings& s);

// The effective configuration as a TOML document, logged at startup so the
// first line of a postmortem can rule out "the config never took effect".
std::string to_string(const settings& s);

}  // namespace vkp::config
