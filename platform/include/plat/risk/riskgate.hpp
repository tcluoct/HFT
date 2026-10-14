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
// passed the cap); the duplicate breaker is a fixed 16-slot ring. Account
// counters are a linear-scanned flat vector — a site trades a handful of
// accounts, and a 3-entry scan beats a map's pointer chase.
//
// Check order matches ARCHITECTURE.md §10.1's table; each check has a unit
// test that proves it can fire and, where relevant, that it doesn't misfire.

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
#include "plat/risk/kill_switch.hpp"
#include "plat/pnl/pnl.hpp"
#include "plat/ref/instrument.hpp"

namespace plat {

struct RiskLimits {
  Qty max_order_qty = 10'000;
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
};

// check_new/check_cancel return this when every check passes.
inline constexpr RiskCheck kRiskPass = static_cast<RiskCheck>(0);

class RiskGate {
 public:
  RiskGate(const InstrumentTable& table, const Oms& oms, const PnlLedger& pnl,
           RiskLimits limits)
      : table_(&table), oms_(&oms), pnl_(&pnl), limits_(limits),
        stock_books_(table.size(), nullptr), fut_books_(table.size(), nullptr) {
    if (limits_.duplicate_window > kDupCap) limits_.duplicate_window = kDupCap;
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
    if (const Price ref = reference_px(r.inst, in); ref > 0) {
      const Price band = static_cast<Price>(limits_.price_collar_ticks) * in.tick;
      if (r.px > ref + band || r.px < ref - band) return RiskCheck::PriceCollar;
    }

    // 3) Size / notional caps.
    if (r.qty <= 0 || r.qty > limits_.max_order_qty) return RiskCheck::SizeNotional;
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
    if (is_duplicate(r, now)) return RiskCheck::DuplicateBurst;

    note_action(c, now, /*cancel=*/false);
    remember(r, now);
    return kRiskPass;
  }

  [[nodiscard]] RiskCheck check_cancel(const CancelReq& c, Nanos now) {
    // No kill-switch gate here by design: cancels must flow under halt —
    // halting *is* cancelling resting orders (§10.3).
    const Instrument& in = table_->at(c.inst);
    if (!phase_accepts_cancels(phase_[static_cast<std::size_t>(in.venue)]))
      return RiskCheck::PhaseGate;
    Counters& cnt = counters_for(c.account);
    if (!rate_ok(cnt, now)) return RiskCheck::RateLimit;
    if (!cancel_ratio_ok(cnt)) return RiskCheck::CancelRatio;
    note_action(cnt, now, /*cancel=*/true);
    return kRiskPass;
  }

  [[nodiscard]] std::uint32_t day_actions(AccountId a) const {
    for (const auto& c : counters_)
      if (c.account == a) return c.day_actions;
    return 0;
  }

  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    h.mix(kill_.state_hash());
    for (const auto& c : counters_) {
      h.mix(c.account);
      h.mix(c.day_actions);
      h.mix(c.day_cancels);
      for (std::uint32_t k = 0; k < c.count; ++k) h.mix(c.at(k));
    }
    for (std::uint32_t k = 0; k < dup_count_; ++k) {  // field-wise: Recent has padding
      const Recent& d = dup_at(k);
      h.mix(d.inst);
      h.mix(d.px);
      h.mix(d.qty);
      h.mix(d.strategy);
      h.mix(d.side);
      h.mix(d.ts);
    }
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
    append(out, static_cast<std::uint64_t>(dup_count_));
    for (std::uint32_t k = 0; k < dup_count_; ++k) append(out, dup_at(k));
  }
  std::size_t load(const std::uint8_t* p, std::size_t avail) {
    counters_.clear();
    dup_head_ = dup_count_ = 0;
    std::size_t off = 0;
    off = kill_.load(p, off, avail,
                     [](const std::uint8_t* q, std::size_t o, std::size_t a, auto& v) {
                       return read(q, o, a, v);
                     });
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
    off = read(p, off, avail, n);
    for (std::uint64_t i = 0; i < n; ++i) {
      Recent r{};
      off = read(p, off, avail, r);
      dup_push(r);
    }
    return off;
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

  [[nodiscard]] bool is_duplicate(const NewOrderReq& r, Nanos now) const {
    for (std::uint32_t k = 0; k < dup_count_; ++k) {
      const Recent& d = dup_at(k);
      if (now - d.ts > limits_.duplicate_horizon) continue;
      if (d.inst == r.inst && d.px == r.px && d.qty == r.qty && d.side == r.side &&
          d.strategy == r.strategy)
        return true;
    }
    return false;
  }
  void remember(const NewOrderReq& r, Nanos now) {
    dup_push({r.inst, r.px, r.qty, r.strategy, r.side, now});
  }

  [[nodiscard]] bool rate_ok(Counters& c, Nanos now) const {
    while (c.count && now - c.at(0) >= kNsPerSec) c.pop_front();
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
  std::vector<const StockBook*> stock_books_;  // dense-id mirrors, one load per check
  std::vector<const FutBook*> fut_books_;
  Phase phase_[kVenueCount] = {};
  std::vector<Counters> counters_;  // linear scan: a site trades a handful of accounts

  struct Recent {
    InstrumentId inst;
    Price px;
    Qty qty;
    StrategyId strategy;
    Side side;
    Nanos ts;
  };
  static constexpr std::uint32_t kDupCap = 16;
  std::array<Recent, kDupCap> dup_{};
  std::uint32_t dup_head_ = 0;
  std::uint32_t dup_count_ = 0;

  [[nodiscard]] const Recent& dup_at(std::uint32_t k) const noexcept {
    return dup_[(dup_head_ + k) % kDupCap];
  }
  void dup_push(const Recent& r) noexcept {
    if (dup_count_ == limits_.duplicate_window) {
      dup_head_ = (dup_head_ + 1) % kDupCap;  // evict oldest
      --dup_count_;
    }
    // Zero the slot first: a plain struct copy may carry the temporary's
    // indeterminate padding into the snapshot bytes save() writes.
    Recent& slot = dup_[(dup_head_ + dup_count_++) % kDupCap];
    std::memset(&slot, 0, sizeof(slot));
    slot.inst = r.inst;
    slot.px = r.px;
    slot.qty = r.qty;
    slot.strategy = r.strategy;
    slot.side = r.side;
    slot.ts = r.ts;
  }
};

}  // namespace plat
