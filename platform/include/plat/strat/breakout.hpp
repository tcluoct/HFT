#pragma once
// plat/strat/breakout.hpp — futures strategy #3: session-range breakout. Per
// instrument, track the session's mid high/low; when the mid clears an
// extreme by N ticks with the imbalance confirming the direction, go with
// the break. Exits are the shared take/stop/timeout cycle.
//
// A third, distinct regime next to momentum (short-delta continuation) and
// mean-reversion (stretch fade): breakout only acts at fresh session
// extremes, so the trio spreads entries across market states instead of
// stacking them.
//
// Multi-instrument slot design; CRTP; exact integer range tracking.

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

namespace plat {

struct BreakoutParams {
  int brk_ticks = 2;                 // clear the extreme by this many ticks
  std::int64_t imb_gate_e4 = 1'500;  // imbalance confirms the break direction
  Qty lots = 1;
  int take_ticks = 3;
  int stop_ticks = 3;
  Nanos timeout = 10 * kNsPerSec;
  int max_entries = 50;              // per instrument
};

class Breakout final : public Strategy<Breakout> {
 public:
  Breakout(StrategyId id, AccountId account, BreakoutParams p, std::size_t table_size,
           const std::vector<InstrumentId>& instruments)
      : Strategy(id, account), p_(p), slots_(table_size), ranges_(table_size),
        traded_(table_size, 0) {
    for (const InstrumentId i : instruments)
      if (i < table_size) traded_[i] = 1;
  }

  template <typename Ctx>
  void on_snapshot(Ctx& ctx, const MdSnapshotEvt& s) {
    if (!traded_[s.inst] || !live_) return;
    const FutBook* b = ctx.fut_book(s.inst);
    if (!b || !b->valid() || b->bid() == 0 || b->ask() == 0) return;
    const Price mid = b->mid();
    Range& rg = ranges_[s.inst];
    if (!rg.primed) {  // session range starts at the first valid mid
      rg.hi = rg.lo = mid;
      rg.primed = true;
      return;
    }
    CycleSlot& sl = slots_[s.inst];
    const auto f = b->features();
    const Instrument& in = ctx.instrument(s.inst);

    if (sl.state == CycleState::Flat && sl.entries < p_.max_entries) {
      const Price margin = static_cast<Price>(p_.brk_ticks) * in.tick;
      // Break decisions read the extremes BEFORE this tick extends them.
      const bool up = mid >= rg.hi + margin && f.imbalance_e4 >= p_.imb_gate_e4;
      const bool dn = mid <= rg.lo - margin && f.imbalance_e4 <= -p_.imb_gate_e4;
      if (up || dn)
        cycle_enter(ctx, sl, s.inst, up ? Side::Buy : Side::Sell,
                    up ? b->ask() : b->bid(), p_.lots, s.ts);
    } else if (sl.state == CycleState::Holding) {
      const Price mark = sl.dir == Side::Buy ? b->bid() : b->ask();
      if (cycle_should_exit(sl, mark, in.tick, p_.take_ticks, p_.stop_ticks, s.ts, p_.timeout))
        cycle_exit(ctx, sl, s.inst, b, s.ts);
    }

    if (mid > rg.hi) rg.hi = mid;
    if (mid < rg.lo) rg.lo = mid;
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

  // Ops-console live re-tune: the whitelisted subset of the config keys.
  template <typename Ctx>
  void on_param(Ctx&, const ParamUpdateEvt& u) {
    const std::string_view k{u.key};
    if (k == "lots") p_.lots = u.value;
    else if (k == "imb_gate_e4") p_.imb_gate_e4 = u.value;
    else if (k == "brk_ticks") p_.brk_ticks = static_cast<int>(u.value);
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
      h.mix(ranges_[i]);
    }
    h.mix(live_);
    return h.value();
  }

  void save(std::vector<std::uint8_t>& out) const {
    for (InstrumentId i = 0; i < slots_.size(); ++i) {
      if (!traded_[i]) continue;
      const auto* sp = reinterpret_cast<const std::uint8_t*>(&slots_[i]);
      out.insert(out.end(), sp, sp + sizeof(CycleSlot));
      const auto* rp = reinterpret_cast<const std::uint8_t*>(&ranges_[i]);
      out.insert(out.end(), rp, rp + sizeof(Range));
    }
    out.push_back(live_ ? 1 : 0);
  }
  std::size_t load(const std::uint8_t* p, std::size_t avail) {
    std::size_t off = 0;
    for (InstrumentId i = 0; i < slots_.size(); ++i) {
      if (!traded_[i]) continue;
      if (off + sizeof(CycleSlot) <= avail) std::memcpy(&slots_[i], p + off, sizeof(CycleSlot));
      off += sizeof(CycleSlot);
      if (off + sizeof(Range) <= avail) std::memcpy(&ranges_[i], p + off, sizeof(Range));
      off += sizeof(Range);
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
  struct Range {
    Price hi = 0;
    Price lo = 0;
    bool primed = false;
  };
  static_assert(std::is_trivially_copyable_v<Range>);

  BreakoutParams p_;
  std::vector<CycleSlot> slots_;
  std::vector<Range> ranges_;        // per-instrument session extremes
  std::vector<std::uint8_t> traded_;
  bool live_ = false;
};

// Registration traits (engine/pack.hpp) — see MomentumDef for the contract.
struct BreakoutDef {
  using Strat = Breakout;
  static constexpr const char* name = "breakout";
  static constexpr StrategyId legacy_id = 3;
  static BreakoutParams params(const Config& c, const std::string& n) {
    BreakoutParams p;
    p.brk_ticks = static_cast<int>(c.i64(n + ".brk_ticks", 2));
    p.imb_gate_e4 = c.i64(n + ".imb_gate_e4", 1'500);
    p.lots = c.i64(n + ".lots", 1);
    p.take_ticks = static_cast<int>(c.i64(n + ".take_ticks", 3));
    p.stop_ticks = static_cast<int>(c.i64(n + ".stop_ticks", 3));
    p.timeout = c.i64(n + ".timeout_ms", 10'000) * kNsPerMs;
    p.max_entries = static_cast<int>(c.i64(n + ".max_entries", 50));
    return p;
  }
};

}  // namespace plat
