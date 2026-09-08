#include "cluster/topology.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <system_error>
#include <utility>

#include "resp/value.hpp"

namespace vkp::cluster {

namespace {

bool ieq(std::string_view a, std::string_view b) noexcept {
  return std::ranges::equal(a, b, [](char x, char y) noexcept {
    return std::tolower(static_cast<unsigned char>(x)) ==
           std::tolower(static_cast<unsigned char>(y));
  });
}

bool is_text(const resp::value& v) noexcept {
  return v.type == resp::value::kind::bulk_string || v.type == resp::value::kind::simple_string ||
         v.type == resp::value::kind::verbatim_string;
}

// RESP2 sends each shard/node as a flat array of key/value pairs and RESP3 as
// a map; the value tree stores both the same way, so one lookup covers both.
const resp::value* field(const resp::value& obj, std::string_view name) noexcept {
  const auto& e = obj.elements;
  for (std::size_t i = 0; i + 1 < e.size(); i += 2) {
    if (is_text(e[i]) && e[i].text == name) {
      return &e[i + 1];
    }
  }
  return nullptr;
}

bool is_kv_aggregate(const resp::value& v) noexcept {
  return v.is_aggregate() && v.elements.size() % 2 == 0;
}

// Accepts both the integer form the server actually sends and a numeric
// string, so a proxy in front of a proxy does not trip over the encoding.
bool as_number(const resp::value& v, long long& out) noexcept {
  if (v.type == resp::value::kind::integer) {
    out = v.integer;
    return true;
  }
  if (!is_text(v)) {
    return false;
  }
  const auto* begin = v.text.data();
  const auto* end = begin + v.text.size();
  const auto parsed = std::from_chars(begin, end, out);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool as_slot(const resp::value& v, std::uint16_t& out) noexcept {
  long long n = 0;
  if (!as_number(v, n) || n < 0 || n >= kSlotCount) {
    return false;
  }
  out = static_cast<std::uint16_t>(n);
  return true;
}

bool as_port(const resp::value& v, std::uint16_t& out) noexcept {
  long long n = 0;
  if (!as_number(v, n) || n <= 0 || n > 65535) {
    return false;
  }
  out = static_cast<std::uint16_t>(n);
  return true;
}

// `endpoint` is the addressable name; `ip` is the fallback for servers that
// leave it empty or send the "?" placeholder.
std::string_view node_host(const resp::value& n) noexcept {
  if (const auto* ep = field(n, "endpoint");
      ep != nullptr && is_text(*ep) && !ep->text.empty() && ep->text != "?") {
    return ep->text;
  }
  if (const auto* ip = field(n, "ip"); ip != nullptr && is_text(*ip) && !ip->text.empty()) {
    return ip->text;
  }
  return {};
}

}  // namespace

std::string node::address() const {
  return host + ":" + std::to_string(port);
}

std::size_t topology::assigned_slots() const noexcept {
  return static_cast<std::size_t>(
      std::ranges::count_if(slots_, [](std::int16_t i) noexcept { return i != kUnassigned; }));
}

std::int16_t topology::add_node(std::string id, std::string host, std::uint16_t port) {
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (nodes_[i].port == port && nodes_[i].host == host) {
      return static_cast<std::int16_t>(i);
    }
  }
  if (nodes_.size() >= 0x7FFF) {
    return kUnassigned;
  }
  nodes_.push_back({.id = std::move(id), .host = std::move(host), .port = port});
  return static_cast<std::int16_t>(nodes_.size() - 1);
}

void topology::assign(std::uint16_t first, std::uint16_t last, std::int16_t index) noexcept {
  if (first >= kSlotCount || first > last) {
    return;
  }
  const std::uint16_t end = std::min<std::uint16_t>(last, kSlotCount - 1);
  for (std::uint32_t s = first; s <= end; ++s) {
    slots_[s] = index;
  }
}

std::string_view describe(topology_errc err) noexcept {
  switch (err) {
    case topology_errc::none:
      return "ok";
    case topology_errc::not_an_array:
      return "CLUSTER SHARDS reply is not an array";
    case topology_errc::bad_shard:
      return "malformed shard entry";
    case topology_errc::bad_slots:
      return "malformed slot range";
    case topology_errc::bad_node:
      return "malformed node entry";
    case topology_errc::no_slot_owner:
      return "no slot has a reachable master";
  }
  return "unknown error";
}

shards_result parse_cluster_shards(const resp::value& reply) {
  shards_result out;
  const auto fail = [&out](topology_errc err) {
    out.err = err;
    out.value = topology{};
    return out;
  };

  if (reply.type != resp::value::kind::array && reply.type != resp::value::kind::set) {
    return fail(topology_errc::not_an_array);
  }

  std::vector<std::pair<std::uint16_t, std::uint16_t>> ranges;
  for (const resp::value& shard : reply.elements) {
    if (!is_kv_aggregate(shard)) {
      return fail(topology_errc::bad_shard);
    }
    const auto* slots = field(shard, "slots");
    const auto* nodes = field(shard, "nodes");
    if (slots == nullptr || nodes == nullptr) {
      return fail(topology_errc::bad_shard);
    }
    if (!slots->is_aggregate() || slots->elements.size() % 2 != 0) {
      return fail(topology_errc::bad_slots);
    }
    if (slots->elements.empty()) {
      continue;  // an empty master: nothing routes there, so skip the node too
    }
    ranges.clear();
    for (std::size_t i = 0; i + 1 < slots->elements.size(); i += 2) {
      std::uint16_t first = 0;
      std::uint16_t last = 0;
      if (!as_slot(slots->elements[i], first) || !as_slot(slots->elements[i + 1], last) ||
          first > last) {
        return fail(topology_errc::bad_slots);
      }
      ranges.emplace_back(first, last);
    }

    if (!nodes->is_aggregate()) {
      return fail(topology_errc::bad_node);
    }
    const resp::value* master = nullptr;
    for (const resp::value& n : nodes->elements) {
      if (!is_kv_aggregate(n)) {
        return fail(topology_errc::bad_node);
      }
      const auto* role = field(n, "role");
      if (role == nullptr || !is_text(*role)) {
        return fail(topology_errc::bad_node);
      }
      if (!ieq(role->text, "master")) {
        continue;
      }
      // A missing health field is treated as online: older servers omit it.
      const auto* health = field(n, "health");
      if (health != nullptr && is_text(*health) && !ieq(health->text, "online")) {
        continue;
      }
      master = &n;
      break;
    }
    if (master == nullptr) {
      continue;  // failover in flight: leave these slots unassigned
    }

    const std::string_view host = node_host(*master);
    std::uint16_t port = 0;
    const auto* port_field = field(*master, "port");
    if (host.empty() || port_field == nullptr || !as_port(*port_field, port)) {
      return fail(topology_errc::bad_node);
    }
    std::string id;
    if (const auto* id_field = field(*master, "id"); id_field != nullptr && is_text(*id_field)) {
      id = std::string{id_field->text};
    }

    const std::int16_t index = out.value.add_node(std::move(id), std::string{host}, port);
    if (index == topology::kUnassigned) {
      return fail(topology_errc::bad_node);
    }
    for (const auto& [first, last] : ranges) {
      out.value.assign(first, last, index);
    }
  }

  if (out.value.assigned_slots() == 0) {
    return fail(topology_errc::no_slot_owner);
  }
  return out;
}

}  // namespace vkp::cluster
