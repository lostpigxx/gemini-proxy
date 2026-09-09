// One pooled backend connection: write coalescing, FIFO request/response
// pairing, reconnect with backoff, timeouts and idle health checks.
// Design: docs/design/m3-workers-pool-pipelining.md §2–§3.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <string_view>

#include "core/buffer.hpp"
#include "core/byte_queue.hpp"
#include "io/event_loop.hpp"
#include "io/socket.hpp"
#include "io/task.hpp"
#include "io/wait_queue.hpp"
#include "resp/parser.hpp"

namespace vkp::proxy {

// Proxy-generated replies (shared by backend_conn and the client logic).
inline constexpr std::string_view kErrBackendUnavailable = "-ERR proxy: backend unavailable\r\n";
inline constexpr std::string_view kErrBackendLost = "-ERR proxy: backend connection lost\r\n";
inline constexpr std::string_view kErrTimeout = "-ERR proxy timeout\r\n";

// Where a paired response goes. Implemented by the client connection;
// deliver() must only buffer (never block, never destroy the sink
// synchronously) — it is called from the backend connection's driver.
//
// `token` is the value the sink handed to enqueue_forward(). Under cluster
// routing one client's requests fan out to several connections, so responses
// come back with no ordering relation between them; the token lets the sink
// put them back in request order (design m4 §1). This connection itself stays
// completely unaware of the ordering — it only echoes the token back.
class reply_sink {
 public:
  virtual void deliver(std::uint64_t token, std::string_view frame) = 0;

 protected:
  ~reply_sink() = default;
};

struct backend_conn_config {
  std::size_t max_inflight = 1024;   // entries per connection (backpressure)
  std::size_t max_outbuf = 1 << 20;  // request bytes buffered (backpressure)
  std::chrono::milliseconds connect_timeout{1000};
  std::chrono::milliseconds request_timeout{1000};  // from enqueue to response
  std::chrono::milliseconds health_interval{5000};  // idle PING period
  std::chrono::milliseconds backoff_base{50};
  std::chrono::milliseconds backoff_max{2000};
};

// Reads until one complete RESP frame sits at the front of `buf`; shared by
// the client reader and the backend driver.
struct frame_result {
  enum class kind : std::uint8_t { frame, eof, io_error, protocol_error };
  kind k = kind::frame;
  std::size_t len = 0;                             // frame bytes at the buffer front
  std::int32_t err = 0;                            // -errno for io_error
  resp::parse_errc perr = resp::parse_errc::none;  // for protocol_error
};

[[nodiscard]] io::task<frame_result> read_frame(io::event_loop& loop, int fd, read_buffer& buf,
                                                resp::parser& parser,
                                                io::cancel_slot* slot = nullptr);

// Single-threaded; owned by the worker's server. Lifecycle: construct →
// start() → (begin_drain() → finished()) → destroy after the loop ran dry.
class backend_conn {
 public:
  backend_conn(io::event_loop& loop, const io::resolved_addr& addr, const backend_conn_config& cfg);

  backend_conn(const backend_conn&) = delete;
  backend_conn& operator=(const backend_conn&) = delete;

  void start();  // spawns driver / writer / watchdog, begins connecting

  // Requests can be enqueued while connecting or connected (queued bytes
  // flush on connect; the connect timeout bounds the wait). Down between
  // backoff retries → callers reply kErrBackendUnavailable themselves.
  [[nodiscard]] bool available() const noexcept {
    return !draining_ && (state_ == state::connecting || state_ == state::connected);
  }
  [[nodiscard]] bool has_capacity() const noexcept {
    return inflight_.size() < cfg_.max_inflight && out_.size() < cfg_.max_outbuf;
  }
  // Notified whenever capacity may have been freed or the connection state
  // changed; waiters re-check available()/has_capacity(). A negative wait()
  // result means the loop is shutting down.
  [[nodiscard]] io::wait_queue& capacity_event() noexcept { return capacity_; }

  // Copies `frame` into the outbound queue and appends a FIFO pairing entry.
  // The response comes back as sink.deliver(token, ...). `sink` must stay
  // valid until delivery or detach().
  //
  // Pre: available(). The client hot path also checks has_capacity() first and
  // parks when it is gone; the redirect path (design m4 §5) deliberately skips
  // that check, because deliver() runs inside this connection's driver and
  // cannot park. The overshoot is bounded by the requests already in flight.
  void enqueue_forward(reply_sink& sink, std::uint64_t token, std::string_view frame);

  // Same, for a request whose reply the proxy discards (ASKING before a
  // redirected command). Pairing still consumes one backend frame.
  void enqueue_internal(std::string_view frame);

  // Tombstones every queued entry pointing at `sink` (client went away;
  // entries must stay for pairing, their responses are discarded).
  void detach(reply_sink& sink) noexcept;

  // Stops reconnecting, cancels in-flight ops, fails the queue. Coroutines
  // unwind asynchronously; poll finished()/wait on finished_event().
  void begin_drain() noexcept;
  [[nodiscard]] bool finished() const noexcept { return live_coroutines_ == 0; }
  [[nodiscard]] io::wait_queue& finished_event() noexcept { return finished_; }

  [[nodiscard]] std::size_t inflight() const noexcept { return inflight_.size(); }
  [[nodiscard]] bool connected() const noexcept { return state_ == state::connected; }

 private:
  enum class state : std::uint8_t { down, connecting, connected };

  // A pure transport queue: every entry consumes exactly one backend frame.
  // Proxy-generated replies never enter here — the client orders those itself
  // through its own reply slots (design m4 §1).
  struct entry {
    reply_sink* sink;  // nullptr: discard (internal health PING / tombstone)
    std::uint64_t token;
    std::chrono::steady_clock::time_point deadline;
  };

  io::task<void> driver();    // reconnect loop + response reader/pairing
  io::task<void> writer();    // flushes out_ while connected
  io::task<void> watchdog();  // connect/request timeouts + idle health PING

  io::task<void> read_responses();  // one connected session; returns on error/kill
  io::task<bool> backoff_wait();    // false: drain/stop, exit the driver loop
  void teardown_session() noexcept;
  void push(reply_sink* sink, std::uint64_t token, std::string_view frame);
  void deliver_head(std::string_view frame);
  void fail_all() noexcept;
  void poke_watchdog() noexcept;

  io::event_loop& loop_;
  const io::resolved_addr& addr_;
  backend_conn_config cfg_;

  io::unique_fd fd_;
  state state_ = state::down;
  bool draining_ = false;
  bool session_was_connected_ = false;  // unavailable vs lost error wording
  bool head_timed_out_ = false;
  bool writer_sending_ = false;

  std::deque<entry> inflight_;
  byte_queue out_;
  read_buffer in_;
  resp::parser parser_;

  std::chrono::steady_clock::time_point connect_deadline_{};
  std::chrono::steady_clock::time_point last_activity_{};
  std::chrono::milliseconds backoff_;

  io::wait_queue capacity_;
  io::wait_queue out_ready_;      // writer parks here
  io::wait_queue writer_parked_;  // driver joins the writer's send batch
  io::wait_queue wake_watchdog_;  // watchdog parks here when nothing to time
  io::wait_queue finished_;

  io::cancel_slot connect_slot_;
  io::cancel_slot recv_slot_;
  io::cancel_slot send_slot_;
  io::cancel_slot tick_slot_;
  io::cancel_slot backoff_slot_;

  int live_coroutines_ = 0;
};

}  // namespace vkp::proxy
