// strategies/fade/src/tests.cpp — the fade team's suite: mean-reversion must
// fade a stretch only when the L1 imbalance OPPOSES the move (the stretch is
// running out of size), and must stay out when the imbalance agrees with it.
//
// The engine is only the host; the assertions are about the team's signal.

#include <cstdio>

#include "plat/engine/site.hpp"
#include "plat/testing/harness.hpp"
#include "strat/fade/meanrev.hpp"

using namespace plat;

int g_checks = 0;
int g_fails = 0;

static void test_meanrev_fade() {
  InstrumentTable table = one_future();
  SiteConfig sc;
  sc.id = 1;
  sc.name = "fade";
  sc.venue = Venue::SHFE;
  sc.hosted = {0};
  sc.day = 20'260'713;

  MeanRevParams rp;
  rp.ema_den = 4;  // fast anchor so a stretch shows quickly
  rp.enter_dev_ticks = 3;
  rp.imb_gate_e4 = 1'000;
  rp.take_ticks = 2;
  rp.stop_ticks = 4;
  using FadeSite = Site<FakeGateway, MeanRevert>;
  FadeSite site(table, sc, FadeSite::Mode::Live,
                {MeanRevert(StrategyId{2}, AccountId{1}, rp, table.size(), {0})});
  FakeGateway gw;
  site.set_gateway(&gw);

  PhaseChangeEvt ph{};
  ph.ts = at_time(9, 0);
  ph.venue = Venue::SHFE;
  ph.phase = Phase::Continuous;
  site.source(ph, ph.ts);

  // Prime the anchor at mid 78005.
  site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010, 50, 10), at_time(9, 0, 1));
  CHECK(gw.placed.empty());

  // Stretch up 6 ticks with the imbalance agreeing with the move: no fade.
  site.source(snap(0, 2, at_time(9, 0, 2), 78'060, 78'070, 50, 10), at_time(9, 0, 2));
  CHECK(gw.placed.empty());

  // Same stretch, now ask-heavy: the move is out of size — sell rich at the
  // bid, expecting reversion toward the anchor.
  site.source(snap(0, 3, at_time(9, 0, 3), 78'060, 78'070, 10, 50), at_time(9, 0, 3));
  CHECK(gw.placed.size() == 1);
  CHECK(gw.placed[0].side == Side::Sell);
  CHECK(gw.placed[0].strategy == 2);
  CHECK(gw.placed[0].px == px_from_cny(78'060.0));
  CHECK(site.state_hash() != 0);

  // Fill the entry: the cycle moves to Holding and the state hash changes.
  const std::uint64_t before = site.state_hash();
  ExecReport fill{};
  fill.cl_id = gw.placed[0].cl_id;
  fill.ts = at_time(9, 0, 4);
  fill.px = gw.placed[0].px;
  fill.qty = gw.placed[0].qty;
  fill.cum = gw.placed[0].qty;
  fill.inst = gw.placed[0].inst;
  fill.strategy = gw.placed[0].strategy;
  fill.account = gw.placed[0].account;
  fill.type = ExecType::Fill;
  fill.side = gw.placed[0].side;
  fill.offset = gw.placed[0].offset;
  site.source(fill, fill.ts);
  CHECK(site.state_hash() != before);
  CHECK(site.pnl().find(1, 2, 0) != nullptr);
}

int main() {
  test_meanrev_fade();
  std::printf("strat_fade_tests: %d checks, %d failures\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
