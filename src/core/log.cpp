#include "core/log.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>
#include <quill/sinks/JsonSink.h>

namespace vkp::log {

namespace {

quill::Logger* g_logger = nullptr;
bool g_backend_started = false;

// Thread id is in there because under thread-per-core it is effectively the
// worker id — every data-plane line comes from exactly one worker thread, so
// it is the field you grep on to isolate one core's story.
constexpr std::string_view kTextPattern = "%(time) [%(thread_id)] %(log_level:<7) %(message)";

std::shared_ptr<quill::Sink> make_sink(const options& opt) {
  const bool to_file = !opt.file.empty();
  if (!to_file) {
    return opt.fmt == format::json
               ? quill::Frontend::create_or_get_sink<quill::JsonConsoleSink>("json_console")
               : quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console");
  }
  // Append: a restarting daemon must not erase the log that explains why it
  // restarted. Rotation is left to logrotate, which already owns that policy
  // everywhere this would run.
  quill::FileSinkConfig fcfg;
  fcfg.set_open_mode('a');
  if (opt.fmt == format::json) {
    return quill::Frontend::create_or_get_sink<quill::JsonFileSink>(opt.file, fcfg);
  }
  return quill::Frontend::create_or_get_sink<quill::FileSink>(opt.file, fcfg);
}

}  // namespace

void init(const options& opt) {
  quill::LogLevel level{};
  try {
    level = quill::loglevel_from_string(opt.level);
  } catch (const std::exception&) {
    throw std::runtime_error(fmt::format("unknown log level '{}'", opt.level));
  }

  if (g_logger != nullptr) {
    g_logger->set_log_level(level);  // re-init: keep the sinks, move the level
    return;
  }

  if (!g_backend_started) {
    quill::Backend::start();
    g_backend_started = true;
  }

  try {
    // The JSON sinks carry their own layout; handing them a text pattern would
    // wrap JSON inside a formatted line.
    g_logger =
        opt.fmt == format::json
            ? quill::Frontend::create_or_get_logger("root", make_sink(opt))
            : quill::Frontend::create_or_get_logger(
                  "root", make_sink(opt),
                  quill::PatternFormatterOptions{std::string{kTextPattern}, "%H:%M:%S.%Qms"});
  } catch (const std::exception& e) {
    throw std::runtime_error(fmt::format("cannot open log sink: {}", e.what()));
  }
  g_logger->set_log_level(level);
}

void shutdown() noexcept {
  if (!g_backend_started) {
    return;
  }
  // Stopping the backend flushes what is still queued; the logger itself is
  // owned by quill and dies with it.
  quill::Backend::stop();
  g_backend_started = false;
  g_logger = nullptr;
}

quill::Logger* logger() noexcept {
  return g_logger;
}

}  // namespace vkp::log
