#include <cstdlib>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/sinks/ConsoleSink.h>

#include "core/version.hpp"

int main(int argc, char** argv) {
  CLI::App app{fmt::format("valkey-proxy {}", vkp::kVersion)};
  app.set_version_flag("--version", std::string{vkp::kVersion});

  std::string config_path;
  app.add_option("-c,--config", config_path, "Path to TOML config file")
      ->check(CLI::ExistingFile);

  CLI11_PARSE(app, argc, argv);

  quill::Backend::start();
  auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console");
  auto* logger = quill::Frontend::create_or_get_logger("root", std::move(sink));

  LOG_INFO(logger, "valkey-proxy {} starting", vkp::kVersion);
  LOG_INFO(logger, "nothing to do yet (M0 skeleton), exiting");

  return EXIT_SUCCESS;
}
