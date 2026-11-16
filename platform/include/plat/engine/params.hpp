#pragma once
// plat/engine/params.hpp — config-file → risk/fee struct mapping, shared by
// platd (live), platbt (backtest) and platops (mirror) so every consumer
// ALWAYS builds the same limits and fee schedule from the same keys. No CTP,
// no strategy lineup, no engine composition here — just the risk/fee readers.
//
// The strategy LINEUP deliberately lives outside the platform, in the desk
// project (desk/pack.hpp): the engine's config mapping must not know which
// strategy teams exist. Key blocks are documented in platd.example.cfg.

#include <cstdint>

#include "plat/core/config.hpp"
#include "plat/core/time.hpp"
#include "plat/core/types.hpp"
#include "plat/ref/fees.hpp"
#include "plat/risk/riskgate.hpp"

namespace platcfg {

using namespace plat;

inline RiskLimits limits_from(const Config& c) {
  RiskLimits l;
  l.max_order_qty = c.i64("risk.max_order_qty", 20);
  l.max_order_notional_e4 = px_from_cny(c.f64("risk.max_order_notional_cny", 2'000'000.0));
  l.max_net_notional_e4 = px_from_cny(c.f64("risk.max_net_notional_cny", 10'000'000.0));
  l.max_margin_e4 = px_from_cny(c.f64("risk.max_margin_cny", 5'000'000.0));
  l.max_actions_per_sec = static_cast<std::uint32_t>(c.i64("risk.max_actions_per_sec", 250));
  l.max_actions_per_day = static_cast<std::uint32_t>(c.i64("risk.max_actions_per_day", 18'000));
  l.cancel_ratio_pct = static_cast<std::uint32_t>(c.i64("risk.cancel_ratio_pct", 80));
  l.price_collar_ticks = static_cast<std::uint32_t>(c.i64("risk.price_collar_ticks", 50));
  l.max_book_age = c.i64("risk.max_book_age_ms", 5'000) * kNsPerMs;
  l.check_close_position = c.i64("risk.check_close_position", 1) != 0;
  l.max_strategy_loss_e4 = px_from_cny(c.f64("risk.max_strategy_loss_cny", 0.0));
  l.max_account_loss_e4 = px_from_cny(c.f64("risk.max_account_loss_cny", 0.0));
  l.max_inst_open_qty = c.i64("risk.max_inst_open_qty", 0);
  l.max_inst_cancels = static_cast<std::uint32_t>(c.i64("risk.max_inst_cancels", 450));
  l.check_cancel_target = c.i64("risk.check_cancel_target", 1) != 0;
  l.cancel_retry_after = c.i64("risk.cancel_retry_ms", 1'000) * kNsPerMs;
  l.max_stock_order_qty = c.i64("risk.max_stock_order_qty", 100'000);
  l.stock_cage_bps = static_cast<std::uint32_t>(c.i64("risk.stock_cage_bps", 200));
  l.stock_cage_ticks = static_cast<std::uint32_t>(c.i64("risk.stock_cage_ticks", 10));
  l.stock_cage_etf = c.i64("risk.stock_cage_etf", 0) != 0;
  l.st_daily_buy_qty = c.i64("risk.st_daily_buy_qty", 500'000);
  l.max_stock_holding_qty = c.i64("risk.max_stock_holding_qty", 0);
  l.max_stock_holding_e4 = px_from_cny(c.f64("risk.max_stock_holding_cny", 0.0));
  return l;
}

inline FeeSchedule fees_from(const Config& c) {
  FeeSchedule f;
  f.fut_per_lot_e4 = px_from_cny(c.f64("fees.fut_per_lot_cny", 5.0));
  f.fut_per_lot_close_today_e4 = px_from_cny(c.f64("fees.fut_per_lot_close_today_cny", 15.0));
  f.fut_notional_bps = static_cast<std::uint32_t>(c.i64("fees.fut_notional_bps", 0));
  return f;
}

}  // namespace platcfg
