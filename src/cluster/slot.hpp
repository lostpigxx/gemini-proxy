// Key → cluster slot: CRC16/XMODEM over the hash tag, mod 16384.
// Reference: valkey cluster-spec appendix A. Design: docs/design/m4-cluster-routing.md §2.
//
// Header-only and constexpr: this sits on the per-request hot path and wants
// to inline, and the table is generated at compile time rather than pasted.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vkp::cluster {

inline constexpr std::uint16_t kSlotCount = 16384;

namespace detail {

// CRC16/XMODEM: poly 0x1021, init 0x0000, no reflection, no final xor.
consteval std::array<std::uint16_t, 256> make_crc16_table() {
  std::array<std::uint16_t, 256> table{};
  for (std::size_t i = 0; i < table.size(); ++i) {
    auto crc = static_cast<std::uint16_t>(i << 8);
    for (int bit = 0; bit < 8; ++bit) {
      const bool high = (crc & 0x8000U) != 0;
      crc = static_cast<std::uint16_t>(crc << 1);
      if (high) {
        crc ^= 0x1021U;
      }
    }
    table[i] = crc;
  }
  return table;
}

inline constexpr std::array<std::uint16_t, 256> kCrc16Table = make_crc16_table();

}  // namespace detail

[[nodiscard]] constexpr std::uint16_t crc16(std::string_view data) noexcept {
  std::uint16_t crc = 0;
  for (const char c : data) {
    const auto index = static_cast<std::uint8_t>((crc >> 8) ^ static_cast<std::uint8_t>(c));
    crc = static_cast<std::uint16_t>(static_cast<std::uint16_t>(crc << 8) ^
                                     detail::kCrc16Table[index]);
  }
  return crc;
}

// The substring between the first '{' and the first '}' after it, when that
// substring is non-empty; otherwise the whole key. Matches cluster-spec.
[[nodiscard]] constexpr std::string_view hash_tag(std::string_view key) noexcept {
  const std::size_t open = key.find('{');
  if (open == std::string_view::npos) {
    return key;
  }
  const std::size_t close = key.find('}', open + 1);
  if (close == std::string_view::npos || close == open + 1) {
    return key;
  }
  return key.substr(open + 1, close - open - 1);
}

[[nodiscard]] constexpr std::uint16_t key_slot(std::string_view key) noexcept {
  return crc16(hash_tag(key)) & (kSlotCount - 1);
}

// Spec test vector; a broken table would otherwise only surface at runtime.
static_assert(crc16("123456789") == 0x31C3);

}  // namespace vkp::cluster
