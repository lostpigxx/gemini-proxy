#include "proxy/command_table.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <system_error>

#include "cluster/slot.hpp"

namespace vkp::proxy {

namespace {

// Shorthand for table rows. `fwd`/`fwd_nk`/`nk` are routed by key; the rest
// say what cluster mode does with a command that carries none.
constexpr command_info fwd(std::string_view name, std::int8_t first, std::int8_t last,
                           std::int8_t step) {
  return {.name = name,
          .policy = cmd_policy::forward,
          .cluster = cluster_policy::by_key,
          .first_key = first,
          .last_key = last,
          .key_step = step};
}
// Range spec plus a `numkeys` list, e.g. ZUNIONSTORE dest numkeys key...
constexpr command_info fwd_nk(std::string_view name, std::int8_t first, std::int8_t last,
                              std::int8_t step, std::int8_t numkeys) {
  return {.name = name,
          .policy = cmd_policy::forward,
          .cluster = cluster_policy::by_key,
          .first_key = first,
          .last_key = last,
          .key_step = step,
          .numkeys_idx = numkeys};
}
// Keys come only from a `numkeys` list, e.g. EVAL script numkeys key...
constexpr command_info nk(std::string_view name, std::int8_t numkeys) {
  return {.name = name,
          .policy = cmd_policy::forward,
          .cluster = cluster_policy::by_key,
          .numkeys_idx = numkeys};
}
// Keyless admin command: in cluster mode any master answers it.
constexpr command_info any(std::string_view name) {
  return {.name = name, .policy = cmd_policy::forward, .cluster = cluster_policy::any_node};
}
// Forwarded standalone, answered by the proxy itself in cluster mode.
constexpr command_info self(std::string_view name) {
  return {.name = name, .policy = cmd_policy::forward, .cluster = cluster_policy::local};
}
// Forwarded standalone, refused in cluster mode (see the header's rationale).
constexpr command_info nocl(std::string_view name) {
  return {.name = name, .policy = cmd_policy::forward, .cluster = cluster_policy::unsupported};
}
constexpr command_info rej(std::string_view name) {
  return {.name = name, .policy = cmd_policy::reject, .cluster = cluster_policy::unsupported};
}
constexpr command_info loc(std::string_view name) {
  return {.name = name, .policy = cmd_policy::local, .cluster = cluster_policy::local};
}

// Sorted by name (static_assert below). Key specs follow valkey's own COMMAND
// output. Commands whose key positions need variadic parsing to pin down
// (SORT's STORE, GEORADIUS's STORE, XREAD's STREAMS) are `nocl` rather than
// guessed: a wrong guess routes to the wrong node and returns an answer that
// is semantically wrong but looks fine.
constexpr std::array kCommands = {
    fwd("APPEND", 1, 1, 1),
    rej("AUTH"),
    fwd("BITCOUNT", 1, 1, 1),
    fwd("BITFIELD", 1, 1, 1),
    fwd("BITOP", 2, -1, 1),
    fwd("BITPOS", 1, 1, 1),
    rej("BLMOVE"),
    rej("BLMPOP"),
    rej("BLPOP"),
    rej("BRPOP"),
    rej("BRPOPLPUSH"),
    rej("BZMPOP"),
    rej("BZPOPMAX"),
    rej("BZPOPMIN"),
    rej("CLIENT"),
    rej("CLUSTER"),  // the proxy always presents itself as standalone
    any("COMMAND"),
    any("CONFIG"),
    fwd("COPY", 1, 2, 1),
    nocl("DBSIZE"),
    any("DEBUG"),
    fwd("DECR", 1, 1, 1),
    fwd("DECRBY", 1, 1, 1),
    fwd("DEL", 1, -1, 1),
    rej("DISCARD"),
    fwd("DUMP", 1, 1, 1),
    self("ECHO"),
    nk("EVAL", 2),
    nk("EVALSHA", 2),
    nk("EVALSHA_RO", 2),
    nk("EVAL_RO", 2),
    rej("EXEC"),
    fwd("EXISTS", 1, -1, 1),
    fwd("EXPIRE", 1, 1, 1),
    fwd("EXPIREAT", 1, 1, 1),
    fwd("EXPIRETIME", 1, 1, 1),
    nk("FCALL", 2),
    nk("FCALL_RO", 2),
    nocl("FLUSHALL"),
    nocl("FLUSHDB"),
    nocl("FUNCTION"),  // LOAD would have to reach every master
    fwd("GEOADD", 1, 1, 1),
    fwd("GEODIST", 1, 1, 1),
    fwd("GEOHASH", 1, 1, 1),
    fwd("GEOPOS", 1, 1, 1),
    nocl("GEORADIUS"),
    nocl("GEORADIUSBYMEMBER"),
    nocl("GEORADIUSBYMEMBER_RO"),
    nocl("GEORADIUS_RO"),
    fwd("GEOSEARCH", 1, 1, 1),
    fwd("GEOSEARCHSTORE", 1, 2, 1),
    fwd("GET", 1, 1, 1),
    fwd("GETBIT", 1, 1, 1),
    fwd("GETDEL", 1, 1, 1),
    fwd("GETEX", 1, 1, 1),
    fwd("GETRANGE", 1, 1, 1),
    fwd("GETSET", 1, 1, 1),
    fwd("HDEL", 1, 1, 1),
    loc("HELLO"),
    fwd("HEXISTS", 1, 1, 1),
    fwd("HGET", 1, 1, 1),
    fwd("HGETALL", 1, 1, 1),
    fwd("HINCRBY", 1, 1, 1),
    fwd("HINCRBYFLOAT", 1, 1, 1),
    fwd("HKEYS", 1, 1, 1),
    fwd("HLEN", 1, 1, 1),
    fwd("HMGET", 1, 1, 1),
    fwd("HMSET", 1, 1, 1),
    fwd("HRANDFIELD", 1, 1, 1),
    fwd("HSCAN", 1, 1, 1),
    fwd("HSET", 1, 1, 1),
    fwd("HSETNX", 1, 1, 1),
    fwd("HSTRLEN", 1, 1, 1),
    fwd("HVALS", 1, 1, 1),
    fwd("INCR", 1, 1, 1),
    fwd("INCRBY", 1, 1, 1),
    fwd("INCRBYFLOAT", 1, 1, 1),
    any("INFO"),
    nocl("KEYS"),
    any("LASTSAVE"),
    fwd("LCS", 1, 2, 1),
    fwd("LINDEX", 1, 1, 1),
    fwd("LINSERT", 1, 1, 1),
    fwd("LLEN", 1, 1, 1),
    fwd("LMOVE", 1, 2, 1),
    nk("LMPOP", 1),
    any("LOLWUT"),
    fwd("LPOP", 1, 1, 1),
    fwd("LPOS", 1, 1, 1),
    fwd("LPUSH", 1, 1, 1),
    fwd("LPUSHX", 1, 1, 1),
    fwd("LRANGE", 1, 1, 1),
    fwd("LREM", 1, 1, 1),
    fwd("LSET", 1, 1, 1),
    fwd("LTRIM", 1, 1, 1),
    any("MEMORY"),
    fwd("MGET", 1, -1, 1),
    rej("MONITOR"),
    fwd("MSET", 1, -1, 2),
    fwd("MSETNX", 1, -1, 2),
    rej("MULTI"),
    fwd("OBJECT", 2, 2, 1),
    fwd("PERSIST", 1, 1, 1),
    fwd("PEXPIRE", 1, 1, 1),
    fwd("PEXPIREAT", 1, 1, 1),
    fwd("PEXPIRETIME", 1, 1, 1),
    fwd("PFADD", 1, 1, 1),
    fwd("PFCOUNT", 1, -1, 1),
    fwd("PFMERGE", 1, -1, 1),
    self("PING"),
    fwd("PSETEX", 1, 1, 1),
    rej("PSUBSCRIBE"),
    fwd("PTTL", 1, 1, 1),
    any("PUBLISH"),
    any("PUBSUB"),
    rej("PUNSUBSCRIBE"),
    loc("QUIT"),
    nocl("RANDOMKEY"),
    fwd("RENAME", 1, 2, 1),
    fwd("RENAMENX", 1, 2, 1),
    loc("RESET"),
    fwd("RESTORE", 1, 1, 1),
    fwd("RPOP", 1, 1, 1),
    fwd("RPOPLPUSH", 1, 2, 1),
    fwd("RPUSH", 1, 1, 1),
    fwd("RPUSHX", 1, 1, 1),
    fwd("SADD", 1, 1, 1),
    nocl("SCAN"),
    fwd("SCARD", 1, 1, 1),
    nocl("SCRIPT"),  // LOAD would have to reach every master
    fwd("SDIFF", 1, -1, 1),
    fwd("SDIFFSTORE", 1, -1, 1),
    loc("SELECT"),
    fwd("SET", 1, 1, 1),
    fwd("SETBIT", 1, 1, 1),
    fwd("SETEX", 1, 1, 1),
    fwd("SETNX", 1, 1, 1),
    fwd("SETRANGE", 1, 1, 1),
    fwd("SINTER", 1, -1, 1),
    nk("SINTERCARD", 1),
    fwd("SINTERSTORE", 1, -1, 1),
    fwd("SISMEMBER", 1, 1, 1),
    any("SLOWLOG"),
    fwd("SMEMBERS", 1, 1, 1),
    fwd("SMISMEMBER", 1, 1, 1),
    fwd("SMOVE", 1, 2, 1),
    nocl("SORT"),  // the STORE destination hides behind variadic options
    nocl("SORT_RO"),
    fwd("SPOP", 1, 1, 1),
    fwd("SPUBLISH", 1, 1, 1),
    fwd("SRANDMEMBER", 1, 1, 1),
    fwd("SREM", 1, 1, 1),
    fwd("SSCAN", 1, 1, 1),
    rej("SSUBSCRIBE"),
    fwd("STRLEN", 1, 1, 1),
    rej("SUBSCRIBE"),
    fwd("SUBSTR", 1, 1, 1),
    fwd("SUNION", 1, -1, 1),
    fwd("SUNIONSTORE", 1, -1, 1),
    rej("SUNSUBSCRIBE"),
    any("TIME"),
    fwd("TOUCH", 1, -1, 1),
    fwd("TTL", 1, 1, 1),
    fwd("TYPE", 1, 1, 1),
    fwd("UNLINK", 1, -1, 1),
    rej("UNSUBSCRIBE"),
    rej("UNWATCH"),
    rej("WAIT"),
    rej("WAITAOF"),
    rej("WATCH"),
    fwd("XACK", 1, 1, 1),
    fwd("XADD", 1, 1, 1),
    fwd("XAUTOCLAIM", 1, 1, 1),
    fwd("XCLAIM", 1, 1, 1),
    fwd("XDEL", 1, 1, 1),
    fwd("XGROUP", 2, 2, 1),
    fwd("XINFO", 2, 2, 1),
    fwd("XLEN", 1, 1, 1),
    fwd("XPENDING", 1, 1, 1),
    fwd("XRANGE", 1, 1, 1),
    nocl("XREAD"),  // keys start after the STREAMS token
    nocl("XREADGROUP"),
    fwd("XREVRANGE", 1, 1, 1),
    fwd("XSETID", 1, 1, 1),
    fwd("XTRIM", 1, 1, 1),
    fwd("ZADD", 1, 1, 1),
    fwd("ZCARD", 1, 1, 1),
    fwd("ZCOUNT", 1, 1, 1),
    nk("ZDIFF", 1),
    fwd_nk("ZDIFFSTORE", 1, 1, 1, 2),
    fwd("ZINCRBY", 1, 1, 1),
    nk("ZINTER", 1),
    nk("ZINTERCARD", 1),
    fwd_nk("ZINTERSTORE", 1, 1, 1, 2),
    nk("ZMPOP", 1),
    fwd("ZMSCORE", 1, 1, 1),
    fwd("ZPOPMAX", 1, 1, 1),
    fwd("ZPOPMIN", 1, 1, 1),
    fwd("ZRANDMEMBER", 1, 1, 1),
    fwd("ZRANGE", 1, 1, 1),
    fwd("ZRANGEBYLEX", 1, 1, 1),
    fwd("ZRANGEBYSCORE", 1, 1, 1),
    fwd("ZRANGESTORE", 1, 2, 1),
    fwd("ZRANK", 1, 1, 1),
    fwd("ZREM", 1, 1, 1),
    fwd("ZREMRANGEBYLEX", 1, 1, 1),
    fwd("ZREMRANGEBYRANK", 1, 1, 1),
    fwd("ZREMRANGEBYSCORE", 1, 1, 1),
    fwd("ZREVRANGE", 1, 1, 1),
    fwd("ZREVRANGEBYLEX", 1, 1, 1),
    fwd("ZREVRANGEBYSCORE", 1, 1, 1),
    fwd("ZREVRANK", 1, 1, 1),
    fwd("ZSCAN", 1, 1, 1),
    fwd("ZSCORE", 1, 1, 1),
    nk("ZUNION", 1),
    fwd_nk("ZUNIONSTORE", 1, 1, 1, 2),
};

consteval bool table_is_sorted_unique() {
  for (std::size_t i = 1; i < kCommands.size(); ++i) {
    if (!(kCommands[i - 1].name < kCommands[i].name)) {
      return false;
    }
  }
  return true;
}
static_assert(table_is_sorted_unique(), "command table must be sorted by name, no duplicates");

consteval std::size_t longest_name() {
  std::size_t n = 0;
  for (const command_info& c : kCommands) {
    n = std::max(n, c.name.size());
  }
  return n;
}
constexpr std::size_t kLongestName = longest_name();

// Walks the key spec and hands each key to `fn`. Shapes that contradict the
// spec (short argv, unparsable or out-of-range numkeys) simply stop early —
// callers treat "no keys" as "let the backend produce the arity error".
template <typename F>
constexpr void each_key(std::span<const std::string_view> args, const command_info& info, F&& fn) {
  const auto argc = static_cast<std::int32_t>(args.size());
  if (info.first_key > 0 && info.key_step > 0) {
    const std::int32_t last = info.last_key >= 0 ? info.last_key : argc + info.last_key;
    for (std::int32_t i = info.first_key; i <= last && i < argc; i += info.key_step) {
      fn(args[static_cast<std::size_t>(i)]);
    }
  }
  if (info.numkeys_idx > 0) {
    const auto idx = static_cast<std::size_t>(info.numkeys_idx);
    if (idx >= args.size()) {
      return;
    }
    const std::string_view text = args[idx];
    long long count = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, count);
    if (parsed.ec != std::errc{} || parsed.ptr != end || count <= 0) {
      return;
    }
    for (long long n = 0; n < count; ++n) {
      const std::size_t at = idx + 1 + static_cast<std::size_t>(n);
      if (at >= args.size()) {
        return;
      }
      fn(args[at]);
    }
  }
}

}  // namespace

const command_info* find_command(std::string_view name) noexcept {
  if (name.empty() || name.size() > kLongestName) {
    return nullptr;
  }
  char upper[kLongestName];
  for (std::size_t i = 0; i < name.size(); ++i) {
    upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[i])));
  }
  const std::string_view key{upper, name.size()};
  const auto it = std::lower_bound(
      kCommands.begin(), kCommands.end(), key,
      [](const command_info& c, std::string_view k) noexcept { return c.name < k; });
  if (it == kCommands.end() || it->name != key) {
    return nullptr;
  }
  return &*it;
}

route_result route_slot(std::span<const std::string_view> args, const command_info& info) noexcept {
  bool seen = false;
  bool crossslot = false;
  std::uint16_t slot = 0;
  each_key(args, info, [&](std::string_view key) noexcept {
    const std::uint16_t s = cluster::key_slot(key);
    if (!seen) {
      seen = true;
      slot = s;
    } else if (s != slot) {
      crossslot = true;
    }
  });
  if (crossslot) {
    return {.k = route_result::kind::crossslot, .slot = 0};
  }
  if (!seen) {
    return {.k = route_result::kind::any_node, .slot = 0};
  }
  return {.k = route_result::kind::by_slot, .slot = slot};
}

std::vector<std::string_view> collect_keys(std::span<const std::string_view> args,
                                           const command_info& info) {
  std::vector<std::string_view> keys;
  each_key(args, info, [&](std::string_view key) { keys.push_back(key); });
  return keys;
}

}  // namespace vkp::proxy
