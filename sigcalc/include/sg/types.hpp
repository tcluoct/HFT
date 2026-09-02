#pragma once
// sigcalc types. Everything the engine touches is an integer: prices in
// ticks, quantities in shares, features in Q-format fixed point (Q16 =
// value * 65536). Integer windows are *exact* — add the newest term,
// subtract the oldest, and the running sum is bit-identical to a full
// recompute forever (no floating-point drift, no periodic resync), which is
// what lets the tests brute-force-audit every feature.

#include <cstdint>

namespace sg {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// One normalized market-data event: top of book after the update, plus the
// trade that caused it (if any).
struct Tick {
  i64 bid_px = 0;   // ticks
  i64 ask_px = 0;   // ticks
  i64 bid_qty = 0;  // shares at the touch (>= 1)
  i64 ask_qty = 0;  // shares at the touch (>= 1)
  i64 trade_qty = 0;  // 0 = quote update only
  i32 trade_side = 0; // +1 buy aggressor, -1 sell aggressor, 0 none
};

// The per-event output. Q16 values are in ticks (mid/microprice) or
// dimensionless in [-1, 1] (imbalance, range position).
struct Features {
  i64 microprice_q16 = 0;  // size-weighted touch price
  i64 imbalance_q16 = 0;   // (bq - aq) / (bq + aq)
  i64 ofi_w = 0;           // order-flow imbalance, sum over window (shares)
  i64 tflow_w = 0;         // signed traded qty over window (shares)
  i64 drift_w = 0;         // sum of mid changes over window (half-ticks)
  i64 rvar_w = 0;          // sum of squared mid changes over window
  i64 range_pos_q16 = 0;   // where mid sits in its window [min, max]
  i64 mom_q16 = 0;         // fast EWMA - slow EWMA of mid (ticks, Q16)
  i64 score = 0;           // fixed-weight combination of the above
};

}  // namespace sg
