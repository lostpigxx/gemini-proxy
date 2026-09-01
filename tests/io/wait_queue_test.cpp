#include "io/wait_queue.hpp"

#include <cerrno>
#include <chrono>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "io/event_loop.hpp"
#include "io/socket.hpp"
#include "io/task.hpp"

using namespace std::chrono_literals;
using vkp::io::cancel_slot;
using vkp::io::event_loop;
using vkp::io::spawn;
using vkp::io::task;
using vkp::io::wait_queue;

namespace {

task<void> waiter(wait_queue& q, char tag, std::string& order, std::vector<std::int32_t>& results) {
  const std::int32_t r = co_await q.wait();
  order.push_back(tag);
  results.push_back(r);
}

}  // namespace

TEST_CASE("wait_queue notify_one wakes in FIFO order with the given result", "[io][wait_queue]") {
  event_loop loop;
  wait_queue q{loop};
  std::string order;
  std::vector<std::int32_t> results;

  spawn(waiter(q, 'a', order, results));
  spawn(waiter(q, 'b', order, results));
  spawn(waiter(q, 'c', order, results));
  spawn([](event_loop& l, wait_queue& wq) -> task<void> {
    (void)co_await l.sleep_for(1ms);
    wq.notify_one(7);
    wq.notify_one();
    wq.notify_all();
  }(loop, q));

  loop.run();
  CHECK(order == "abc");
  REQUIRE(results.size() == 3);
  CHECK(results[0] == 7);
  CHECK(results[1] == 0);
  CHECK(results[2] == 0);
  CHECK(q.empty());
}

TEST_CASE("run() cancels waiters nobody can ever notify", "[io][wait_queue]") {
  event_loop loop;
  wait_queue q{loop};
  std::string order;
  std::vector<std::int32_t> results;

  spawn(waiter(q, 'x', order, results));
  spawn(waiter(q, 'y', order, results));

  loop.run();  // no other pending work: the idle rule must fail both waiters
  REQUIRE(results.size() == 2);
  CHECK(results[0] == -ECANCELED);
  CHECK(results[1] == -ECANCELED);
}

TEST_CASE("stop() cancels parked waiters", "[io][wait_queue]") {
  event_loop loop;
  wait_queue q{loop};
  std::string order;
  std::vector<std::int32_t> results;

  spawn(waiter(q, 'x', order, results));
  spawn([](event_loop& l) -> task<void> {
    (void)co_await l.sleep_for(1ms);
    l.stop();
  }(loop));

  loop.run();
  REQUIRE(results.size() == 1);
  CHECK(results[0] == -ECANCELED);
}

TEST_CASE("wait() on a stopping loop completes immediately with -ECANCELED", "[io][wait_queue]") {
  event_loop loop;
  wait_queue q{loop};
  std::string order;
  std::vector<std::int32_t> results;

  spawn([](event_loop& l, wait_queue& wq, std::string& o,
           std::vector<std::int32_t>& r) -> task<void> {
    l.stop();
    co_await waiter(wq, 'z', o, r);
  }(loop, q, order, results));

  loop.run();
  REQUIRE(results.size() == 1);
  CHECK(results[0] == -ECANCELED);
}

TEST_CASE("cancel_slot::reset re-arms a fired slot", "[io][cancel_slot]") {
  event_loop loop;
  cancel_slot slot;
  std::vector<std::int32_t> results;

  spawn([](event_loop& l, cancel_slot& s, std::vector<std::int32_t>& r) -> task<void> {
    // First sleep gets cancelled below.
    r.push_back(co_await l.sleep_for(10s, &s));
    s.reset();
    // Re-armed slot must submit normally again.
    r.push_back(co_await l.sleep_for(1ms, &s));
  }(loop, slot, results));

  spawn([](event_loop& l, cancel_slot& s) -> task<void> {
    (void)co_await l.sleep_for(1ms);
    l.cancel(s);
  }(loop, slot));

  loop.run();
  REQUIRE(results.size() == 2);
  CHECK(results[0] == -ECANCELED);
  CHECK(results[1] == 0);
}
