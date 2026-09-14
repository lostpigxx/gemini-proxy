// Process-wide logger. A thin lid on quill so the rest of the tree logs
// through one header and one initialisation point.
// Design: docs/design/m5-observability-config.md §0.
//
// Until init() runs, logger() is null and every VKP_LOG_* expands to nothing.
// That is the state unit tests live in: no quill backend thread, no output.
#pragma once

#include <string>

#include <quill/LogMacros.h>
#include <quill/Logger.h>

namespace vkp::log {

enum class format : std::uint8_t {
  text,  // human-readable, coloured when the sink is a tty
  json,  // one JSON object per line, for log shippers
};

struct options {
  std::string level = "info";  // quill::loglevel_from_string spelling
  log::format fmt = format::text;
  std::string file;  // empty = stdout
};

// Starts the quill backend and installs the process logger. Throws
// std::runtime_error on an unknown level or an unopenable file. Calling it
// twice replaces the level but keeps the original sinks.
void init(const options& opt);

// Flushes and stops the backend thread. Safe to call without init().
void shutdown() noexcept;

// Null until init() has run.
[[nodiscard]] quill::Logger* logger() noexcept;

}  // namespace vkp::log

// The guard matters: quill's macros dereference the logger without checking,
// and `logger` is a runtime pointer here rather than a fixed global.
#define VKP_LOG_IMPL(macro_, ...)               \
  do {                                          \
    if (auto* vkp_l_ = ::vkp::log::logger()) {  \
      macro_(vkp_l_, __VA_ARGS__); /* NOLINT */ \
    }                                           \
  } while (0)

#define VKP_LOG_DEBUG(...) VKP_LOG_IMPL(LOG_DEBUG, __VA_ARGS__)
#define VKP_LOG_INFO(...) VKP_LOG_IMPL(LOG_INFO, __VA_ARGS__)
#define VKP_LOG_WARN(...) VKP_LOG_IMPL(LOG_WARNING, __VA_ARGS__)
#define VKP_LOG_ERROR(...) VKP_LOG_IMPL(LOG_ERROR, __VA_ARGS__)

// Rate-limited variant for events that can arrive in storms (a resharding
// cluster emits MOVED by the thousand). `interval` is a std::chrono duration.
#define VKP_LOG_WARN_EVERY(interval, ...)               \
  do {                                                  \
    if (auto* vkp_l_ = ::vkp::log::logger()) {          \
      LOG_WARNING_LIMIT(interval, vkp_l_, __VA_ARGS__); \
    }                                                   \
  } while (0)
