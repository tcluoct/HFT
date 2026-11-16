// desk/src/tests.cpp — the desk suite: the cross-team composition and the
// config-facing lineup machinery. A desk is the unit that decides which
// strategy-team libraries run together, so its tests are exactly those two
// concerns:
//
//   test_three_strategies  — trend + fade + breakout instances on ONE site:
//                            same dispatch pass, separate ids, no interference.
//   test_strategy_config   — `strategies = ...` parsing, legacy lineup,
//                            fail-closed validation, per-bank instance order.

#include <cstdio>
#include <cstring>
#include <vector>

#include "desk/pack.hpp"
#include "plat/engine/site.hpp"
#include "plat/testing/harness.hpp"

using namespace plat;
using namespace desk;

int g_checks = 0;
int g_fails = 0;

// ------------------------------------ three concurrent strategies, one site

static void test_three_strategies() {
  using TrioSite = Site<FakeGateway, SnapMomentum, MeanRevert, Breakout>;
  static_assert(!std::is_polymorphic_v<TrioSite>);

  InstrumentTable table = one_future();
  SiteConfig sc;
  sc.id = 1;
  sc.name = "trio";
  sc.venue = Venue::SHFE;
  sc.hosted = {0};
  sc.day = 20'260'713;
  RiskLimits wide;  // let all three act in the same window
  wide.max_actions_per_sec = 1'000;
  wide.duplicate_horizon = 0;
  sc.limits = wide;

  MomentumParams mp;
  mp.imb_gate_e4 = 1'000;
  MeanRevParams rp;
  rp.ema_den = 4;  // fast anchor so a stretch shows quickly
  rp.enter_dev_ticks = 3;
  rp.imb_gate_e4 = 1'000;
  BreakoutParams bp;
  bp.brk_ticks = 1;
  bp.imb_gate_e4 = 1'000;
  TrioSite site(table, sc, TrioSite::Mode::Live,
                {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})},
                {MeanRevert(StrategyId{2}, AccountId{1}, rp, table.size(), {0})},
                {Breakout(StrategyId{3}, AccountId{1}, bp, table.size(), {0})});
  FakeGateway gw;
  site.set_gateway(&gw);

  PhaseChangeEvt ph{};
  ph.ts = at_time(9, 0);
  ph.venue = Venue::SHFE;
  ph.phase = Phase::Continuous;
  site.source(ph, ph.ts);

  // Baseline, then a strong bid-heavy up-thrust: momentum (delta+imb) AND
  // breakout (fresh session high) both fire on the SAME event — two orders,
  // two strategy ids, one dispatch pass. Mean-reversion stays out: the
  // imbalance agrees with the move, so the fade gate cannot open.
  site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010, 50, 10), at_time(9, 0, 1));
  site.source(snap(0, 2, at_time(9, 0, 2), 78'020, 78'030, 50, 10), at_time(9, 0, 2));
  CHECK(gw.placed.size() == 2);
  CHECK(gw.placed[0].strategy != gw.placed[1].strategy);
  const bool momentum_fired =
      gw.placed[0].strategy == 1 || gw.placed[1].strategy == 1;
  const bool breakout_fired =
      gw.placed[0].strategy == 3 || gw.placed[1].strategy == 3;
  CHECK(momentum_fired && breakout_fired);

  // Now a stretch far above the anchor with ASK-heavy imbalance: only the
  // fade can act (momentum needs agreement, breakout needs bid confirm).
  site.source(snap(0, 3, at_time(9, 0, 3), 78'060, 78'070, 10, 50), at_time(9, 0, 3));
  CHECK(gw.placed.size() == 3);
  CHECK(gw.placed[2].strategy == 2);
  CHECK(gw.placed[2].side == Side::Sell);  // sell rich
}

// The shared config parsing: instance lists, legacy default, fail-closed
// validation, disabled instances, per-instance params/universe fallback.
static void test_strategy_config() {
  const char* path = "desk_strats_test.cfg";
  const auto write_cfg = [&](const char* text) {
    std::FILE* f = std::fopen(path, "wb");
    std::fwrite(text, 1, std::strlen(text), f);
    std::fclose(f);
  };

  // Legacy: no `strategies` key → the classic trio with ids 1/2/3.
  write_cfg("momentum.lots = 2\n");
  {
    Config c;
    CHECK(c.load(path));
    std::vector<StratSpec> s;
    CHECK(parse_strategies(c, AccountId{7}, s));
    CHECK(s.size() == 3);
    CHECK(s[0].kind == Pack::kind_of("momentum") && s[0].id == 1 && s[0].account == 7);
    CHECK(s[1].kind == Pack::kind_of("meanrev") && s[1].id == 2);
    CHECK(s[2].kind == Pack::kind_of("breakout") && s[2].id == 3);
    CHECK(MomentumDef::params(c, s[0].name).lots == 2);
  }

  // Legacy: `<name>.enabled = 0` drops the instance but keeps the others.
  write_cfg("meanrev.enabled = 0\n");
  {
    Config c;
    CHECK(c.load(path));
    std::vector<StratSpec> s;
    CHECK(parse_strategies(c, AccountId{1}, s));
    CHECK(s.size() == 2 && s[0].id == 1 && s[1].id == 3);
  }

  // Instance list: two momentum instances with own ids/params/universes,
  // plus a per-instance account override.
  write_cfg(
      "instruments = cu2609, al2609\n"
      "strategies  = fast_cu, slow_al\n"
      "fast_cu.type = momentum\n"
      "fast_cu.id   = 4\n"
      "fast_cu.instruments = cu2609\n"
      "fast_cu.lots = 2\n"
      "slow_al.type = momentum\n"
      "slow_al.id   = 9\n"
      "slow_al.account = 3\n");
  {
    Config c;
    CHECK(c.load(path));
    std::vector<StratSpec> s;
    CHECK(parse_strategies(c, AccountId{1}, s));
    CHECK(s.size() == 2);
    CHECK(s[0].name == "fast_cu" && s[0].id == 4 && s[0].account == 1);
    CHECK(s[1].name == "slow_al" && s[1].id == 9 && s[1].account == 3);
    CHECK(MomentumDef::params(c, "fast_cu").lots == 2);
    CHECK(MomentumDef::params(c, "slow_al").lots == 1);  // type default

    InstrumentTable t = one_future();  // cu2609 only; al2609 unresolvable
    const auto u_fast = universe_from(c, t, "fast_cu");
    CHECK(u_fast.size() == 1 && u_fast[0] == 0);
    // No slow_al.instruments key → falls back to the global list (al2609
    // logged + skipped because this table doesn't host it).
    const auto u_slow = universe_from(c, t, "slow_al");
    CHECK(u_slow.size() == 1 && u_slow[0] == 0);
  }

  // Fail closed: duplicate id, missing id, unknown type.
  write_cfg("strategies = a, b\na.type = momentum\na.id = 5\nb.type = breakout\nb.id = 5\n");
  {
    Config c;
    c.load(path);
    std::vector<StratSpec> s;
    CHECK(!parse_strategies(c, AccountId{1}, s));
  }
  write_cfg("strategies = a\na.type = momentum\n");
  {
    Config c;
    c.load(path);
    std::vector<StratSpec> s;
    CHECK(!parse_strategies(c, AccountId{1}, s));  // explicit instance, no id
  }
  write_cfg("strategies = a\na.type = warp_drive\na.id = 1\n");
  {
    Config c;
    c.load(path);
    std::vector<StratSpec> s;
    CHECK(!parse_strategies(c, AccountId{1}, s));
  }

  // Banks: instances grouped per type, in config order; empty banks are fine.
  write_cfg(
      "instruments = cu2609\n"
      "strategies  = m1, m2, brk\n"
      "m1.type = momentum\nm1.id = 1\n"
      "m2.type = momentum\nm2.id = 2\nm2.lots = 5\n"
      "brk.type = breakout\nbrk.id = 3\n");
  {
    Config c;
    CHECK(c.load(path));
    std::vector<StratSpec> s;
    CHECK(parse_strategies(c, AccountId{1}, s));
    InstrumentTable t = one_future();
    const auto banks = Pack::banks(c, t, s);
    const auto& mom = std::get<0>(banks);
    const auto& rev = std::get<1>(banks);
    const auto& brk = std::get<2>(banks);
    CHECK(mom.size() == 2 && rev.empty() && brk.size() == 1);
    CHECK(mom[0].id() == 1 && mom[1].id() == 2 && brk[0].id() == 3);
  }

  std::remove(path);
}

int main() {
  test_three_strategies();
  test_strategy_config();
  std::printf("desk_tests: %d checks, %d failures\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
