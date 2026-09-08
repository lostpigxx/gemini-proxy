#include "proxy/command_table.hpp"

#include <initializer_list>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cluster/slot.hpp"

using vkp::cluster::key_slot;
using vkp::proxy::cluster_policy;
using vkp::proxy::cmd_policy;
using vkp::proxy::collect_keys;
using vkp::proxy::find_command;
using vkp::proxy::route_result;
using vkp::proxy::route_slot;

namespace {

std::vector<std::string_view> keys_of(std::initializer_list<std::string_view> argv) {
  const std::vector<std::string_view> args{argv};
  const auto* info = find_command(args.front());
  REQUIRE(info != nullptr);
  return collect_keys(args, *info);
}

route_result route_of(std::initializer_list<std::string_view> argv) {
  const std::vector<std::string_view> args{argv};
  const auto* info = find_command(args.front());
  REQUIRE(info != nullptr);
  return route_slot(args, *info);
}

}  // namespace

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
  CHECK(find_command("ZREMRANGEBYSCOREX") == nullptr);
  CHECK(find_command("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") == nullptr);  // past the longest
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

TEST_CASE("cluster policy classifies every forwarded command", "[proxy][command_table]") {
  const auto policy_of = [](std::string_view name) {
    const auto* info = find_command(name);
    REQUIRE(info != nullptr);
    return info->cluster;
  };

  for (const auto name : {"GET", "MSET", "DEL", "EVAL", "ZUNIONSTORE", "LMPOP", "XADD"}) {
    CHECK(policy_of(name) == cluster_policy::by_key);
  }
  for (const auto name : {"INFO", "CONFIG", "COMMAND", "TIME", "SLOWLOG", "MEMORY", "PUBLISH"}) {
    CHECK(policy_of(name) == cluster_policy::any_node);
  }
  for (const auto name : {"PING", "ECHO"}) {
    CHECK(policy_of(name) == cluster_policy::local);
  }
  // Single-node semantics, or key positions we refuse to guess.
  for (const auto name :
       {"SCAN", "KEYS", "DBSIZE", "RANDOMKEY", "FLUSHALL", "FLUSHDB", "SORT", "SORT_RO",
        "GEORADIUS", "GEORADIUSBYMEMBER_RO", "XREAD", "XREADGROUP", "SCRIPT", "FUNCTION"}) {
    CHECK(policy_of(name) == cluster_policy::unsupported);
  }
  // The proxy always presents itself as standalone: CLUSTER never reaches
  // routing, in either mode.
  const auto* cluster_cmd = find_command("CLUSTER");
  REQUIRE(cluster_cmd != nullptr);
  CHECK(cluster_cmd->policy == cmd_policy::reject);
}

TEST_CASE("range key specs extract the right arguments", "[proxy][command_table]") {
  CHECK(keys_of({"GET", "k"}) == std::vector<std::string_view>{"k"});
  CHECK(keys_of({"MGET", "a", "b", "c"}) == std::vector<std::string_view>{"a", "b", "c"});
  CHECK(keys_of({"MSET", "a", "1", "b", "2"}) == std::vector<std::string_view>{"a", "b"});
  CHECK(keys_of({"BITOP", "AND", "dst", "s1", "s2"}) ==
        std::vector<std::string_view>{"dst", "s1", "s2"});
  CHECK(keys_of({"OBJECT", "ENCODING", "k"}) == std::vector<std::string_view>{"k"});
  CHECK(keys_of({"LMOVE", "src", "dst", "LEFT", "RIGHT"}) ==
        std::vector<std::string_view>{"src", "dst"});
  // Keyless rows and short argv yield nothing rather than reading past the end.
  CHECK(keys_of({"INFO"}).empty());
  CHECK(keys_of({"GET"}).empty());
  CHECK(keys_of({"OBJECT", "HELP"}).empty());
}

TEST_CASE("numkeys key specs extract the right arguments", "[proxy][command_table]") {
  CHECK(keys_of({"EVAL", "return 1", "2", "a", "b", "arg"}) ==
        std::vector<std::string_view>{"a", "b"});
  CHECK(keys_of({"EVAL", "return 1", "0", "arg"}).empty());
  CHECK(keys_of({"FCALL_RO", "f", "1", "k"}) == std::vector<std::string_view>{"k"});
  CHECK(keys_of({"LMPOP", "2", "a", "b", "LEFT"}) == std::vector<std::string_view>{"a", "b"});
  CHECK(keys_of({"SINTERCARD", "1", "k"}) == std::vector<std::string_view>{"k"});
  // Range and numkeys combine: destination first, then the counted list.
  CHECK(keys_of({"ZUNIONSTORE", "dst", "2", "a", "b"}) ==
        std::vector<std::string_view>{"dst", "a", "b"});
  CHECK(keys_of({"ZDIFFSTORE", "dst", "1", "a"}) == std::vector<std::string_view>{"dst", "a"});
}

TEST_CASE("malformed numkeys stops extraction instead of over-reading", "[proxy][command_table]") {
  CHECK(keys_of({"EVAL", "s", "abc", "k"}).empty());
  CHECK(keys_of({"EVAL", "s", "-1", "k"}).empty());
  CHECK(keys_of({"EVAL", "s", "3", "a", "b"}) == std::vector<std::string_view>{"a", "b"});
  CHECK(keys_of({"EVAL", "s", "99999999999999999999", "a"}) ==
        std::vector<std::string_view>{});       // overflows: from_chars refuses it
  CHECK(keys_of({"EVAL", "s", "2 "}).empty());  // trailing junk is not a count
  CHECK(keys_of({"EVAL", "s"}).empty());
  // The destination still routes even when the counted list is unusable.
  CHECK(keys_of({"ZUNIONSTORE", "dst", "x"}) == std::vector<std::string_view>{"dst"});
}

TEST_CASE("route_slot hashes keys and detects cross-slot requests", "[proxy][command_table]") {
  const auto single = route_of({"GET", "foo"});
  CHECK(single.k == route_result::kind::by_slot);
  CHECK(single.slot == key_slot("foo"));

  // Same hash tag: one slot, routable.
  const auto tagged = route_of({"MGET", "{u1}:a", "{u1}:b"});
  CHECK(tagged.k == route_result::kind::by_slot);
  CHECK(tagged.slot == key_slot("u1"));

  const auto spread = route_of({"MGET", "foo", "bar"});
  CHECK(spread.k == route_result::kind::crossslot);

  const auto script = route_of({"EVAL", "s", "2", "{t}1", "{t}2"});
  CHECK(script.k == route_result::kind::by_slot);
  CHECK(script.slot == key_slot("t"));
  CHECK(route_of({"EVAL", "s", "2", "foo", "bar"}).k == route_result::kind::crossslot);

  // ZUNIONSTORE must agree across the destination and the source list too.
  CHECK(route_of({"ZUNIONSTORE", "{t}d", "2", "{t}a", "{t}b"}).k == route_result::kind::by_slot);
  CHECK(route_of({"ZUNIONSTORE", "d", "2", "{t}a", "{t}b"}).k == route_result::kind::crossslot);
}

TEST_CASE("route_slot falls back to any node when no key is available", "[proxy][command_table]") {
  CHECK(route_of({"INFO"}).k == route_result::kind::any_node);
  CHECK(route_of({"TIME"}).k == route_result::kind::any_node);
  // Arity violations route somewhere so the backend gives the real error.
  CHECK(route_of({"GET"}).k == route_result::kind::any_node);
  CHECK(route_of({"EVAL", "s", "abc"}).k == route_result::kind::any_node);
}
