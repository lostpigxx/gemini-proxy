#include "core/log.hpp"

#include <catch2/catch_test_macros.hpp>

#include "io/socket.hpp"

// These tests deliberately never call log::init(). The whole test binary runs
// with no quill backend thread and no output, which is what keeps the ASan
// leak run and the TSan run quiet — so the property worth pinning down is
// that logging in that state is a no-op rather than a null dereference.
TEST_CASE("logging before init() is a no-op") {
  REQUIRE(vkp::log::logger() == nullptr);

  int evaluated = 0;
  const auto side_effect = [&] {
    ++evaluated;
    return 7;
  };

  VKP_LOG_INFO("this must not crash: {}", side_effect());
  VKP_LOG_WARN("nor this");
  VKP_LOG_ERROR("nor this: {}", 1);
  VKP_LOG_DEBUG("nor this: {}", 2);
  VKP_LOG_WARN_EVERY(std::chrono::seconds{1}, "nor the rate-limited one: {}", 3);

  // The guard short-circuits before quill sees the arguments, so the argument
  // expressions never run. Anything with a side effect in a log call would be
  // silently dropped — worth stating out loud.
  REQUIRE(evaluated == 0);
}

TEST_CASE("shutdown() without init() is safe") {
  vkp::log::shutdown();
  vkp::log::shutdown();
  REQUIRE(vkp::log::logger() == nullptr);
}

TEST_CASE("to_string renders an address the way logs and /topology want it") {
  const vkp::io::resolved_addr v4 = vkp::io::resolve_tcp("127.0.0.1", 6379);
  REQUIRE(vkp::io::to_string(v4) == "127.0.0.1:6379");

  const vkp::io::resolved_addr v6 = vkp::io::resolve_tcp("::1", 6380);
  REQUIRE(vkp::io::to_string(v6) == "[::1]:6380");

  // A default-constructed address has family AF_UNSPEC; it must come back as a
  // placeholder rather than reading uninitialised sockaddr bytes.
  REQUIRE(vkp::io::to_string(vkp::io::resolved_addr{}) == "<unknown>");
}
