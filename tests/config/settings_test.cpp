#include "config/settings.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace config = vkp::config;
using Catch::Matchers::ContainsSubstring;

namespace {

// Writes a config to a uniquely named file and removes it again. Named after
// the test case so a failure leaves an identifiable file behind if the
// destructor is skipped by a crash.
class temp_config {
 public:
  explicit temp_config(std::string_view body) : path_(unique_path()) {
    std::ofstream{path_} << body;
  }
  ~temp_config() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  temp_config(const temp_config&) = delete;
  temp_config& operator=(const temp_config&) = delete;

  [[nodiscard]] std::string str() const { return path_.string(); }

 private:
  static std::filesystem::path unique_path() {
    static int n = 0;
    return std::filesystem::temp_directory_path() /
           ("vkp_settings_test_" + std::to_string(::getpid()) + "_" + std::to_string(n++) +
            ".toml");
  }

  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("endpoints parse into host and port", "[config]") {
  CHECK(config::parse_endpoint("127.0.0.1:6380") ==
        std::pair<std::string, std::uint16_t>{"127.0.0.1", 6380});
  CHECK(config::parse_endpoint("[::1]:6380") == std::pair<std::string, std::uint16_t>{"::1", 6380});
  // An empty host means "every interface", which is how -l :6380 is spelled.
  CHECK(config::parse_endpoint(":6380") == std::pair<std::string, std::uint16_t>{"0.0.0.0", 6380});
  CHECK(config::parse_endpoint("localhost:0") ==
        std::pair<std::string, std::uint16_t>{"localhost", 0});

  CHECK_THROWS_AS(config::parse_endpoint("127.0.0.1"), std::runtime_error);
  CHECK_THROWS_AS(config::parse_endpoint("127.0.0.1:"), std::runtime_error);
  CHECK_THROWS_AS(config::parse_endpoint("127.0.0.1:-1"), std::runtime_error);
  CHECK_THROWS_AS(config::parse_endpoint("127.0.0.1:99999"), std::runtime_error);
  // stoi would happily read "80" out of this one.
  CHECK_THROWS_AS(config::parse_endpoint("127.0.0.1:80x"), std::runtime_error);
}

TEST_CASE("an empty file leaves every default in place", "[config]") {
  const temp_config f{""};
  const config::settings s = config::load_toml(f.str());
  const config::settings defaults;
  CHECK(s.proxy.listen_port == defaults.proxy.listen_port);
  CHECK(s.workers == defaults.workers);
  CHECK(s.io_backend == defaults.io_backend);
  CHECK(s.admin_endpoint == defaults.admin_endpoint);
  CHECK_NOTHROW(config::validate(s));
}

TEST_CASE("a full file maps onto the structs the rest of the tree takes", "[config]") {
  const temp_config f{R"(
[listen]
endpoint = "[::1]:7000"
backlog = 128

[backend]
endpoint = "db.internal:6379"
conns_per_worker = 2
request_timeout_ms = 250
backoff_base_ms = 10
backoff_max_ms = 400

[cluster]
seeds = ["10.0.0.1:7000", "10.0.0.2:7000"]
refresh_ms = 1500
max_redirects = 9

[worker]
threads = 0
cpu_affinity = true
io_backend = "epoll"

[limits]
max_inflight = 64
client_outbuf_bytes = 65536

[log]
level = "debug"
format = "json"
file = "/var/log/proxyd.log"

[admin]
endpoint = "0.0.0.0:9999"

[shutdown]
grace_ms = 12000
)"};

  const config::settings s = config::load_toml(f.str());
  CHECK(s.proxy.listen_host == "::1");
  CHECK(s.proxy.listen_port == 7000);
  CHECK(s.proxy.backlog == 128);
  CHECK(s.proxy.backend_host == "db.internal");
  CHECK(s.proxy.conns_per_backend == 2);
  CHECK(s.proxy.backend.request_timeout == std::chrono::milliseconds{250});
  // Untouched keys keep their defaults rather than resetting to zero.
  CHECK(s.proxy.backend.connect_timeout == std::chrono::milliseconds{1000});
  CHECK(s.proxy.cluster_seeds == std::vector<std::string>{"10.0.0.1:7000", "10.0.0.2:7000"});
  CHECK(s.proxy.cluster_refresh == std::chrono::milliseconds{1500});
  CHECK(s.proxy.max_redirects == 9);
  CHECK(s.workers == 0);
  CHECK(s.cpu_affinity);
  CHECK(s.io_backend == "epoll");
  CHECK(s.proxy.backend.max_inflight == 64);
  CHECK(s.proxy.client_outbuf_limit == 65536);
  CHECK(s.log.level == "debug");
  CHECK(s.log_format == "json");
  CHECK(s.log.file == "/var/log/proxyd.log");
  CHECK(s.admin_endpoint == "0.0.0.0:9999");
  CHECK(s.proxy.shutdown_grace == std::chrono::milliseconds{12000});
  CHECK_NOTHROW(config::validate(s));
}

TEST_CASE("malformed input is rejected with a position", "[config]") {
  auto fails_with = [](std::string_view body, std::string_view needle) {
    const temp_config f{body};
    try {
      (void)config::load_toml(f.str());
    } catch (const std::runtime_error& e) {
      const std::string msg = e.what();
      INFO(msg);
      CHECK_THAT(msg, ContainsSubstring(std::string{needle}));
      // Every diagnostic names the file, so a multi-file deployment can tell
      // which one is wrong.
      CHECK_THAT(msg, ContainsSubstring(f.str()));
      return;
    }
    FAIL("expected load_toml to throw for: " << body);
  };

  SECTION("syntax error") {
    fails_with("[listen\nendpoint = \"x:1\"\n", "1:8");
  }
  SECTION("wrong type") {
    fails_with("[listen]\nbacklog = \"big\"\n", "'backlog' must be an integer");
  }
  SECTION("out of range") {
    fails_with("[cluster]\nmax_redirects = 500\n", "must be between 1 and 64");
  }
  SECTION("bad endpoint") {
    fails_with("[listen]\nendpoint = \"127.0.0.1\"\n", "expected host:port");
  }
  SECTION("unknown key") {
    fails_with("[listen]\nendpiont = \"x:1\"\n", "unknown key 'endpiont' in [listen]");
  }
  SECTION("unknown section") {
    fails_with("[lisen]\nendpoint = \"x:1\"\n", "unknown top-level key 'lisen'");
  }
  SECTION("section is not a table") {
    fails_with("listen = 3\n", "[listen] must be a table");
  }
  SECTION("array of the wrong thing") {
    fails_with("[cluster]\nseeds = [1, 2]\n", "must contain only strings");
  }
  SECTION("not a bool") {
    fails_with("[worker]\ncpu_affinity = \"yes\"\n", "must be true or false");
  }
}

TEST_CASE("a missing file is reported as such, not as a parse error", "[config]") {
  CHECK_THROWS_WITH(config::load_toml("/nonexistent/vkp/proxy.toml"),
                    ContainsSubstring("cannot open config file"));
}

TEST_CASE("validate catches what parsing cannot", "[config]") {
  config::settings s;
  CHECK_NOTHROW(config::validate(s));

  SECTION("enumerations the CLI also checks") {
    s.io_backend = "uring";
    CHECK_THROWS_WITH(config::validate(s), ContainsSubstring("unknown io_backend 'uring'"));
    s = config::settings{};
    s.log_format = "logfmt";
    CHECK_THROWS_WITH(config::validate(s), ContainsSubstring("unknown log format"));
    s = config::settings{};
    s.log.level = "verbose";
    CHECK_THROWS_WITH(config::validate(s), ContainsSubstring("unknown log level"));
  }
  SECTION("a backoff ceiling below its floor") {
    s.proxy.backend.backoff_base = std::chrono::milliseconds{1000};
    s.proxy.backend.backoff_max = std::chrono::milliseconds{100};
    CHECK_THROWS_WITH(config::validate(s), ContainsSubstring("below backoff_base_ms"));
  }
  SECTION("a seed that would look like an unreachable node at bootstrap") {
    s.proxy.cluster_seeds = {"10.0.0.1:7000", "10.0.0.2"};
    CHECK_THROWS_WITH(config::validate(s), ContainsSubstring("expected host:port"));
  }
  SECTION("an admin endpoint that would fail at bind time") {
    s.admin_endpoint = "9180";
    CHECK_THROWS_AS(config::validate(s), std::runtime_error);
    // Empty is not an error: it is how the admin server is switched off.
    s.admin_endpoint = "";
    CHECK_NOTHROW(config::validate(s));
  }
}

TEST_CASE("to_string round-trips through the parser", "[config]") {
  // The printed configuration is what a postmortem reads, so it has to be the
  // whole thing — parsing it back must reproduce every field.
  const temp_config src{R"(
[listen]
endpoint = "0.0.0.0:6390"
[cluster]
seeds = ["10.0.0.1:7000", "10.0.0.2:7000"]
[worker]
threads = 4
cpu_affinity = true
[log]
format = "json"
)"};
  const config::settings a = config::load_toml(src.str());

  const temp_config printed{config::to_string(a)};
  const config::settings b = config::load_toml(printed.str());

  CHECK(config::to_string(b) == config::to_string(a));
  CHECK(b.proxy.listen_port == 6390);
  CHECK(b.proxy.cluster_seeds == a.proxy.cluster_seeds);
  CHECK(b.workers == 4);
  CHECK(b.cpu_affinity);
  CHECK(b.log_format == "json");
}
