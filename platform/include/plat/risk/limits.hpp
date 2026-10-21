#pragma once
// plat/risk/limits.hpp — the riskgate's configured limits (§10.1), one
// field group per check. Cold config: parsed once from the site config
// (platcfg::limits_from), never part of snapshot state — replay rebuilds it
// from the same file. Split from riskgate.hpp for size only.

#include <cstdint>

#include "plat/codec/msgs.hpp"
#include "plat/core/time.hpp"
#include "plat/core/types.hpp"

namespace plat {

struct RiskLimits {
  Qty max_order_qty = 10'000;           // futures lots per order (#3)
  Qty max_stock_order_qty = 100'000;    // shares per stock order (#3); the board max (#17) also applies
  std::int64_t max_order_notional_e4 = px_from_cny(2'000'000.0);  // 200万 per order
  std::int64_t max_net_notional_e4 = px_from_cny(10'000'000.0);   // per strategy
  std::int64_t max_margin_e4 = px_from_cny(5'000'000.0);          // per account
  std::uint32_t max_actions_per_sec = 250;   // below the 300/s HFT line by default
  std::uint32_t max_actions_per_day = 18'000;
  std::uint32_t cancel_ratio_pct = 80;       // cancels/actions ceiling after warmup
  std::uint32_t cancel_ratio_min_actions = 100;
  std::uint32_t price_collar_ticks = 50;     // fat-finger band around last/mid
  Nanos max_book_age = 5 * kNsPerSec;        // stale-data interlock
  std::uint32_t duplicate_window = 8;        // identical-order suppression depth (≤16)
  Nanos duplicate_horizon = 20 * kNsPerMs;   // ...within this window (repeat-fire breaker)
  // Futures close-vs-position check (5b). Off ONLY for counter-error evidence
  // runs, so a close with nothing to close reaches the counter and its own
  // 无仓位可平 rejection can be shown; never off in trading configs.
  bool check_close_position = true;

  // 13) Daily stop-loss on today's PnL net of fees, unrealized included
  //     (PnlLedger::day_pnl_e4). 0 = off. A breach latches for the session:
  //     position-increasing orders are refused, closes still pass — a stop
  //     that also froze the exit would turn a loss limit into a stuck book.
  std::int64_t max_strategy_loss_e4 = 0;
  std::int64_t max_account_loss_e4 = 0;
  // 14) 日内开仓限额: filled + working opens per (account, contract) per day.
  //     0 = off; set it to the product's exchange line where one exists
  //     (CFFEX index futures, DCE/CZCE restricted contracts) — exceeding it
  //     is a violation, not a surveillance flag.
  Qty max_inst_open_qty = 0;
  // 15) Per-contract daily cancels per account. SHFE/INE/DCE/CZCE/GFEX treat
  //     单合约日撤单 ≥500 as 异常交易; CFFEX index futures ≥400 — use 380 on
  //     CFFEX sites. 0 = off.
  std::uint32_t max_inst_cancels = 450;
  // 16) Cancel hygiene: refuse cancels of unknown/terminal/foreign orders and
  //     repeats while one is in flight — each is a 申报 the venue counts
  //     toward the HFT and cancel lines. A repeat passes after the retry
  //     window so a lost cancel can't strand an order. Off ONLY for
  //     counter-error evidence runs, like check_close_position.
  bool check_cancel_target = true;
  Nanos cancel_retry_after = kNsPerSec;

  // ---- Cash equities (SSE/SZSE). Stocks skip the tick-count fat-finger
  // band (#2): a fixed tick count is 0.03% on a 1,500 CNY stock and 17% on
  // a 3 CNY one. Their band is the limit band plus the price cage.
  // 18) 价格笼子 width: base ±bps, or ±ticks if wider. 0 bps = off.
  std::uint32_t stock_cage_bps = 200;
  std::uint32_t stock_cage_ticks = 10;
  bool stock_cage_etf = false;  // the cage covers stocks/存托凭证; funds opt in
  // 19) 风险警示股票: an investor's daily buys of one ST stock ≤ 50万股
  //     (filled + working + this order). 0 = off.
  Qty st_daily_buy_qty = 500'000;
  // 20) Single-stock concentration per account across every strategy book:
  //     holding + working buys + this order. Shares keep the account under
  //     the 5% 举牌 line (set from 总股本); notional is the 单票 cap from the
  //     fund contract. 0 = off.
  Qty max_stock_holding_qty = 0;
  std::int64_t max_stock_holding_e4 = 0;
};

// check_new/check_cancel return this when every check passes.
inline constexpr RiskCheck kRiskPass = static_cast<RiskCheck>(0);

}  // namespace plat
