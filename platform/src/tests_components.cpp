// tests_components.cpp — component suite: CTP conversions, config, books,
// OMS, the riskgate battery, PnL, journal, ring (incl. broadcast), and the
// instrument table. No site engine here — see tests_engine.cpp.

#include <cfloat>
#include <cstring>
#include <memory>
#include <string>

#include "plat/bus/journal.hpp"
#include "plat/bus/ring.hpp"
#include "plat/core/config.hpp"
#include "plat/core/phase.hpp"
#include "plat/ctp/conv.hpp"
#include "plat/md/fut_book.hpp"
#include "plat/oms/oms.hpp"
#include "plat/pnl/pnl.hpp"
#include "plat/ref/fees.hpp"
#include "plat/ref/positions_file.hpp"
#include "plat/risk/riskgate.hpp"
#include "tests_util.hpp"

// ------------------------------------------------------------- ctp::conv

void test_conv() {
  using namespace plat::ctp;
  CHECK(px_from_ctp(78'010.0) == px_from_cny(78'010.0));
  CHECK(px_from_ctp(4.123) == 41'230);
  CHECK(px_from_ctp(DBL_MAX) == 0);
  CHECK(px_from_ctp(0.0) == 0);
  CHECK(px_to_ctp(px_from_cny(78'010.0)) == 78'010.0);
  CHECK(margin_bps_from_ratio(0.12) == 1'200);
  CHECK(margin_bps_from_ratio(0.0) == 0);
  CHECK(margin_bps_from_ratio(-1.0) == 0);

  CHECK(ns_from_ctp_time("09:30:15", 500) == at_time(9, 30, 15, 500 * kNsPerMs));
  CHECK(ns_from_ctp_time("21:00:00", 0) == at_time(21, 0));
  CHECK(ns_from_ctp_time("bad", 0) == -1);
  CHECK(ns_from_ctp_time(nullptr, 0) == -1);

  CHECK(side_from_ctp(side_to_ctp(Side::Buy)) == Side::Buy);
  CHECK(side_from_ctp(side_to_ctp(Side::Sell)) == Side::Sell);
  CHECK(offset_from_ctp(offset_to_ctp(Offset::Open)) == Offset::Open);
  CHECK(offset_from_ctp(offset_to_ctp(Offset::CloseToday)) == Offset::CloseToday);
  CHECK(offset_from_ctp(offset_to_ctp(Offset::CloseYesterday)) == Offset::CloseYesterday);
  CHECK(offset_from_ctp(THOST_FTDC_OF_Close) == Offset::CloseYesterday);

  CHECK(venue_from_exchange_id("SHFE") == Venue::SHFE);
  CHECK(venue_from_exchange_id("CFFEX") == Venue::CFFEX);
  CHECK(venue_from_exchange_id("GFEX") == Venue::GFEX);

  char ref[13];
  order_ref_from_seq(123'456'789, ref);
  CHECK(seq_from_order_ref(ref) == 123'456'789);
  CHECK(seq_from_order_ref("00042") == 42);
  CHECK(seq_from_order_ref("manual1") == 0);  // not ours
  CHECK(seq_from_order_ref(nullptr) == 0);

  CHECK(venue_no_from_sys_id("       12345") == 12'345);
  CHECK(venue_no_from_sys_id("") == 0);
  CHECK(day_from_ctp("20260713") == 20'260'713u);
  CHECK(day_from_ctp("garbage!") == 0u);
}

// ---------------------------------------------------------------- config

void test_config() {
  const char* path = "plat_test.cfg";
  {
    std::FILE* f = std::fopen(path, "wb");
    const char* text =
        "# comment line\n"
        "td_front = tcp://1.2.3.4:10130\n"
        "lots=3\n"
        "ratio = 0.5   # trailing comment\n"
        "spin = true\n"
        "instruments = cu2609, al2609 ,zn2609\n";
    std::fwrite(text, 1, std::strlen(text), f);
    std::fclose(f);
  }
  Config c;
  CHECK(c.load(path));
  CHECK(c.str("td_front") == "tcp://1.2.3.4:10130");
  CHECK(c.i64("lots") == 3);
  CHECK(c.f64("ratio") == 0.5);
  CHECK(c.flag("spin"));
  CHECK(!c.flag("absent"));
  CHECK(c.str("absent", "dflt") == "dflt");
  // A present-but-empty value must fall back to the default, not shadow it
  // as "" — the recorder's `outfile =` / platd's `investor_id =` rely on this.
  {
    const char* ep = "plat_empty_test.cfg";
    std::FILE* f = std::fopen(ep, "wb");
    const char* t = "outfile =\nname = real\n";
    std::fwrite(t, 1, std::strlen(t), f);
    std::fclose(f);
    Config e;
    CHECK(e.load(ep));
    CHECK(e.str("outfile", "default.mdr") == "default.mdr");  // empty -> default
    CHECK(e.str("name", "x") == "real");                       // present -> value
    std::remove(ep);
  }
  const auto v = c.list("instruments");
  CHECK(v.size() == 3 && v[0] == "cu2609" && v[1] == "al2609" && v[2] == "zn2609");
  std::remove(path);

  // Fleet shard view: shardN.* overrides shared keys; derived defaults fill
  // only what the section left unset.
  const char* fpath = "plat_fleet_test.cfg";
  {
    std::FILE* f = std::fopen(fpath, "wb");
    const char* text =
        "td_front = tcp://1.2.3.4:10130\n"
        "risk.max_actions_per_sec = 80\n"
        "shard2.instruments = zn2609\n"
        "shard2.engine_core = 4\n"
        "shard2.risk.max_actions_per_sec = 40\n";
    std::fwrite(text, 1, std::strlen(text), f);
    std::fclose(f);
  }
  Config fleet;
  CHECK(fleet.load(fpath));
  Config s2 = fleet.overlay("shard2");
  CHECK(s2.str("td_front") == "tcp://1.2.3.4:10130");        // shared key inherited
  CHECK(s2.str("instruments") == "zn2609");                  // section key promoted
  CHECK(s2.i64("engine_core") == 4);
  CHECK(s2.i64("risk.max_actions_per_sec") == 40);           // override beats shared
  s2.set_default("journal", "shard2.jnl");
  s2.set_default("td_front", "must-not-override");
  CHECK(s2.str("journal") == "shard2.jnl");                  // default fills a hole
  CHECK(s2.str("td_front") == "tcp://1.2.3.4:10130");        // default never overrides
  std::remove(fpath);
}

// -------------------------------------------------------------- fut book

void test_fut_book() {
  FutBook b;
  CHECK(!b.valid());
  CHECK(b.on_snapshot(snap(0, 1, at_time(9, 0), 78'000, 78'010, 10, 20, 100)));
  CHECK(b.valid() && b.bid() == px_from_cny(78'000.0) && b.ask() == px_from_cny(78'010.0));
  CHECK(!b.on_snapshot(snap(0, 1, at_time(9, 0, 1), 78'020, 78'030)));  // stale seq
  CHECK(b.on_snapshot(snap(0, 2, at_time(9, 0, 1), 78'010, 78'020, 30, 10, 160)));
  const auto f = b.features();
  CHECK(f.d_mid == px_from_cny(10.0));
  CHECK(f.traded == 60);
  CHECK(f.imbalance_e4 == (30 - 10) * 10'000 / 40);
}

// ------------------------------------------------------------------- oms

void test_oms() {
  Oms oms;
  NewOrderReq r{};
  r.cl_id = make_order_id(1, 7);
  r.ts = at_time(9, 30);
  r.px = px_from_cny(78'000.0);
  r.qty = 4;
  r.inst = 0;
  r.strategy = 1;
  r.account = 1;
  r.side = Side::Buy;
  oms.on_new(r);
  CHECK(oms.find(r.cl_id) && oms.find(r.cl_id)->state == OrderState::PendingNew);
  CHECK(oms.open_qty(0, Side::Buy) == 4);

  ExecReport ack{};
  ack.cl_id = r.cl_id;
  ack.venue_no = 555;
  ack.ts = r.ts + kNsPerMs;
  ack.type = ExecType::Ack;
  ack.inst = 0;
  oms.on_exec(ack);
  CHECK(oms.find(r.cl_id)->state == OrderState::Live);
  CHECK(oms.find(r.cl_id)->venue_no == 555);

  ExecReport fill = ack;
  fill.type = ExecType::Fill;
  fill.qty = 3;
  fill.cum = 3;
  fill.leaves = 1;
  oms.on_exec(fill);
  CHECK(oms.find(r.cl_id)->open() && oms.find(r.cl_id)->leaves == 1);

  // Reconcile: venue agrees on our order, plus one foreign open order.
  std::vector<VenueOpenOrder> venue{{r.cl_id, 555, 1}, {kNoOrder, 777, 2}};
  auto breaks = oms.reconcile(venue);
  CHECK(breaks.size() == 1 && breaks[0].venue_no == 777);

  ExecReport cxl = ack;
  cxl.type = ExecType::Canceled;
  oms.on_exec(cxl);
  CHECK(!oms.find(r.cl_id)->open());
  CHECK(oms.open_qty(0, Side::Buy) == 0);
}

// -------------------------------------------------------------- riskgate

struct RiskRig {
  InstrumentTable table = one_future();
  Oms oms;
  FeeSchedule fees{};
  PnlLedger pnl{table, fees};
  RiskLimits limits{};
  std::unique_ptr<RiskGate> gate;
  FutBook book;

  explicit RiskRig(RiskLimits l = {}) : limits(l) {
    gate = std::make_unique<RiskGate>(table, oms, pnl, limits);
    gate->attach_fut_book(0, &book);
    gate->set_phase(Venue::SHFE, Phase::Continuous);
    book.on_snapshot(snap(0, 1, at_time(9, 30), 78'000, 78'010));
  }

  NewOrderReq order(Price px, Qty qty, Side side = Side::Buy,
                    Offset offset = Offset::Open) const {
    NewOrderReq r{};
    static std::uint64_t seq = 100;
    r.cl_id = make_order_id(1, ++seq);
    r.ts = at_time(9, 30, 1);
    r.px = px;
    r.qty = qty;
    r.inst = 0;
    r.strategy = 1;
    r.account = 1;
    r.side = side;
    r.offset = offset;
    r.tif = Tif::Ioc;
    return r;
  }
};

void test_riskgate() {
  {
    RiskRig rig;
    CHECK(rig.gate->check_new(rig.order(px_from_cny(78'010.0), 1), at_time(9, 30, 1)) ==
          kRiskPass);
  }
  {  // kill switch overrides everything
    RiskRig rig;
    KillSwitchCmd k{};
    k.cmd_seq = 1;
    k.scope = KillScope::Site;
    k.action = KillAction::Halt;
    rig.gate->kill().apply(k);
    CHECK(rig.gate->check_new(rig.order(px_from_cny(78'010.0), 1), at_time(9, 30, 1)) ==
          RiskCheck::KillSwitch);
    // versioned disarm re-opens; stale command does not
    KillSwitchCmd d = k;
    d.cmd_seq = 2;
    d.action = KillAction::Disarm;
    rig.gate->kill().apply(d);
    KillSwitchCmd stale = k;  // cmd_seq 1: must lose to the newer disarm
    rig.gate->kill().apply(stale);
    CHECK(rig.gate->check_new(rig.order(px_from_cny(78'010.0), 1), at_time(9, 30, 2)) ==
          kRiskPass);
  }
  {  // phase gate
    RiskRig rig;
    rig.gate->set_phase(Venue::SHFE, Phase::Break);
    CHECK(rig.gate->check_new(rig.order(px_from_cny(78'010.0), 1), at_time(10, 20)) ==
          RiskCheck::PhaseGate);
  }
  {  // stale book interlock
    RiskRig rig;
    CHECK(rig.gate->check_new(rig.order(px_from_cny(78'010.0), 1), at_time(9, 30, 30)) ==
          RiskCheck::StaleData);
  }
  {  // price collar: outside limit band, and fat-finger band
    RiskRig rig;
    CHECK(rig.gate->check_new(rig.order(px_from_cny(85'000.0), 1), at_time(9, 30, 1)) ==
          RiskCheck::PriceCollar);
    CHECK(rig.gate->check_new(rig.order(px_from_cny(79'000.0), 1), at_time(9, 30, 1)) ==
          RiskCheck::PriceCollar);  // > 50 ticks from last
  }
  {  // size cap
    RiskRig rig;
    CHECK(rig.gate->check_new(rig.order(px_from_cny(78'010.0), 50'000), at_time(9, 30, 1)) ==
          RiskCheck::SizeNotional);
  }
  {  // margin cap: 10% of 78000×5 ≈ 39000/lot → 200 lots ≈ 7.8M > 5M default
    RiskLimits l;
    l.max_order_qty = 1'000;
    l.max_order_notional_e4 = px_from_cny(500'000'000.0);
    l.max_net_notional_e4 = px_from_cny(500'000'000.0);
    RiskRig rig(l);
    CHECK(rig.gate->check_new(rig.order(px_from_cny(78'010.0), 200), at_time(9, 30, 1)) ==
          RiskCheck::Margin);
  }
  {  // close-today must fit the bucket
    RiskRig rig;
    CHECK(rig.gate->check_new(
              rig.order(px_from_cny(78'000.0), 1, Side::Sell, Offset::CloseToday),
              at_time(9, 30, 1)) == RiskCheck::SellableInventory);
  }
  {  // rate limit
    RiskLimits l;
    l.max_actions_per_sec = 3;
    RiskRig rig(l);
    const Nanos t = at_time(9, 30, 1);
    for (int i = 0; i < 3; ++i)
      CHECK(rig.gate->check_new(rig.order(px_from_cny(78'010.0) + (i + 1) * rig.table.at(0).tick, 1),
                                t + i) == kRiskPass);
    CHECK(rig.gate->check_new(rig.order(px_from_cny(78'000.0), 2), t + 3) ==
          RiskCheck::RateLimit);
  }
  {  // duplicate burst breaker
    RiskRig rig;
    const auto r1 = rig.order(px_from_cny(78'010.0), 1);
    CHECK(rig.gate->check_new(r1, at_time(9, 30, 1)) == kRiskPass);
    auto r2 = rig.order(px_from_cny(78'010.0), 1);
    CHECK(rig.gate->check_new(r2, at_time(9, 30, 1, 5 * kNsPerMs)) ==
          RiskCheck::DuplicateBurst);
  }
}

// ------------------------------------------------------------------- pnl

void test_pnl() {
  InstrumentTable t = one_future();
  FeeSchedule fees{};
  PnlLedger pnl(t, fees);

  ExecReport open{};
  open.cl_id = make_order_id(1, 1);
  open.ts = at_time(9, 30);
  open.px = px_from_cny(78'000.0);
  open.qty = 2;
  open.inst = 0;
  open.strategy = 1;
  open.account = 1;
  open.type = ExecType::Fill;
  open.side = Side::Buy;
  open.offset = Offset::Open;
  pnl.on_exec(open);
  const Position* p = pnl.find(1, 1, 0);
  CHECK(p && p->long_td == 2 && p->long_yd == 0);
  CHECK(p->fees_e4 == 2 * fees.fut_per_lot_e4);

  // close-today 1 lot, +10 CNY move: realized = 10 × 1 × 5 = 50 CNY − fees.
  ExecReport close = open;
  close.cl_id = make_order_id(1, 2);
  close.px = px_from_cny(78'010.0);
  close.qty = 1;
  close.side = Side::Sell;
  close.offset = Offset::CloseToday;
  pnl.on_exec(close);
  CHECK(p->long_td == 1);
  const std::int64_t expect_fees = 2 * fees.fut_per_lot_e4 + fees.fut_per_lot_close_today_e4;
  CHECK(p->fees_e4 == expect_fees);
  CHECK(p->realized_e4 == px_from_cny(50.0) - expect_fees);

  pnl.mark(0, px_from_cny(78'020.0));
  CHECK(pnl.total_unrealized_e4(1) == (px_from_cny(78'020.0) - px_from_cny(78'000.0)) * 1 * 5);
  CHECK(pnl.margin_used_e4(1) == margin_e4(t.at(0), px_from_cny(78'020.0), 1));

  // reconcile against venue totals
  CHECK(pnl.reconcile_fills(2, 1).ok);
  CHECK(!pnl.reconcile_fills(2, 2).ok);
}

// --------------------------------------------------------------- journal

void test_journal() {
  const char* path = "plat_test.jnl";
  std::uint64_t live_hash = 0;
  {
    JournalWriter w;
    CHECK(w.open(path, 20'260'713, 1));
    for (int i = 0; i < 100; ++i) {
      const auto s = snap(0, static_cast<std::uint64_t>(i + 1), at_time(9, 0, i), 78'000,
                          78'010);
      w.append(make_envelope(static_cast<std::uint64_t>(i), s.ts, s, Origin::Source, 1));
    }
    CHECK(w.seal());
    live_hash = w.stream_hash();
  }
  {
    JournalReader r;
    CHECK(r.open(path));
    CHECK(r.verified());
    CHECK(r.header().count == 100);
    CHECK(r.header().day == 20'260'713);
    CHECK(r.header().stream_hash == live_hash);
    CHECK(r.envelopes().size() == 100);
    CHECK(r.envelopes()[42].is<MdSnapshotEvt>());
  }
  std::remove(path);
}

// ------------------------------------------------------------------ ring

void test_ring() {
  Ring ring(8);
  Ring::Writer w(ring);
  Ring::Reader tolerant(ring, true, Ring::LossPolicy::Tolerant);
  Ring::Reader intolerant(ring, true, Ring::LossPolicy::Intolerant);

  for (int i = 0; i < 5; ++i) {
    const auto s = snap(0, static_cast<std::uint64_t>(i + 1), at_time(9, 0, i), 78'000, 78'010);
    w.publish(make_envelope(0, s.ts, s, Origin::Source, 0));
  }
  Envelope e;
  for (int i = 0; i < 5; ++i) {
    CHECK(tolerant.poll(e).kind == Ring::PollKind::Msg);
    CHECK(e.as<MdSnapshotEvt>().feed_seq == static_cast<std::uint64_t>(i + 1));
  }
  CHECK(tolerant.poll(e).kind == Ring::PollKind::Empty);

  // Lap both readers (capacity 8, publish 20 more).
  for (int i = 5; i < 25; ++i) {
    const auto s = snap(0, static_cast<std::uint64_t>(i + 1), at_time(9, 0, i), 78'000, 78'010);
    w.publish(make_envelope(0, s.ts, s, Origin::Source, 0));
  }
  const auto pt = tolerant.poll(e);
  CHECK(pt.kind == Ring::PollKind::Gap && pt.gap > 0);        // resyncs to live edge
  CHECK(tolerant.poll(e).kind == Ring::PollKind::Empty);      // at the edge, drained
  {
    const auto s = snap(0, 26, at_time(9, 0, 26), 78'000, 78'010);
    w.publish(make_envelope(0, s.ts, s, Origin::Source, 0));
  }
  CHECK(tolerant.poll(e).kind == Ring::PollKind::Msg);        // flows again after resync
  const auto pi = intolerant.poll(e);
  CHECK(pi.kind == Ring::PollKind::Fault);                    // sticky fail-closed
  CHECK(intolerant.poll(e).kind == Ring::PollKind::Fault);
}

// The ring is the seqbus broadcast protocol — single writer, ANY number of
// independent readers, each with its own cursor; the writer is wait-free and
// reader-oblivious. This is why one md feed can serve the trading engine,
// capture, and a standby simultaneously: fan-out at the transport is free,
// while strategy fan-out stays behind the engine's single sequencing point.
void test_ring_broadcast() {
  Ring ring(64);
  Ring::Writer w(ring);
  Ring::Reader engine(ring, true, Ring::LossPolicy::Tolerant);
  Ring::Reader capture(ring, true, Ring::LossPolicy::Tolerant);

  for (int i = 0; i < 10; ++i) {
    const auto s = snap(0, static_cast<std::uint64_t>(i + 1), at_time(9, 0, i), 78'000, 78'010);
    w.publish(make_envelope(0, s.ts, s, Origin::Source, 0));
  }

  // A late reader attaching at the live edge sees only what comes after —
  // exactly how a mid-session capture process would join.
  Ring::Reader late(ring, /*from_origin=*/false, Ring::LossPolicy::Tolerant);

  Envelope e;
  // Both from-origin readers independently receive ALL ten, in order.
  for (int i = 0; i < 10; ++i) {
    CHECK(engine.poll(e).kind == Ring::PollKind::Msg);
    CHECK(e.as<MdSnapshotEvt>().feed_seq == static_cast<std::uint64_t>(i + 1));
  }
  for (int i = 0; i < 10; ++i) {
    CHECK(capture.poll(e).kind == Ring::PollKind::Msg);
    CHECK(e.as<MdSnapshotEvt>().feed_seq == static_cast<std::uint64_t>(i + 1));
  }
  CHECK(late.poll(e).kind == Ring::PollKind::Empty);

  const auto s = snap(0, 11, at_time(9, 0, 11), 78'000, 78'010);
  w.publish(make_envelope(0, s.ts, s, Origin::Source, 0));
  CHECK(engine.poll(e).kind == Ring::PollKind::Msg);   // every reader gets it,
  CHECK(capture.poll(e).kind == Ring::PollKind::Msg);  // nobody consumes it away
  CHECK(late.poll(e).kind == Ring::PollKind::Msg);
  CHECK(e.as<MdSnapshotEvt>().feed_seq == 11);
}

// ---------------------------------------------- EOD position attribution

void test_positions_file() {
  const char* path = "plat_test.positions";
  std::vector<PositionsRow> rows(2);
  rows[0].strategy = 2;
  std::snprintf(rows[0].symbol, sizeof(rows[0].symbol), "cu2609");
  rows[0].long_lots = 3;
  rows[0].long_cost_e4 = px_from_cny(78'010.0);
  rows[1].strategy = 3;
  std::snprintf(rows[1].symbol, sizeof(rows[1].symbol), "rb2610");
  rows[1].short_lots = 2;
  rows[1].short_cost_e4 = px_from_cny(3'069.0);
  CHECK(save_positions(path, 20'260'714, rows));

  std::uint32_t day = 0;
  std::vector<PositionsRow> back;
  CHECK(load_positions(path, day, back));
  CHECK(day == 20'260'714);
  CHECK(back.size() == 2);
  CHECK(back[0].strategy == 2 && std::strcmp(back[0].symbol, "cu2609") == 0);
  CHECK(back[0].long_lots == 3 && back[0].long_cost_e4 == px_from_cny(78'010.0));
  CHECK(back[1].strategy == 3 && back[1].short_lots == 2);
  CHECK(back[1].short_cost_e4 == px_from_cny(3'069.0));
  std::remove(path);
}

// ------------------------------------------------------- instrument table

void test_instrument_table() {
  InstrumentTable t = one_future();
  CHECK(t.find("cu2609") == 0);
  CHECK(t.find("missing") == kNoInstrument);
  // venue-supplied band overrides the board approximation
  t.set_limit_band(0, px_from_cny(84'240.0), px_from_cny(71'760.0));
  CHECK(t.at(0).upper_limit == px_from_cny(84'240.0));
  std::vector<std::uint8_t> blob;
  t.save(blob);
  InstrumentTable t2;
  CHECK(t2.load(blob.data(), blob.size()));
  CHECK(t2.size() == 1 && t2.at(0).upper_limit == px_from_cny(84'240.0));
  CHECK(t2.state_hash() == t.state_hash());
}

// ---------------------------------------------- site loop w/ fake gateway
