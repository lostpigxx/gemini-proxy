#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <string_view>

#include "core/buffer.hpp"

using vkp::read_buffer;

namespace {

void fill(read_buffer& buf, std::string_view data) {
  const auto w = buf.prepare(data.size());
  std::memcpy(w.data(), data.data(), data.size());
  buf.commit(data.size());
}

}  // namespace

TEST_CASE("read_buffer starts empty", "[core][buffer]") {
  read_buffer buf;
  REQUIRE(buf.readable_bytes() == 0);
  REQUIRE(buf.readable().empty());
  REQUIRE(buf.capacity() == read_buffer::kDefaultInitialCapacity);
}

TEST_CASE("read_buffer append and consume", "[core][buffer]") {
  read_buffer buf;
  fill(buf, "hello ");
  fill(buf, "world");
  REQUIRE(buf.readable() == "hello world");

  buf.consume(6);
  REQUIRE(buf.readable() == "world");

  buf.consume(5);
  REQUIRE(buf.readable_bytes() == 0);
}

TEST_CASE("read_buffer consume-all rewinds cursors", "[core][buffer]") {
  read_buffer buf(16);
  fill(buf, "0123456789");
  buf.consume(10);
  // Cursors rewound: the full capacity is writable again without growth.
  REQUIRE(buf.prepare(16).size() == 16);
  REQUIRE(buf.capacity() == 16);
}

TEST_CASE("read_buffer compacts instead of growing when possible", "[core][buffer]") {
  read_buffer buf(16);
  fill(buf, "0123456789ab");  // 12 bytes
  buf.consume(10);            // 2 unconsumed, head at 10
  // 8 free bytes needed, only 4 at the tail, but compaction frees 14.
  const auto w = buf.prepare(8);
  REQUIRE(w.size() >= 8);
  REQUIRE(buf.capacity() == 16);
  REQUIRE(buf.readable() == "ab");
}

TEST_CASE("read_buffer grows geometrically and preserves data", "[core][buffer]") {
  read_buffer buf(8);
  fill(buf, "abcdef");
  const auto w = buf.prepare(100);
  REQUIRE(w.size() >= 100);
  REQUIRE(buf.readable() == "abcdef");
  REQUIRE(buf.capacity() == 128);  // bit_ceil(6 + 100)
}

TEST_CASE("read_buffer window only grows between appends", "[core][buffer]") {
  // The parser contract: between parses of one message, the old window must
  // be a prefix of the new one, across growth and compaction.
  read_buffer buf(8);
  std::string seen;
  const std::string_view stream = "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
  for (const char c : stream) {
    fill(buf, {&c, 1});
    const std::string_view window{buf.readable()};
    REQUIRE(window.substr(0, seen.size()) == seen);
    seen = std::string(window);
  }
  REQUIRE(buf.readable() == stream);
}
