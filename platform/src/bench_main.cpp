// plat_bench — production hot-path budget lines (§12.1). No simulation:
// this measures the exact code platd runs, by pushing synthetic depth
// snapshots through the compile-time-composed engine — StreamCore seal →
// dispatch → FutBook → SnapMomentum → riskgate battery → gateway — with a
// capture gateway standing where the CTP trader client stands. Also measures
// the adapter→engine ring's one-way cost, seqbus-style.
//
// Expect sub-µs p50 with ~200 µs max outliers on the dev VM (hypervisor
// scheduling; see lab notes) — run pinned on the trading host for truth.

#include <cstdio>
#include <vector>

#include "hft/common/bench.hpp"
#include "plat/bus/ring.hpp"
#include "plat/core/time.hpp"
#include "plat/engine/site.hpp"
#include "plat/strat/momentum.hpp"

using namespace plat;

namespace {

struct NullGateway {
  std::uint64_t placed = 0;
  NewOrderReq last{};
  bool fresh = false;
  void place(const NewOrderReq& r) {
    ++placed;
    last = r;
    fresh = true;
  }
  void cancel(const CancelReq&) {}
  [[nodiscard]] SessState state() const { return SessState::Up; }
};
static_assert(OrderGateway<NullGateway>);

using BenchSite = Site<NullGateway, SnapMomentum>;

MdSnapshotEvt snap(std::uint64_t seq, Nanos ts, Price bid, Price ask, bool bid_heavy = true) {
  MdSnapshotEvt s{};
  s.feed_seq = seq;
  s.ts = ts;
  s.bid_px[0] = bid;
  s.bid_qty[0] = bid_heavy ? 40 : 10;
  s.ask_px[0] = ask;
  s.ask_qty[0] = bid_heavy ? 10 : 40;
  s.last_px = (bid + ask) / 2;
  s.inst = 0;
  s.venue = Venue::SHFE;
  return s;
}

}  // namespace

int main() {
  InstrumentTable table;
  table.add("cu2609", Venue::SHFE, Board::Future, px_from_cny(10.0), 1, 5,
            px_from_cny(78'000.0), 1'000);

  SiteConfig sc;
  sc.id = 1;
  sc.name = "bench";
  sc.venue = Venue::SHFE;
  sc.hosted = {0};
  sc.day = 20'260'713;

  MomentumParams mp;
  mp.imb_gate_e4 = 1'000;
  mp.max_entries = 1'000'000'000;
  BenchSite site(table, sc, BenchSite::Mode::Live,
                 {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})});
  NullGateway gw;
  site.set_gateway(&gw);

  PhaseChangeEvt ph{};
  ph.ts = at_time(9, 0);
  ph.venue = Venue::SHFE;
  ph.phase = Phase::Continuous;
  site.source(ph, ph.ts);

  // ---- line 1: MD snapshot → book → strategy evaluate (no order fires:
  // flat mid so the momentum gate stays closed) — the per-tick floor.
  constexpr std::uint64_t kTicks = 200'000;
  std::uint64_t seq = 0;
  Nanos ts = at_time(9, 0, 1);
  const double idle_ns = hft::bench_ns_per_op(kTicks, [&] {
    hft::bench_sink;
    for (std::uint64_t i = 0; i < kTicks; ++i) {
      ts += 1'000;
      site.source(snap(++seq, ts, px_from_cny(78'000.0), px_from_cny(78'010.0)), ts);
    }
    hft::bench_sink = site.tel().get("stream.envelopes");
  });

  // ---- line 2: alternating mid moves with agreeing imbalance — every other
  // snapshot fires the full order path (strategy → riskgate battery →
  // gateway), rate limit permitting. Measures tick-to-order-at-gateway.
  RiskLimits wide;
  wide.max_actions_per_sec = 1'000'000'000;
  wide.max_actions_per_day = 1'000'000'000;
  wide.max_margin_e4 = px_from_cny(1e12);        // wide but no int64 overflow
  wide.max_net_notional_e4 = px_from_cny(1e12);
  wide.duplicate_horizon = 0;
  SiteConfig sc2 = sc;
  sc2.limits = wide;
  BenchSite site2(table, sc2, BenchSite::Mode::Live,
                  {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})});
  NullGateway gw2;
  site2.set_gateway(&gw2);
  site2.source(ph, ph.ts);

  // Sawtooth mid inside the limit band; imbalance agrees with the move so
  // the momentum gate opens. Every order the gateway captures is echoed back
  // as a full fill (the wire's job in production), so entries and exits keep
  // cycling — each iteration costs snapshot dispatch + strategy + riskgate
  // battery, plus the OMS/PnL/strategy cost of any resulting fill.
  constexpr std::uint64_t kEvents = 100'000;
  std::uint64_t seq2 = 0;
  Nanos ts2 = at_time(9, 0, 1);
  const Price base = px_from_cny(78'000.0);
  const Price step = px_from_cny(10.0);
  const double order_ns = hft::bench_ns_per_op(kEvents, [&] {
    hft::bench_sink;
    for (std::uint64_t i = 0; i < kEvents; ++i) {
      const std::uint64_t ph8 = i & 7;         // 4 up-steps then 4 down-steps
      const bool rising = ph8 < 4;
      const Price off = static_cast<Price>(rising ? ph8 : 8 - ph8) * step;
      ts2 += 1'000;
      site2.source(snap(++seq2, ts2, base + off, base + off + step, rising), ts2);
      if (gw2.fresh) {
        gw2.fresh = false;
        const NewOrderReq& r = gw2.last;
        ts2 += 1'000;
        ExecReport fill{};
        fill.cl_id = r.cl_id;
        fill.ts = ts2;
        fill.trigger_ts = r.trigger_ts;
        fill.px = r.px;
        fill.qty = r.qty;
        fill.cum = r.qty;
        fill.inst = r.inst;
        fill.strategy = r.strategy;
        fill.account = r.account;
        fill.type = ExecType::Fill;
        fill.side = r.side;
        fill.offset = r.offset;
        site2.source(fill, ts2);
      }
    }
    hft::bench_sink = gw2.placed;
  });

  // ---- line 2b: the same tick floor WITH the async journal attached — the
  // engine's journal cost is one ring publish; file I/O runs on the journal
  // thread. (The synchronous fwrite this replaced sat directly on the tick.)
  SiteConfig sc_j = sc;
  sc_j.journal_path = "plat_bench.jnl";
  double jnl_ns = 0;
  {
    BenchSite site_j(table, sc_j, BenchSite::Mode::Live,
                     {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})});
    NullGateway gw_j;
    site_j.set_gateway(&gw_j);
    site_j.source(ph, ph.ts);
    std::uint64_t seq_j = 0;
    Nanos ts_j = at_time(9, 0, 1);
    jnl_ns = hft::bench_ns_per_op(kTicks, [&] {
      hft::bench_sink;
      for (std::uint64_t i = 0; i < kTicks; ++i) {
        ts_j += 1'000;
        site_j.source(snap(++seq_j, ts_j, px_from_cny(78'000.0), px_from_cny(78'010.0)), ts_j);
      }
      hft::bench_sink = site_j.tel().get("stream.envelopes");
    });
    site_j.run_eod({}, 0, 0);  // drain + seal + join before teardown
  }
  std::remove("plat_bench.jnl");

  // ---- line 3: adapter→engine ring one-way (seqbus discipline).
  Ring ring(1 << 12);
  Ring::Writer w(ring);
  Ring::Reader r(ring, true, Ring::LossPolicy::Tolerant);
  const auto payload = snap(1, at_time(9, 0), px_from_cny(78'000.0), px_from_cny(78'010.0));
  const Envelope env = make_envelope(0, payload.ts, payload, Origin::Source, 0);
  constexpr std::uint64_t kMsgs = 500'000;
  const double ring_ns = hft::bench_ns_per_op(kMsgs, [&] {
    hft::bench_sink;
    Envelope out;
    for (std::uint64_t i = 0; i < kMsgs; ++i) {
      w.publish(env);
      (void)r.poll(out);
      hft::bench_sink = out.seq;
    }
  });

  std::printf("plat_bench (production dispatch chain, no sim):\n");
  std::printf("  md snapshot -> book -> strategy (no order):  %8.1f ns/tick\n", idle_ns);
  std::printf("  same tick + async journal attached:          %8.1f ns/tick\n", jnl_ns);
  std::printf("  md snapshot -> ... -> riskgate -> gateway:   %8.1f ns/event "
              "(%llu orders reached gateway)\n",
              order_ns, static_cast<unsigned long long>(gw2.placed));
  std::printf("  adapter ring publish+poll (same core):       %8.1f ns/msg\n", ring_ns);
  std::printf("  envelopes: %llu / %llu\n",
              static_cast<unsigned long long>(site.tel().get("stream.envelopes")),
              static_cast<unsigned long long>(site2.tel().get("stream.envelopes")));
  std::printf("  [diag] entries %d, risk pass %llu deny %llu:", site2.strategies<0>()[0].total_entries(),
              static_cast<unsigned long long>(site2.tel().get("risk.pass")),
              static_cast<unsigned long long>(site2.tel().get("risk.deny")));
  for (int c = 1; c <= 12; ++c) {
    const auto n = site2.tel().get("risk.deny." + std::to_string(c));
    if (n) std::printf(" [%d]=%llu", c, static_cast<unsigned long long>(n));
  }
  std::printf("\n");
  return 0;
}
