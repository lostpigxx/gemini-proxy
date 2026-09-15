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

// The metrics bucket, abbreviated because it appears on every row below.
// `cc::write` means "may mutate", so the read/write split is safe to reason
// about from the write side: BITFIELD, GETEX and GETDEL are writes even though
// they mostly read, and PFCOUNT is a read even though it may rewrite the
// sparse encoding. The SCAN-family and KEYS are reads.
using cc = metrics::cmd_class;

// Shorthand for table rows. `fwd`/`fwd_nk`/`nk` are routed by key; the rest
// say what cluster mode does with a command that carries none.
constexpr command_info fwd(cc cls, std::string_view name, std::int8_t first, std::int8_t last,
                           std::int8_t step) {
  return {.name = name,
          .policy = cmd_policy::forward,
          .cluster = cluster_policy::by_key,
          .cls = cls,
          .first_key = first,
          .last_key = last,
          .key_step = step};
}
// Range spec plus a `numkeys` list, e.g. ZUNIONSTORE dest numkeys key...
constexpr command_info fwd_nk(cc cls, std::string_view name, std::int8_t first, std::int8_t last,
                              std::int8_t step, std::int8_t numkeys) {
  return {.name = name,
          .policy = cmd_policy::forward,
          .cluster = cluster_policy::by_key,
          .cls = cls,
          .first_key = first,
          .last_key = last,
          .key_step = step,
          .numkeys_idx = numkeys};
}
// Keys come only from a `numkeys` list, e.g. EVAL script numkeys key...
constexpr command_info nk(cc cls, std::string_view name, std::int8_t numkeys) {
  return {.name = name,
          .policy = cmd_policy::forward,
          .cluster = cluster_policy::by_key,
          .cls = cls,
          .numkeys_idx = numkeys};
}
// Keyless admin command: in cluster mode any master answers it.
constexpr command_info any(cc cls, std::string_view name) {
  return {
      .name = name, .policy = cmd_policy::forward, .cluster = cluster_policy::any_node, .cls = cls};
}
// Forwarded standalone, answered by the proxy itself in cluster mode.
constexpr command_info self(std::string_view name) {
  return {.name = name,
          .policy = cmd_policy::forward,
          .cluster = cluster_policy::local,
          .cls = cc::connection};
}
// Forwarded standalone, refused in cluster mode (see the header's rationale).
constexpr command_info nocl(cc cls, std::string_view name) {
  return {.name = name,
          .policy = cmd_policy::forward,
          .cluster = cluster_policy::unsupported,
          .cls = cls};
}
// Rejected outright. Classed `other` rather than by what the command would
// have done: a refused BLPOP never wrote anything, and calling it a write
// would put it in the same series as the writes that did land.
constexpr command_info rej(std::string_view name) {
  return {.name = name,
          .policy = cmd_policy::reject,
          .cluster = cluster_policy::unsupported,
          .cls = cc::other};
}
constexpr command_info loc(std::string_view name) {
  return {.name = name,
          .policy = cmd_policy::local,
          .cluster = cluster_policy::local,
          .cls = cc::connection};
}

// Sorted by name (static_assert below). Key specs follow valkey's own COMMAND
// output. Commands whose key positions need variadic parsing to pin down
// (SORT's STORE, GEORADIUS's STORE, XREAD's STREAMS) are `nocl` rather than
// guessed: a wrong guess routes to the wrong node and returns an answer that
// is semantically wrong but looks fine.
constexpr std::array kCommands = {
    fwd(cc::write, "APPEND", 1, 1, 1),
    rej("AUTH"),
    fwd(cc::read, "BITCOUNT", 1, 1, 1),
    fwd(cc::write, "BITFIELD", 1, 1, 1),
    fwd(cc::write, "BITOP", 2, -1, 1),
    fwd(cc::read, "BITPOS", 1, 1, 1),
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
    any(cc::admin, "COMMAND"),
    any(cc::admin, "CONFIG"),
    fwd(cc::write, "COPY", 1, 2, 1),
    nocl(cc::read, "DBSIZE"),
    any(cc::admin, "DEBUG"),
    fwd(cc::write, "DECR", 1, 1, 1),
    fwd(cc::write, "DECRBY", 1, 1, 1),
    fwd(cc::write, "DEL", 1, -1, 1),
    rej("DISCARD"),
    fwd(cc::read, "DUMP", 1, 1, 1),
    self("ECHO"),
    nk(cc::write, "EVAL", 2),
    nk(cc::write, "EVALSHA", 2),
    nk(cc::read, "EVALSHA_RO", 2),
    nk(cc::read, "EVAL_RO", 2),
    rej("EXEC"),
    fwd(cc::read, "EXISTS", 1, -1, 1),
    fwd(cc::write, "EXPIRE", 1, 1, 1),
    fwd(cc::write, "EXPIREAT", 1, 1, 1),
    fwd(cc::read, "EXPIRETIME", 1, 1, 1),
    nk(cc::write, "FCALL", 2),
    nk(cc::read, "FCALL_RO", 2),
    nocl(cc::write, "FLUSHALL"),
    nocl(cc::write, "FLUSHDB"),
    nocl(cc::admin, "FUNCTION"),  // LOAD would have to reach every master
    fwd(cc::write, "GEOADD", 1, 1, 1),
    fwd(cc::read, "GEODIST", 1, 1, 1),
    fwd(cc::read, "GEOHASH", 1, 1, 1),
    fwd(cc::read, "GEOPOS", 1, 1, 1),
    nocl(cc::write, "GEORADIUS"),
    nocl(cc::write, "GEORADIUSBYMEMBER"),
    nocl(cc::read, "GEORADIUSBYMEMBER_RO"),
    nocl(cc::read, "GEORADIUS_RO"),
    fwd(cc::read, "GEOSEARCH", 1, 1, 1),
    fwd(cc::write, "GEOSEARCHSTORE", 1, 2, 1),
    fwd(cc::read, "GET", 1, 1, 1),
    fwd(cc::read, "GETBIT", 1, 1, 1),
    fwd(cc::write, "GETDEL", 1, 1, 1),
    fwd(cc::write, "GETEX", 1, 1, 1),
    fwd(cc::read, "GETRANGE", 1, 1, 1),
    fwd(cc::write, "GETSET", 1, 1, 1),
    fwd(cc::write, "HDEL", 1, 1, 1),
    loc("HELLO"),
    fwd(cc::read, "HEXISTS", 1, 1, 1),
    fwd(cc::read, "HGET", 1, 1, 1),
    fwd(cc::read, "HGETALL", 1, 1, 1),
    fwd(cc::write, "HINCRBY", 1, 1, 1),
    fwd(cc::write, "HINCRBYFLOAT", 1, 1, 1),
    fwd(cc::read, "HKEYS", 1, 1, 1),
    fwd(cc::read, "HLEN", 1, 1, 1),
    fwd(cc::read, "HMGET", 1, 1, 1),
    fwd(cc::write, "HMSET", 1, 1, 1),
    fwd(cc::read, "HRANDFIELD", 1, 1, 1),
    fwd(cc::read, "HSCAN", 1, 1, 1),
    fwd(cc::write, "HSET", 1, 1, 1),
    fwd(cc::write, "HSETNX", 1, 1, 1),
    fwd(cc::read, "HSTRLEN", 1, 1, 1),
    fwd(cc::read, "HVALS", 1, 1, 1),
    fwd(cc::write, "INCR", 1, 1, 1),
    fwd(cc::write, "INCRBY", 1, 1, 1),
    fwd(cc::write, "INCRBYFLOAT", 1, 1, 1),
    any(cc::admin, "INFO"),
    nocl(cc::read, "KEYS"),
    any(cc::admin, "LASTSAVE"),
    fwd(cc::read, "LCS", 1, 2, 1),
    fwd(cc::read, "LINDEX", 1, 1, 1),
    fwd(cc::write, "LINSERT", 1, 1, 1),
    fwd(cc::read, "LLEN", 1, 1, 1),
    fwd(cc::write, "LMOVE", 1, 2, 1),
    nk(cc::write, "LMPOP", 1),
    any(cc::admin, "LOLWUT"),
    fwd(cc::write, "LPOP", 1, 1, 1),
    fwd(cc::read, "LPOS", 1, 1, 1),
    fwd(cc::write, "LPUSH", 1, 1, 1),
    fwd(cc::write, "LPUSHX", 1, 1, 1),
    fwd(cc::read, "LRANGE", 1, 1, 1),
    fwd(cc::write, "LREM", 1, 1, 1),
    fwd(cc::write, "LSET", 1, 1, 1),
    fwd(cc::write, "LTRIM", 1, 1, 1),
    any(cc::admin, "MEMORY"),
    fwd(cc::read, "MGET", 1, -1, 1),
    rej("MONITOR"),
    fwd(cc::write, "MSET", 1, -1, 2),
    fwd(cc::write, "MSETNX", 1, -1, 2),
    rej("MULTI"),
    fwd(cc::read, "OBJECT", 2, 2, 1),
    fwd(cc::write, "PERSIST", 1, 1, 1),
    fwd(cc::write, "PEXPIRE", 1, 1, 1),
    fwd(cc::write, "PEXPIREAT", 1, 1, 1),
    fwd(cc::read, "PEXPIRETIME", 1, 1, 1),
    fwd(cc::write, "PFADD", 1, 1, 1),
    fwd(cc::read, "PFCOUNT", 1, -1, 1),
    fwd(cc::write, "PFMERGE", 1, -1, 1),
    self("PING"),
    fwd(cc::write, "PSETEX", 1, 1, 1),
    rej("PSUBSCRIBE"),
    fwd(cc::read, "PTTL", 1, 1, 1),
    any(cc::other, "PUBLISH"),
    any(cc::other, "PUBSUB"),
    rej("PUNSUBSCRIBE"),
    loc("QUIT"),
    nocl(cc::read, "RANDOMKEY"),
    fwd(cc::write, "RENAME", 1, 2, 1),
    fwd(cc::write, "RENAMENX", 1, 2, 1),
    loc("RESET"),
    fwd(cc::write, "RESTORE", 1, 1, 1),
    fwd(cc::write, "RPOP", 1, 1, 1),
    fwd(cc::write, "RPOPLPUSH", 1, 2, 1),
    fwd(cc::write, "RPUSH", 1, 1, 1),
    fwd(cc::write, "RPUSHX", 1, 1, 1),
    fwd(cc::write, "SADD", 1, 1, 1),
    nocl(cc::read, "SCAN"),
    fwd(cc::read, "SCARD", 1, 1, 1),
    nocl(cc::admin, "SCRIPT"),  // LOAD would have to reach every master
    fwd(cc::read, "SDIFF", 1, -1, 1),
    fwd(cc::write, "SDIFFSTORE", 1, -1, 1),
    loc("SELECT"),
    fwd(cc::write, "SET", 1, 1, 1),
    fwd(cc::write, "SETBIT", 1, 1, 1),
    fwd(cc::write, "SETEX", 1, 1, 1),
    fwd(cc::write, "SETNX", 1, 1, 1),
    fwd(cc::write, "SETRANGE", 1, 1, 1),
    fwd(cc::read, "SINTER", 1, -1, 1),
    nk(cc::read, "SINTERCARD", 1),
    fwd(cc::write, "SINTERSTORE", 1, -1, 1),
    fwd(cc::read, "SISMEMBER", 1, 1, 1),
    any(cc::admin, "SLOWLOG"),
    fwd(cc::read, "SMEMBERS", 1, 1, 1),
    fwd(cc::read, "SMISMEMBER", 1, 1, 1),
    fwd(cc::write, "SMOVE", 1, 2, 1),
    nocl(cc::write, "SORT"),  // the STORE destination hides behind variadic options
    nocl(cc::read, "SORT_RO"),
    fwd(cc::write, "SPOP", 1, 1, 1),
    fwd(cc::other, "SPUBLISH", 1, 1, 1),
    fwd(cc::read, "SRANDMEMBER", 1, 1, 1),
    fwd(cc::write, "SREM", 1, 1, 1),
    fwd(cc::read, "SSCAN", 1, 1, 1),
    rej("SSUBSCRIBE"),
    fwd(cc::read, "STRLEN", 1, 1, 1),
    rej("SUBSCRIBE"),
    fwd(cc::read, "SUBSTR", 1, 1, 1),
    fwd(cc::read, "SUNION", 1, -1, 1),
    fwd(cc::write, "SUNIONSTORE", 1, -1, 1),
    rej("SUNSUBSCRIBE"),
    any(cc::admin, "TIME"),
    fwd(cc::read, "TOUCH", 1, -1, 1),
    fwd(cc::read, "TTL", 1, 1, 1),
    fwd(cc::read, "TYPE", 1, 1, 1),
    fwd(cc::write, "UNLINK", 1, -1, 1),
    rej("UNSUBSCRIBE"),
    rej("UNWATCH"),
    rej("WAIT"),
    rej("WAITAOF"),
    rej("WATCH"),
    fwd(cc::write, "XACK", 1, 1, 1),
    fwd(cc::write, "XADD", 1, 1, 1),
    fwd(cc::write, "XAUTOCLAIM", 1, 1, 1),
    fwd(cc::write, "XCLAIM", 1, 1, 1),
    fwd(cc::write, "XDEL", 1, 1, 1),
    fwd(cc::write, "XGROUP", 2, 2, 1),
    fwd(cc::read, "XINFO", 2, 2, 1),
    fwd(cc::read, "XLEN", 1, 1, 1),
    fwd(cc::read, "XPENDING", 1, 1, 1),
    fwd(cc::read, "XRANGE", 1, 1, 1),
    nocl(cc::read, "XREAD"),  // keys start after the STREAMS token
    nocl(cc::read, "XREADGROUP"),
    fwd(cc::read, "XREVRANGE", 1, 1, 1),
    fwd(cc::write, "XSETID", 1, 1, 1),
    fwd(cc::write, "XTRIM", 1, 1, 1),
    fwd(cc::write, "ZADD", 1, 1, 1),
    fwd(cc::read, "ZCARD", 1, 1, 1),
    fwd(cc::read, "ZCOUNT", 1, 1, 1),
    nk(cc::read, "ZDIFF", 1),
    fwd_nk(cc::write, "ZDIFFSTORE", 1, 1, 1, 2),
    fwd(cc::write, "ZINCRBY", 1, 1, 1),
    nk(cc::read, "ZINTER", 1),
    nk(cc::read, "ZINTERCARD", 1),
    fwd_nk(cc::write, "ZINTERSTORE", 1, 1, 1, 2),
    nk(cc::write, "ZMPOP", 1),
    fwd(cc::read, "ZMSCORE", 1, 1, 1),
    fwd(cc::write, "ZPOPMAX", 1, 1, 1),
    fwd(cc::write, "ZPOPMIN", 1, 1, 1),
    fwd(cc::read, "ZRANDMEMBER", 1, 1, 1),
    fwd(cc::read, "ZRANGE", 1, 1, 1),
    fwd(cc::read, "ZRANGEBYLEX", 1, 1, 1),
    fwd(cc::read, "ZRANGEBYSCORE", 1, 1, 1),
    fwd(cc::write, "ZRANGESTORE", 1, 2, 1),
    fwd(cc::read, "ZRANK", 1, 1, 1),
    fwd(cc::write, "ZREM", 1, 1, 1),
    fwd(cc::write, "ZREMRANGEBYLEX", 1, 1, 1),
    fwd(cc::write, "ZREMRANGEBYRANK", 1, 1, 1),
    fwd(cc::write, "ZREMRANGEBYSCORE", 1, 1, 1),
    fwd(cc::read, "ZREVRANGE", 1, 1, 1),
    fwd(cc::read, "ZREVRANGEBYLEX", 1, 1, 1),
    fwd(cc::read, "ZREVRANGEBYSCORE", 1, 1, 1),
    fwd(cc::read, "ZREVRANK", 1, 1, 1),
    fwd(cc::read, "ZSCAN", 1, 1, 1),
    fwd(cc::read, "ZSCORE", 1, 1, 1),
    nk(cc::read, "ZUNION", 1),
    fwd_nk(cc::write, "ZUNIONSTORE", 1, 1, 1, 2),
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
