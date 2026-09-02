#pragma once
// plat/pnl/pnl.hpp — positions and PnL (pnld graduation, §9.5). One ledger
// per (account, strategy, instrument) implementing the two Chinese-market
// rules that reshape strategy design:
//
//   Stocks, T+1 (§4.2): `sellable` is yesterday's carry minus today's sells;
//   today's buys raise `total` but become sellable only at the EOD roll.
//   T+0 trading therefore works a 底仓 — sell inventory first, buy it back.
//
//   Futures, 平今/平昨: today's and yesterday's lots are separate buckets
//   with different close fees; Offset::CloseToday draws from today's,
//   CloseYesterday from yesterday's, and riskgate validates availability
//   before the order leaves the building.
//
// Fees come from the one shared fee library (§4.2) — backtest and live PnL
// cannot disagree by construction.

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "hft/common/platform.hpp"
#include "plat/codec/msgs.hpp"
#include "plat/core/hash.hpp"
#include "plat/core/types.hpp"
#include "plat/ref/fees.hpp"
#include "plat/ref/instrument.hpp"

namespace plat {

struct PosKey {
  AccountId account;
  StrategyId strategy;
  InstrumentId inst;
  auto operator<=>(const PosKey&) const = default;
};

struct Position {
  // Stocks: net long inventory.
  Qty total = 0;         // shares held
  Qty sellable = 0;      // T+1: sellable today
  Qty bought_today = 0;
  Qty sold_today = 0;
  // Futures: four buckets.
  Qty long_yd = 0, long_td = 0;
  Qty short_yd = 0, short_td = 0;
  // Cost bases (1e-4 CNY, px terms). Futures keep *per-direction* costs —
  // a shared blend misstates realized PnL whenever both directions traded.
  std::int64_t avg_cost_e4 = 0;    // stocks: volume-weighted holding cost
  std::int64_t long_cost_e4 = 0;   // futures: open cost of the long book
  std::int64_t short_cost_e4 = 0;  // futures: open cost of the short book
  std::int64_t realized_e4 = 0;
  std::int64_t fees_e4 = 0;

  [[nodiscard]] Qty net() const noexcept {
    return total + (long_yd + long_td) - (short_yd + short_td);
  }
};

class PnlLedger {
 public:
  PnlLedger(const InstrumentTable& table, FeeSchedule fees)
      : table_(&table), fees_(fees), marks_(table.size(), 0) {}

  // Grant opening inventory (the T+0 底仓) / carried futures positions.
  void carry_in(AccountId a, StrategyId s, InstrumentId i, Qty stock_qty, Qty fut_long_yd = 0,
                Qty fut_short_yd = 0, Price cost_px = 0) {
    Position& p = pos_[{a, s, i}];
    const Price cost = cost_px ? cost_px : table_->at(i).prev_close;
    p.total += stock_qty;
    p.sellable += stock_qty;
    p.long_yd += fut_long_yd;
    p.short_yd += fut_short_yd;
    p.avg_cost_e4 = cost;
    if (fut_long_yd) p.long_cost_e4 = cost;
    if (fut_short_yd) p.short_cost_e4 = cost;
    marks_[i] = table_->at(i).prev_close;
  }

  void on_exec(const ExecReport& e) {
    if (e.type != ExecType::Fill || e.cl_id == kNoOrder) return;
    const Instrument& in = table_->at(e.inst);
    Position& p = pos_[{e.account, e.strategy, e.inst}];
    const std::int64_t fee = fee_for(fees_, in, e.side, e.offset, e.px, e.qty);
    p.fees_e4 += fee;
    p.realized_e4 -= fee;
    marks_[e.inst] = e.px;  // our own print is a mark

    if (in.is_stock()) apply_stock(p, e);
    else apply_future(p, in, e);
  }

  // One indexed store per market print (dense-id flat table) — never a
  // positions scan, never a tree walk.
  HFT_FORCE_INLINE void mark(InstrumentId i, Price px) noexcept { marks_[i] = px; }
  [[nodiscard]] Price mark_for(InstrumentId i) const noexcept {
    const Price m = marks_[i];
    return m ? m : table_->at(i).prev_close;
  }

  [[nodiscard]] const Position* find(AccountId a, StrategyId s, InstrumentId i) const {
    const auto it = pos_.find({a, s, i});
    return it == pos_.end() ? nullptr : &it->second;
  }
  // Walk every ledger entry (cold path: EOD carry, reconciliation, reports).
  // Covers positions whose strategy has no live instance this session — a
  // stopped strategy's carry must keep flowing through the .positions chain.
  template <typename F>
  void for_each(F&& f) const {
    for (const auto& [k, p] : pos_) f(k, p);
  }
  [[nodiscard]] Position& at(AccountId a, StrategyId s, InstrumentId i) {
    return pos_[{a, s, i}];
  }

  // Unrealized on the current mark, per direction for futures.
  [[nodiscard]] std::int64_t unrealized_e4(const PosKey& k, const Position& p) const {
    const Instrument& in = table_->at(k.inst);
    const Price mark = mark_for(k.inst);
    if (in.is_stock()) return p.total ? (mark - p.avg_cost_e4) * p.total : 0;
    const Qty lg = p.long_yd + p.long_td;
    const Qty sh = p.short_yd + p.short_td;
    std::int64_t u = 0;
    if (lg) u += (mark - p.long_cost_e4) * lg * in.multiplier;
    if (sh) u += (p.short_cost_e4 - mark) * sh * in.multiplier;
    return u;
  }

  [[nodiscard]] std::int64_t total_realized_e4(StrategyId s) const {
    std::int64_t r = 0;
    for (const auto& [k, p] : pos_)
      if (k.strategy == s) r += p.realized_e4;
    return r;
  }
  [[nodiscard]] std::int64_t total_unrealized_e4(StrategyId s) const {
    std::int64_t r = 0;
    for (const auto& [k, p] : pos_)
      if (k.strategy == s) r += unrealized_e4(k, p);
    return r;
  }
  [[nodiscard]] std::int64_t total_fees_e4(StrategyId s) const {
    std::int64_t r = 0;
    for (const auto& [k, p] : pos_)
      if (k.strategy == s) r += p.fees_e4;
    return r;
  }

  // Margin in use across futures positions of an account (§10.1 #6).
  [[nodiscard]] std::int64_t margin_used_e4(AccountId a) const {
    std::int64_t m = 0;
    for (const auto& [k, p] : pos_) {
      if (k.account != a) continue;
      const Instrument& in = table_->at(k.inst);
      if (in.is_stock()) continue;
      const Qty gross = p.long_yd + p.long_td + p.short_yd + p.short_td;
      m += margin_e4(in, mark_for(k.inst), gross);
    }
    return m;
  }

  // Operator bookkeeping transfer (ops console, §14): move qty between two
  // strategy books in the same account — a stopped strategy's carry to a
  // live one, a fat finger's lots to the manual book. Cost basis moves with
  // the lots (blended into the destination); nothing is realized and no
  // order leaves the building. Returns the qty actually moved (clamped to
  // what the source holds); yd drains before td, so carried lots move first.
  Qty transfer(AccountId a, StrategyId from, StrategyId to, InstrumentId i, Side side,
               Qty qty) {
    if (qty <= 0 || from == to) return 0;
    const auto it = pos_.find({a, from, i});
    if (it == pos_.end()) return 0;
    Position& src = it->second;                  // std::map: stable across the
    Position& dst = pos_[{a, to, i}];            // destination's insertion
    if (table_->at(i).is_stock()) {
      const Qty n = qty < src.total ? qty : src.total;
      if (n <= 0) return 0;
      const Qty new_total = dst.total + n;
      dst.avg_cost_e4 = (dst.avg_cost_e4 * dst.total + src.avg_cost_e4 * n) / new_total;
      dst.total = new_total;
      src.total -= n;
      const Qty sellable = n < src.sellable ? n : src.sellable;
      src.sellable -= sellable;
      dst.sellable += sellable;
      return n;
    }
    Qty& src_yd = side == Side::Buy ? src.long_yd : src.short_yd;
    Qty& src_td = side == Side::Buy ? src.long_td : src.short_td;
    Qty& dst_yd = side == Side::Buy ? dst.long_yd : dst.short_yd;
    Qty& dst_td = side == Side::Buy ? dst.long_td : dst.short_td;
    std::int64_t& src_cost = side == Side::Buy ? src.long_cost_e4 : src.short_cost_e4;
    std::int64_t& dst_cost = side == Side::Buy ? dst.long_cost_e4 : dst.short_cost_e4;
    const Qty have = src_yd + src_td;
    const Qty n = qty < have ? qty : have;
    if (n <= 0) return 0;
    const Qty prior = dst_yd + dst_td;
    dst_cost = (dst_cost * prior + src_cost * n) / (prior + n);
    const Qty from_yd = n < src_yd ? n : src_yd;
    src_yd -= from_yd;
    dst_yd += from_yd;
    src_td -= n - from_yd;
    dst_td += n - from_yd;
    return n;
  }

  // EOD roll (§11.3): today becomes yesterday; sellable resets to total.
  void roll_day() {
    for (auto& [k, p] : pos_) {
      p.sellable = p.total;
      p.bought_today = p.sold_today = 0;
      p.long_yd += p.long_td;
      p.long_td = 0;
      p.short_yd += p.short_td;
      p.short_td = 0;
    }
  }

  // 对账 (§11.3): our fill aggregates must equal the venue's member stats.
  struct ReconResult {
    bool ok;
    Qty buy_qty, sell_qty;
    Qty venue_buy_qty, venue_sell_qty;
  };
  [[nodiscard]] ReconResult reconcile_fills(Qty venue_buy, Qty venue_sell) const {
    ReconResult r{};
    for (const auto& [k, p] : pos_) {
      if (table_->at(k.inst).is_stock()) {
        r.buy_qty += p.bought_today;
        r.sell_qty += p.sold_today;
      }
    }
    // Futures fills tracked directly as they arrive.
    r.buy_qty += fut_buy_qty_;
    r.sell_qty += fut_sell_qty_;
    r.venue_buy_qty = venue_buy;
    r.venue_sell_qty = venue_sell;
    r.ok = r.buy_qty == venue_buy && r.sell_qty == venue_sell;
    return r;
  }

  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    for (const auto& [k, p] : pos_) {
      h.mix(k);
      h.mix(p.total);
      h.mix(p.sellable);
      h.mix(p.long_yd);
      h.mix(p.long_td);
      h.mix(p.short_yd);
      h.mix(p.short_td);
      h.mix(p.long_cost_e4);
      h.mix(p.short_cost_e4);
      h.mix(p.realized_e4);
      h.mix(p.fees_e4);
    }
    for (InstrumentId i = 0; i < marks_.size(); ++i) {
      if (marks_[i] == 0) continue;
      h.mix(i);
      h.mix(marks_[i]);
    }
    h.mix(fut_buy_qty_);
    h.mix(fut_sell_qty_);
    return h.value();
  }

  void save(std::vector<std::uint8_t>& out) const {
    const std::uint64_t n = pos_.size();
    append(out, n);
    for (const auto& [k, p] : pos_) {
      append(out, k);
      append(out, p);
    }
    // Marks are behavioral state: margin/exposure checks read them (§11.1).
    std::uint64_t m = 0;
    for (const Price px : marks_)
      if (px) ++m;
    append(out, m);
    for (InstrumentId i = 0; i < marks_.size(); ++i) {
      if (marks_[i] == 0) continue;
      append(out, i);
      append(out, marks_[i]);
    }
    append(out, fut_buy_qty_);
    append(out, fut_sell_qty_);
  }
  std::size_t load(const std::uint8_t* p, std::size_t avail) {
    pos_.clear();
    marks_.assign(table_->size(), 0);
    std::uint64_t n = 0;
    std::size_t off = read(p, 0, avail, n);
    for (std::uint64_t i = 0; i < n; ++i) {
      PosKey k{};
      Position v{};
      off = read(p, off, avail, k);
      off = read(p, off, avail, v);
      pos_[k] = v;
    }
    off = read(p, off, avail, n);
    for (std::uint64_t i = 0; i < n; ++i) {
      InstrumentId id{};
      Price px{};
      off = read(p, off, avail, id);
      off = read(p, off, avail, px);
      marks_[id] = px;
    }
    off = read(p, off, avail, fut_buy_qty_);
    off = read(p, off, avail, fut_sell_qty_);
    return off;
  }

 private:
  void apply_stock(Position& p, const ExecReport& e) {
    if (e.side == Side::Buy) {
      // New cost basis = volume-weighted across the enlarged holding.
      const Qty new_total = p.total + e.qty;
      if (new_total > 0)
        p.avg_cost_e4 = (p.avg_cost_e4 * p.total + e.px * e.qty) / new_total;
      p.total = new_total;
      p.bought_today += e.qty;
      // NOT sellable today — that is the T+1 rule.
    } else {
      p.total -= e.qty;
      p.sellable -= e.qty;
      p.sold_today += e.qty;
      p.realized_e4 += (e.px - p.avg_cost_e4) * e.qty;
    }
  }

  void apply_future(Position& p, const Instrument& in, const ExecReport& e) {
    if (e.side == Side::Buy) fut_buy_qty_ += e.qty;
    else fut_sell_qty_ += e.qty;

    if (e.offset == Offset::Open) {
      if (e.side == Side::Buy) {
        const Qty prior = p.long_yd + p.long_td;
        p.long_cost_e4 = prior + e.qty > 0
                             ? (p.long_cost_e4 * prior + e.px * e.qty) / (prior + e.qty)
                             : e.px;
        p.long_td += e.qty;
      } else {
        const Qty prior = p.short_yd + p.short_td;
        p.short_cost_e4 = prior + e.qty > 0
                              ? (p.short_cost_e4 * prior + e.px * e.qty) / (prior + e.qty)
                              : e.px;
        p.short_td += e.qty;
      }
      return;
    }
    // Close: buy closes shorts, sell closes longs — realized against that
    // direction's own cost basis.
    if (e.side == Side::Sell) {
      Qty& bucket = e.offset == Offset::CloseToday ? p.long_td : p.long_yd;
      bucket -= e.qty;
      p.realized_e4 += (e.px - p.long_cost_e4) * e.qty * in.multiplier;
    } else {
      Qty& bucket = e.offset == Offset::CloseToday ? p.short_td : p.short_yd;
      bucket -= e.qty;
      p.realized_e4 += (p.short_cost_e4 - e.px) * e.qty * in.multiplier;
    }
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
  FeeSchedule fees_;
  std::map<PosKey, Position> pos_;  // cold: touched per fill, not per tick
  std::vector<Price> marks_;        // dense-id last print; 0 = no print yet
  Qty fut_buy_qty_ = 0;
  Qty fut_sell_qty_ = 0;
};

}  // namespace plat
