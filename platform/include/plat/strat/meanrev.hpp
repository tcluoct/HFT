#pragma once
// plat/strat/meanrev.hpp — futures strategy #2: mean-reversion (fade). An
// EMA of the snapshot mid is the anchor; when price stretches ≥ N ticks away
// AND the L1 imbalance opposes the move's continuation (the stretch is
// running out of size), fade it — sell rich, buy cheap — expecting reversion
// toward the anchor. Exits are the shared take/stop/timeout cycle.
//
// Complements momentum by construction: momentum needs move + agreeing
// imbalance, this needs stretch + opposing imbalance — the two cannot fire
// on the same tick in the same direction, so running both concurrently
// diversifies rather than doubles exposure.
//
// Multi-instrument slot design; CRTP; integer EMA (sigcalc discipline).

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "plat/core/config.hpp"
#include "plat/core/hash.hpp"
#include "plat/md/fut_book.hpp"
#include "plat/ref/instrument.hpp"
#include "plat/strat/cycle.hpp"
#include "plat/strat/framework.hpp"
#include "plat/strat/signals.hpp"

namespace plat {

struct MeanRevParams {
  std::int64_t ema_num = 1, ema_den = 16;  // anchor smoothing
  int enter_dev_ticks = 4;                 // stretch from anchor to act on
  std::int64_t imb_gate_e4 = 1'000;        // imbalance must OPPOSE the stretch
  Qty lots = 1;
  int take_ticks = 2;
  int stop_ticks = 4;
  Nanos timeout = 10 * kNsPerSec;
  int max_entries = 100;                   // per instrument
};

class MeanRevert final : public Strategy<MeanRevert> {
 public:
  MeanRevert(StrategyId id, AccountId account, MeanRevParams p, std::size_t table_size,
             const std::vector<InstrumentId>& instruments)
      : Strategy(id, account), p_(p), slots_(table_size), emas_(table_size),
        traded_(table_size, 0) {
    for (const InstrumentId i : instruments) {
      if (i < table_size) {
        traded_[i] = 1;
        emas_[i] = EmaE4(p_.ema_num, p_.ema_den);
      }
    }
  }

  template <typename Ctx>
  void on_snapshot(Ctx& ctx, const MdSnapshotEvt& s) {
    if (!traded_[s.inst] || !live_) return;
    const FutBook* b = ctx.fut_book(s.inst);
    if (!b || !b->valid() || b->bid() == 0 || b->ask() == 0) return;
    const Price mid = b->mid();
    const std::int64_t anchor = emas_[s.inst].update(mid);  // updates every tick
    CycleSlot& sl = slots_[s.inst];
    const auto f = b->features();
    const Instrument& in = ctx.instrument(s.inst);

    if (sl.state == CycleState::Flat) {
      if (sl.entries >= p_.max_entries) return;
      const Price stretch = static_cast<Price>(p_.enter_dev_ticks) * in.tick;
      // Rich vs anchor + bid side thinning → fade short. Cheap + ask side
      // thinning → fade long. Imbalance OPPOSES continuation by requirement.
      const bool rich = mid - anchor >= stretch && f.imbalance_e4 <= -p_.imb_gate_e4;
      const bool cheap = anchor - mid >= stretch && f.imbalance_e4 >= p_.imb_gate_e4;
      if (!rich && !cheap) return;
      cycle_enter(ctx, sl, s.inst, rich ? Side::Sell : Side::Buy,
                  rich ? b->bid() : b->ask(), p_.lots, s.ts);
      return;
    }
    if (sl.state == CycleState::Holding) {
      const Price mark = sl.dir == Side::Buy ? b->bid() : b->ask();
      if (cycle_should_exit(sl, mark, in.tick, p_.take_ticks, p_.stop_ticks, s.ts, p_.timeout))
        cycle_exit(ctx, sl, s.inst, b, s.ts);
    }
  }

  template <typename Ctx>
  void on_exec(Ctx&, const ExecReport& e) {
    if (e.inst >= traded_.size() || !traded_[e.inst]) return;
    if (e.cl_id == slots_[e.inst].pending) cycle_on_exec(slots_[e.inst], e);
  }
  template <typename Ctx>
  void on_deny(Ctx&, const RiskDeny& d) {
    if (d.inst >= traded_.size() || !traded_[d.inst]) return;
    if (d.cl_id == slots_[d.inst].pending) cycle_on_deny(slots_[d.inst]);
  }
  template <typename Ctx>
  void on_phase(Ctx&, const PhaseChangeEvt& e) {
    live_ = e.phase == Phase::Continuous;
  }

  // Ops-console live re-tune (ema_num/ema_den are structural: restart-only).
  template <typename Ctx>
  void on_param(Ctx&, const ParamUpdateEvt& u) {
    const std::string_view k{u.key};
    if (k == "lots") p_.lots = u.value;
    else if (k == "imb_gate_e4") p_.imb_gate_e4 = u.value;
    else if (k == "enter_dev_ticks") p_.enter_dev_ticks = static_cast<int>(u.value);
    else if (k == "take_ticks") p_.take_ticks = static_cast<int>(u.value);
    else if (k == "stop_ticks") p_.stop_ticks = static_cast<int>(u.value);
    else if (k == "timeout_ms") p_.timeout = u.value * kNsPerMs;
    else if (k == "max_entries") p_.max_entries = static_cast<int>(u.value);
  }

  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    for (InstrumentId i = 0; i < slots_.size(); ++i) {
      if (!traded_[i]) continue;
      h.mix(i);
      h.mix(slots_[i]);
      h.mix(emas_[i].value());
    }
    h.mix(live_);
    return h.value();
  }

  void save(std::vector<std::uint8_t>& out) const {
    for (InstrumentId i = 0; i < slots_.size(); ++i) {
      if (!traded_[i]) continue;
      const auto* sp = reinterpret_cast<const std::uint8_t*>(&slots_[i]);
      out.insert(out.end(), sp, sp + sizeof(CycleSlot));
      const auto* ep = reinterpret_cast<const std::uint8_t*>(&emas_[i]);
      out.insert(out.end(), ep, ep + sizeof(EmaE4));
    }
    out.push_back(live_ ? 1 : 0);
  }
  std::size_t load(const std::uint8_t* p, std::size_t avail) {
    std::size_t off = 0;
    for (InstrumentId i = 0; i < slots_.size(); ++i) {
      if (!traded_[i]) continue;
      if (off + sizeof(CycleSlot) <= avail) std::memcpy(&slots_[i], p + off, sizeof(CycleSlot));
      off += sizeof(CycleSlot);
      if (off + sizeof(EmaE4) <= avail) std::memcpy(&emas_[i], p + off, sizeof(EmaE4));
      off += sizeof(EmaE4);
    }
    if (off < avail) live_ = p[off] != 0;
    return off + 1;
  }

  [[nodiscard]] int total_entries() const noexcept {
    int n = 0;
    for (InstrumentId i = 0; i < slots_.size(); ++i)
      if (traded_[i]) n += slots_[i].entries;
    return n;
  }

 private:
  MeanRevParams p_;
  std::vector<CycleSlot> slots_;
  std::vector<EmaE4> emas_;          // per-instrument anchor
  std::vector<std::uint8_t> traded_;
  bool live_ = false;
};

// Registration traits (engine/pack.hpp) — see MomentumDef for the contract.
struct MeanRevDef {
  using Strat = MeanRevert;
  static constexpr const char* name = "meanrev";
  static constexpr StrategyId legacy_id = 2;
  static MeanRevParams params(const Config& c, const std::string& n) {
    MeanRevParams p;
    p.ema_num = c.i64(n + ".ema_num", 1);
    p.ema_den = c.i64(n + ".ema_den", 16);
    p.enter_dev_ticks = static_cast<int>(c.i64(n + ".enter_dev_ticks", 4));
    p.imb_gate_e4 = c.i64(n + ".imb_gate_e4", 1'000);
    p.lots = c.i64(n + ".lots", 1);
    p.take_ticks = static_cast<int>(c.i64(n + ".take_ticks", 2));
    p.stop_ticks = static_cast<int>(c.i64(n + ".stop_ticks", 4));
    p.timeout = c.i64(n + ".timeout_ms", 10'000) * kNsPerMs;
    p.max_entries = static_cast<int>(c.i64(n + ".max_entries", 100));
    return p;
  }
};

}  // namespace plat
