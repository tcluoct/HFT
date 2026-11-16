// tests_engine.cpp — engine suite: the full dispatch loop driven through a
// fake gateway at the production OrderGateway boundary — site loop, latency
// measurement, crash resume, and the async journal thread. Strategy-specific
// behaviour lives with its team (strategies/<team>/src/tests.cpp); the
// cross-team composition lives with the desk (desk/src/tests.cpp).

#include <cstring>
#include <memory>
#include <string>

#include "plat/bus/journal.hpp"
#include "plat/engine/fingerprint.hpp"
#include "plat/engine/snapshot.hpp"
#include "plat/replay/replayer.hpp"  // compile coverage: recovery tooling API
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

// ------------------------------------------------ FLATTEN close-out (§10.3)

// HALT refuses everything and never sends a close. FLATTEN is reduce-only:
// it cancels resting orders, closes every position bucket with IOC orders
// (split to the firm's max order size), keeps opening orders blocked, and
// becomes flat once the closes fill.
void test_flatten_closeout() {
  InstrumentTable table = one_future();
  SiteConfig sc;
  sc.id = 1;
  sc.name = "flat";
  sc.venue = Venue::SHFE;
  sc.hosted = {0};
  sc.day = 20'260'713;
  RiskLimits wide;
  wide.max_order_qty = 20;  // 25-lot long must split into 20 + 5
  wide.max_actions_per_sec = 1'000;
  wide.max_actions_per_day = 10'000;
  wide.max_order_notional_e4 = px_from_cny(1e9);
  wide.max_net_notional_e4 = px_from_cny(1e9);
  wide.max_margin_e4 = px_from_cny(1e9);
  wide.duplicate_horizon = 0;
  wide.price_collar_ticks = 1'000;
  sc.limits = wide;

  MomentumParams mp;
  mp.imb_gate_e4 = 9'000'000;  // stay quiet: this test drives kill commands only
  TestSite site(table, sc, TestSite::Mode::Live,
                {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})});
  FakeGateway gw;
  site.set_gateway(&gw);

  PhaseChangeEvt ph{};
  ph.ts = at_time(9, 0);
  ph.venue = Venue::SHFE;
  ph.phase = Phase::Continuous;
  site.source(ph, ph.ts);
  site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010, 50, 10), at_time(9, 0, 1));

  // Two books, three close buckets: long-yesterday (splits), short-today.
  site.pnl().at(1, 1, 0).long_yd = 25;
  site.pnl().at(1, 2, 0).short_td = 3;

  // HALT is not flatten: nothing is sent, the positions stay.
  site.command_kill(KillScope::Site, KillAction::Halt, 0, at_time(9, 0, 2));
  CHECK(gw.placed.empty());
  CHECK(site.pnl().find(1, 1, 0)->long_yd == 25);

  // FLATTEN closes both books: 25 long-yesterday -> sell 20 + 5 close-yesterday
  // at the bid; 3 short-today -> buy 3 close-today at the ask.
  site.command_kill(KillScope::Site, KillAction::Disarm, 0, at_time(9, 0, 3));
  site.command_kill(KillScope::Site, KillAction::Flatten, 0, at_time(9, 0, 4));
  CHECK(gw.placed.size() == 3);
  CHECK(gw.placed[0].side == Side::Sell && gw.placed[0].offset == Offset::CloseYesterday &&
        gw.placed[0].qty == 20 && gw.placed[0].px == px_from_cny(78'000.0));
  CHECK(gw.placed[1].side == Side::Sell && gw.placed[1].offset == Offset::CloseYesterday &&
        gw.placed[1].qty == 5);
  CHECK(gw.placed[2].side == Side::Buy && gw.placed[2].offset == Offset::CloseToday &&
        gw.placed[2].qty == 3 && gw.placed[2].px == px_from_cny(78'010.0));

  // Opening stays refused while FLATTEN is armed.
  const OrderId blocked =
      site.place(0, Side::Buy, Offset::Open, px_from_cny(78'010.0), 1, Tif::Ioc,
                 at_time(9, 0, 5));
  CHECK(gw.placed.size() == 3);
  CHECK(site.last_deny().cl_id == blocked && site.last_deny().check == RiskCheck::KillSwitch);

  // Fill the closes: both books go flat.
  for (const NewOrderReq& r : gw.placed) {
    ExecReport fill{};
    fill.cl_id = r.cl_id;
    fill.ts = at_time(9, 0, 6);
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
  CHECK(site.pnl().find(1, 1, 0)->net() == 0);
  CHECK(site.pnl().find(1, 2, 0)->net() == 0);
  CHECK(site.state_hash() != 0);

  // Disarm lifts the reduce-only mode (the operator takes the book back).
  site.command_kill(KillScope::Site, KillAction::Disarm, 0, at_time(9, 0, 7));
  CHECK(!site.risk().kill().any_armed());
}

// FLATTEN must be deterministic like every other engine action: the close
// orders it generates are Derived envelopes, so replaying only the journal's
// Source prefix must regenerate the exact same canonical stream.
void test_flatten_replay() {
  const char* path = "plat_flatten_replay.jnl";
  InstrumentTable table = one_future();
  SiteConfig sc;
  sc.id = 1;
  sc.name = "flat-replay";
  sc.venue = Venue::SHFE;
  sc.hosted = {0};
  sc.day = 20'260'713;
  sc.journal_path = path;
  RiskLimits wide;
  wide.max_order_qty = 20;
  wide.max_actions_per_sec = 1'000;
  wide.max_actions_per_day = 10'000;
  wide.max_order_notional_e4 = px_from_cny(1e9);
  wide.max_net_notional_e4 = px_from_cny(1e9);
  wide.max_margin_e4 = px_from_cny(1e9);
  wide.duplicate_horizon = 0;
  wide.price_collar_ticks = 1'000;
  sc.limits = wide;

  MomentumParams mp;
  mp.imb_gate_e4 = 9'000'000;
  const auto banks = [&] {
    return std::vector<SnapMomentum>{
        SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})};
  };
  {
    TestSite site(table, sc, TestSite::Mode::Live, banks());
    FakeGateway gw;
    site.set_gateway(&gw);
    PhaseChangeEvt ph{};
    ph.ts = at_time(9, 0);
    ph.venue = Venue::SHFE;
    ph.phase = Phase::Continuous;
    site.source(ph, ph.ts);
    site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010, 50, 10), at_time(9, 0, 1));
    // Seed the position through the JOURNALED fill path (a direct PnL poke
    // would not exist in replay, so the replay would legitimately flatten
    // nothing and the hash comparison would be meaningless).
    ExecReport fill{};
    fill.cl_id = 0x1234;
    fill.ts = at_time(9, 0, 1, 500 * kNsPerMs);
    fill.px = px_from_cny(78'000.0);
    fill.qty = 7;
    fill.cum = 7;
    fill.leaves = 0;
    fill.inst = 0;
    fill.strategy = 1;
    fill.account = 1;
    fill.type = ExecType::Fill;
    fill.side = Side::Buy;
    fill.offset = Offset::Open;
    site.source(fill, fill.ts);
    CHECK(site.pnl().find(1, 1, 0)->long_td == 7);  // a fill today opens today's bucket
    site.command_kill(KillScope::Site, KillAction::Flatten, 0, at_time(9, 0, 2));
    CHECK(gw.placed.size() == 1);  // 7 lots <= max_order_qty: one close
    site.run_eod({}, 0, 0);
  }
  JournalReader jr;
  CHECK(jr.open(path));
  CHECK(jr.sealed() && jr.verified());

  TestSite replay(table, sc, TestSite::Mode::Replay, banks());
  FakeGateway gw;
  replay.set_gateway(&gw);
  for (const Envelope& e : jr.envelopes()) replay.inject(e);
  CHECK(gw.placed.empty());  // Replay mode never sends
  CHECK(replay.stream().stream_hash() == jr.header().stream_hash);
  std::remove(path);
}

// Session identity: the `.session` sidecar must round-trip and every input
// that changes behavior must change the config hash (otherwise a resume guard
// would pass while replaying against a different composition).
void test_session_fingerprint() {
  const char* path = "plat_session_fp.bin";
  SessionFingerprint a;
  a.build = 0x1111;
  a.pack = 0x2222;
  a.config = 0x3333;
  a.day = 20'260'713;
  a.site = 1;
  CHECK(save_session_fingerprint(path, a));
  SessionFingerprint b;
  CHECK(load_session_fingerprint(path, b));
  CHECK(same_fingerprint(a, b));
  b.config ^= 1;
  CHECK(!same_fingerprint(a, b));
  std::remove(path);
  CHECK(build_fingerprint() != 0);

  InstrumentTable t = one_future();
  FeeSchedule f;
  std::vector<StratSpec> specs;
  const auto c1 = config_fingerprint(specs, f, t, {0}, 250);
  f.fut_per_lot_e4 += 1;
  const auto c2 = config_fingerprint(specs, f, t, {0}, 250);
  CHECK(c1 != c2);  // fee schedule is part of the behavior identity
  CHECK(config_fingerprint(specs, f, t, {0}, 251) != c2);  // rate cap is structural
}

// Live risk-limit reload is a journaled Source event, applied on the engine
// thread: an order that was too large before the update passes after it, and
// a rate-ring resize (restart-only) is rejected rather than silently ignored.
void test_risk_limits_reload() {
  InstrumentTable table = one_future();
  SiteConfig sc;
  sc.id = 1;
  sc.name = "limits";
  sc.venue = Venue::SHFE;
  sc.hosted = {0};
  sc.day = 20'260'713;
  RiskLimits l;
  l.max_order_qty = 1;
  l.max_actions_per_sec = 1'000;
  l.max_actions_per_day = 10'000;
  l.max_order_notional_e4 = px_from_cny(1e9);
  l.max_net_notional_e4 = px_from_cny(1e9);
  l.max_margin_e4 = px_from_cny(1e9);
  l.duplicate_horizon = 0;
  l.price_collar_ticks = 1'000;
  sc.limits = l;

  MomentumParams mp;
  mp.imb_gate_e4 = 9'000'000;
  TestSite site(table, sc, TestSite::Mode::Live,
                {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})});
  FakeGateway gw;
  site.set_gateway(&gw);
  PhaseChangeEvt ph{};
  ph.ts = at_time(9, 0);
  ph.venue = Venue::SHFE;
  ph.phase = Phase::Continuous;
  site.source(ph, ph.ts);
  site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010, 50, 10), at_time(9, 0, 1));

  const OrderId too_big =
      site.place(0, Side::Buy, Offset::Open, px_from_cny(78'010.0), 2, Tif::Ioc,
                 at_time(9, 0, 2));
  CHECK(gw.placed.empty());
  CHECK(site.last_deny().cl_id == too_big && site.last_deny().check == RiskCheck::SizeNotional);

  RiskLimits next = site.risk().limits();
  next.max_order_qty = 5;
  RiskLimitsEvt ev{};
  ev.ts = at_time(9, 0, 3);
  ev.version = 1;
  ev.limits = next;
  site.source(ev, ev.ts);
  CHECK(site.risk().limits().max_order_qty == 5);

  site.place(0, Side::Buy, Offset::Open, px_from_cny(78'010.0), 2, Tif::Ioc,
             at_time(9, 0, 4));
  CHECK(gw.placed.size() == 1 && gw.placed[0].qty == 2);

  // Changing the rate-ring size is restart-only: the gate keeps the old value.
  RiskLimits bad = site.risk().limits();
  bad.max_actions_per_sec += 1;
  RiskLimitsEvt bad_ev{};
  bad_ev.ts = at_time(9, 0, 5);
  bad_ev.version = 2;
  bad_ev.limits = bad;
  site.source(bad_ev, bad_ev.ts);
  CHECK(site.risk().limits().max_actions_per_sec == 1'000);
}

// Checkpoint recovery: a snapshot at envelope K plus the journal suffix must
// reconstruct the identical state and canonical stream. The prefix is copied
// into the new stream without dispatch (fast), the snapshot supplies state,
// and only the Source suffix is dispatched.
void test_snapshot_resume() {
  const char* jnl = "plat_snapshot_test.jnl";
  const char* snap_file = "plat_snapshot_test.jnl.snap";
  const char* sess = "plat_snapshot_test.jnl.session";
  InstrumentTable table = one_future();
  SiteConfig sc;
  sc.id = 1;
  sc.name = "snap";
  sc.venue = Venue::SHFE;
  sc.hosted = {0};
  sc.day = 20'260'713;
  sc.journal_path = jnl;
  RiskLimits wide;
  wide.max_order_qty = 20;
  wide.max_actions_per_sec = 1'000;
  wide.max_actions_per_day = 10'000;
  wide.max_order_notional_e4 = px_from_cny(1e9);
  wide.max_net_notional_e4 = px_from_cny(1e9);
  wide.max_margin_e4 = px_from_cny(1e9);
  wide.duplicate_horizon = 0;
  wide.price_collar_ticks = 1'000;
  sc.limits = wide;
  MomentumParams mp;
  mp.imb_gate_e4 = 9'000'000;
  const auto banks = [&] {
    return std::vector<SnapMomentum>{
        SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})};
  };
  SessionFingerprint fp;
  fp.build = 1;
  fp.pack = 2;
  fp.config = 3;
  fp.day = sc.day;
  fp.site = sc.id;

  auto fill = [](OrderId cl_id, Nanos ts, Qty qty) {
    ExecReport f{};
    f.cl_id = cl_id;
    f.ts = ts;
    f.px = px_from_cny(78'000.0);
    f.qty = qty;
    f.cum = qty;
    f.leaves = 0;
    f.inst = 0;
    f.strategy = 1;
    f.account = 1;
    f.type = ExecType::Fill;
    f.side = Side::Buy;
    f.offset = Offset::Open;
    return f;
  };

  std::uint64_t end_hash = 0, stream_hash = 0, snap_index = 0;
  {
    TestSite site(table, sc, TestSite::Mode::Live, banks());
    FakeGateway gw;
    site.set_gateway(&gw);
    PhaseChangeEvt ph{};
    ph.ts = at_time(9, 0);
    ph.venue = Venue::SHFE;
    ph.phase = Phase::Continuous;
    site.source(ph, ph.ts);
    site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010, 50, 10), at_time(9, 0, 1));
    site.source(fill(0x11, at_time(9, 0, 2), 7), at_time(9, 0, 2));
    // A hot-reloaded limit is part of recovery state: the snapshot must carry
    // it, not silently revert to the startup config on resume.
    RiskLimits next = site.risk().limits();
    next.max_order_qty = 7;
    RiskLimitsEvt lev{};
    lev.ts = at_time(9, 0, 2, 500 * kNsPerMs);
    lev.version = 1;
    lev.limits = next;
    site.source(lev, lev.ts);
    CHECK(save_snapshot(site, snap_file, fp));
    snap_index = site.stream().next_seq();
    // More events after the checkpoint: the resume must replay this suffix.
    site.source(snap(0, 2, at_time(9, 0, 3), 78'010, 78'020, 50, 10), at_time(9, 0, 3));
    site.source(fill(0x12, at_time(9, 0, 4), 3), at_time(9, 0, 4));
    end_hash = site.state_hash();
    stream_hash = site.stream().stream_hash();
    site.run_eod({}, 0, 0);
  }
  JournalReader jr;
  CHECK(jr.open(jnl));
  CHECK(jr.sealed() && jr.verified());
  CHECK(jr.envelopes().size() > snap_index);

  TestSite replay(table, sc, TestSite::Mode::Replay, banks());
  FakeGateway gw;
  replay.set_gateway(&gw);
  SnapshotInfo info;
  CHECK(load_snapshot(replay, snap_file, fp, info, jr.envelopes().size()));
  CHECK(info.index == snap_index);
  for (std::size_t i = 0; i < info.index; ++i) replay.stream().absorb(jr.envelopes()[i]);
  for (std::size_t i = static_cast<std::size_t>(info.index); i < jr.envelopes().size(); ++i)
    replay.inject(jr.envelopes()[i]);
  CHECK(replay.state_hash() == end_hash);
  CHECK(replay.stream().stream_hash() == stream_hash);
  CHECK(replay.risk().limits().max_order_qty == 7);  // snapshot carried the reload
  std::remove(jnl);
  std::remove(snap_file);
  std::remove(sess);
}

// Credentials can come from a permissioned file instead of the config text;
// the resolver must read exactly the first line and never fall through to an
// empty/absent value silently.
void test_secret_resolution() {
  const char* secret = "plat_secret_test.txt";
  const char* cfg_path = "plat_secret_test.cfg";
  {
    std::FILE* f = std::fopen(secret, "wb");
    CHECK(f != nullptr);
    if (!f) return;
    std::fputs("  s3cret-value \r\nsecond-line\n", f);
    std::fclose(f);
  }
  {
    std::FILE* f = std::fopen(cfg_path, "wb");
    CHECK(f != nullptr);
    if (!f) return;
    std::fputs("password_file = plat_secret_test.txt\n", f);
    std::fclose(f);
  }
  Config c;
  CHECK(c.load(cfg_path));
  CHECK(resolve_secret(c, "password") == "s3cret-value");
  CHECK(resolve_secret(c, "auth_code").empty());
  std::remove(secret);
  std::remove(cfg_path);
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
    site.refresh_latency();
    const auto& hist = site.tel().histos();
    CHECK(hist.at("journal.enqueue_ns").count() == envelopes);
    CHECK(hist.at("journal.queue_ns").count() == envelopes);
    CHECK(hist.at("journal.write_batch_ns").count() > 0);
    CHECK(hist.at("journal.write_batch_ns").count() == hist.at("journal.flush_ns").count());
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

// A faulted journal must NEVER be sealed: an audit record with a hole has to
// look exactly like a crashed session (unsealed, count derived from the file),
// so recovery/replay treats it as incomplete instead of certifying it.
void test_journal_fault_unsealed() {
  const char* path = "plat_jnl_fault_test.jnl";
  {
    AsyncJournal journal;
    CHECK(journal.open(path, 20'260'713, 1));
    const auto evt = snap(0, 1, at_time(9, 0), 78'000, 78'010);
    journal.append(make_envelope(0, evt.ts, evt, Origin::Source, 1));
    journal.simulate_fault_for_test();
    journal.finish();
    CHECK(!journal.healthy());
  }
  JournalHeader hdr{};
  CHECK(JournalReader::peek(path, hdr));
  CHECK(hdr.magic == kJournalMagic);
  CHECK(hdr.sealed == 0);  // left exactly like a crash, never certified
  JournalReader r;
  CHECK(r.open(path));
  CHECK(!r.sealed());
  CHECK(!r.verified());
  CHECK(r.header().count == 1);  // the bytes are still there for post-mortem
  std::remove(path);
}

namespace {
Nanos timing_now = 0;
Nanos test_perf_clock() { timing_now += 10; return timing_now; }
struct TimedGateway : FakeGateway {
  void place(const NewOrderReq& r) { timing_now += 1000; FakeGateway::place(r); }
};
struct TimedStrategy : Strategy<TimedStrategy> {
  using Strategy<TimedStrategy>::Strategy;
  template <typename Ctx> void on_snapshot(Ctx& ctx, const MdSnapshotEvt& s) {
    timing_now += 200;
    ctx.place(s.inst, Side::Buy, Offset::Open, s.ask_px[0], 1, Tif::Day, s.ts);
  }
};
}

void test_latency_measurements() {
  auto table = one_future();
  SiteConfig cfg;
  cfg.hosted = {0};
  cfg.venue = Venue::SHFE;
  using TimedSite = Site<TimedGateway, TimedStrategy>;
  auto bank = [&] { return std::vector<TimedStrategy>{
    TimedStrategy(1, 1),
    TimedStrategy(2, 1)}; };
  TimedSite site(table, cfg, TimedSite::Mode::Live, bank());
  TimedGateway gateway;
  site.set_gateway(&gateway);
  site.set_latency_clock(&test_perf_clock);
  PhaseChangeEvt phase{};
  phase.venue = Venue::SHFE;
  phase.phase = Phase::Continuous;
  phase.ts = at_time(9, 0);
  timing_now = phase.ts;
  site.source(phase, phase.ts);
  auto md = snap(0, 1, phase.ts + 100000, 78000, 78010);
  timing_now = md.ts + 500; // real input queue delay
  site.source(md, md.ts, md.ts);
  const auto& hist = site.tel().histos();
  CHECK(hist.at("md.queue_ns").count() == 1);
  CHECK(hist.at("md.queue_ns").max() == 510);
  CHECK(hist.at("book.update_ns").count() == 1);
  CHECK(hist.at("order.prepare_ns").count() == 2);
  CHECK(hist.at("gateway.place_ns").count() == gateway.placed.size());
  CHECK(hist.at("gateway.place_ns").max() == 1010);
  CHECK(hist.at("engine.event_ns").count() == 2);
  CHECK(hist.at("risk.new_order_ns").count() == 2);
  CHECK(hist.at("risk.new_order_ns").max() == 10);
  CHECK(hist.at("strategy.1.calc_ns").max() >= 200);
  CHECK(hist.at("strategy.1.callback_ns").max() > hist.at("strategy.1.calc_ns").max() + 1000);
  CHECK(hist.at("strategy.2.calc_ns").count() >= 2);
  CHECK(!gateway.placed.empty());
  CHECK(hist.at("tick_to_trade_ns").max() >= 1500);
  if (!gateway.placed.empty()) {
    const auto request = gateway.placed.front();
    ExecReport ack{};
    ack.cl_id = request.cl_id;
    ack.strategy = request.strategy;
    ack.inst = request.inst;
    ack.type = ExecType::Ack;
    ack.ts = timing_now + 10000;
    ack.trigger_ts = request.trigger_ts;
    ack.leaves = request.qty;
    const auto other_count = hist.at("strategy.2.callback_ns").count();
    timing_now = ack.ts + 300;
    site.source(ack, ack.ts, ack.ts);
    CHECK(hist.at("response.queue_ns").max() == 310);
    CHECK(hist.at("response.update_ns").count() == 1);
    CHECK(hist.at("order_roundtrip_ns").count() == 1);
    CHECK(hist.at("tick_to_ack_ns").max() > hist.at("order_roundtrip_ns").max());
    CHECK(hist.at("strategy.2.callback_ns").count() == other_count);
    site.source(ack, ack.ts); // duplicate ack does not double-count RTT
    CHECK(hist.at("order_roundtrip_ns").count() == 1);
    site.cancel(request.cl_id);
    CHECK(hist.at("risk.cancel_ns").count() == 1);
    CHECK(hist.at("risk.cancel_ns").max() == 10);
    CHECK(hist.at("cancel.prepare_ns").count() == 1);
    CHECK(hist.at("gateway.cancel_ns").count() == gateway.cancels.size());
  }
  // Measurement clock cannot affect stream or state, nor produce replay samples.
  TimedSite replay(table, cfg, TimedSite::Mode::Replay, bank());
  replay.set_latency_clock(&test_perf_clock);
  replay.source(phase, phase.ts);
  replay.source(md, md.ts);
  CHECK(replay.tel().histos().at("risk.new_order_ns").count() == 0);
  CHECK(replay.tel().histos().at("strategy.1.calc_ns").count() == 0);
  TimedSite recovery(table, cfg, TimedSite::Mode::Live, bank());
  recovery.set_recovering(true);
  recovery.source(phase, phase.ts);
  recovery.source(md, md.ts);
  CHECK(recovery.state_hash() == replay.state_hash());
  CHECK(recovery.tel().histos().at("strategy.1.calc_ns").count() == 0);
  for (const auto& [name, h] : replay.tel().histos()) CHECK(h.count() == 0);
  for (const auto& [name, h] : recovery.tel().histos()) CHECK(h.count() == 0);
  const auto report = latency_report(site.tel());
  CHECK(report.find("Strategies") != std::string::npos);
  CHECK(report.find("journal.flush_ns") != std::string::npos);
  CHECK(report.find("no samples") != std::string::npos);
}

void test_latency_boundaries() {
  // Multiple and nested context actions pause calculation exactly once.
  StrategyCalcTimer timer;
  CHECK(!timer.pause(5)); // action outside a callback
  timer.begin(100);
  const bool outer = timer.pause(120);
  const bool inner = timer.pause(130);
  CHECK(outer && !inner);
  timer.resume(200, inner);
  timer.resume(250, outer);
  const bool second = timer.pause(280);
  timer.resume(400, second);
  CHECK(timer.finish(450) == 100); // 20 + 30 + 50; engine work excluded
  CHECK(!timer.pause(500));

  // Overflow and terminal responses cannot attribute an ACK to another order.
  Telemetry tel;
  LatencyMetrics metrics;
  metrics.wire(tel, true);
  metrics.set_session_clock(&test_perf_clock);
  metrics.order_sent(1, 100);
  metrics.order_sent(65537, 200);
  CHECK(tel.get("latency.correlation_overwrite") == 1);
  metrics.acknowledgment(1, 50, 500);
  CHECK(tel.histos().at("order_roundtrip_ns").count() == 0);
  metrics.acknowledgment(65537, 150, 700);
  CHECK(tel.histos().at("order_roundtrip_ns").max() == 500);
  metrics.acknowledgment(65537, 150, 800);
  CHECK(tel.histos().at("order_roundtrip_ns").count() == 1);
  metrics.order_sent(2, 900);
  metrics.terminal(2);
  metrics.acknowledgment(2, 850, 1000);
  CHECK(tel.histos().at("order_roundtrip_ns").count() == 1);

  // Transport timestamps are paired atomically with the correct payload,
  // including concurrent publication. They never alter canonical bytes.
  Ring ring(8192);
  Ring::Writer writer(ring);
  Ring::Reader reader(ring, true, Ring::LossPolicy::Intolerant);
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < 4096; ++i) {
      const auto md = snap(0, i, 100, 78000, 78010);
      writer.publish(make_envelope(i, 100, md, Origin::Source, 1), 1000 + i);
    }
  });
  for (std::uint64_t i = 0; i < 4096;) {
    Envelope out{};
    Nanos received = 0;
    const auto result = reader.poll(out, &received);
    if (result.kind == Ring::PollKind::Empty) { std::this_thread::yield(); continue; }
    CHECK(result.kind == Ring::PollKind::Msg);
    if (result.kind != Ring::PollKind::Msg) break;
    CHECK(received == static_cast<Nanos>(1000 + i));
    CHECK(out.as<MdSnapshotEvt>().feed_seq == i);
    const auto expected = make_envelope(i, 100, out.as<MdSnapshotEvt>(), Origin::Source, 1);
    CHECK(std::memcmp(&out, &expected, sizeof(out)) == 0);
    ++i;
  }
  producer.join();
  // Untimed publishers clear metadata, including a reused slot.
  Ring small(1);
  Ring::Writer sw(small);
  Ring::Reader sr(small);
  const auto md = snap(0, 0, 100, 78000, 78010);
  const auto env = make_envelope(0, 100, md, Origin::Source, 1);
  Envelope out{};
  Nanos received = 0;
  sw.publish(env, 999);
  CHECK(sr.poll(out, &received).kind == Ring::PollKind::Msg && received == 999);
  sw.publish(env);
  CHECK(sr.poll(out, &received).kind == Ring::PollKind::Msg && received == 0);

  const char* path = "plat_latency_recovery_test.jnl";
  AsyncJournal journal;
  CHECK(journal.open(path, 20260713, 1));
  journal.set_latency_enabled(false);
  journal.append(env);
  journal.finish();
  const auto snapshot = journal.timing_snapshot();
  CHECK(snapshot.enqueue.count() == 0 && snapshot.queue.count() == 0);
  CHECK(snapshot.write_batch.count() == 0 && snapshot.flush.count() == 0);
  JournalReader persisted;
  CHECK(persisted.open(path) && persisted.verified());
  CHECK(persisted.header().count == 1);
  std::remove(path);
}
