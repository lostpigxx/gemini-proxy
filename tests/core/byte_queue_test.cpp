#include "core/byte_queue.hpp"

#include <string>

#include <catch2/catch_test_macros.hpp>

using vkp::byte_queue;

TEST_CASE("byte_queue batches appends and keeps the send view stable", "[core][byte_queue]") {
  byte_queue q;
  CHECK(q.empty());
  CHECK(q.size() == 0);

  q.append("hello ");
  q.append("world");
  CHECK_FALSE(q.empty());
  CHECK(q.size() == 11);

  const std::string_view batch = q.begin_send();
  CHECK(batch == "hello world");
  const char* stable = batch.data();

  // Appends during an active send go to the pending half and must not move
  // the in-flight batch (this is the whole point of the double buffer).
  q.append(std::string(4096, 'x'));
  CHECK(batch.data() == stable);
  CHECK(batch == "hello world");
  CHECK(q.size() == 11 + 4096);
  CHECK_FALSE(q.empty());

  q.end_send();
  CHECK(q.size() == 4096);

  const std::string_view second = q.begin_send();
  CHECK(second.size() == 4096);
  q.end_send();
  CHECK(q.empty());
  CHECK(q.size() == 0);
}

TEST_CASE("byte_queue is empty only when both halves drained", "[core][byte_queue]") {
  byte_queue q;
  q.append("a");
  (void)q.begin_send();
  // Nothing pending, but a batch is in flight: not empty.
  CHECK_FALSE(q.empty());
  CHECK(q.size() == 1);
  q.end_send();
  CHECK(q.empty());
}
