// tests_strats.cpp — flexible strategy composition: several instances of one
// type in one site (own ids/params/universes), per-instance exec routing and
// PnL attribution, empty banks, and the `strategies = ...` config parsing
// that platd and platbt share.

#include <cstdio>
#include <cstring>
#include <tuple>

#include "strat_params.hpp"
#include "tests_util.hpp"

// Two momentum instances side by side: different ids, different universes,
// different gates. Events route to both; execs/PnL stay per-instance.
void test_two_momentum_instances() {
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

  using TwinSite = Site<FakeGateway, SnapMomentum, MeanRevert, Breakout>;
  TwinSite site(table, sc, TwinSite::Mode::Live,
                {SnapMomentum(StrategyId{11}, AccountId{1}, tight, table.size(), {0}),
                 SnapMomentum(StrategyId{12}, AccountId{1}, strict, table.size(), {0, 1})},
                {},   // no meanrev instance configured at this site
                {});  // no breakout either
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
  TwinSite site2(table, sc, TwinSite::Mode::Live,
                 {SnapMomentum(StrategyId{11}, AccountId{1}, tight, table.size(), {0}),
                  SnapMomentum(StrategyId{12}, AccountId{1}, strict, table.size(), {0, 1})},
                 {}, {});
  site2.load_state(snap_state);
  CHECK(site2.state_hash() == h);
}

// The shared config parsing: instance lists, legacy default, fail-closed
// validation, disabled instances, per-instance params/universe fallback.
void test_strategy_config() {
  using namespace platcfg;
  const char* path = "plat_strats_test.cfg";
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
