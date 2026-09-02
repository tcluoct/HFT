#pragma once
// plat/strat/basisarb.hpp — the futures–spot arbitrage pairs (sord
// graduation, §9.3). Two cooperating strategies, one per site, coordinating
// only through the cross-site messages a real private line would carry:
//
//   ArbHedgeLeg (futures site): exports BasisSummary on every snapshot;
//   executes inbound HedgeIntents aggressively within the intent's limit
//   (the slippage budget), reports HedgeDone either way.
//
//   ArbCashLeg (cash site): maintains the pair view — local ETF book plus
//   the *stale by construction* remote leg (staleness = now − origin_ts,
//   checked before every action). When the basis edge clears costs it lifts
//   the ETF leg IOC; fills accumulate in the opportunity ledger; when
//   unhedged notional reaches one futures lot the hedge intent fires
//   (hedge banding — cash clips are small against a 300-multiplier lot).
//
// Multi-pair: one instance per leg runs any number of pairs. Per-pair state
// lives in a slot array; instrument-keyed events resolve their pair through
// dense-id index tables (cash side / futures side), remote messages through
// their pair_id. One instrument belongs to at most one pair per leg.
//
// The ledger is the §9.3 opportunity ledger: detected → legged → hedged,
// with realized hedge slippage recorded. Riskgate's exposure cap is the
// hard bound; the ledger's band is the working bound.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "plat/core/hash.hpp"
#include "plat/md/stock_book.hpp"
#include "plat/pnl/pnl.hpp"
#include "plat/ref/instrument.hpp"
#include "plat/strat/framework.hpp"

namespace plat {

struct ArbPairParams {
  std::uint64_t pair_id = 1;
  InstrumentId cash_inst = kNoInstrument;  // ETF at the cash site
  InstrumentId fut_inst = kNoInstrument;   // future at the hedge site
  std::int64_t px_ratio = 1000;            // index-points per ETF-CNY (4000 ↔ 4.000)
  std::int64_t entry_edge_e4 = 80;         // net edge (ETF px terms) to act, ≈2 ticks
  Qty cash_clip = 20'000;                  // ETF shares per lift
  Qty hedge_band_lots = 1;                 // hedge when unhedged ≥ this many lots
  int hedge_budget_ticks = 3;              // slippage budget on the hedge leg
  // Remote-leg staleness bound: must exceed the far venue's snapshot cadence
  // (500 ms) — it exists to catch a broken line, not the disclosure rhythm.
  Nanos max_remote_age = 800 * kNsPerMs;
  std::int64_t max_unhedged_e4 = px_from_cny(3'000'000.0);  // ledger hard stop
};

namespace detail {
// Dense inst → pair-slot index (-1 = not part of any pair on this side).
inline std::vector<std::int32_t> pair_index(std::size_t table_size,
                                            const std::vector<ArbPairParams>& pairs,
                                            InstrumentId ArbPairParams::* side) {
  std::vector<std::int32_t> idx(table_size, -1);
  for (std::size_t k = 0; k < pairs.size(); ++k) {
    const InstrumentId i = pairs[k].*side;
    if (i < table_size) idx[i] = static_cast<std::int32_t>(k);
  }
  return idx;
}
}  // namespace detail

// ------------------------------------------------------------ hedge leg site

class ArbHedgeLeg final : public Strategy<ArbHedgeLeg> {
 public:
  ArbHedgeLeg(StrategyId id, AccountId account, std::vector<ArbPairParams> pairs,
              std::size_t table_size)
      : Strategy(id, account), pairs_(std::move(pairs)), slots_(pairs_.size()),
        by_fut_(detail::pair_index(table_size, pairs_, &ArbPairParams::fut_inst)) {}

  template <typename Ctx>
  void on_snapshot(Ctx& ctx, const MdSnapshotEvt& s) {
    const std::int32_t k = s.inst < by_fut_.size() ? by_fut_[s.inst] : -1;
    if (k < 0) return;
    BasisSummary b{};
    b.pair_id = pairs_[static_cast<std::size_t>(k)].pair_id;
    b.origin_ts = s.ts;
    b.leg_bid = s.bid_px[0];
    b.leg_ask = s.ask_px[0];
    b.leg_bid_qty = s.bid_qty[0];
    b.leg_ask_qty = s.ask_qty[0];
    b.inst = s.inst;
    ctx.send_basis(b);
  }

  template <typename Ctx>
  void on_hedge_intent(Ctx& ctx, const HedgeIntent& h) {
    const std::int32_t k = pair_by_id(h.pair_id);
    if (k < 0) return;
    const ArbPairParams& p = pairs_[static_cast<std::size_t>(k)];
    Slot& sl = slots_[static_cast<std::size_t>(k)];
    sl.active = h;
    // Aggress within the budget the origin site priced.
    sl.pending = ctx.place(p.fut_inst, h.side, Offset::Open, h.limit_px, h.qty, Tif::Ioc,
                           h.origin_ts);
    sl.filled = 0;
    sl.notional = 0;
    if (sl.pending == kNoOrder) report(ctx, p, sl, 0, 0, h.qty);
  }

  template <typename Ctx>
  void on_exec(Ctx& ctx, const ExecReport& e) {
    const std::int32_t k = e.inst < by_fut_.size() ? by_fut_[e.inst] : -1;
    if (k < 0) return;
    Slot& sl = slots_[static_cast<std::size_t>(k)];
    if (e.cl_id != sl.pending) return;
    if (e.type == ExecType::Fill) {
      sl.filled += e.qty;
      sl.notional += e.px * e.qty;
      if (e.leaves == 0) finish(ctx, static_cast<std::size_t>(k));
    } else if (e.type == ExecType::Canceled || e.type == ExecType::Reject) {
      finish(ctx, static_cast<std::size_t>(k));  // IOC remainder dead — report what we got
    }
  }

  template <typename Ctx>
  void on_deny(Ctx& ctx, const RiskDeny& d) {
    const std::int32_t k = d.inst < by_fut_.size() ? by_fut_[d.inst] : -1;
    if (k < 0 || d.cl_id != slots_[static_cast<std::size_t>(k)].pending) return;
    finish(ctx, static_cast<std::size_t>(k));
  }

  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    for (const Slot& sl : slots_) {
      h.mix(sl.filled);
      h.mix(sl.hedges_done);
    }
    return h.value();
  }
  [[nodiscard]] int hedges_done(std::size_t pair = 0) const noexcept {
    return slots_[pair].hedges_done;
  }

 private:
  struct Slot {
    HedgeIntent active{};
    OrderId pending = kNoOrder;
    Qty filled = 0;
    std::int64_t notional = 0;
    int hedges_done = 0;
  };

  [[nodiscard]] std::int32_t pair_by_id(std::uint64_t pair_id) const noexcept {
    for (std::size_t k = 0; k < pairs_.size(); ++k)  // pairs are few: linear
      if (pairs_[k].pair_id == pair_id) return static_cast<std::int32_t>(k);
    return -1;
  }

  template <typename Ctx>
  void finish(Ctx& ctx, std::size_t k) {
    Slot& sl = slots_[k];
    report(ctx, pairs_[k], sl, sl.filled, sl.filled ? sl.notional / sl.filled : 0,
           sl.active.qty - sl.filled);
    sl.pending = kNoOrder;
    ++sl.hedges_done;
  }
  template <typename Ctx>
  void report(Ctx& ctx, const ArbPairParams& p, const Slot& sl, Qty filled, Price avg_px,
              Qty unfilled) {
    HedgeDone d{};
    d.pair_id = p.pair_id;
    d.origin_ts = sl.active.origin_ts;
    d.avg_px = avg_px;
    d.filled = filled;
    d.unfilled = unfilled;
    d.inst = p.fut_inst;
    d.strategy = sl.active.strategy;  // route back to the cash leg
    d.side = sl.active.side;
    ctx.send_hedge_done(d);
  }

  std::vector<ArbPairParams> pairs_;
  std::vector<Slot> slots_;
  std::vector<std::int32_t> by_fut_;
};

// ------------------------------------------------------------- cash leg site

class ArbCashLeg final : public Strategy<ArbCashLeg> {
 public:
  ArbCashLeg(StrategyId id, AccountId account, std::vector<ArbPairParams> pairs,
             std::size_t table_size)
      : Strategy(id, account), pairs_(std::move(pairs)), slots_(pairs_.size()),
        by_cash_(detail::pair_index(table_size, pairs_, &ArbPairParams::cash_inst)) {}

  // Residual hedging on a heartbeat (§9.3): fills drive hedge banding, but
  // when lifting stops (caps, faded edge) the residual must not park — the
  // timer sweeps it. Timer events are Source-journaled, so this replays.
  template <typename Ctx>
  void on_start(Ctx& ctx) {
    ctx.set_timer(ctx.now() + kNsPerSec, 1);
  }
  template <typename Ctx>
  void on_timer(Ctx& ctx, const TimerEvt& t) {
    for (std::size_t k = 0; k < pairs_.size(); ++k) maybe_hedge(ctx, k, t.ts);
    ctx.set_timer(t.ts + kNsPerSec, 1);
  }

  template <typename Ctx>
  void on_basis(Ctx& ctx, const BasisSummary& b) {
    const std::int32_t k = pair_by_id(b.pair_id);
    if (k < 0) return;
    Slot& sl = slots_[static_cast<std::size_t>(k)];
    sl.remote = b;
    sl.have_remote = true;
    evaluate(ctx, static_cast<std::size_t>(k), b.ts);
  }

  template <typename Ctx>
  void on_book(Ctx& ctx, const BookTopEvt& t) {
    const std::int32_t k = t.inst < by_cash_.size() ? by_cash_[t.inst] : -1;
    if (k >= 0) evaluate(ctx, static_cast<std::size_t>(k), t.ts);
  }

  template <typename Ctx>
  void on_exec(Ctx& ctx, const ExecReport& e) {
    const std::int32_t k = e.inst < by_cash_.size() ? by_cash_[e.inst] : -1;
    if (k < 0) return;
    Slot& sl = slots_[static_cast<std::size_t>(k)];
    if (e.cl_id != sl.pending) return;
    if (e.type == ExecType::Fill) {
      // Cash leg filled: exposure exists until hedged.
      const std::int64_t signed_notl =
          (e.side == Side::Buy ? 1 : -1) * static_cast<std::int64_t>(e.px) * e.qty;
      sl.unhedged_cash_e4 += signed_notl;
      if (e.leaves == 0) sl.pending = kNoOrder;
      maybe_hedge(ctx, static_cast<std::size_t>(k), e.ts);
    } else if (e.type == ExecType::Canceled || e.type == ExecType::Reject) {
      sl.pending = kNoOrder;
    }
  }

  template <typename Ctx>
  void on_deny(Ctx&, const RiskDeny& d) {
    const std::int32_t k = d.inst < by_cash_.size() ? by_cash_[d.inst] : -1;
    if (k >= 0 && d.cl_id == slots_[static_cast<std::size_t>(k)].pending)
      slots_[static_cast<std::size_t>(k)].pending = kNoOrder;
  }

  template <typename Ctx>
  void on_hedge_done(Ctx& ctx, const HedgeDone& d) {
    const std::int32_t ki = pair_by_id(d.pair_id);
    if (ki < 0) return;
    const std::size_t k = static_cast<std::size_t>(ki);
    Slot& sl = slots_[k];
    const Instrument& fut = ctx.instrument(pairs_[k].fut_inst);
    // Hedged futures *notional* offsets cash notional one-for-one (both are
    // CNY): a hedge sell reduces positive (long-cash) unhedged exposure, a
    // hedge buy reduces negative. px_ratio converts prices, never notionals.
    const std::int64_t hedged_notional = d.filled * d.avg_px * fut.multiplier;
    sl.unhedged_cash_e4 += (d.side == Side::Sell ? -1 : +1) * hedged_notional;
    sl.intent_out = false;
    ++sl.hedges_confirmed;
    if (d.unfilled > 0) ++sl.hedge_shortfalls;  // budget exhausted: exposure stays on
  }

  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    for (const Slot& sl : slots_) {
      h.mix(sl.unhedged_cash_e4);
      h.mix(sl.lifts);
      h.mix(sl.hedges_confirmed);
    }
    return h.value();
  }

  [[nodiscard]] int lifts(std::size_t pair = 0) const noexcept { return slots_[pair].lifts; }
  [[nodiscard]] int intents_sent(std::size_t pair = 0) const noexcept {
    return slots_[pair].intents_sent;
  }
  [[nodiscard]] int hedges_confirmed(std::size_t pair = 0) const noexcept {
    return slots_[pair].hedges_confirmed;
  }
  [[nodiscard]] std::int64_t unhedged_cash_e4(std::size_t pair = 0) const noexcept {
    return slots_[pair].unhedged_cash_e4;
  }

 private:
  struct Slot {
    BasisSummary remote{};
    bool have_remote = false;
    bool intent_out = false;
    OrderId pending = kNoOrder;
    std::int64_t unhedged_cash_e4 = 0;
    int lifts = 0;
    int intents_sent = 0;
    int hedges_confirmed = 0;
    int hedge_shortfalls = 0;
  };

  [[nodiscard]] std::int32_t pair_by_id(std::uint64_t pair_id) const noexcept {
    for (std::size_t k = 0; k < pairs_.size(); ++k)
      if (pairs_[k].pair_id == pair_id) return static_cast<std::int32_t>(k);
    return -1;
  }

  template <typename Ctx>
  void evaluate(Ctx& ctx, std::size_t k, Nanos now) {
    const ArbPairParams& p = pairs_[k];
    Slot& sl = slots_[k];
    if (!sl.have_remote || sl.pending != kNoOrder || sl.intent_out) return;
    if (now - sl.remote.origin_ts > p.max_remote_age + bridge_allowance_) return;  // stale
    const StockBook* book = ctx.stock_book(p.cash_inst);
    if (!book) return;
    const auto top = book->top();
    if (top.ask_px == 0 || top.bid_px == 0 || sl.remote.leg_bid == 0 || sl.remote.leg_ask == 0)
      return;
    if (std::llabs(sl.unhedged_cash_e4) >= p.max_unhedged_e4) return;  // ledger hard stop

    // Fair ETF value implied by the future (ETF px terms).
    const Price fut_bid_cash = static_cast<Price>(sl.remote.leg_bid / p.px_ratio);
    const Price fut_ask_cash = static_cast<Price>(sl.remote.leg_ask / p.px_ratio);

    // Future rich: buy ETF at ask now, hedge with future sell later.
    if (fut_bid_cash - top.ask_px >= p.entry_edge_e4) {
      sl.pending = ctx.place(p.cash_inst, Side::Buy, Offset::Open, top.ask_px, p.cash_clip,
                             Tif::Ioc, now);
      if (sl.pending != kNoOrder) ++sl.lifts;
      return;
    }
    // Future cheap: sell ETF inventory, hedge with future buy.
    if (top.bid_px - fut_ask_cash >= p.entry_edge_e4) {
      const Position* pos = ctx.position(p.cash_inst);
      if (pos && pos->sellable >= p.cash_clip) {
        sl.pending = ctx.place(p.cash_inst, Side::Sell, Offset::Open, top.bid_px, p.cash_clip,
                               Tif::Ioc, now);
        if (sl.pending != kNoOrder) ++sl.lifts;
      }
    }
  }

  template <typename Ctx>
  void maybe_hedge(Ctx& ctx, std::size_t k, Nanos now) {
    const ArbPairParams& p = pairs_[k];
    Slot& sl = slots_[k];
    if (sl.intent_out || !sl.have_remote) return;
    const Instrument& fut = ctx.instrument(p.fut_inst);
    // One futures lot hedges its CNY notional worth of cash exposure.
    const std::int64_t lot_notional =
        static_cast<std::int64_t>(sl.remote.leg_bid) * fut.multiplier;
    if (lot_notional <= 0) return;
    const Qty lots = static_cast<Qty>(std::llabs(sl.unhedged_cash_e4) / lot_notional);
    if (lots < p.hedge_band_lots) return;

    HedgeIntent h{};
    h.pair_id = p.pair_id;
    h.origin_ts = now;
    h.qty = lots;
    h.inst = p.fut_inst;
    h.strategy = id();
    // Long cash → sell futures; the budget is priced off the remote top.
    if (sl.unhedged_cash_e4 > 0) {
      h.side = Side::Sell;
      h.limit_px = sl.remote.leg_bid - p.hedge_budget_ticks * fut.tick;
    } else {
      h.side = Side::Buy;
      h.limit_px = sl.remote.leg_ask + p.hedge_budget_ticks * fut.tick;
    }
    ctx.send_intent(h);
    sl.intent_out = true;
    ++sl.intents_sent;
  }

  std::vector<ArbPairParams> pairs_;
  std::vector<Slot> slots_;
  std::vector<std::int32_t> by_cash_;
  // Bridge latency allowance added to remote-age checks: BasisSummary.ts is
  // stamped on local injection, origin_ts at the far site.
  Nanos bridge_allowance_ = 5 * kNsPerMs;
};

}  // namespace plat
