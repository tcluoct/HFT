#pragma once
// plat/strat/t0alpha.hpp — short-horizon L2 alpha on stock inventory (§4.2,
// §9.1). The Chinese-market shape of intraday stock trading: T+1 means a
// fresh buy cannot round-trip today, so the strategy works a 底仓 —
// *sell first* out of carried inventory on ask-pressure signals, buy back on
// reversion/target. Inventory is restored by EOD (buy-back leg), realized
// PnL is the captured swing minus costs.
//
// Signal: EMA of L1 imbalance from the order-by-order book. Deliberately
// simple — the strategy exists to exercise the platform's T+1 ledger, IOC
// path, deny handling, and deterministic replay, not to be clever.
//
// Multi-instrument: one instance runs the whole 底仓 list; per-stock state
// (EMA, leg state machine) lives in dense-id slots — same discipline as
// momentum.hpp.

#include <cstdint>
#include <cstring>
#include <vector>

#include "plat/core/hash.hpp"
#include "plat/md/stock_book.hpp"
#include "plat/pnl/pnl.hpp"
#include "plat/ref/instrument.hpp"
#include "plat/strat/framework.hpp"
#include "plat/strat/signals.hpp"

namespace plat {

struct T0Params {
  std::int64_t ema_num = 1, ema_den = 8;
  std::int64_t entry_e4 = -4'000;  // sell when EMA imbalance below (ask pressure)
  std::int64_t exit_e4 = 0;        // buy back when pressure clears
  Qty clip = 200;                  // shares per round trip (lot multiple)
  int take_ticks = 3;              // buy back at profit ≥ this
  int stop_ticks = 5;              // give up beyond this adverse move
  int max_round_trips = 50;        // per instrument
};

class T0Alpha final : public Strategy<T0Alpha> {
 public:
  T0Alpha(StrategyId id, AccountId account, T0Params p, std::size_t table_size,
          const std::vector<InstrumentId>& instruments)
      : Strategy(id, account), p_(p), slots_(table_size), traded_(table_size, 0) {
    for (const InstrumentId i : instruments) {
      if (i < table_size) {
        traded_[i] = 1;
        slots_[i].ema = EmaE4(p_.ema_num, p_.ema_den);
      }
    }
  }

  template <typename Ctx>
  void on_book(Ctx& ctx, const BookTopEvt& t) {
    if (!traded_[t.inst] || t.bid_qty == 0 || t.ask_qty == 0) return;
    if (!live_) return;  // IOC legs only make sense in continuous trading
    if (t.limit_state != LimitState::Normal) return;  // no T0 games at the limit
    Slot& sl = slots_[t.inst];
    const std::int64_t sig = sl.ema.update(imbalance_e4(t.bid_qty, t.ask_qty));

    switch (sl.state) {
      case State::Flat: {
        if (sl.trips >= p_.max_round_trips) return;
        if (sig > p_.entry_e4) return;
        const Position* pos = ctx.position(t.inst);
        if (!pos || pos->sellable < p_.clip) return;  // T+1: no inventory, no trade
        // Sell into the bid (IOC taker leg).
        sl.pending = ctx.place(t.inst, Side::Sell, Offset::Open, t.bid_px, p_.clip, Tif::Ioc,
                               t.ts);
        if (sl.pending != kNoOrder) sl.state = State::SellSent;
        break;
      }
      case State::Sold: {
        const Instrument& in = ctx.instrument(t.inst);
        const Price take = sl.entry_px - p_.take_ticks * in.tick;
        const Price stop = sl.entry_px + p_.stop_ticks * in.tick;
        const bool reverted = sig >= p_.exit_e4;
        if (t.ask_px != 0 && (t.ask_px <= take || t.ask_px >= stop || reverted)) {
          sl.pending = ctx.place(t.inst, Side::Buy, Offset::Open, t.ask_px, sl.sold_qty,
                                 Tif::Ioc, t.ts);
          if (sl.pending != kNoOrder) sl.state = State::BuySent;
        }
        break;
      }
      case State::SellSent:
      case State::BuySent:
        break;  // one order in flight per instrument
    }
  }

  template <typename Ctx>
  void on_exec(Ctx&, const ExecReport& e) {
    if (e.inst >= traded_.size() || !traded_[e.inst]) return;
    Slot& sl = slots_[e.inst];
    if (e.cl_id != sl.pending) return;
    switch (e.type) {
      case ExecType::Fill:
        if (sl.state == State::SellSent) {
          sl.sold_qty += e.qty;
          sl.entry_px = e.px;
          if (e.leaves == 0) {
            sl.state = State::Sold;
            sl.pending = kNoOrder;
          }
        } else if (sl.state == State::BuySent) {
          sl.sold_qty -= e.qty;
          if (e.leaves == 0) {
            sl.pending = kNoOrder;
            if (sl.sold_qty <= 0) {
              sl.sold_qty = 0;
              sl.state = State::Flat;
              ++sl.trips;
            } else {
              sl.state = State::Sold;  // partial buy-back: keep working
            }
          }
        }
        break;
      case ExecType::Canceled:  // IOC remainder cancelled
      case ExecType::Reject:
        sl.pending = kNoOrder;
        sl.state = sl.sold_qty > 0 ? State::Sold : State::Flat;
        break;
      case ExecType::Ack:
      case ExecType::CancelReject:
        break;
    }
  }

  template <typename Ctx>
  void on_deny(Ctx&, const RiskDeny& d) {
    if (d.inst >= traded_.size() || !traded_[d.inst]) return;
    Slot& sl = slots_[d.inst];
    if (d.cl_id != sl.pending) return;
    sl.pending = kNoOrder;
    sl.state = sl.sold_qty > 0 ? State::Sold : State::Flat;
  }

  // Close-out discipline: buy back any open short-vs-inventory leg into the
  // closing auction rather than carrying an unintended overnight change.
  template <typename Ctx>
  void on_phase(Ctx& ctx, const PhaseChangeEvt& e) {
    live_ = e.phase == Phase::Continuous;
    if (e.phase != Phase::CloseAuction) return;
    for (InstrumentId i = 0; i < slots_.size(); ++i) {
      if (!traded_[i]) continue;
      Slot& sl = slots_[i];
      if (sl.sold_qty <= 0 || sl.state != State::Sold) continue;
      const StockBook* b = ctx.stock_book(i);
      if (!b) continue;
      const auto top = b->top();
      if (top.ask_px == 0) continue;
      sl.pending = ctx.place(i, Side::Buy, Offset::Open, top.ask_px, sl.sold_qty, Tif::Day,
                             e.ts);
      if (sl.pending != kNoOrder) sl.state = State::BuySent;
    }
  }

  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    for (InstrumentId i = 0; i < slots_.size(); ++i) {
      if (!traded_[i]) continue;
      const Slot& sl = slots_[i];
      h.mix(i);
      h.mix(sl.state);
      h.mix(sl.sold_qty);
      h.mix(sl.entry_px);
      h.mix(sl.trips);
      h.mix(sl.ema.value());
    }
    h.mix(live_);
    return h.value();
  }

  void save(std::vector<std::uint8_t>& out) const {
    for (InstrumentId i = 0; i < slots_.size(); ++i) {
      if (!traded_[i]) continue;
      const auto* p = reinterpret_cast<const std::uint8_t*>(&slots_[i]);
      out.insert(out.end(), p, p + sizeof(Slot));
    }
    out.push_back(live_ ? 1 : 0);
  }
  std::size_t load(const std::uint8_t* p, std::size_t avail) {
    std::size_t off = 0;
    for (InstrumentId i = 0; i < slots_.size(); ++i) {
      if (!traded_[i]) continue;
      if (off + sizeof(Slot) <= avail) std::memcpy(&slots_[i], p + off, sizeof(Slot));
      off += sizeof(Slot);
    }
    if (off < avail) live_ = p[off] != 0;
    return off + 1;
  }

  [[nodiscard]] int round_trips(InstrumentId i) const noexcept { return slots_[i].trips; }

 private:
  enum class State : std::uint8_t { Flat, SellSent, Sold, BuySent };

  struct Slot {
    EmaE4 ema{};
    State state = State::Flat;
    Qty sold_qty = 0;
    Price entry_px = 0;
    int trips = 0;
    OrderId pending = kNoOrder;
  };
  static_assert(std::is_trivially_copyable_v<Slot> && sizeof(Slot) <= 128);  // EMA-bearing slot: two lines

  T0Params p_;
  std::vector<Slot> slots_;
  std::vector<std::uint8_t> traded_;
  bool live_ = false;
};

}  // namespace plat
