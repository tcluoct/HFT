#pragma once
// strat_params.hpp — config-file → strategy/risk/fee struct mapping, shared
// by platd (live) and platbt (backtest) so the two ALWAYS build the same
// strategies from the same keys. No CTP, no engine composition here — just
// the strategy-type REGISTRY (the `Pack` list below) and the risk/fee
// param readers. Key blocks are documented in platd.example.cfg.
//
// Strategies are configured as named INSTANCES:
//
//   strategies   = fast_cu, slow_al, brk        # instance names
//   fast_cu.type = momentum                     # any Pack type name
//   fast_cu.id   = 1                            # required, unique, nonzero
//   fast_cu.instruments = cu2609                # defaults to global `instruments`
//   fast_cu.lots = 2                            # type params under the instance name
//
// so one site can run any subset of the strategy types, several instances of
// the same type with different params/universes, and different sites (or
// fleet shards via shardN.*) freely differ. `<name>.enabled = 0` stops an
// instance without deleting its block: it is simply not constructed next
// session, while its StrategyId stays reserved so PnL attribution and the
// overnight .positions carry chain survive until it is switched back on.
// The id is the durable identity (risk scopes, kill targets, PnL, carry) —
// never reuse one for a different purpose.
//
// Without a `strategies` key the classic trio (momentum=1, meanrev=2,
// breakout=3) is assumed, so existing configs keep working unchanged.
//
// ADDING A STRATEGY TYPE is a two-line change here:
//   1. write plat/strat/<new>.hpp — the strategy, its params struct, and its
//      Def (config name, legacy id, params reader; see engine/pack.hpp), and
//   2. #include it and append the Def to `Pack` below.
// Parsing, bank building, Site composition, platd, platbt and --replay are
// all derived from this list — nothing else changes. APPEND ONLY, NEVER
// REORDER: banks are dispatched, saved and hashed in pack order
// (site_state.hpp), so reordering breaks snapshot/replay compatibility.

#include <cstdint>
#include <vector>

#include "plat/core/config.hpp"
#include "plat/core/time.hpp"
#include "plat/core/types.hpp"
#include "plat/engine/pack.hpp"
#include "plat/ref/fees.hpp"
#include "plat/risk/riskgate.hpp"
#include "plat/strat/breakout.hpp"
#include "plat/strat/meanrev.hpp"
#include "plat/strat/momentum.hpp"

namespace platcfg {

using namespace plat;

// The strategy-type pack the platform ships — THE one list (append only,
// never reorder; see the header comment). Each type is a BANK in the Site:
// zero or more configured instances.
using Pack = StratPack<MomentumDef, MeanRevDef, BreakoutDef>;

// The pack composed over any gateway (CTP counter, backtest simulator, …).
template <typename Gateway>
using PackSite = Pack::SiteFor<Gateway>;

inline const char* kind_name(std::uint8_t kind) noexcept { return Pack::kind_name(kind); }

// Parse + validate the instance list (fail closed — see StratPack::parse).
inline bool parse_strategies(const Config& c, AccountId default_account,
                             std::vector<StratSpec>& out) {
  return Pack::parse(c, default_account, out);
}

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
