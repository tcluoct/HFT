#pragma once
// plat/risk/stock_rules.hpp — SSE/SZSE cash-equity order rules the riskgate
// enforces before a stock order leaves the building (§10.1 #17–#18). Pure,
// constexpr functions of (instrument, order, book top): no state, so each
// rule is unit-testable on its own and costs a handful of integer ops.
//
//   申报数量 (#17): buys in whole board lots — 100 shares on main board,
//   ChiNext, ST and ETFs; 科创板 ≥200 shares in steps of 1. Odd lots only
//   sell, and only the entire odd remainder at once (余额不足一手部分应当
//   一次性申报卖出). Per-order maximum for limit orders by board.
//
//   价格笼子 (#18, continuous auction only): buy ≤ max(base×102%, base+10
//   ticks), sell ≥ min(base×98%, base−10 ticks). The buy base is the best
//   ask, else best bid, else last trade, else prev close; the sell base is
//   the best bid, else best ask, else last, else prev close. Orders outside
//   the cage are parked by the exchange (暂存), not matched — a strategy
//   that thinks it is quoting is in fact absent, so the gate refuses them.

#include <cstdint>

#include "plat/core/types.hpp"
#include "plat/ref/instrument.hpp"

namespace plat {

// 单笔限价申报最大数量, by board.
constexpr Qty board_max_order_qty(Board b) noexcept {
  switch (b) {
    case Board::Star:    return 100'000;    // 科创板
    case Board::ChiNext: return 300'000;    // 创业板
    default:             return 1'000'000;  // 主板 / 风险警示 / 基金
  }
}

// #17 — `holding` is the account's whole position in the stock (the odd
// remainder is a property of the account's balance, not of one book).
constexpr bool stock_qty_ok(const Instrument& in, Side side, Qty qty, Qty holding) noexcept {
  if (qty <= 0 || qty > board_max_order_qty(in.board)) return false;
  if (in.board == Board::Star) {
    if (side == Side::Buy) return qty >= 200;
    return qty >= 200 || qty == holding;  // a sub-200 remainder goes in one order
  }
  const Qty lot = in.lot > 0 ? in.lot : 100;
  if (side == Side::Buy) return qty % lot == 0;
  return qty % lot == 0 || (holding > 0 && qty % lot == holding % lot);
}

// #18 — the cage's benchmark price for an order on `side`.
constexpr Price cage_base(Side side, Price bid, Price ask, Price last, Price prev_close) noexcept {
  const Price first = side == Side::Buy ? ask : bid;
  const Price second = side == Side::Buy ? bid : ask;
  if (first > 0) return first;
  if (second > 0) return second;
  return last > 0 ? last : prev_close;
}

constexpr bool in_price_cage(Side side, Price px, Price base, Price tick, std::uint32_t bps,
                             std::uint32_t ticks) noexcept {
  if (base <= 0) return true;  // nothing to anchor to: the limit band (#2) still applies
  const Price pct = base * static_cast<Price>(bps) / 10'000;
  const Price abs = static_cast<Price>(ticks) * tick;
  const Price width = pct > abs ? pct : abs;  // 孰高 / 孰低: the wider of the two
  return side == Side::Buy ? px <= base + width : px >= base - width;
}

}  // namespace plat
