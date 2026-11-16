// strategies/basis/src/tests.cpp — the basis team's suite: both arb legs
// (cash leg + hedge leg) instantiate in one Site and every CRTP handler
// template compiles. The legs coordinate only through the cross-site
// messages (BasisSummary / HedgeIntent / HedgeDone), so the compile coverage
// is the meaningful contract until a federation venue is wired (Phase 2).
//
// The engine is only the host; no signal fires on a single book update.

#include <cstdio>

#include "plat/engine/site.hpp"
#include "plat/testing/harness.hpp"
#include "strat/basis/basisarb.hpp"

using namespace plat;

int g_checks = 0;
int g_fails = 0;

static void test_arb_legs_compose() {
  using ArbSite = Site<FakeGateway, ArbCashLeg, ArbHedgeLeg>;
  InstrumentTable table = one_future();
  table.add("510300", Venue::SSE, Board::Etf, px_from_cny(0.001), 100, 1, px_from_cny(4.0));
  SiteConfig sc;
  sc.id = 9;
  sc.name = "arb";
  sc.venue = Venue::SSE;
  sc.hosted = {0, 1};
  sc.day = 20'260'713;
  ArbPairParams ap;
  ap.cash_inst = 1;  // ETF at the cash site
  ap.fut_inst = 0;   // future at the hedge site
  ArbSite site(table, sc, ArbSite::Mode::Live,
               {ArbCashLeg(StrategyId{6}, AccountId{1}, {ap}, table.size())},
               {ArbHedgeLeg(StrategyId{7}, AccountId{1}, {ap}, table.size())});
  FakeGateway gw;
  site.set_gateway(&gw);
  PhaseChangeEvt ph{};
  ph.ts = at_time(9, 30);
  ph.venue = Venue::SSE;
  ph.phase = Phase::Continuous;
  site.source(ph, ph.ts);                      // every handler path instantiates
  site.source(snap(0, 1, at_time(9, 30, 1), 78'000, 78'010), at_time(9, 30, 1));
  CHECK(site.state_hash() != 0);
  CHECK(gw.placed.empty());                    // no edge from one snapshot
}

int main() {
  test_arb_legs_compose();
  std::printf("strat_basis_tests: %d checks, %d failures\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
