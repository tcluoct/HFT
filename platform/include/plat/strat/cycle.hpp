#pragma once
// plat/strat/cycle.hpp — the shared position-cycle state machine every
// taker strategy runs per instrument: Flat → EntrySent → Holding →
// ExitSent → Flat, with IOC entries, CloseToday exits, and take/stop/
// timeout exit triggers. The *signal* differs per strategy (momentum delta,
// mean-reversion stretch, breakout); the cycle bookkeeping is identical, so
// it exists once (§4.2's one-fee-library discipline, applied to strategy
// plumbing).
//
// CycleSlot is the per-instrument state — one cache line, trivially
// copyable, dense-id indexed by the owning strategy's slot table.

#include <cstdint>

#include "plat/codec/msgs.hpp"
#include "plat/core/types.hpp"
#include "plat/md/fut_book.hpp"

namespace plat {

enum class CycleState : std::uint8_t { Flat, EntrySent, Holding, ExitSent };

struct CycleSlot {
  CycleState state = CycleState::Flat;
  Side dir = Side::Buy;
  Qty held = 0;
  Price entry_px = 0;
  Nanos entry_ts = 0;
  int entries = 0;
  OrderId pending = kNoOrder;
};
static_assert(std::is_trivially_copyable_v<CycleSlot> && sizeof(CycleSlot) <= 64);

// IOC entry crossing the spread in `side`'s direction.
template <typename Ctx>
HFT_FORCE_INLINE void cycle_enter(Ctx& ctx, CycleSlot& sl, InstrumentId inst, Side side,
                                  Price px, Qty lots, Nanos trigger) {
  sl.pending = ctx.place(inst, side, Offset::Open, px, lots, Tif::Ioc, trigger);
  if (sl.pending != kNoOrder) {
    sl.state = CycleState::EntrySent;
    sl.dir = side;
  }
}

// Take/stop/timeout evaluation against the holding direction's exit mark.
HFT_FORCE_INLINE bool cycle_should_exit(const CycleSlot& sl, Price mark, Price tick,
                                        int take_ticks, int stop_ticks, Nanos now,
                                        Nanos timeout) {
  const Price take = sl.dir == Side::Buy ? sl.entry_px + take_ticks * tick
                                         : sl.entry_px - take_ticks * tick;
  const Price stop = sl.dir == Side::Buy ? sl.entry_px - stop_ticks * tick
                                         : sl.entry_px + stop_ticks * tick;
  const bool take_hit = sl.dir == Side::Buy ? mark >= take : mark <= take;
  const bool stop_hit = sl.dir == Side::Buy ? mark <= stop : mark >= stop;
  return take_hit || stop_hit || now - sl.entry_ts >= timeout;
}

// IOC close-today exit at the book's exit side.
template <typename Ctx>
HFT_FORCE_INLINE void cycle_exit(Ctx& ctx, CycleSlot& sl, InstrumentId inst, const FutBook* b,
                                 Nanos trigger) {
  const Side exit_side = sl.dir == Side::Buy ? Side::Sell : Side::Buy;
  const Price px = exit_side == Side::Sell ? b->bid() : b->ask();
  if (px == 0) return;
  sl.pending = ctx.place(inst, exit_side, Offset::CloseToday, px, sl.held, Tif::Ioc, trigger);
  if (sl.pending != kNoOrder) sl.state = CycleState::ExitSent;
}

// Exec-report bookkeeping for the pending order; entries count completed
// entry fills. Reports for other orders must be filtered by the caller.
HFT_FORCE_INLINE void cycle_on_exec(CycleSlot& sl, const ExecReport& e) {
  switch (e.type) {
    case ExecType::Fill:
      if (sl.state == CycleState::EntrySent) {
        sl.held += e.qty;
        sl.entry_px = e.px;
        sl.entry_ts = e.ts;
        if (e.leaves == 0) {
          sl.pending = kNoOrder;
          sl.state = sl.held > 0 ? CycleState::Holding : CycleState::Flat;
          if (sl.held > 0) ++sl.entries;
        }
      } else if (sl.state == CycleState::ExitSent) {
        sl.held -= e.qty;
        if (e.leaves == 0) {
          sl.pending = kNoOrder;
          sl.state = sl.held > 0 ? CycleState::Holding : CycleState::Flat;
        }
      }
      break;
    case ExecType::Canceled:
    case ExecType::Reject:
      sl.pending = kNoOrder;
      sl.state = sl.held > 0 ? CycleState::Holding : CycleState::Flat;
      break;
    case ExecType::Ack:
    case ExecType::CancelReject:
      break;
  }
}

HFT_FORCE_INLINE void cycle_on_deny(CycleSlot& sl) {
  sl.pending = kNoOrder;
  sl.state = sl.held > 0 ? CycleState::Holding : CycleState::Flat;
}

}  // namespace plat
