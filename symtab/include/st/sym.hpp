#pragma once
// Sym — an exchange ticker as a single machine word.
//
// Every venue protocol carries symbols as a fixed-width, space-padded ASCII
// field (ITCH "Stock(8)", OUCH, most binary feeds). symtab keeps that exact
// representation: 8 bytes, first character in the lowest byte, right-padded
// with spaces. Resolving a wire symbol is then a single unaligned 8-byte
// load — no parsing, no trimming, no string hashing over characters — and
// symbol equality is one 64-bit CMP.
//
// Key 0 (all NULs) is reserved as the empty-slot sentinel; a space-padded
// ASCII ticker can never encode it.

#include <cstdint>
#include <cstring>
#include <string_view>

#include "hft/common/platform.hpp"

namespace st {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

struct Sym {
  u64 v = 0;

  static Sym from(std::string_view s) noexcept {
    char buf[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    const std::size_t n = s.size() < 8 ? s.size() : 8;
    for (std::size_t i = 0; i < n; ++i) buf[i] = s[i];
    Sym k;
    std::memcpy(&k.v, buf, 8);
    return k;
  }

  // The 8-byte symbol field straight off a feed buffer.
  static HFT_FORCE_INLINE Sym from_wire(const void* p) noexcept {
    Sym k;
    std::memcpy(&k.v, p, 8);
    return k;
  }

  void to_chars(char out[9]) const noexcept {
    std::memcpy(out, &v, 8);
    out[8] = '\0';
  }

  friend bool operator==(Sym a, Sym b) noexcept { return a.v == b.v; }
};

// splitmix64-style finalizer with a seed folded in up front. Full avalanche:
// used both to pick a bucket and (re-seeded per bucket) to place keys.
HFT_FORCE_INLINE u64 mix(u64 x, u64 seed) noexcept {
  x += seed * 0x9E3779B97F4A7C15ull;
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}

// Map a full-width hash onto [0, n) with a multiply-high instead of an
// integer divide (fastrange): ~3 cycles vs ~25 for DIV, no power-of-2
// capacity requirement, so the table can be exactly n slots — minimal.
HFT_FORCE_INLINE u64 fastrange(u64 h, u64 n) noexcept {
#if defined(_MSC_VER)
  return __umulh(h, n);
#else
  return static_cast<u64>((static_cast<unsigned __int128>(h) * n) >> 64);
#endif
}

}  // namespace st
