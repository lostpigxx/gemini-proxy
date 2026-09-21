#include "proxy/worker_pool.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include <fmt/format.h>

#include "core/log.hpp"
#include "io/task.hpp"

namespace vkp::proxy {

struct worker_pool::worker {
  std::unique_ptr<io::event_loop> loop;
  std::unique_ptr<server> srv;
  int wake_read_fd = -1;
  int wake_write_fd = -1;
  std::string error;  // first exception escaping this worker's thread

  ~worker() {
    if (wake_read_fd >= 0) {
      ::close(wake_read_fd);
    }
    if (wake_write_fd >= 0) {
      ::close(wake_write_fd);
    }
  }
};

namespace {

// Self-pipe watcher: one byte = drain, next byte = force stop (server
// semantics). Exits with the loop (-ECANCELED).
io::task<void> shutdown_watcher(io::event_loop& loop, server& srv, int fd) {
  char buf[16];
  for (;;) {
    const std::int32_t n = co_await loop.async_recv(fd, buf);
    if (n <= 0) {
      co_return;
    }
    srv.begin_shutdown();
  }
}

void set_nonblocking(int fd) {
  (void)::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

// Pins worker i to the i-th CPU *this process is already allowed to run on*,
// not to CPU i. Numbering from zero ignores any mask we were started under, so
// `taskset -c 5-8 proxyd --cpu-affinity -w 4` used to ask for CPUs 0..3, get
// EINVAL from every call, and silently leave the workers floating — which is
// how a pinned benchmark still came out bistable (two workers sharing a core
// on some runs and not others, an 8% throughput swing between rounds).
//
// No-op off Linux — but still a call, so the flag is read on every platform
// (an #if around the call site instead would make cpu_affinity_ an unused
// private field on macOS).
void pin_threads([[maybe_unused]] bool enabled,
                 [[maybe_unused]] std::vector<std::thread>& threads) {
#if defined(__linux__)
  if (!enabled) {
    return;
  }
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
    VKP_LOG_WARN("cpu affinity: sched_getaffinity failed: {error}", std::strerror(errno));
    return;
  }
  std::vector<int> cpus;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &allowed)) {
      cpus.push_back(cpu);
    }
  }
  if (cpus.empty()) {
    return;
  }
  if (cpus.size() < threads.size()) {
    // Oversubscribed: pinning would stack workers on the same core with none
    // of them able to migrate off it. Leaving the mask alone is strictly
    // better, and silently doing so is how you end up debugging it twice.
    VKP_LOG_WARN("cpu affinity: {workers} worker(s) but only {cpus} cpu(s) available, not pinning",
                 threads.size(), cpus.size());
    return;
  }
  for (std::size_t i = 0; i < threads.size(); ++i) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpus[i], &set);
    const int rc = pthread_setaffinity_np(threads[i].native_handle(), sizeof(set), &set);
    if (rc != 0) {
      VKP_LOG_WARN("cpu affinity: worker {worker} -> cpu {cpu} failed: {error}", i, cpus[i],
                   std::strerror(rc));
    }
  }
  VKP_LOG_INFO("cpu affinity: {workers} worker(s) pinned, first cpu {cpu}", threads.size(),
               cpus.front());
#endif
}

}  // namespace

std::size_t worker_pool::resolve_workers(std::size_t requested) noexcept {
  return requested != 0 ? requested : std::max(1U, std::thread::hardware_concurrency());
}

worker_pool::worker_pool(options opt)
    : stats_(resolve_workers(opt.workers)), cpu_affinity_(opt.cpu_affinity) {
  const std::size_t n = stats_.workers();
  config cfg = std::move(opt.cfg);
  cfg.reuseport = n > 1;

  workers_.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    auto w = std::make_unique<worker>();
    w->loop = std::make_unique<io::event_loop>(opt.io_backend ? io::make_backend(*opt.io_backend)
                                                              : io::make_backend());
    w->srv = std::make_unique<server>(*w->loop, cfg, stats_.worker(i));
    if (i == 0) {
      cfg.listen_port = w->srv->port();  // ephemeral bind: the rest join it
    }

    int sv[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
      throw std::system_error(errno, std::generic_category(), "socketpair");
    }
    set_nonblocking(sv[0]);
    set_nonblocking(sv[1]);
    w->wake_read_fd = sv[0];
    w->wake_write_fd = sv[1];
    wake_write_fds_.push_back(sv[1]);

    workers_.push_back(std::move(w));
  }
}

worker_pool::~worker_pool() = default;

std::uint16_t worker_pool::port() const noexcept {
  return workers_.front()->srv->port();
}

std::string_view worker_pool::io_backend_name() const noexcept {
  return workers_.front()->loop->backend_name();
}

void worker_pool::request_shutdown() noexcept {
  const char byte = 1;
  for (const int fd : wake_write_fds_) {
    (void)!::write(fd, &byte, 1);
  }
}

void worker_pool::run() {
  std::vector<std::thread> threads;
  threads.reserve(workers_.size());
  for (const auto& wp : workers_) {
    worker* w = wp.get();
    threads.emplace_back([this, w] {
      try {
        w->srv->start();
        io::spawn(shutdown_watcher(*w->loop, *w->srv, w->wake_read_fd));
        w->loop->run();
      } catch (const std::exception& e) {
        w->error = e.what();
        request_shutdown();  // take the rest of the pool down gracefully
      }
    });
  }

  pin_threads(cpu_affinity_, threads);

  for (std::thread& t : threads) {
    t.join();
  }
  for (const auto& w : workers_) {
    if (!w->error.empty()) {
      throw std::runtime_error(fmt::format("worker failed: {}", w->error));
    }
  }
}

}  // namespace vkp::proxy
