#pragma once
// plat/strat/momentum.hpp — futures strategy #1 (§17): snapshot-regime
// momentum. Chinese futures venues disclose 250–500 ms snapshots (§4.2), so
// the signal is the snapshot delta: mid-move plus imbalance agreement opens
// in the move's direction; exits are the shared take/stop/timeout cycle,
// closed with Offset::CloseToday — deliberately eating the 平今 surcharge so
// the fee model, the closeable-bucket risk check, and the futures PnL path
// all get exercised end to end.
//
// Multi-instrument by design: ONE instance runs any number of contracts;
// per-contract state is a dense-id CycleSlot table (strat/cycle.hpp). CRTP:
// no vtable between a snapshot arriving and the order leaving.

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

struct MomentumParams {
  std::int64_t imb_gate_e4 = 2'000;  // imbalance must agree with the mid move
  Price min_move = 0;                // mid move to act on (0 → any move ≥ 1 tick)
  Qty lots = 1;
  int take_ticks = 2;
  int stop_ticks = 3;
  Nanos timeout = 5 * kNsPerSec;     // flatten if neither hit
  int max_entries = 100;             // per instrument
};

class SnapMomentum final : public Strategy<SnapMomentum> {
 public:
  SnapMomentum(StrategyId id, AccountId account, MomentumParams p, std::size_t table_size,
               const std::vector<InstrumentId>& instruments)
      : Strategy(id, account), p_(p), slots_(table_size), traded_(table_size, 0) {
    for (const InstrumentId i : instruments)
      if (i < table_size) traded_[i] = 1;
  }

  template <typename Ctx>
  void on_snapshot(Ctx& ctx, const MdSnapshotEvt& s) {
    if (!traded_[s.inst] || !live_) return;
    CycleSlot& sl = slots_[s.inst];
    const FutBook* b = ctx.fut_book(s.inst);
    if (!b || !b->valid() || b->bid() == 0 || b->ask() == 0) return;
    const auto f = b->features();
    const Instrument& in = ctx.instrument(s.inst);
    const Price min_move = p_.min_move ? p_.min_move : in.tick;

    if (sl.state == CycleState::Flat) {
      if (sl.entries >= p_.max_entries) return;
      const bool up = f.d_mid >= min_move && f.imbalance_e4 >= p_.imb_gate_e4;
      const bool dn = f.d_mid <= -min_move && f.imbalance_e4 <= -p_.imb_gate_e4;
      if (!up && !dn) return;
      cycle_enter(ctx, sl, s.inst, up ? Side::Buy : Side::Sell, up ? b->ask() : b->bid(),
                  p_.lots, s.ts);
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

  // Trade only in continuous phases; the timeout exit handles pauses.
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
    else if (k == "take_ticks") p_.take_ticks = static_cast<int>(u.value);
    else if (k == "stop_ticks") p_.stop_ticks = static_cast<int>(u.value);
    else if (k == "timeout_ms") p_.timeout = u.value * kNsPerMs;
    else if (k == "max_entries") p_.max_entries = static_cast<int>(u.value);
    else if (k == "min_move") p_.min_move = u.value;
  }

  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    for (InstrumentId i = 0; i < slots_.size(); ++i) {
      if (!traded_[i]) continue;
      h.mix(i);
      h.mix(slots_[i]);
    }
    h.mix(live_);
    return h.value();
  }

  void save(std::vector<std::uint8_t>& out) const {
    for (InstrumentId i = 0; i < slots_.size(); ++i) {
      if (!traded_[i]) continue;
      const auto* p = reinterpret_cast<const std::uint8_t*>(&slots_[i]);
      out.insert(out.end(), p, p + sizeof(CycleSlot));
    }
    out.push_back(live_ ? 1 : 0);
  }
  std::size_t load(const std::uint8_t* p, std::size_t avail) {
    std::size_t off = 0;
    for (InstrumentId i = 0; i < slots_.size(); ++i) {
      if (!traded_[i]) continue;
      if (off + sizeof(CycleSlot) <= avail) std::memcpy(&slots_[i], p + off, sizeof(CycleSlot));
      off += sizeof(CycleSlot);
    }
    if (off < avail) live_ = p[off] != 0;
    return off + 1;
  }

  [[nodiscard]] int entries(InstrumentId i) const noexcept { return slots_[i].entries; }
  [[nodiscard]] int total_entries() const noexcept {
    int n = 0;
    for (InstrumentId i = 0; i < slots_.size(); ++i)
      if (traded_[i]) n += slots_[i].entries;
    return n;
  }

 private:
  MomentumParams p_;
  std::vector<CycleSlot> slots_;     // dense InstrumentId → per-contract cycle
  std::vector<std::uint8_t> traded_; // membership: is this instrument ours?
  bool live_ = false;
};

// Registration traits (engine/pack.hpp): the config `type` name, this type's
// fixed id in the legacy no-`strategies`-key lineup, and the Config→params
// reader (keys under the instance name) — everything the platform's Pack
// list (strat_params.hpp) needs to host the type.
struct MomentumDef {
  using Strat = SnapMomentum;
  static constexpr const char* name = "momentum";
  static constexpr StrategyId legacy_id = 1;
  static MomentumParams params(const Config& c, const std::string& n) {
    MomentumParams p;
    p.imb_gate_e4 = c.i64(n + ".imb_gate_e4", 2'000);
    p.lots = c.i64(n + ".lots", 1);
    p.take_ticks = static_cast<int>(c.i64(n + ".take_ticks", 2));
    p.stop_ticks = static_cast<int>(c.i64(n + ".stop_ticks", 3));
    p.timeout = c.i64(n + ".timeout_ms", 5'000) * kNsPerMs;
    p.max_entries = static_cast<int>(c.i64(n + ".max_entries", 100));
    return p;
  }
};

}  // namespace plat
