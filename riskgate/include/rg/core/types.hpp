#pragma once
// Order-entry types for the pre-trade risk gate. Prices are integer ticks,
// quantities integer shares — no floating point anywhere on the hot path.

#include <cstdint>

#include "hft/common/platform.hpp"

namespace rg {

using SymbolId = std::uint32_t;
using Qty = std::uint64_t;         // shares
using PriceTicks = std::int64_t;   // price in integer ticks

enum class Side : std::uint8_t { kBuy = 0, kSell = 1 };

struct OrderReq {
  SymbolId symbol;
  Side side;
  Qty qty;
  PriceTicks price;
};

// One bit per rule. check() returns the OR of every rule the order violates
// (0 == accept): a rejected order reports *all* of its problems in one pass,
// and the accumulation compiles to setcc/shift/or — no data-dependent branch
// per rule.
enum Reject : std::uint32_t {
  kKillSwitch = 1u << 0,
  kUnknownSymbol = 1u << 1,   // id out of range or symbol not enabled
  kQtyZero = 1u << 2,
  kQtyTooLarge = 1u << 3,
  kNotionalTooLarge = 1u << 4,
  kPriceBandLow = 1u << 5,
  kPriceBandHigh = 1u << 6,
  kPositionLong = 1u << 7,
  kPositionShort = 1u << 8,
  kThrottled = 1u << 9,
};

// notional = price * qty needs 128 bits before the limit comparison — a
// 64-bit product would wrap and wave a fat-fingered order through. One MULX
// on x86-64, no division, no branch.
HFT_FORCE_INLINE bool notional_exceeds(std::uint64_t px, std::uint64_t qty,
                                       std::uint64_t limit) noexcept {
#if defined(_MSC_VER)
  unsigned long long hi;
  const unsigned long long lo = _umul128(px, qty, &hi);
  return (hi != 0) | (lo > limit);
#else
  return (static_cast<unsigned __int128>(px) * qty) > limit;
#endif
}

}  // namespace rg
