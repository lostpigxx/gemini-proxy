#include "proxy/backend_conn.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <utility>

namespace vkp::proxy {

namespace {

constexpr std::string_view kHealthPing = "*1\r\n$4\r\nPING\r\n";

}  // namespace

io::task<frame_result> read_frame(io::event_loop& loop, int fd, read_buffer& buf,
                                  resp::parser& parser, io::cancel_slot* slot) {
  for (;;) {
    if (buf.readable_bytes() > 0) {
      switch (parser.parse(buf.readable())) {
        case resp::parse_status::complete:
          co_return frame_result{.k = frame_result::kind::frame,
                                 .len = parser.message().raw.size()};
        case resp::parse_status::protocol_error:
          co_return frame_result{.k = frame_result::kind::protocol_error, .perr = parser.error()};
        case resp::parse_status::need_more:
          break;
      }
    }
    const std::span<char> w = buf.prepare(4096);
    const std::int32_t n = co_await loop.async_recv(fd, w, slot);
    if (n == 0) {
      co_return frame_result{.k = frame_result::kind::eof};
    }
    if (n < 0) {
      co_return frame_result{.k = frame_result::kind::io_error, .err = n};
    }
    buf.commit(static_cast<std::size_t>(n));
  }
}

backend_conn::backend_conn(io::event_loop& loop, const io::resolved_addr& addr,
                           const backend_conn_config& cfg)
    : loop_(loop),
      addr_(addr),
      cfg_(cfg),
      backoff_(cfg.backoff_base),
      capacity_(loop),
      out_ready_(loop),
      writer_parked_(loop),
      wake_watchdog_(loop),
      finished_(loop) {}

void backend_conn::start() {
  io::spawn(driver());
  io::spawn(writer());
  io::spawn(watchdog());
}

void backend_conn::enqueue_forward(reply_sink& sink, std::string_view frame) {
  assert(available() && has_capacity());
  const bool was_empty = inflight_.empty();
  const auto now = std::chrono::steady_clock::now();
  out_.append(frame);
  inflight_.push_back(
      {.sink = &sink, .local = false, .local_reply = {}, .deadline = now + cfg_.request_timeout});
  last_activity_ = now;
  out_ready_.notify_one();
  if (was_empty) {
    poke_watchdog();  // switch from idle/health timing to the head deadline
  }
}

void backend_conn::enqueue_local(reply_sink& sink, std::string reply) {
  inflight_.push_back(
      {.sink = &sink, .local = true, .local_reply = std::move(reply), .deadline = {}});
  drain_local_heads();  // delivers immediately when it landed at the head
}

void backend_conn::detach(reply_sink& sink) noexcept {
  for (entry& e : inflight_) {
    if (e.sink == &sink) {
      e.sink = nullptr;
    }
  }
}

void backend_conn::begin_drain() noexcept {
  if (draining_) {
    return;
  }
  draining_ = true;
  loop_.cancel(connect_slot_);
  loop_.cancel(recv_slot_);
  if (writer_sending_) {
    loop_.cancel(send_slot_);
  }
  loop_.cancel(tick_slot_);
  loop_.cancel(backoff_slot_);
  out_ready_.notify_all(-ECANCELED);
  wake_watchdog_.notify_all(-ECANCELED);
  capacity_.notify_all(-ECANCELED);
}

void backend_conn::poke_watchdog() noexcept {
  loop_.cancel(tick_slot_);  // wake a sleeping watchdog so it recomputes
  wake_watchdog_.notify_all();
}

void backend_conn::deliver_head(std::string_view frame) {
  entry e = std::move(inflight_.front());
  inflight_.pop_front();
  if (e.sink != nullptr) {
    e.sink->deliver(frame);
  }
  last_activity_ = std::chrono::steady_clock::now();
  drain_local_heads();
  if (inflight_.empty()) {
    poke_watchdog();  // switch from the (stale) head deadline to idle timing
  }
  capacity_.notify_all();
}

void backend_conn::drain_local_heads() {
  while (!inflight_.empty() && inflight_.front().local) {
    entry e = std::move(inflight_.front());
    inflight_.pop_front();
    if (e.sink != nullptr) {
      e.sink->deliver(e.local_reply);
    }
  }
}

void backend_conn::fail_all() noexcept {
  const auto now = std::chrono::steady_clock::now();
  const std::string_view fwd_error =
      session_was_connected_ ? kErrBackendLost : kErrBackendUnavailable;
  while (!inflight_.empty()) {
    entry e = std::move(inflight_.front());
    inflight_.pop_front();
    if (e.sink == nullptr) {
      continue;
    }
    if (e.local) {
      e.sink->deliver(e.local_reply);
    } else {
      const bool timed = head_timed_out_ && e.deadline <= now;
      e.sink->deliver(timed ? kErrTimeout : fwd_error);
    }
  }
  head_timed_out_ = false;
  capacity_.notify_all();
}

io::task<bool> backend_conn::backoff_wait() {
  const std::int32_t r = co_await loop_.sleep_for(backoff_, &backoff_slot_);
  backoff_slot_.reset();
  if (r < 0 || draining_) {
    co_return false;
  }
  backoff_ = std::min(backoff_ * 2, cfg_.backoff_max);
  co_return true;
}

io::task<void> backend_conn::read_responses() {
  for (;;) {
    const frame_result r = co_await read_frame(loop_, fd_.get(), in_, parser_, &recv_slot_);
    if (r.k != frame_result::kind::frame) {
      co_return;  // eof / io error / -ECANCELED (timeout kill, drain) / garbage
    }
    if (inflight_.empty() || inflight_.front().local) {
      co_return;  // unsolicited frame: backend out of sync, rebuild
    }
    deliver_head(in_.readable().substr(0, r.len));
    in_.consume(r.len);
  }
}

io::task<void> backend_conn::driver() {
  ++live_coroutines_;
  while (!draining_) {
    // ---- connect (bounded by the watchdog via connect_slot_) ----
    state_ = state::connecting;
    session_was_connected_ = false;
    connect_deadline_ = std::chrono::steady_clock::now() + cfg_.connect_timeout;
    poke_watchdog();

    io::unique_fd s{::socket(addr_.addr.ss_family, SOCK_STREAM, IPPROTO_TCP)};
    std::int32_t rc = -errno;
    if (s) {
      io::setup_stream_socket(s.get());
      rc = co_await loop_.async_connect(s.get(), reinterpret_cast<const sockaddr*>(&addr_.addr),
                                        addr_.len, &connect_slot_);
    }
    connect_slot_.reset();
    if (draining_) {
      break;
    }
    if (rc < 0) {
      s.reset();
      state_ = state::down;
      poke_watchdog();
      fail_all();  // requests queued while we were connecting
      if (!co_await backoff_wait()) {
        break;
      }
      continue;
    }

    // ---- connected session ----
    fd_ = std::move(s);
    state_ = state::connected;
    session_was_connected_ = true;
    backoff_ = cfg_.backoff_base;
    last_activity_ = std::chrono::steady_clock::now();
    poke_watchdog();
    out_ready_.notify_all();  // flush bytes queued during the connect
    capacity_.notify_all();

    co_await read_responses();
    teardown_session();
    // Cancel-before-close: never close an fd with an op still in flight
    // (reactor slots would dangle). The writer clears writer_sending_ and
    // notifies once its cancelled send resumed.
    while (writer_sending_) {
      (void)co_await writer_parked_.wait();
    }
    fail_all();
    out_.clear();
    fd_.reset();
    if (draining_) {
      break;
    }
    if (!co_await backoff_wait()) {
      break;
    }
  }

  // Drain path: same cancel-before-close discipline, then fail leftovers.
  state_ = state::down;
  while (writer_sending_) {
    (void)co_await writer_parked_.wait();
  }
  fail_all();
  out_.clear();
  fd_.reset();
  --live_coroutines_;
  finished_.notify_all();
}

void backend_conn::teardown_session() noexcept {
  state_ = state::down;
  poke_watchdog();
  recv_slot_.reset();
  if (writer_sending_) {
    loop_.cancel(send_slot_);
  }
  parser_.reset();
  in_.consume(in_.readable_bytes());
}

io::task<void> backend_conn::writer() {
  ++live_coroutines_;
  for (;;) {
    if (draining_) {
      break;
    }
    if (state_ != state::connected || out_.empty()) {
      if (co_await out_ready_.wait() < 0) {
        break;
      }
      continue;
    }
    writer_sending_ = true;
    const std::string_view batch = out_.begin_send();
    const std::int32_t r = co_await io::send_all(loop_, fd_.get(), batch, &send_slot_);
    out_.end_send();
    writer_sending_ = false;
    send_slot_.reset();
    writer_parked_.notify_all();
    if (r < 0) {
      if (r != -ECANCELED && state_ == state::connected) {
        loop_.cancel(recv_slot_);  // driver notices and rebuilds
      }
      // Park until the driver reconnected (it notifies out_ready_) instead
      // of retrying on a dying fd.
      if (co_await out_ready_.wait() < 0) {
        break;
      }
      continue;
    }
    capacity_.notify_all();
  }
  --live_coroutines_;
  finished_.notify_all();
}

io::task<void> backend_conn::watchdog() {
  ++live_coroutines_;
  using clock = std::chrono::steady_clock;
  enum class why : std::uint8_t { none, connect, request, health };
  while (!draining_) {
    clock::time_point deadline{};
    why reason = why::none;
    if (state_ == state::connecting) {
      deadline = connect_deadline_;
      reason = why::connect;
    } else if (state_ == state::connected) {
      if (!inflight_.empty()) {
        deadline = inflight_.front().deadline;
        reason = why::request;
      } else {
        deadline = last_activity_ + cfg_.health_interval;
        reason = why::health;
      }
    }
    if (reason == why::none) {
      // Down: the driver's backoff timer owns the clock; park until poked.
      if (co_await wake_watchdog_.wait() < 0) {
        break;
      }
      continue;
    }

    const auto now = clock::now();
    if (deadline > now) {
      const std::int32_t r = co_await loop_.sleep_for(deadline - now, &tick_slot_);
      tick_slot_.reset();
      if (r < 0) {
        if (draining_ || loop_.stopping()) {
          break;
        }
        continue;  // poked: state changed, recompute
      }
    }

    // Deadline reached — revalidate, then act.
    const auto fired = clock::now();
    bool park_failed = false;
    switch (reason) {
      case why::connect:
        if (state_ == state::connecting && fired >= connect_deadline_) {
          loop_.cancel(connect_slot_);
          // Park until the driver moves on; avoids a hot revalidation loop.
          park_failed = co_await wake_watchdog_.wait() < 0;
        }
        break;
      case why::request:
        if (state_ == state::connected && !inflight_.empty() &&
            fired >= inflight_.front().deadline) {
          head_timed_out_ = true;
          loop_.cancel(recv_slot_);  // driver fails the queue and rebuilds
          park_failed = co_await wake_watchdog_.wait() < 0;
        }
        break;
      case why::health:
        if (state_ == state::connected && inflight_.empty() &&
            fired >= last_activity_ + cfg_.health_interval) {
          // Internal PING: paired FIFO like any request, response discarded;
          // its timeout rides the regular request path.
          out_.append(kHealthPing);
          inflight_.push_back({.sink = nullptr,
                               .local = false,
                               .local_reply = {},
                               .deadline = fired + cfg_.request_timeout});
          last_activity_ = fired;
          out_ready_.notify_one();
        }
        break;
      case why::none:
        break;
    }
    if (park_failed) {
      break;
    }
  }
  --live_coroutines_;
  finished_.notify_all();
}

}  // namespace vkp::proxy
