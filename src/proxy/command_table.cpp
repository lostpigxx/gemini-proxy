#include "proxy/command_table.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace vkp::proxy {

namespace {

// Shorthand for table rows.
constexpr command_info fwd(std::string_view name, std::int8_t first, std::int8_t last,
                           std::int8_t step) {
  return {.name = name,
          .policy = cmd_policy::forward,
          .first_key = first,
          .last_key = last,
          .key_step = step};
}
constexpr command_info fwd0(std::string_view name) {
  return {.name = name, .policy = cmd_policy::forward};
}
constexpr command_info rej(std::string_view name) {
  return {.name = name, .policy = cmd_policy::reject};
}
constexpr command_info loc(std::string_view name) {
  return {.name = name, .policy = cmd_policy::local};
}

// Sorted by name (static_assert below). Key specs follow valkey's own
// COMMAND output for the common cases; exotic variable specs (EVAL, XREAD,
// GEORADIUS STORE, SORT BY ...) stay out until M4 needs them.
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
    fwd0("COMMAND"),
    fwd0("CONFIG"),
    fwd("COPY", 1, 2, 1),
    fwd0("DBSIZE"),
    fwd0("DEBUG"),
    fwd("DECR", 1, 1, 1),
    fwd("DECRBY", 1, 1, 1),
    fwd("DEL", 1, -1, 1),
    rej("DISCARD"),
    fwd("DUMP", 1, 1, 1),
    fwd("ECHO", 0, 0, 0),
    rej("EXEC"),
    fwd("EXISTS", 1, -1, 1),
    fwd("EXPIRE", 1, 1, 1),
    fwd("EXPIREAT", 1, 1, 1),
    fwd("EXPIRETIME", 1, 1, 1),
    fwd0("FLUSHALL"),
    fwd0("FLUSHDB"),
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
    fwd0("INFO"),
    fwd0("KEYS"),
    fwd0("LASTSAVE"),
    fwd("LINDEX", 1, 1, 1),
    fwd("LINSERT", 1, 1, 1),
    fwd("LLEN", 1, 1, 1),
    fwd("LMOVE", 1, 2, 1),
    fwd("LMPOP", 0, 0, 0),
    fwd0("LOLWUT"),
    fwd("LPOP", 1, 1, 1),
    fwd("LPOS", 1, 1, 1),
    fwd("LPUSH", 1, 1, 1),
    fwd("LPUSHX", 1, 1, 1),
    fwd("LRANGE", 1, 1, 1),
    fwd("LREM", 1, 1, 1),
    fwd("LSET", 1, 1, 1),
    fwd("LTRIM", 1, 1, 1),
    fwd0("MEMORY"),
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
    fwd0("PING"),
    fwd("PSETEX", 1, 1, 1),
    rej("PSUBSCRIBE"),
    fwd("PTTL", 1, 1, 1),
    rej("PUNSUBSCRIBE"),
    loc("QUIT"),
    fwd0("RANDOMKEY"),
    fwd("RENAME", 1, 2, 1),
    fwd("RENAMENX", 1, 2, 1),
    loc("RESET"),
    fwd("RESTORE", 1, 1, 1),
    fwd("RPOP", 1, 1, 1),
    fwd("RPOPLPUSH", 1, 2, 1),
    fwd("RPUSH", 1, 1, 1),
    fwd("RPUSHX", 1, 1, 1),
    fwd("SADD", 1, 1, 1),
    fwd0("SCAN"),
    fwd("SCARD", 1, 1, 1),
    fwd("SDIFF", 1, -1, 1),
    fwd("SDIFFSTORE", 1, -1, 1),
    loc("SELECT"),
    fwd("SET", 1, 1, 1),
    fwd("SETBIT", 1, 1, 1),
    fwd("SETEX", 1, 1, 1),
    fwd("SETNX", 1, 1, 1),
    fwd("SETRANGE", 1, 1, 1),
    fwd("SINTER", 1, -1, 1),
    fwd("SINTERCARD", 0, 0, 0),
    fwd("SINTERSTORE", 1, -1, 1),
    fwd("SISMEMBER", 1, 1, 1),
    fwd0("SLOWLOG"),
    fwd("SMEMBERS", 1, 1, 1),
    fwd("SMISMEMBER", 1, 1, 1),
    fwd("SMOVE", 1, 2, 1),
    fwd("SPOP", 1, 1, 1),
    fwd("SRANDMEMBER", 1, 1, 1),
    fwd("SREM", 1, 1, 1),
    fwd("SSCAN", 1, 1, 1),
    rej("SSUBSCRIBE"),
    fwd("STRLEN", 1, 1, 1),
    rej("SUBSCRIBE"),
    fwd("SUNION", 1, -1, 1),
    fwd("SUNIONSTORE", 1, -1, 1),
    rej("SUNSUBSCRIBE"),
    fwd0("TIME"),
    fwd("TOUCH", 1, -1, 1),
    fwd("TTL", 1, 1, 1),
    fwd("TYPE", 1, 1, 1),
    fwd("UNLINK", 1, -1, 1),
    rej("UNSUBSCRIBE"),
    rej("UNWATCH"),
    rej("WAIT"),
    rej("WAITAOF"),
    rej("WATCH"),
    fwd("XADD", 1, 1, 1),
    fwd("XLEN", 1, 1, 1),
    fwd("XRANGE", 1, 1, 1),
    fwd("XREVRANGE", 1, 1, 1),
    fwd("ZADD", 1, 1, 1),
    fwd("ZCARD", 1, 1, 1),
    fwd("ZCOUNT", 1, 1, 1),
    fwd("ZINCRBY", 1, 1, 1),
    fwd("ZMSCORE", 1, 1, 1),
    fwd("ZPOPMAX", 1, 1, 1),
    fwd("ZPOPMIN", 1, 1, 1),
    fwd("ZRANDMEMBER", 1, 1, 1),
    fwd("ZRANGE", 1, 1, 1),
    fwd("ZRANGEBYLEX", 1, 1, 1),
    fwd("ZRANGEBYSCORE", 1, 1, 1),
    fwd("ZRANK", 1, 1, 1),
    fwd("ZREM", 1, 1, 1),
    fwd("ZREMRANGEBYLEX", 1, 1, 1),
    fwd("ZREMRANGEBYRANK", 1, 1, 1),
    fwd("ZREMRANGEBYSCORE", 1, 1, 1),
    fwd("ZREVRANGE", 1, 1, 1),
    fwd("ZREVRANGEBYSCORE", 1, 1, 1),
    fwd("ZREVRANK", 1, 1, 1),
    fwd("ZSCAN", 1, 1, 1),
    fwd("ZSCORE", 1, 1, 1),
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

}  // namespace vkp::proxy
