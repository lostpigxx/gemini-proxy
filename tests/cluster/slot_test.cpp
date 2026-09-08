#include "cluster/slot.hpp"

#include <string>

#include <catch2/catch_test_macros.hpp>

using vkp::cluster::crc16;
using vkp::cluster::hash_tag;
using vkp::cluster::key_slot;
using vkp::cluster::kSlotCount;

TEST_CASE("CRC16/XMODEM matches the spec vectors", "[cluster][slot]") {
  CHECK(crc16("") == 0x0000);
  CHECK(crc16("123456789") == 0x31C3);
  CHECK(crc16("A") == 0x58E5);
  // Appendix A of the cluster spec pairs these with their slots below.
  CHECK(crc16("123456789") % kSlotCount == 12739);
}

TEST_CASE("hash tag extraction follows the cluster spec", "[cluster][slot]") {
  CHECK(hash_tag("foo") == "foo");
  CHECK(hash_tag("{user1000}.following") == "user1000");
  CHECK(hash_tag("foo{}{bar}") == "foo{}{bar}");  // first {} is empty: whole key
  CHECK(hash_tag("foo{{bar}}") == "{bar");        // ends at the first }
  CHECK(hash_tag("foo{bar}{zap}") == "bar");      // only the first tag counts
  CHECK(hash_tag("{bar") == "{bar");              // unterminated: whole key
  CHECK(hash_tag("bar}") == "bar}");              // no opening brace
  CHECK(hash_tag("}{bar}") == "bar");             // } before { does not matter
  CHECK(hash_tag("{}") == "{}");                  // empty tag: whole key
  CHECK(hash_tag("") == "");
  CHECK(hash_tag("{a}") == "a");
}

TEST_CASE("keys sharing a hash tag land in the same slot", "[cluster][slot]") {
  const std::uint16_t slot = key_slot("{user1000}.following");
  CHECK(key_slot("{user1000}.followers") == slot);
  CHECK(key_slot("{user1000}") == slot);
  CHECK(key_slot("user1000") == slot);  // the tag hashes as the bare key
  CHECK(key_slot("user1001") != slot);
}

TEST_CASE("slots stay in range for arbitrary keys", "[cluster][slot]") {
  for (int i = 0; i < 5000; ++i) {
    CHECK(key_slot("key:" + std::to_string(i)) < kSlotCount);
  }
  CHECK(key_slot("") < kSlotCount);
  CHECK(key_slot(std::string(4096, 'x')) < kSlotCount);
  CHECK(key_slot(std::string_view("a\0b", 3)) < kSlotCount);  // keys are binary safe
}

TEST_CASE("key_slot is usable in constant expressions", "[cluster][slot]") {
  static_assert(key_slot("foo") == 12182);
  static_assert(key_slot("bar") == 5061);
  static_assert(key_slot("{foo}bar") == key_slot("foo"));
  SUCCEED();
}
