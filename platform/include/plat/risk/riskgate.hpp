#pragma once
// plat/risk/riskgate.hpp — in-line pre-trade risk (riskgate graduation,
// §10.1). The only component allowed to hand an order to a gateway; every
// check reads local state (book mirrors, position ledger, its own counters)
// so the full battery costs nanoseconds. Denials are messages, never silence.
//
// Hot-path discipline (the riskgate lab's): the order path allocates
// nothing. Book mirrors are dense-id flat arrays (InstrumentIds index the
// per-day table, symtab-style); the per-second rate window is a fixed-size
// circular buffer sized by the limit itself (an entry can only exist if it
// passed the cap); the duplicate breaker is a fixed 16-slot ring
// (dup_breaker.hpp). Account counters are a linear-scanned flat vector — a
// site trades a handful of accounts; a 3-entry scan beats a map's pointer chase.
//
// Check order matches ARCHITECTURE.md §10.1's table; each check has a unit
// test that proves it can fire and, where relevant, that it doesn't misfire.
// #13–#16 are the risk desk's additions (state in session_guard.hpp): a
// latched daily stop-loss that still lets the book be closed, the
// exchanges' per-contract lines (日内开仓限额, 单合约日撤单), and cancel-target
// hygiene. Cancels under an armed kill scope bypass every throttle — a halt
// must always be able to pull its resting orders. #17–#20: SSE/SZSE cash-equity
// rules (stock_rules.hpp) — 申报数量, 价格笼子, ST daily buys, concentration.

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include "hft/common/platform.hpp"
#include "plat/codec/msgs.hpp"
#include "plat/core/phase.hpp"
#include "plat/core/time.hpp"
#include "plat/core/types.hpp"
#include "plat/md/fut_book.hpp"
#include "plat/md/stock_book.hpp"
#include "plat/oms/oms.hpp"
#include "plat/risk/dup_breaker.hpp"
#include "plat/risk/kill_switch.hpp"
#include "plat/risk/limits.hpp"
#include "plat/risk/session_guard.hpp"
#include "plat/risk/stock_rules.hpp"
#include "plat/pnl/pnl.hpp"
#include "plat/ref/instrument.hpp"

namespace plat {

class RiskGate {
 public:
  RiskGate(const InstrumentTable& table, const Oms& oms, const PnlLedger& pnl,
           RiskLimits limits)
      : table_(&table), oms_(&oms), pnl_(&pnl), limits_(limits),
        dup_(limits.duplicate_window, limits.duplicate_horizon), guard_(table.size()),
        stock_books_(table.size(), nullptr),
        fut_books_(table.size(), nullptr) {
  }

  // Book mirrors: dense-id flat arrays — one indexed load per check, no
  // tree walk (shared-memory tops in production, direct pointers here).
  void attach_stock_book(InstrumentId i, const StockBook* b) { stock_books_[i] = b; }
  void attach_fut_book(InstrumentId i, const FutBook* b) { fut_books_[i] = b; }

  void set_phase(Venue v, Phase p) { phase_[static_cast<std::size_t>(v)] = p; }
  [[nodiscard]] KillSwitch& kill() noexcept { return kill_; }
  [[nodiscard]] const KillSwitch& kill() const noexcept { return kill_; }
  // Cold-path only (console/ops use): the price the collar check (§10.1 #2)
  // is currently anchored to for this instrument — last trade, or prev_close
  // before any book arrives.
  [[nodiscard]] Price reference_px_of(InstrumentId i) const {
    return reference_px(i, table_->at(i));
  }

  // Evaluate a new order. RiskCheck return of 0 (as optional) means pass.
  // The engine publishes RiskDeny on failure and forwards on pass.
  [[nodiscard]] RiskCheck check_new(const NewOrderReq& r, Nanos now) {
    const Instrument& in = table_->at(r.inst);

    // 12) Kill switch first — cheapest and overrides everything.
    if (kill_.halted(r.account, r.strategy)) [[unlikely]] return RiskCheck::KillSwitch;

    // 1) Phase gate.
    if (!phase_accepts_orders(phase_[static_cast<std::size_t>(in.venue)]))
      return RiskCheck::PhaseGate;

    // 11) Stale-data interlock: refuse to trade a quarantined or aged book.
    if (!book_fresh(r.inst, now)) return RiskCheck::StaleData;

    // 2) Price collar: inside the limit band AND inside the fat-finger band
    //    around the last traded price.
    if (r.px < in.lower_limit || r.px > in.upper_limit) return RiskCheck::PriceCollar;
    // 2b) Price must sit on the instrument's tick grid (合约最小变动价位).
    if (in.tick > 0 && r.px % in.tick != 0) return RiskCheck::TickSize;
    if (const Price ref = reference_px(r.inst, in); ref > 0 && !in.is_stock()) {
      const Price band = static_cast<Price>(limits_.price_collar_ticks) * in.tick;
      if (r.px > ref + band || r.px < ref - band) return RiskCheck::PriceCollar;
    }

    // 3) Size / notional caps.
    if (r.qty <= 0 ||
        r.qty > (in.is_stock() ? limits_.max_stock_order_qty : limits_.max_order_qty))
      return RiskCheck::SizeNotional;
    if (notional_e4(in, r.px, r.qty) > limits_.max_order_notional_e4)
      return RiskCheck::SizeNotional;

    // 4) Position / exposure cap per strategy (net + this order's add).
    if (exposure_after(r) > limits_.max_net_notional_e4) return RiskCheck::Exposure;

    // 5) Sellable inventory (stocks, T+1): sells draw down carry-in minus
    //    today's sells minus open sell orders already working.
    if (in.is_stock() && r.side == Side::Sell) {
      const Position* p = pnl_->find(r.account, r.strategy, r.inst);
      const Qty sellable = p ? p->sellable : 0;
      const Qty working = oms_open_sells(r);
      if (r.qty + working > sellable) return RiskCheck::SellableInventory;
    }

    // 5b) Futures close validation rides the same slot: close-today must fit
    //     today's bucket, close-yesterday yesterday's.
    if (limits_.check_close_position && !in.is_stock() && r.offset != Offset::Open) {
      const Position* p = pnl_->find(r.account, r.strategy, r.inst);
      const Qty avail = closeable(p, r.side, r.offset);
      if (r.qty > avail) return RiskCheck::SellableInventory;
    }

    // 17–20) Cash-equity exchange rules.
    if (in.is_stock())
      if (const RiskCheck v = check_stock(r, in); v != kRiskPass) return v;

    // 13) Daily stop-loss: only orders that add risk are refused.
    const bool adds = in.is_stock() ? r.side == Side::Buy : r.offset == Offset::Open;
    if (adds && loss_stopped(r)) return RiskCheck::LossLimit;

    // 14) Per-contract daily open volume, reservation view like #4/#6.
    if (limits_.max_inst_open_qty > 0 && !in.is_stock() && r.offset == Offset::Open) {
      const Qty w = working(r, [](const OmsOrder& o) { return o.offset == Offset::Open; });
      if (guard_.opened(r.account, r.inst) + w + r.qty > limits_.max_inst_open_qty)
        return RiskCheck::InstOpenLimit;
    }

    // 6) Margin headroom (futures opens). Working open orders reserve margin
    //    at their order prices — an order burst must not pass the cap before
    //    its first fill lands (same discipline as the exposure check; §10.1
    //    reservation model, review R1).
    if (!in.is_stock() && r.offset == Offset::Open) {
      if (pnl_->margin_used_e4(r.account) + open_margin_e4(r) +
              margin_e4(in, r.px, r.qty) > limits_.max_margin_e4)
        return RiskCheck::Margin;
    }

    // 7) Rate token bucket per account (order actions = news + cancels).
    Counters& c = counters_for(r.account);
    if (!rate_ok(c, now)) return RiskCheck::RateLimit;

    // 8) Cancel-ratio guard is a cancel-side check; news pass through.

    // 9) Self-trade prevention: would this cross our own resting order?
    if (crosses_own(r)) return RiskCheck::SelfTrade;

    // 10) Duplicate/burst breaker: identical order repeated inside the
    //     repeat-fire horizon.
    if (dup_.is_duplicate(r, now)) return RiskCheck::DuplicateBurst;

    note_action(c, now, /*cancel=*/false);
    dup_.remember(r, now);
    return kRiskPass;
  }

  [[nodiscard]] RiskCheck check_cancel(const CancelReq& c, Nanos now) {
    // No kill-switch gate here by design: cancels must flow under halt —
    // halting *is* cancelling resting orders (§10.3).
    const Instrument& in = table_->at(c.inst);
    if (!phase_accepts_cancels(phase_[static_cast<std::size_t>(in.venue)]))
      return RiskCheck::PhaseGate;
    // 16) The target must be ours, open, and not already being cancelled.
    if (limits_.check_cancel_target && !cancel_target_ok(c, now)) return RiskCheck::CancelTarget;
    Counters& cnt = counters_for(c.account);
    if (kill_.halted(c.account, c.strategy)) [[unlikely]] {
      // Under an armed scope the throttles step aside: a halt that cannot
      // pull resting orders because the runaway already burned the cancel
      // budget (#7/#8/#15) is exactly the failure the kill switch exists to
      // prevent. Still counted, so the compliance numbers stay true.
      prune(cnt, now);
      if (cnt.count == cnt.ring.size()) cnt.pop_front();  // keep the ring bounded
    } else {
      if (!rate_ok(cnt, now)) return RiskCheck::RateLimit;
      if (!cancel_ratio_ok(cnt)) return RiskCheck::CancelRatio;
      // 15) Per-contract daily cancel line.
      if (limits_.max_inst_cancels &&
          guard_.cancels(c.account, c.inst) >= limits_.max_inst_cancels)
        return RiskCheck::InstCancelLimit;
    }
    note_action(cnt, now, /*cancel=*/true);
    guard_.on_cancel_sent(c.account, c.inst, c.cl_id, now);
    return kRiskPass;
  }

  // Execution-report feed (engine dispatch, after OMS/PnL): per-contract
  // filled opens for #14, in-flight cancel resolution for #16.
  void on_exec(const ExecReport& e) { guard_.on_exec(e, table_->at(e.inst).is_stock()); }

  [[nodiscard]] const SessionGuard& session() const noexcept { return guard_; }

  [[nodiscard]] std::uint32_t day_actions(AccountId a) const {
    for (const auto& c : counters_)
      if (c.account == a) return c.day_actions;
    return 0;
  }

  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    h.mix(kill_.state_hash());
    h.mix(guard_.state_hash());
    for (const auto& c : counters_) {
      h.mix(c.account);
      h.mix(c.day_actions);
      h.mix(c.day_cancels);
      for (std::uint32_t k = 0; k < c.count; ++k) h.mix(c.at(k));
    }
    dup_.mix_into(h);
    return h.value();
  }

  // Full snapshot (§11.1): rate windows and the duplicate ring are behavioral
  // state — recovery that dropped them could admit a burst the uninterrupted
  // run would have denied, and the state-hash equality test would catch it.
  void save(std::vector<std::uint8_t>& out) const {
    // Venue phases are consumed state (set from PhaseChange envelopes): a
    // snapshot that dropped them would phase-gate orders the uninterrupted
    // run allowed — the recovery hash test caught exactly this. Kill-switch
    // arm state is the same class of state with higher stakes: recovery that
    // dropped it would re-open a halted order path (§10.3, review R1).
    kill_.save(out, [](std::vector<std::uint8_t>& o, const auto& v) { append(o, v); });
    for (const Phase p : phase_) append(out, p);
    append(out, static_cast<std::uint64_t>(counters_.size()));
    for (const auto& c : counters_) {
      append(out, c.account);
      append(out, c.day_actions);
      append(out, c.day_cancels);
      append(out, static_cast<std::uint64_t>(c.count));
      for (std::uint32_t k = 0; k < c.count; ++k) append(out, c.at(k));
    }
    const auto app = [](std::vector<std::uint8_t>& o, const auto& v) { append(o, v); };
    dup_.save(out, app);
    guard_.save(out, app);
  }
  std::size_t load(const std::uint8_t* p, std::size_t avail) {
    counters_.clear();
    const auto rd = [](const std::uint8_t* q, std::size_t o, std::size_t a, auto& v) {
      return read(q, o, a, v);
    };
    std::size_t off = kill_.load(p, 0, avail, rd);
    for (Phase& ph : phase_) off = read(p, off, avail, ph);
    std::uint64_t n = 0;
    off = read(p, off, avail, n);
    for (std::uint64_t i = 0; i < n; ++i) {
      AccountId a{};
      off = read(p, off, avail, a);
      Counters& c = counters_for(a);
      off = read(p, off, avail, c.day_actions);
      off = read(p, off, avail, c.day_cancels);
      std::uint64_t w = 0;
      off = read(p, off, avail, w);
      for (std::uint64_t j = 0; j < w; ++j) {
        Nanos t{};
        off = read(p, off, avail, t);
        c.push(t);
      }
    }
    off = dup_.load(p, off, avail, rd);
    return guard_.load(p, off, avail, rd);
  }

 private:
  // Per-account action counters. The rolling window is a circular buffer
  // whose capacity IS the per-second cap: an action is only recorded after
  // it passed the cap, so the buffer can never need more slots.
  struct Counters {
    AccountId account = 0;
    std::uint32_t day_actions = 0;
    std::uint32_t day_cancels = 0;
    std::uint32_t head = 0;
    std::uint32_t count = 0;
    std::vector<Nanos> ring;  // sized once at first touch; never reallocates

    [[nodiscard]] Nanos at(std::uint32_t k) const noexcept {
      return ring[(head + k) % ring.size()];
    }
    void push(Nanos t) noexcept { ring[(head + count++) % ring.size()] = t; }
    void pop_front() noexcept {
      head = (head + 1) % static_cast<std::uint32_t>(ring.size());
      --count;
    }
  };

  Counters& counters_for(AccountId a) {
    for (auto& c : counters_)
      if (c.account == a) [[likely]] return c;
    Counters& c = counters_.emplace_back();  // cold: first action of an account
    c.account = a;
    c.ring.resize(limits_.max_actions_per_sec ? limits_.max_actions_per_sec : 1);
    return c;
  }

  // #13: latched scopes first (empty-vector fast path); the ledger walk only
  // runs while the scope still trades and a limit is configured.
  [[nodiscard]] bool loss_stopped(const NewOrderReq& r) {
    if (guard_.loss_latched(r.account, r.strategy)) [[unlikely]] return true;
    if (limits_.max_strategy_loss_e4 > 0 &&
        pnl_->strategy_day_pnl_e4(r.strategy) <= -limits_.max_strategy_loss_e4) {
      guard_.latch_strategy(r.strategy);
      return true;
    }
    if (limits_.max_account_loss_e4 > 0 &&
        pnl_->account_day_pnl_e4(r.account) <= -limits_.max_account_loss_e4) {
      guard_.latch_account(r.account);
      return true;
    }
    return false;
  }

  [[nodiscard]] RiskCheck check_stock(const NewOrderReq& r, const Instrument& in) const {
    const Qty holding = pnl_->account_holding(r.account, r.inst);
    // 17) 申报数量.
    if (!stock_qty_ok(in, r.side, r.qty, holding)) return RiskCheck::StockQty;
    // 18) 价格笼子, continuous auction only (auctions have no cage).
    if (limits_.stock_cage_bps && (in.board != Board::Etf || limits_.stock_cage_etf) &&
        phase_[static_cast<std::size_t>(in.venue)] == Phase::Continuous) {
      const StockBook* b = stock_books_[r.inst];
      const StockBook::Top t = b ? b->top() : StockBook::Top{};
      const Price base = cage_base(r.side, t.bid_px, t.ask_px, t.last_px, in.prev_close);
      if (!in_price_cage(r.side, r.px, base, in.tick, limits_.stock_cage_bps,
                         limits_.stock_cage_ticks))
        return RiskCheck::PriceCage;
    }
    if (r.side != Side::Buy) return kRiskPass;
    const Qty buys = working(r, [](const OmsOrder& o) { return o.side == Side::Buy; });
    // 19) 风险警示股票 daily buys (filled + working + this).
    if (in.board == Board::StSpecial && limits_.st_daily_buy_qty > 0 &&
        guard_.opened(r.account, r.inst) + buys + r.qty > limits_.st_daily_buy_qty)
      return RiskCheck::StBuyLimit;
    // 20) Concentration across every book of the account.
    const Qty after = holding + buys + r.qty;
    if (limits_.max_stock_holding_qty > 0 && after > limits_.max_stock_holding_qty)
      return RiskCheck::Concentration;
    if (limits_.max_stock_holding_e4 > 0) {
      const Price ref = reference_px(r.inst, in);
      if (notional_e4(in, ref > r.px ? ref : r.px, after) > limits_.max_stock_holding_e4)
        return RiskCheck::Concentration;
    }
    return kRiskPass;
  }

  // Leaves of the account's other working orders on r.inst matching `pred`.
  template <typename Pred>
  [[nodiscard]] Qty working(const NewOrderReq& r, Pred&& pred) const {
    Qty q = 0;
    oms_->for_each_open(r.inst, [&](const OmsOrder& o) {
      if (o.account == r.account && o.cl_id != r.cl_id && pred(o)) q += o.leaves;
    });
    return q;
  }

  [[nodiscard]] bool cancel_target_ok(const CancelReq& c, Nanos now) const {
    const OmsOrder* o = oms_->find(c.cl_id);
    if (!o || !o->open() || o->account != c.account) return false;
    return !guard_.cancel_in_flight(c.cl_id, now, limits_.cancel_retry_after);
  }

  [[nodiscard]] bool book_fresh(InstrumentId i, Nanos now) const {
    if (const StockBook* b = stock_books_[i]) {
      return !b->quarantined() &&
             (b->last_event_ts() == 0 || now - b->last_event_ts() <= limits_.max_book_age);
    }
    if (const FutBook* b = fut_books_[i]) {
      // Futures snapshots have a cadence; age allowance covers it.
      return b->valid() && now - b->last_event_ts() <= limits_.max_book_age;
    }
    return false;  // no book attached: nothing to trust, refuse (§10.1 #11)
  }

  [[nodiscard]] Price reference_px(InstrumentId i, const Instrument& in) const {
    if (const StockBook* b = stock_books_[i]) return b->top().last_px;
    if (const FutBook* b = fut_books_[i]) return b->valid() ? b->snap().last_px : in.prev_close;
    return in.prev_close;
  }

  // Worst-case exposure after this order: position plus *working* orders
  // assumed filled in their direction, at the book reference price. An order
  // burst must not pass the cap before its first fill lands.
  [[nodiscard]] std::int64_t exposure_after(const NewOrderReq& r) const {
    const Instrument& in = table_->at(r.inst);
    const Price ref = [&] {
      const Price px = reference_px(r.inst, in);
      return px > 0 ? px : in.prev_close;
    }();
    std::int64_t net = 0;
    if (const Position* p = pnl_->find(r.account, r.strategy, r.inst))
      net = p->net() * ref * in.multiplier;
    std::int64_t open_buys = 0, open_sells = 0;
    oms_->for_each_open(r.inst, [&](const OmsOrder& o) {
      if (o.account != r.account || o.strategy != r.strategy || o.cl_id == r.cl_id) return;
      (o.side == Side::Buy ? open_buys : open_sells) +=
          o.leaves * ref * in.multiplier;
    });
    const std::int64_t add = notional_e4(in, r.px, r.qty);
    const std::int64_t high = net + open_buys + (r.side == Side::Buy ? add : 0);
    const std::int64_t low = net - open_sells - (r.side == Side::Sell ? add : 0);
    const std::int64_t hi_abs = high < 0 ? -high : high;
    const std::int64_t lo_abs = low < 0 ? -low : low;
    return hi_abs > lo_abs ? hi_abs : lo_abs;
  }

  // Margin reserved by this account's working futures opens across all
  // hosted futures instruments, at their order prices (the reservation view
  // of check #6). Excludes the in-flight request itself.
  [[nodiscard]] std::int64_t open_margin_e4(const NewOrderReq& r) const {
    std::int64_t m = 0;
    for (InstrumentId inst = 0; inst < fut_books_.size(); ++inst) {
      if (!fut_books_[inst]) continue;
      const Instrument& in = table_->at(inst);
      oms_->for_each_open(inst, [&](const OmsOrder& o) {
        if (o.account != r.account || o.offset != Offset::Open || o.cl_id == r.cl_id) return;
        m += margin_e4(in, o.px, o.leaves);
      });
    }
    return m;
  }

  [[nodiscard]] Qty oms_open_sells(const NewOrderReq& r) const {
    Qty q = 0;
    oms_->for_each_open(r.inst, [&](const OmsOrder& o) {
      if (o.side == Side::Sell && o.account == r.account && o.strategy == r.strategy &&
          o.cl_id != r.cl_id)
        q += o.leaves;
    });
    return q;
  }

  static Qty closeable(const Position* p, Side side, Offset offset) {
    if (!p) return 0;
    if (side == Side::Sell)  // closing long
      return offset == Offset::CloseToday ? p->long_td : p->long_yd;
    return offset == Offset::CloseToday ? p->short_td : p->short_yd;
  }

  [[nodiscard]] bool crosses_own(const NewOrderReq& r) const {
    bool crossed = false;
    oms_->for_each_open(r.inst, [&](const OmsOrder& o) {
      if (crossed || o.account != r.account || o.side == r.side || o.cl_id == r.cl_id) return;
      crossed = r.side == Side::Buy ? r.px >= o.px : r.px <= o.px;
    });
    return crossed;
  }

  static void prune(Counters& c, Nanos now) noexcept {
    while (c.count && now - c.at(0) >= kNsPerSec) c.pop_front();
  }
  [[nodiscard]] bool rate_ok(Counters& c, Nanos now) const {
    prune(c, now);
    if (c.count >= limits_.max_actions_per_sec) return false;  // cap == ring capacity
    return c.day_actions < limits_.max_actions_per_day;
  }

  [[nodiscard]] bool cancel_ratio_ok(const Counters& c) const {
    if (c.day_actions < limits_.cancel_ratio_min_actions) return true;
    return c.day_cancels * 100 < c.day_actions * limits_.cancel_ratio_pct;
  }

  void note_action(Counters& c, Nanos now, bool cancel) {
    c.push(now);
    ++c.day_actions;
    if (cancel) ++c.day_cancels;
  }

  template <typename T>
  static void append(std::vector<std::uint8_t>& out, const T& v) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), b, b + sizeof(T));
  }
  template <typename T>
  static std::size_t read(const std::uint8_t* p, std::size_t off, std::size_t avail, T& v) {
    if (off + sizeof(T) <= avail) std::memcpy(&v, p + off, sizeof(T));
    return off + sizeof(T);
  }

  const InstrumentTable* table_;
  const Oms* oms_;
  const PnlLedger* pnl_;
  RiskLimits limits_;
  KillSwitch kill_;
  DupBreaker dup_;
  SessionGuard guard_;
  std::vector<const StockBook*> stock_books_;  // dense-id mirrors, one load per check
  std::vector<const FutBook*> fut_books_;
  Phase phase_[kVenueCount] = {};
  std::vector<Counters> counters_;  // linear scan: a site trades a handful of accounts
};

}  // namespace plat
