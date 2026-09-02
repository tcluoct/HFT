// tests_engine.cpp — engine suite: the full dispatch loop driven through a
// fake gateway at the production OrderGateway boundary — single- and
// multi-instrument strategies, three concurrent strategies, Phase-2 compile
// coverage, crash resume, and the async journal thread.

#include <cstring>
#include <memory>
#include <string>

#include "plat/bus/journal.hpp"
#include "plat/replay/replayer.hpp"  // compile coverage: recovery tooling API
#include "plat/strat/basisarb.hpp"
#include "plat/strat/breakout.hpp"
#include "plat/strat/meanrev.hpp"
#include "plat/strat/t0alpha.hpp"
#include "tests_util.hpp"

void test_site_loop() {
  InstrumentTable table = one_future();
  SiteConfig sc;
  sc.id = 1;
  sc.name = "test";
  sc.venue = Venue::SHFE;
  sc.hosted = {0};
  sc.day = 20'260'713;

  MomentumParams mp;
  mp.imb_gate_e4 = 1'000;
  mp.lots = 1;
  TestSite site(table, sc, TestSite::Mode::Live,
                {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})});
  FakeGateway gw;
  site.set_gateway(&gw);

  SessionStateEvt up{};
  up.ts = at_time(8, 58);
  up.venue = Venue::SHFE;
  up.state = SessState::Up;
  site.source(up, up.ts);
  CHECK(site.arm_session(at_time(8, 59)));

  PhaseChangeEvt ph{};
  ph.ts = at_time(9, 0);
  ph.venue = Venue::SHFE;
  ph.phase = Phase::Continuous;
  site.source(ph, ph.ts);

  // Two rising snapshots with strong bid imbalance → momentum entry (buy).
  site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010, 50, 10), at_time(9, 0, 1));
  site.source(snap(0, 2, at_time(9, 0, 2), 78'010, 78'020, 50, 10), at_time(9, 0, 2));
  CHECK(gw.placed.size() == 1);
  CHECK(gw.placed[0].side == Side::Buy);
  CHECK(gw.placed[0].px == px_from_cny(78'020.0));  // crosses to enter

  // Venue responds through the same Source path the live driver uses.
  const NewOrderReq& r = gw.placed[0];
  ExecReport ack{};
  ack.cl_id = r.cl_id;
  ack.venue_no = 9'001;
  ack.ts = at_time(9, 0, 2, kNsPerMs);
  ack.trigger_ts = r.trigger_ts;
  ack.px = r.px;
  ack.leaves = r.qty;
  ack.inst = r.inst;
  ack.strategy = r.strategy;
  ack.account = r.account;
  ack.type = ExecType::Ack;
  ack.side = r.side;
  ack.offset = r.offset;
  site.source(ack, ack.ts);

  ExecReport fill = ack;
  fill.type = ExecType::Fill;
  fill.qty = r.qty;
  fill.cum = r.qty;
  fill.leaves = 0;
  fill.ts = ack.ts + kNsPerMs;
  site.source(fill, fill.ts);

  const Position* p = site.pnl().find(1, 1, 0);
  CHECK(p && p->long_td == 1);
  CHECK(site.oms().find(r.cl_id)->state == OrderState::Filled);
  CHECK(site.tel().get("fills") == 1);

  // Kill switch: halt cancels nothing (no resting orders) but denies new flow.
  site.command_kill(KillScope::Site, KillAction::Halt, 0, at_time(9, 0, 3));
  site.source(snap(0, 3, at_time(9, 0, 3), 78'030, 78'040, 50, 10), at_time(9, 0, 3));
  site.source(snap(0, 4, at_time(9, 0, 4), 78'040, 78'050, 50, 10), at_time(9, 0, 4));
  CHECK(gw.placed.size() == 1);            // nothing new reached the gateway
  CHECK(site.tel().get("risk.deny.12") >= 1);  // kill-switch denial recorded

  // EOD reconciliation: venue agrees with our fills, no open orders.
  site.run_eod({}, 1, 0);
  CHECK(site.eod().ran && site.eod().fills_ok && site.eod().order_breaks == 0);
}

// ------------------------------------- multi-instrument slots (one instance)

void test_momentum_multi_instrument() {
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
  TestSite site(table, sc, TestSite::Mode::Live,
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

// ------------------------------------ three concurrent strategies, one site

void test_three_strategies() {
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

// ---------------------------------------------------- crash resume replay

// Emulate a crash: strip the seal from a journal (header count/checksum are
// only finalized at seal, so a crashed file looks exactly like this) and
// prove the reader recovers the envelope count from the file size and a
// fresh site replays it to the same state — with the gateway muted.
void test_crash_resume() {
  const char* path = "plat_crash_test.jnl";
  std::uint64_t crash_state_hash = 0;
  std::uint64_t crash_envelopes = 0;
  OrderId open_cl_id = kNoOrder;
  {
    InstrumentTable table = one_future();
    SiteConfig sc;
    sc.id = 1;
    sc.name = "crash";
    sc.venue = Venue::SHFE;
    sc.hosted = {0};
    sc.day = 20'260'713;
    sc.journal_path = path;
    MomentumParams mp;
    mp.imb_gate_e4 = 1'000;
    TestSite site(table, sc, TestSite::Mode::Live,
                  {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})});
    FakeGateway gw;
    site.set_gateway(&gw);

    PhaseChangeEvt ph{};
    ph.ts = at_time(9, 0);
    ph.venue = Venue::SHFE;
    ph.phase = Phase::Continuous;
    site.source(ph, ph.ts);
    // Entry fires and FILLS (a held position), then a second entry is
    // placed and left IN FLIGHT — the awkward crash state.
    site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010, 50, 10), at_time(9, 0, 1));
    site.source(snap(0, 2, at_time(9, 0, 2), 78'010, 78'020, 50, 10), at_time(9, 0, 2));
    CHECK(gw.placed.size() == 1);
    const NewOrderReq& r = gw.placed[0];
    ExecReport fill{};
    fill.cl_id = r.cl_id;
    fill.ts = at_time(9, 0, 2, kNsPerMs);
    fill.px = r.px;
    fill.qty = r.qty;
    fill.cum = r.qty;
    fill.inst = 0;
    fill.strategy = 1;
    fill.account = 1;
    fill.type = ExecType::Fill;
    fill.side = r.side;
    fill.offset = r.offset;
    site.source(fill, fill.ts);

    crash_state_hash = site.state_hash();
    crash_envelopes = site.tel().get("stream.envelopes");
    open_cl_id = r.cl_id;
    site.run_eod({}, 1, 0);  // flush + seal so the bytes are on disk
  }
  {
    // Strip the seal in place: header = magic(8) count(8) hash(8) day(4)
    // site(1) sealed(1) — zero count and sealed, as a crash leaves them.
    std::FILE* f = std::fopen(path, "rb+");
    CHECK(f != nullptr);
    const std::uint64_t zero64 = 0;
    const std::uint8_t zero8 = 0;
    std::fseek(f, 8, SEEK_SET);
    std::fwrite(&zero64, sizeof(zero64), 1, f);  // count
    std::fseek(f, 29, SEEK_SET);
    std::fwrite(&zero8, sizeof(zero8), 1, f);    // sealed
    std::fclose(f);
  }

  // Header peek — what the startup auto-resume decision reads: unsealed +
  // trading day, without loading the envelopes.
  JournalHeader peeked{};
  CHECK(JournalReader::peek(path, peeked));
  CHECK(peeked.magic == kJournalMagic);
  CHECK(peeked.sealed == 0);
  CHECK(peeked.day == 20'260'713);

  JournalReader jr;
  CHECK(jr.open(path));
  CHECK(!jr.sealed());
  CHECK(!jr.verified());                       // no checksum to verify against
  CHECK(jr.header().count == crash_envelopes); // recovered from file size

  // Resume: fresh site, gateway muted during the replay, journal continuity
  // into a new file.
  const char* resumed_path = "plat_resume_test.jnl";
  {
    InstrumentTable table = one_future();
    SiteConfig sc;
    sc.id = 1;
    sc.name = "crash";
    sc.venue = Venue::SHFE;
    sc.hosted = {0};
    sc.day = 20'260'713;
    sc.journal_path = resumed_path;
    MomentumParams mp;
    mp.imb_gate_e4 = 1'000;
    TestSite site(table, sc, TestSite::Mode::Live,
                  {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})});
    FakeGateway gw;
    site.set_gateway(&gw);

    site.set_recovering(true);
    for (const Envelope& e : jr.envelopes()) site.inject(e);
    site.set_recovering(false);

    CHECK(gw.placed.empty());                        // nothing re-sent while recovering
    CHECK(site.state_hash() == crash_state_hash);    // exact state at the crash point
    const Position* p = site.pnl().find(1, 1, 0);
    CHECK(p && p->long_td == 1);                     // the filled entry survived
    CHECK(site.oms().find(open_cl_id) != nullptr);   // and the order book state
    site.run_eod({}, 1, 0);
  }
  // The resumed journal is the complete day again: same envelope count and
  // byte-identical stream content to what the crashed run produced.
  JournalReader jr2;
  CHECK(jr2.open(resumed_path));
  CHECK(jr2.verified());
  CHECK(jr2.header().count == crash_envelopes);
  StateHash crashed_hash;
  for (const auto& e : jr.envelopes()) {
    crashed_hash.mix(e.seq);
    crashed_hash.mix(e.schema);
    crashed_hash.mix_bytes(e.payload, e.size);
  }
  CHECK(jr2.header().stream_hash == crashed_hash.value());
  std::remove(path);
  std::remove(resumed_path);
}

// ------------------------------------- Phase-2 strategies: compile coverage

// T0Alpha and the arb legs have no live venue yet (stocks / federation are
// Phase 2). Instantiating them in a Site forces every CRTP handler template
// to compile, so the dormant code cannot bit-rot while it waits.
void test_phase2_strategies_compile() {
  using Phase2Site = Site<FakeGateway, T0Alpha, ArbCashLeg, ArbHedgeLeg>;
  InstrumentTable table = one_future();
  table.add("510300", Venue::SSE, Board::Etf, px_from_cny(0.001), 100, 1, px_from_cny(4.0));
  SiteConfig sc;
  sc.id = 9;
  sc.name = "p2";
  sc.venue = Venue::SSE;
  sc.hosted = {0, 1};
  sc.day = 20'260'713;
  ArbPairParams ap;
  ap.cash_inst = 1;
  ap.fut_inst = 0;
  Phase2Site site(table, sc, Phase2Site::Mode::Live,
                  {T0Alpha(StrategyId{5}, AccountId{1}, T0Params{}, table.size(), {1})},
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
  CHECK(gw.placed.empty());                    // no signals from one snapshot
}

// -------------------------------------------------- async journal thread

void test_async_journal() {
  const char* path = "plat_async_test.jnl";
  std::uint64_t stream_hash = 0;
  std::uint64_t envelopes = 0;
  {
    InstrumentTable table = one_future();
    SiteConfig sc;
    sc.id = 1;
    sc.name = "ajnl";
    sc.venue = Venue::SHFE;
    sc.hosted = {0};
    sc.day = 20'260'713;
    sc.journal_path = path;
    MomentumParams mp;
    mp.imb_gate_e4 = 1'000;
    TestSite site(table, sc, TestSite::Mode::Live,
                  {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})});
    FakeGateway gw;
    site.set_gateway(&gw);

    PhaseChangeEvt ph{};
    ph.ts = at_time(9, 0);
    ph.venue = Venue::SHFE;
    ph.phase = Phase::Continuous;
    site.source(ph, ph.ts);
    for (int i = 0; i < 500; ++i)
      site.source(snap(0, static_cast<std::uint64_t>(i + 1), at_time(9, 0, 1, i), 78'010,
                       78'020, 50, 10),
                  at_time(9, 0, 1, i));
    CHECK(site.journal_healthy());
    site.run_eod({}, 0, 0);  // drains the journal ring, seals, joins
    stream_hash = site.stream().stream_hash();
    envelopes = site.tel().get("stream.envelopes");
  }
  // The file the journal THREAD wrote must be exactly the canonical stream:
  // verified checksum, every envelope, hash equal to the engine's. A clean
  // EOD leaves it sealed — the header peek the auto-resume decision reads.
  JournalHeader peeked{};
  CHECK(JournalReader::peek(path, peeked));
  CHECK(peeked.sealed == 1);
  JournalReader r;
  CHECK(r.open(path));
  CHECK(r.verified());
  CHECK(r.header().count == envelopes);
  CHECK(r.header().stream_hash == stream_hash);
  std::remove(path);
}

