// Per-worker node pool + routing. Owns every backend connection this worker
// has, decides which one a command goes to, and (in cluster mode) keeps the
// topology fresh. Design: docs/design/m4-cluster-routing.md §4.
//
// Standalone is modelled as the degenerate cluster: one node owning all
// 16384 slots that never refreshes. There is one routing entry point, and
// the standalone path never touches slot arithmetic.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cluster/topology.hpp"
#include "io/event_loop.hpp"
#include "io/socket.hpp"
#include "io/task.hpp"
#include "io/wait_queue.hpp"
#include "proxy/backend_conn.hpp"
#include "proxy/command_table.hpp"

namespace vkp::proxy {

inline constexpr std::string_view kErrNoTopology = "-ERR proxy: cluster topology unavailable\r\n";
inline constexpr std::string_view kErrCrossSlot =
    "-CROSSSLOT Keys in request don't hash to the same slot\r\n";
inline constexpr std::string_view kErrTooManyRedirects = "-ERR proxy: too many redirections\r\n";

struct router_config {
  std::string backend_host = "127.0.0.1";
  std::uint16_t backend_port = 6379;
  // Non-empty switches the router to cluster mode; entries are "host:port".
  std::vector<std::string> cluster_seeds;
  std::chrono::milliseconds refresh_interval{5000};
  std::size_t conns_per_node = 1;
  backend_conn_config backend;
};

// "host:port" → {host, port}. Throws std::runtime_error on a malformed
// endpoint. Shared with the CLI.
struct endpoint {
  std::string host;
  std::uint16_t port = 0;
};
[[nodiscard]] endpoint parse_endpoint(std::string_view text);

// Where a command should go. `slot` points at the pool's own shared_ptr, so
// resolving a route costs no refcount traffic; callers copy it only when
// they need to keep the connection alive across suspension points.
struct routing {
  enum class status : std::uint8_t {
    ok,
    crossslot,    // the command's keys span several slots
    unsupported,  // no sane cluster route (see cluster_policy::unsupported)
    unavailable,  // topology not ready yet
  };

  const std::shared_ptr<backend_conn>* slot = nullptr;
  status st = status::unavailable;

  [[nodiscard]] backend_conn* get() const noexcept {
    return slot != nullptr ? slot->get() : nullptr;
  }
};

class router {
 public:
  // Resolves addresses and builds the pool (throws std::runtime_error /
  // std::system_error); coroutines and connecting start on start().
  router(io::event_loop& loop, router_config cfg);
  ~router();

  router(const router&) = delete;
  router& operator=(const router&) = delete;

  void start();

  [[nodiscard]] bool cluster_mode() const noexcept { return cluster_; }
  // Only cluster backends emit MOVED/ASK. Clients use this to skip keeping a
  // retry copy of the request bytes on the standalone hot path.
  [[nodiscard]] bool may_redirect() const noexcept { return cluster_; }

  // Never blocks and never allocates on the standalone path.
  [[nodiscard]] routing route(std::span<const std::string_view> args,
                              const command_info* info) noexcept;

  // Any master will do (keyless admin commands, and every command in
  // standalone mode). Round-robins so one node does not take all of it.
  [[nodiscard]] routing pick_any_node() noexcept;

  // The pool entry for an explicit endpoint, creating and starting it when
  // the node is new. Used by the MOVED/ASK paths (§5).
  [[nodiscard]] routing node_at(std::string_view host, std::uint16_t port);

  // MOVED: repoint one slot immediately and schedule a debounced full
  // refresh, since one MOVED usually means many slots just moved.
  void apply_moved(std::uint16_t slot, std::string_view host, std::uint16_t port);

  // Asks the refresher to run now instead of at the next period.
  void request_refresh() noexcept;

  // Stops refreshing and drains every connection, live and retired.
  void begin_drain() noexcept;
  // Parks until the drain completed (or the loop hard-stopped).
  [[nodiscard]] io::task<void> await_drained();

  // Diagnostics / tests.
  [[nodiscard]] std::size_t node_count() const noexcept { return by_index_.size(); }
  [[nodiscard]] std::size_t retired_count() const noexcept { return retired_.size(); }
  [[nodiscard]] const cluster::topology& topology() const noexcept { return topology_; }

 private:
  // One backend node: its resolved address and the connections to it.
  // `backend_conn` holds `addr` by reference, so this object must never move
  // once a connection exists — it lives in a std::map node for that reason.
  struct node_conns {
    io::resolved_addr addr;
    std::vector<std::shared_ptr<backend_conn>> conns;
    std::size_t next = 0;

    [[nodiscard]] const std::shared_ptr<backend_conn>& pick() noexcept {
      const std::shared_ptr<backend_conn>& c = conns[next];
      next = next + 1 < conns.size() ? next + 1 : 0;
      return c;
    }
  };
  using pool_map = std::map<std::string, node_conns, std::less<>>;

  // Collects one control-plane reply. Rides an ordinary data connection, so
  // it inherits the timeout / reconnect / backpressure machinery for free.
  struct probe final : reply_sink {
    std::string reply;
    bool done = false;
    io::wait_queue* wake = nullptr;
    void deliver(std::uint64_t /*token*/, std::string_view frame) override;
  };

  // Throws on a resolver failure; try_ensure_node yields nullptr instead,
  // for the paths that run inside coroutines where throwing would terminate.
  node_conns& ensure_node(std::string_view host, std::uint16_t port);
  node_conns* try_ensure_node(std::string_view host, std::uint16_t port) noexcept;
  void adopt(cluster::topology t);
  void retire_unreferenced();

  io::task<void> refresher();
  io::task<void> reaper();
  // Sends CLUSTER SHARDS on `c` and parks until the reply lands. False when
  // the connection could not take it or we are shutting down.
  io::task<bool> fetch_shards(backend_conn& c, std::string& out);
  io::task<bool> refresh_once();  // true when a topology was adopted
  // Sleeps, interruptibly. False means stop; a bare poke returns true early.
  [[nodiscard]] io::task<bool> nap(std::chrono::milliseconds d, io::cancel_slot& slot);

  io::event_loop& loop_;
  router_config cfg_;
  std::vector<endpoint> seeds_;
  bool cluster_ = false;
  bool started_ = false;
  bool draining_ = false;
  bool refresh_pending_ = false;  // debounce flag set by request_refresh()

  pool_map pool_;
  std::vector<pool_map::node_type> retired_;  // drained, awaiting last owner
  cluster::topology topology_;
  std::vector<node_conns*> by_index_;  // parallel to topology_.nodes()
  std::size_t next_any_ = 0;
  std::chrono::milliseconds retry_{0};

  io::wait_queue probe_wake_;
  io::wait_queue finished_;
  io::cancel_slot refresh_nap_;
  io::cancel_slot reap_nap_;
  int live_coroutines_ = 0;
};

}  // namespace vkp::proxy
