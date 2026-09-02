#pragma once
// plat/ref/fees.hpp — the one fee/tax/margin library (§4.2, §13.2). Backtest
// and live PnL call the same functions; a fee model that exists twice will
// eventually disagree, so it exists once. All amounts in 1e-4 CNY.

#include <cstdint>

#include "plat/core/types.hpp"
#include "plat/ref/instrument.hpp"

namespace plat {

struct FeeSchedule {
  // Cash equities (per side unless noted).
  std::uint32_t stock_commission_bps = 3;   // 0.03% brokerage, rounded up from typical
  std::uint32_t stamp_duty_sell_bps = 5;    // 0.05% sell side only (2023 schedule)
  std::uint32_t transfer_fee_bps = 0;       // folded into commission here
  // Futures: per-lot fixed + notional bps components; close-today elevated
  // on many products (§4.2) — the 平今 surcharge is the one that reshapes
  // intraday strategies, so it is explicit.
  std::int64_t fut_per_lot_e4 = 50'000;         // 5 CNY/lot open & close-yesterday
  std::int64_t fut_per_lot_close_today_e4 = 150'000;  // 15 CNY/lot close-today
  std::uint32_t fut_notional_bps = 0;
};

// Notional in 1e-4 CNY for qty at px (multiplier for derivatives).
constexpr std::int64_t notional_e4(const Instrument& in, Price px, Qty qty) noexcept {
  return px * qty * in.multiplier;
}

// Fee for one fill. Offset only matters for futures (close-today surcharge).
constexpr std::int64_t fee_for(const FeeSchedule& f, const Instrument& in, Side side,
                               Offset offset, Price px, Qty qty) noexcept {
  const std::int64_t notl = notional_e4(in, px, qty);
  if (in.is_stock()) {
    std::int64_t fee = notl * f.stock_commission_bps / 10'000;
    fee += notl * f.transfer_fee_bps / 10'000;
    if (side == Side::Sell) fee += notl * f.stamp_duty_sell_bps / 10'000;
    return fee;
  }
  const std::int64_t per_lot =
      offset == Offset::CloseToday ? f.fut_per_lot_close_today_e4 : f.fut_per_lot_e4;
  return per_lot * qty + notl * f.fut_notional_bps / 10'000;
}

// Initial margin requirement for a futures position increment.
constexpr std::int64_t margin_e4(const Instrument& in, Price px, Qty qty) noexcept {
  return notional_e4(in, px, qty) * in.margin_bps / 10'000;
}

}  // namespace plat
