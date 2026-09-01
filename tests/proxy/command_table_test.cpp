#include "proxy/command_table.hpp"

#include <catch2/catch_test_macros.hpp>

using vkp::proxy::cmd_policy;
using vkp::proxy::find_command;

TEST_CASE("lookup is case-insensitive", "[proxy][command_table]") {
  for (const auto name : {"get", "GET", "Get", "gEt"}) {
    const auto* info = find_command(name);
    REQUIRE(info != nullptr);
    CHECK(info->name == "GET");
    CHECK(info->policy == cmd_policy::forward);
  }
}

TEST_CASE("unknown / oversized / empty names return nullptr", "[proxy][command_table]") {
  CHECK(find_command("NOSUCHCOMMAND") == nullptr);
  CHECK(find_command("") == nullptr);
  CHECK(find_command("GETT") == nullptr);
  CHECK(find_command("ZREMRANGEBYSCOREX") == nullptr);  // one past the longest name
  CHECK(find_command("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") == nullptr);
}

TEST_CASE("sticky / blocking / stateful commands are rejected", "[proxy][command_table]") {
  for (const auto name :
       {"MULTI", "EXEC", "DISCARD", "WATCH", "UNWATCH", "SUBSCRIBE", "PSUBSCRIBE", "UNSUBSCRIBE",
        "SSUBSCRIBE", "BLPOP", "BRPOPLPUSH", "BZMPOP", "WAIT", "MONITOR", "AUTH", "CLIENT"}) {
    const auto* info = find_command(name);
    REQUIRE(info != nullptr);
    CHECK(info->policy == cmd_policy::reject);
  }
}

TEST_CASE("connection-state commands are handled locally", "[proxy][command_table]") {
  for (const auto name : {"HELLO", "QUIT", "SELECT", "RESET"}) {
    const auto* info = find_command(name);
    REQUIRE(info != nullptr);
    CHECK(info->policy == cmd_policy::local);
  }
}

TEST_CASE("key positions follow the redis convention", "[proxy][command_table]") {
  const auto* get = find_command("GET");
  REQUIRE(get != nullptr);
  CHECK(get->first_key == 1);
  CHECK(get->last_key == 1);
  CHECK(get->key_step == 1);

  const auto* mget = find_command("MGET");
  REQUIRE(mget != nullptr);
  CHECK(mget->first_key == 1);
  CHECK(mget->last_key == -1);
  CHECK(mget->key_step == 1);

  const auto* mset = find_command("MSET");
  REQUIRE(mset != nullptr);
  CHECK(mset->first_key == 1);
  CHECK(mset->last_key == -1);
  CHECK(mset->key_step == 2);

  const auto* lmove = find_command("LMOVE");
  REQUIRE(lmove != nullptr);
  CHECK(lmove->first_key == 1);
  CHECK(lmove->last_key == 2);

  const auto* ping = find_command("PING");
  REQUIRE(ping != nullptr);
  CHECK(ping->policy == cmd_policy::forward);
  CHECK(ping->first_key == 0);  // no keys
}
