#include "proxy/router.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>

#include "resp/value.hpp"

namespace vkp::proxy {

namespace {

using namespace std::chrono_literals;

constexpr std::string_view kClusterShards = "*2\r\n$7\r\nCLUSTER\r\n$6\r\nSHARDS\r\n";

std::string address_of(std::string_view host, std::uint16_t port) {
  return fmt::format("{}:{}", host, port);
}

}  // namespace

endpoint parse_endpoint(std::string_view text) {
  // Rightmost colon so that bare IPv6 literals ("::1:6379") still split, and
  // bracketed forms ("[::1]:6379") keep their brackets out of the host.
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 == text.size()) {
    throw std::runtime_error{fmt::format("malformed endpoint '{}', want host:port", text)};
  }
  std::string_view host = text.substr(0, colon);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  const std::string_view port_text = text.substr(colon + 1);
  unsigned long value = 0;
  const auto* begin = port_text.data();
  const auto* end = begin + port_text.size();
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0 || value > 65535) {
    throw std::runtime_error{fmt::format("malformed endpoint '{}', bad port", text)};
  }
  return {.host = std::string{host}, .port = static_cast<std::uint16_t>(value)};
}

void router::probe::deliver(std::uint64_t /*token*/, std::string_view frame) {
  reply.assign(frame);
  done = true;
  wake->notify_all();
}

router::router(io::event_loop& loop, router_config cfg)
    : loop_(loop), cfg_(std::move(cfg)), probe_wake_(loop), finished_(loop) {
  if (cfg_.conns_per_node == 0) {
    cfg_.conns_per_node = 1;
  }
  seeds_.reserve(cfg_.cluster_seeds.size());
  for (const std::string& text : cfg_.cluster_seeds) {
    seeds_.push_back(parse_endpoint(text));  // fail fast on a bad CLI value
  }
  cluster_ = !seeds_.empty();
  if (cluster_) {
    return;  // the topology arrives from CLUSTER SHARDS
  }
  // Standalone: one node owning every slot, never refreshed.
  node_conns& only = ensure_node(cfg_.backend_host, cfg_.backend_port);
  cluster::topology t;
  const std::int16_t idx = t.add_node({}, cfg_.backend_host, cfg_.backend_port);
  t.assign(0, cluster::kSlotCount - 1, idx);
  topology_ = std::move(t);
  by_index_ = {&only};
}

router::~router() = default;

void router::start() {
  started_ = true;
  for (auto& [address, node] : pool_) {
    for (const auto& c : node.conns) {
      c->start();
    }
  }
  if (cluster_) {
    io::spawn(refresher());
    io::spawn(reaper());
  }
}

router::node_conns& router::ensure_node(std::string_view host, std::uint16_t port) {
  std::string key = address_of(host, port);
  const auto it = pool_.find(key);
  if (it != pool_.end()) {
    return it->second;
  }
  node_conns fresh;
  fresh.addr = io::resolve_tcp(std::string{host}, port);
  const auto [pos, inserted] = pool_.emplace(std::move(key), std::move(fresh));
  node_conns& node = pos->second;
  node.conns.reserve(cfg_.conns_per_node);
  for (std::size_t i = 0; i < cfg_.conns_per_node; ++i) {
    // node.addr is held by reference, which is why node_conns lives in a map
    // node and is never moved again.
    node.conns.push_back(std::make_shared<backend_conn>(loop_, node.addr, cfg_.backend));
    if (started_) {
      node.conns.back()->start();
    }
  }
  return node;
}

router::node_conns* router::try_ensure_node(std::string_view host, std::uint16_t port) noexcept {
  try {
    return &ensure_node(host, port);
  } catch (const std::exception&) {
    return nullptr;  // resolver failure: treat the node as unreachable
  }
}

routing router::pick_any_node() noexcept {
  const std::size_t n = by_index_.size();
  for (std::size_t tried = 0; tried < n; ++tried) {
    node_conns* node = by_index_[next_any_];
    next_any_ = next_any_ + 1 < n ? next_any_ + 1 : 0;
    if (node != nullptr && !node->conns.empty()) {
      return {.slot = &node->pick(), .st = routing::status::ok};
    }
  }
  return {.st = routing::status::unavailable};
}

routing router::route(std::span<const std::string_view> args, const command_info* info) noexcept {
  if (!cluster_) {
    return pick_any_node();  // the one node owns every slot; no hashing needed
  }
  if (info == nullptr) {
    return {.st = routing::status::unsupported};  // unknown command: never guess
  }
  switch (info->cluster) {
    case cluster_policy::unsupported:
    case cluster_policy::local:  // answered before routing; defensive
      return {.st = routing::status::unsupported};
    case cluster_policy::any_node:
      return pick_any_node();
    case cluster_policy::by_key:
      break;
  }

  const route_result rr = route_slot(args, *info);
  if (rr.k == route_result::kind::crossslot) {
    return {.st = routing::status::crossslot};
  }
  if (rr.k == route_result::kind::any_node) {
    return pick_any_node();  // arity is off; let the backend say so
  }
  const std::int16_t idx = topology_.owner_of(rr.slot);
  if (idx < 0 || static_cast<std::size_t>(idx) >= by_index_.size()) {
    return {.st = routing::status::unavailable};
  }
  node_conns* node = by_index_[static_cast<std::size_t>(idx)];
  if (node == nullptr || node->conns.empty()) {
    return {.st = routing::status::unavailable};
  }
  return {.slot = &node->pick(), .st = routing::status::ok};
}

routing router::node_at(std::string_view host, std::uint16_t port) {
  node_conns* node = try_ensure_node(host, port);
  if (node == nullptr || node->conns.empty()) {
    return {.st = routing::status::unavailable};
  }
  return {.slot = &node->pick(), .st = routing::status::ok};
}

void router::apply_moved(std::uint16_t slot, std::string_view host, std::uint16_t port) {
  node_conns* node = try_ensure_node(host, port);
  if (node == nullptr) {
    request_refresh();
    return;
  }
  const auto at = std::find(by_index_.begin(), by_index_.end(), node);
  std::size_t idx = static_cast<std::size_t>(at - by_index_.begin());
  if (at == by_index_.end()) {
    // add_node dedupes by address, so this also repairs an index whose pool
    // entry was missing (an earlier resolve failure that has since healed).
    idx = static_cast<std::size_t>(topology_.add_node({}, std::string{host}, port));
    by_index_.resize(topology_.nodes().size(), nullptr);
    by_index_[idx] = node;
  }
  topology_.assign(slot, slot, static_cast<std::int16_t>(idx));
  request_refresh();  // one MOVED usually means a whole batch of slots moved
}

void router::request_refresh() noexcept {
  if (!cluster_ || draining_) {
    return;
  }
  refresh_pending_ = true;
  loop_.cancel(refresh_nap_);  // cut the period short
}

void router::adopt(cluster::topology t) {
  std::vector<node_conns*> fresh;
  fresh.reserve(t.nodes().size());
  for (const cluster::node& n : t.nodes()) {
    fresh.push_back(try_ensure_node(n.host, n.port));
  }
  topology_ = std::move(t);
  by_index_ = std::move(fresh);
  next_any_ = 0;
  retire_unreferenced();
}

void router::retire_unreferenced() {
  for (auto it = pool_.begin(); it != pool_.end();) {
    const auto next = std::next(it);
    if (std::find(by_index_.begin(), by_index_.end(), &it->second) == by_index_.end()) {
      for (const auto& c : it->second.conns) {
        c->begin_drain();
      }
      // extract keeps the element at its address, so connections holding
      // `addr` by reference stay valid while they unwind.
      retired_.push_back(pool_.extract(it));
    }
    it = next;
  }
}

io::task<bool> router::nap(std::chrono::milliseconds d, io::cancel_slot& slot) {
  (void)co_await loop_.sleep_for(d, &slot);
  slot.reset();
  co_return !draining_ && !loop_.stopping();
}

io::task<bool> router::fetch_shards(backend_conn& c, std::string& out) {
  if (!c.available() || !c.has_capacity()) {
    co_return false;
  }
  probe p;
  p.wake = &probe_wake_;
  c.enqueue_forward(p, 0, kClusterShards);
  while (!p.done) {
    if (co_await probe_wake_.wait() < 0) {
      c.detach(p);  // tombstone: the reply lands after `p` is gone
      co_return false;
    }
  }
  out = std::move(p.reply);
  co_return true;
}

io::task<bool> router::refresh_once() {
  std::vector<node_conns*> candidates;
  candidates.reserve(std::max(by_index_.size(), seeds_.size()));
  for (node_conns* node : by_index_) {
    if (node != nullptr) {
      candidates.push_back(node);
    }
  }
  if (candidates.empty()) {
    for (const endpoint& seed : seeds_) {  // bootstrap
      if (node_conns* node = try_ensure_node(seed.host, seed.port); node != nullptr) {
        candidates.push_back(node);
      }
    }
  }

  std::string frame;
  for (node_conns* node : candidates) {
    for (const auto& c : node->conns) {
      if (draining_) {
        co_return false;
      }
      if (!co_await fetch_shards(*c, frame)) {
        continue;
      }
      const auto tree = resp::parse_tree(frame);
      if (!tree) {
        continue;  // truncated or not RESP at all
      }
      auto parsed = cluster::parse_cluster_shards(*tree);
      if (!parsed) {
        continue;  // an error reply, or a shape we refuse to half-apply
      }
      adopt(std::move(parsed.value));
      co_return true;
    }
  }
  co_return false;
}

io::task<void> router::refresher() {
  ++live_coroutines_;
  while (!draining_) {
    refresh_pending_ = false;
    const bool ok = co_await refresh_once();
    if (draining_) {
      break;
    }
    if (ok) {
      retry_ = 0ms;
      if (refresh_pending_) {
        continue;  // a MOVED landed while we were fetching; go again
      }
      if (!co_await nap(cfg_.refresh_interval, refresh_nap_)) {
        break;
      }
    } else {
      // Bootstrap or refresh failed: back off like the data connections do
      // rather than hammering an unreachable seed.
      retry_ = retry_ == 0ms ? cfg_.backend.backoff_base
                             : std::min(retry_ * 2, cfg_.backend.backoff_max);
      if (!co_await nap(retry_, refresh_nap_)) {
        break;
      }
    }
  }
  --live_coroutines_;
  finished_.notify_all();
}

io::task<void> router::reaper() {
  ++live_coroutines_;
  while (!draining_) {
    if (!co_await nap(1s, reap_nap_)) {
      break;
    }
    std::erase_if(retired_, [](pool_map::node_type& node) {
      return std::ranges::all_of(node.mapped().conns, [](const std::shared_ptr<backend_conn>& c) {
        // finished(): the coroutine frames have unwound (cancel-before-close).
        // use_count(): no client still holds it. Both are required.
        return c->finished() && c.use_count() == 1;
      });
    });
  }
  --live_coroutines_;
  finished_.notify_all();
}

void router::begin_drain() noexcept {
  if (draining_) {
    return;
  }
  draining_ = true;
  loop_.cancel(refresh_nap_);
  loop_.cancel(reap_nap_);
  probe_wake_.notify_all(-ECANCELED);
  for (auto& [address, node] : pool_) {
    for (const auto& c : node.conns) {
      c->begin_drain();
    }
  }
  for (auto& node : retired_) {
    for (const auto& c : node.mapped().conns) {
      c->begin_drain();
    }
  }
}

io::task<void> router::await_drained() {
  while (live_coroutines_ > 0) {
    if (co_await finished_.wait() < 0) {
      co_return;
    }
  }
  for (auto& [address, node] : pool_) {
    for (const auto& c : node.conns) {
      while (!c->finished()) {
        if (co_await c->finished_event().wait() < 0) {
          co_return;  // hard stop already underway
        }
      }
    }
  }
  for (auto& node : retired_) {
    for (const auto& c : node.mapped().conns) {
      while (!c->finished()) {
        if (co_await c->finished_event().wait() < 0) {
          co_return;
        }
      }
    }
  }
}

}  // namespace vkp::proxy
