#pragma once
// pnld types. Money is integers end to end: prices in ticks, quantities in
// shares/contracts, cash in tick-shares (the per-instrument multiplier is a
// report-time concern). Every P&L number in the engine is therefore exact —
// the equity identity in book.hpp holds by ==, not epsilon.
//
// Magnitude contract (asserted by the sim, documented here): |px| < 2^21,
// |position| < 2^22, fill qty < 2^12. That bounds the worst intermediate
// (wac_cost * qty ~ 2^{21+22+12} = 2^55) safely inside i64 without needing
// 128-bit arithmetic (riskgate's trick; not repeated here).

#include <cstdint>

namespace pl {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

struct Fill {
  u32 sym = 0;   // dense instrument id (symtab's output)
  i32 side = 1;  // +1 buy, -1 sell
  i64 qty = 0;   // > 0
  i64 px = 0;    // ticks
  i64 fee = 0;   // tick-shares, always a cost (>= 0)
};

struct Mark {
  u32 sym = 0;
  i64 px = 0;  // ticks
};

}  // namespace pl
