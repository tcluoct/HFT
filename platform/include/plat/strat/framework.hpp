#pragma once
// plat/strat/framework.hpp — the strategy container contract (§9.1),
// production inline mode: **no virtual dispatch anywhere on the hot path**.
//
// Strategies are CRTP: `class MyStrat : public Strategy<MyStrat>`. Every
// handler is a template on the context (the site engine itself), so a
// strategy's book/position reads and its `ctx.place()` compile to direct —
// inlinable — calls into the concrete Site<...> instantiation, the same
// discipline tick2trade uses for its feed→book→strategy→gateway chain. The
// base provides empty defaults; a derived strategy shadows only the handlers
// it uses, and the optimizer deletes the rest at the call site.
//
// Determinism contract unchanged: a strategy is a pure function of the
// events it is handed. No syscalls, no allocation, no clocks except
// ctx.now() (event time), no state outside the object. The handlers below
// are the complete sensory set: books, ticks, snapshots, execs, denials,
// phases, timers, and the three cross-site messages.
//
// The context API every Ctx instantiation provides (see engine/site.hpp):
//   OrderId place(inst, side, offset, px, qty, tif, trigger_ts)
//   void    cancel(OrderId)
//   void    set_timer(Nanos when, u64 token)
//   void    send_intent/send_basis/send_hedge_done(...)
//   Nanos   now()
//   const Instrument& instrument(InstrumentId)
//   const Position*   position(InstrumentId)
//   const StockBook*  stock_book(InstrumentId)
//   const FutBook*    fut_book(InstrumentId)

#include <cstdint>
#include <vector>

#include "hft/common/platform.hpp"
#include "plat/codec/msgs.hpp"
#include "plat/core/types.hpp"

namespace plat {

// Everything the engine needs from a strategy type besides its handlers.
template <typename S>
concept AnyStrategy = requires(const S s) {
  { s.id() } -> std::same_as<StrategyId>;
  { s.account() } -> std::same_as<AccountId>;
};

template <typename Derived>
class Strategy {
 public:
  Strategy(StrategyId id, AccountId account) : id_(id), account_(account) {}

  [[nodiscard]] StrategyId id() const noexcept { return id_; }
  [[nodiscard]] AccountId account() const noexcept { return account_; }

  // Sensory defaults — shadow what you use. All statically dispatched.
  template <typename Ctx> HFT_FORCE_INLINE void on_start(Ctx&) {}
  template <typename Ctx> HFT_FORCE_INLINE void on_book(Ctx&, const BookTopEvt&) {}
  template <typename Ctx> HFT_FORCE_INLINE void on_trade(Ctx&, const MdTradeEvt&) {}
  template <typename Ctx> HFT_FORCE_INLINE void on_snapshot(Ctx&, const MdSnapshotEvt&) {}
  template <typename Ctx> HFT_FORCE_INLINE void on_exec(Ctx&, const ExecReport&) {}
  template <typename Ctx> HFT_FORCE_INLINE void on_deny(Ctx&, const RiskDeny&) {}
  template <typename Ctx> HFT_FORCE_INLINE void on_phase(Ctx&, const PhaseChangeEvt&) {}
  template <typename Ctx> HFT_FORCE_INLINE void on_timer(Ctx&, const TimerEvt&) {}
  template <typename Ctx> HFT_FORCE_INLINE void on_hedge_intent(Ctx&, const HedgeIntent&) {}
  template <typename Ctx> HFT_FORCE_INLINE void on_basis(Ctx&, const BasisSummary&) {}
  template <typename Ctx> HFT_FORCE_INLINE void on_hedge_done(Ctx&, const HedgeDone&) {}
  // Operator live re-tune (ops console): apply the whitelisted keys you
  // support, ignore the rest. Journaled Source, so replay re-applies it.
  template <typename Ctx> HFT_FORCE_INLINE void on_param(Ctx&, const ParamUpdateEvt&) {}

  // Deterministic state fingerprint for replay/recovery verification (§16)
  // and snapshot hooks (§11.1); POD-state strategies shadow these cheaply.
  [[nodiscard]] std::uint64_t state_hash() const { return 0; }
  void save(std::vector<std::uint8_t>&) const {}
  std::size_t load(const std::uint8_t*, std::size_t) { return 0; }

 private:
  StrategyId id_;
  AccountId account_;
};

}  // namespace plat
