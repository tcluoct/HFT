// strategies/trend/src/tests.cpp — the trend team's suite: momentum's
// per-instrument slot design (one instance, many contracts) and the
// multi-instance form (several instances of one type, separate ids, separate
// PnL books, snapshot round-trip), plus breakout's session-extreme trigger.
//
// Only the team's own strategies and the strategy SDK are used here; the
// engine appears solely as the host (`Site<FakeGateway, ...>`), exactly as a
// strategy team would test its code against a harness.

#include <cstdio>
#include <cstring>
#include <vector>

#include "plat/engine/site.hpp"
#include "plat/testing/harness.hpp"
#include "strat/trend/breakout.hpp"
#include "strat/trend/momentum.hpp"

using namespace plat;

int g_checks = 0;
int g_fails = 0;

// ------------------------------------- multi-instrument slots (one instance)

static void test_momentum_slots() {
  InstrumentTable table = one_future();
  table.add("al2609", Venue::SHFE, Board::Future, px_from_cny(5.0), 1, 5,
            px_from_cny(20'000.0), 1'000);
  SiteConfig sc;
  sc.id = 1;
  sc.name = "multi";
  sc.venue = Venue::SHFE;
  sc.hosted = {0, 1};
  sc.day = 20'260'713;

  MomentumParams mp;
  mp.imb_gate_e4 = 1'000;
  mp.lots = 1;
  // ONE instance trades both contracts; per-contract state is slots.
  using MultiSite = Site<FakeGateway, SnapMomentum>;
  MultiSite site(table, sc, MultiSite::Mode::Live,
                 {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0, 1})});
  FakeGateway gw;
  site.set_gateway(&gw);

  PhaseChangeEvt ph{};
  ph.ts = at_time(9, 0);
  ph.venue = Venue::SHFE;
  ph.phase = Phase::Continuous;
  site.source(ph, ph.ts);

  // Rising cu → entry on instrument 0.
  site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010, 50, 10), at_time(9, 0, 1));
  site.source(snap(0, 2, at_time(9, 0, 2), 78'010, 78'020, 50, 10), at_time(9, 0, 2));
  CHECK(gw.placed.size() == 1 && gw.placed[0].inst == 0);

  // Falling al with ask pressure → independent short entry on instrument 1,
  // even while instrument 0 has an order in flight.
  site.source(snap(1, 1, at_time(9, 0, 3), 20'000, 20'005, 10, 50), at_time(9, 0, 3));
  site.source(snap(1, 2, at_time(9, 0, 4), 19'995, 20'000, 10, 50), at_time(9, 0, 4));
  CHECK(gw.placed.size() == 2);
  CHECK(gw.placed[1].inst == 1 && gw.placed[1].side == Side::Sell);
}

// Two momentum instances side by side: different ids, different universes,
// different gates. Events route to both; execs/PnL stay per-instance.
static void test_two_momentum_instances() {
  InstrumentTable table = one_future();
  table.add("al2609", Venue::SHFE, Board::Future, px_from_cny(5.0), 1, 5,
            px_from_cny(20'000.0), 1'000);
  SiteConfig sc;
  sc.id = 1;
  sc.name = "twins";
  sc.venue = Venue::SHFE;
  sc.hosted = {0, 1};
  sc.day = 20'260'713;
  RiskLimits wide;
  wide.max_actions_per_sec = 1'000;
  wide.duplicate_horizon = 0;
  sc.limits = wide;

  MomentumParams tight;   // fires on the cu move below
  tight.imb_gate_e4 = 1'000;
  MomentumParams strict;  // imbalance gate too high to fire on the same move
  strict.imb_gate_e4 = 9'000;
  strict.lots = 3;

  using TwinSite = Site<FakeGateway, SnapMomentum>;
  const auto banks = [&] {
    return std::vector<SnapMomentum>{
        SnapMomentum(StrategyId{11}, AccountId{1}, tight, table.size(), {0}),
        SnapMomentum(StrategyId{12}, AccountId{1}, strict, table.size(), {0, 1})};
  };
  TwinSite site(table, sc, TwinSite::Mode::Live, banks());
  FakeGateway gw;
  site.set_gateway(&gw);

  PhaseChangeEvt ph{};
  ph.ts = at_time(9, 0);
  ph.venue = Venue::SHFE;
  ph.phase = Phase::Continuous;
  site.source(ph, ph.ts);

  // Rising cu with ~67% imbalance: instance 11 fires, 12's gate stays shut.
  site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010, 50, 10), at_time(9, 0, 1));
  site.source(snap(0, 2, at_time(9, 0, 2), 78'010, 78'020, 50, 10), at_time(9, 0, 2));
  CHECK(gw.placed.size() == 1);
  CHECK(gw.placed[0].strategy == 11 && gw.placed[0].qty == 1);

  // Rising al with ~98% imbalance: only instance 12 trades al (its lots = 3).
  site.source(snap(1, 1, at_time(9, 0, 3), 20'000, 20'005, 99, 1), at_time(9, 0, 3));
  site.source(snap(1, 2, at_time(9, 0, 4), 20'005, 20'010, 99, 1), at_time(9, 0, 4));
  CHECK(gw.placed.size() == 2);
  CHECK(gw.placed[1].strategy == 12 && gw.placed[1].qty == 3 && gw.placed[1].inst == 1);

  // Fill both through the Source path; each lands on its own PnL book.
  for (const NewOrderReq& r : gw.placed) {
    ExecReport fill{};
    fill.cl_id = r.cl_id;
    fill.ts = at_time(9, 0, 5);
    fill.px = r.px;
    fill.qty = r.qty;
    fill.cum = r.qty;
    fill.inst = r.inst;
    fill.strategy = r.strategy;
    fill.account = r.account;
    fill.type = ExecType::Fill;
    fill.side = r.side;
    fill.offset = r.offset;
    site.source(fill, fill.ts);
  }
  const Position* p11 = site.pnl().find(1, 11, 0);
  const Position* p12 = site.pnl().find(1, 12, 1);
  CHECK(p11 && p11->long_td == 1);
  CHECK(p12 && p12->long_td == 3);
  CHECK(site.pnl().find(1, 11, 1) == nullptr);  // 11 never touched al
  CHECK(site.state_hash() != 0);

  // Snapshot round-trip: the banked instances serialize/restore in order.
  std::vector<std::uint8_t> snap_state;
  site.save_state(snap_state);
  const std::uint64_t h = site.state_hash();
  TwinSite site2(table, sc, TwinSite::Mode::Live, banks());
  site2.load_state(snap_state);
  CHECK(site2.state_hash() == h);
}

// ----------------------------------------------------- breakout trigger

static void test_breakout_entry() {
  InstrumentTable table = one_future();
  SiteConfig sc;
  sc.id = 1;
  sc.name = "brk";
  sc.venue = Venue::SHFE;
  sc.hosted = {0};
  sc.day = 20'260'713;

  BreakoutParams bp;
  bp.brk_ticks = 1;
  bp.imb_gate_e4 = 1'000;
  using BrkSite = Site<FakeGateway, Breakout>;
  BrkSite site(table, sc, BrkSite::Mode::Live,
               {Breakout(StrategyId{3}, AccountId{1}, bp, table.size(), {0})});
  FakeGateway gw;
  site.set_gateway(&gw);

  PhaseChangeEvt ph{};
  ph.ts = at_time(9, 0);
  ph.venue = Venue::SHFE;
  ph.phase = Phase::Continuous;
  site.source(ph, ph.ts);

  // First snapshot primes the session range at mid 78005.
  site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010, 50, 10), at_time(9, 0, 1));
  CHECK(gw.placed.empty());
  // Second clears the high by ≥ 1 tick with the bid heavy → go with the break.
  site.source(snap(0, 2, at_time(9, 0, 2), 78'010, 78'020, 50, 10), at_time(9, 0, 2));
  CHECK(gw.placed.size() == 1);
  CHECK(gw.placed[0].side == Side::Buy);
  CHECK(gw.placed[0].px == px_from_cny(78'020.0));  // crosses to enter
}

int main() {
  test_momentum_slots();
  test_two_momentum_instances();
  test_breakout_entry();
  std::printf("strat_trend_tests: %d checks, %d failures\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
