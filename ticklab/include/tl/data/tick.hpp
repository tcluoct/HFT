#pragma once
// The tick model: L1 (top-of-book) quotes and trades. This is the classic
// minimal dataset for strategy research; integer ticks for prices as always.

#include <cstdint>

namespace tl {

using Price = std::int64_t;  // integer ticks; assumed >= 0 (stored as u64)
using Qty = std::uint32_t;
using SymbolId = std::uint16_t;

inline constexpr std::uint8_t kQuote = 0;  // px1=bid px2=ask q1=bid_qty q2=ask_qty
inline constexpr std::uint8_t kTrade = 1;  // px1=trade price, q1=trade qty

struct Tick {
  std::uint64_t ts_ns;  // session-relative nanoseconds, non-decreasing
  Price px1, px2;
  Qty q1, q2;
  SymbolId symbol;
  std::uint8_t kind;

  [[nodiscard]] Price bid_px() const noexcept { return px1; }
  [[nodiscard]] Price ask_px() const noexcept { return px2; }
  [[nodiscard]] Qty bid_qty() const noexcept { return q1; }
  [[nodiscard]] Qty ask_qty() const noexcept { return q2; }
  [[nodiscard]] Price trade_px() const noexcept { return px1; }
  [[nodiscard]] Qty trade_qty() const noexcept { return q1; }
};

// The row-format footprint a naive recorder would write; the columnar codec's
// bytes/tick is measured against this.
inline constexpr std::size_t kRawRowBytes = 8 + 8 + 8 + 4 + 4 + 2 + 1 + 1 /*pad*/;

}  // namespace tl
