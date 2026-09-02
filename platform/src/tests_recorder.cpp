// tests_recorder.cpp — market-data recorder suite: the delta/zigzag tick
// codec, the .mdr file (seal + checksum + crash-tail recovery), and the
// cross-datacenter compare/merge (UDP reorder repair, gap fill, golden).

#include <cstdio>
#include <cstring>
#include <vector>

#include "plat/core/time.hpp"
#include "plat/engine/site.hpp"
#include "plat/rec/backtest_gateway.hpp"
#include "plat/rec/tick_file.hpp"
#include "plat/rec/tick_merge.hpp"
#include "plat/strat/momentum.hpp"
#include "tests_util.hpp"

// ---------------------------------------------- market-data recorder codec

namespace {
using namespace plat::rec;

MdRecord mk_rec(std::uint32_t inst, Nanos exch_ts, Price last, Qty vol, Price bid0, Qty bq0,
                Price ask0, Qty aq0) {
  MdRecord r{};
  r.inst = inst;
  r.recv_ts = exch_ts + 3'000;  // some local latency
  r.exch_ts = exch_ts;
  r.last_px = last;
  r.day_volume = vol;
  r.turnover_e1 = vol * last / 10;
  r.open_interest = 100'000;
  for (int i = 0; i < kMaxLevels; ++i) {
    r.bid_px[i] = bid0 - i * px_from_cny(1.0);
    r.bid_qty[i] = bq0 + i;
    r.ask_px[i] = ask0 + i * px_from_cny(1.0);
    r.ask_qty[i] = aq0 + i;
  }
  return r;
}
}  // namespace

void test_tick_codec() {
  using namespace plat::rec;
  // Varint + zigzag round trips over the tricky values.
  for (std::int64_t v : {0LL, 1LL, -1LL, 127LL, -128LL, 300LL, -300LL, 1'000'000LL,
                         -1'000'000'000'000LL}) {
    std::uint8_t buf[10];
    const std::size_t n = put_varint(buf, zigzag(v));
    std::uint64_t got = 0;
    CHECK(get_varint(buf, n, got) == n);
    CHECK(unzigzag(got) == v);
  }
  // Delta encode/decode against a previous record.
  const MdRecord prev = mk_rec(0, at_time(9, 0, 0), px_from_cny(78'000.0), 1'000,
                               px_from_cny(78'000.0), 5, px_from_cny(78'010.0), 7);
  const MdRecord cur = mk_rec(0, at_time(9, 0, 1), px_from_cny(78'010.0), 1'003,
                              px_from_cny(78'010.0), 6, px_from_cny(78'020.0), 4);
  std::uint8_t buf[kMaxEncoded];
  const std::size_t n = encode_md(prev, cur, 5, buf);
  CHECK(n < 96);  // a small move stays tiny
  MdRecord out{};
  CHECK(decode_md(prev, buf, n, 5, out) == n);
  CHECK(same_market_content(out, cur) && out.recv_ts == cur.recv_ts && out.inst == cur.inst);
  // An unchanged tick is nearly free (inst + 26 zero-deltas).
  std::uint8_t buf2[kMaxEncoded];
  CHECK(encode_md(cur, cur, 5, buf2) <= 28);
}

void test_tick_file() {
  using namespace plat::rec;
  const char* path = "plat_test.mdr";
  std::vector<MdrInstrument> dir(2);
  std::snprintf(dir[0].symbol, sizeof(dir[0].symbol), "cu2609");
  std::snprintf(dir[0].exchange, sizeof(dir[0].exchange), "SHFE");
  std::snprintf(dir[1].symbol, sizeof(dir[1].symbol), "rb2610");
  std::snprintf(dir[1].exchange, sizeof(dir[1].exchange), "SHFE");

  std::vector<MdRecord> recs;
  for (int i = 0; i < 100; ++i) {
    recs.push_back(mk_rec(i % 2, at_time(9, 0, 0) + i * kNsPerMs,
                          px_from_cny(78'000.0) + i * px_from_cny(1.0), 1'000 + i,
                          px_from_cny(78'000.0), 5, px_from_cny(78'010.0), 7));
  }
  {
    TickWriter w;
    CHECK(w.open(path, 20'260'715, "dc1", 5, dir));
    for (const auto& r : recs) w.append(r);
    w.close();  // seals
  }
  TickReader r;
  CHECK(r.open(path));
  CHECK(r.header().day == 20'260'715);
  CHECK(std::strcmp(r.header().source, "dc1") == 0);
  MdRecord out;
  std::size_t k = 0;
  while (r.next(out)) {
    CHECK(k < recs.size() && same_market_content(out, recs[k]));
    ++k;
  }
  CHECK(k == recs.size());
  CHECK(r.sealed() && r.verified());  // checksum + count reproduced
  std::remove(path);
}

void test_tick_depth_l1_l2() {
  using namespace plat::rec;
  std::vector<MdrInstrument> dir(1);
  std::snprintf(dir[0].symbol, sizeof(dir[0].symbol), "cu2609");
  std::snprintf(dir[0].exchange, sizeof(dir[0].exchange), "SHFE");

  // The SAME market ticks recorded as L1 (level 0 only) and as L2 (5 levels).
  std::vector<MdRecord> recs;
  for (int i = 0; i < 200; ++i)
    recs.push_back(mk_rec(0, at_time(9, 0, 0) + i * kNsPerMs,
                          px_from_cny(78'000.0) + i * px_from_cny(1.0), 1'000 + i,
                          px_from_cny(78'000.0), 5, px_from_cny(78'010.0), 7));

  auto write = [&](const char* path, int depth) -> std::uint64_t {
    TickWriter w;
    CHECK(w.open(path, 20'260'715, "dc1", depth, dir));
    for (const auto& r : recs) w.append(r);
    const std::uint64_t bytes = w.bytes();
    w.close();
    return bytes;
  };
  const std::uint64_t l1_bytes = write("plat_l1.mdr", 1);
  const std::uint64_t l2_bytes = write("plat_l2.mdr", 5);

  // L1 must be materially smaller — it drops the four deep levels entirely,
  // not merely delta-zeroes them.
  CHECK(l1_bytes < l2_bytes);

  // The header advertises the depth, and an L1 decode reconstructs level 0
  // exactly while leaving the deep levels zero (never recorded).
  TickReader r1;
  CHECK(r1.open("plat_l1.mdr"));
  CHECK(r1.depth() == 1 && r1.header().depth == 1);
  MdRecord out;
  std::size_t k = 0;
  bool deep_all_zero = true;
  while (r1.next(out)) {
    CHECK(out.bid_px[0] == recs[k].bid_px[0] && out.ask_qty[0] == recs[k].ask_qty[0]);
    for (int lvl = 1; lvl < kMaxLevels; ++lvl)
      if (out.bid_px[lvl] || out.ask_px[lvl]) deep_all_zero = false;
    ++k;
  }
  CHECK(k == recs.size() && deep_all_zero && r1.verified());

  // L2 reconstructs all five levels.
  TickReader r2;
  CHECK(r2.open("plat_l2.mdr"));
  CHECK(r2.depth() == 5);
  k = 0;
  bool deep_present = false;
  while (r2.next(out)) {
    if (out.bid_px[4] == recs[k].bid_px[4] && out.bid_px[4] != 0) deep_present = true;
    ++k;
  }
  CHECK(k == recs.size() && deep_present && r2.verified());

  std::remove("plat_l1.mdr");
  std::remove("plat_l2.mdr");
}

// Full backtest path: TickWriter (verified) → TickReader → the SAME Site +
// SnapMomentum + BacktestGateway platbt uses → fills + PnL. Proves recorded
// data replays through the production strategy and the writer/reader checksum
// holds on a large, swinging record set.
void test_backtest_replay() {
  using namespace plat;
  using namespace plat::rec;
  const char* path = "plat_bt_test.mdr";

  std::vector<MdrInstrument> dir(1);
  std::snprintf(dir[0].symbol, sizeof(dir[0].symbol), "cu2609");
  std::snprintf(dir[0].exchange, sizeof(dir[0].exchange), "SHFE");
  dir[0].price_tick = px_from_cny(10.0);
  dir[0].multiplier = 5;
  dir[0].margin_bps = 1'000;

  {
    TickWriter w;
    CHECK(w.open(path, 20'260'716, "dc1", 5, dir));
    Price last = px_from_cny(78'000.0);
    Qty vol = 1'000;
    for (int i = 0; i < 2'000; ++i) {
      const int ph = i % 40;
      const bool up = ph < 30;                 // 30 up-ticks then 10 down: a trend
      last += up ? px_from_cny(10.0) : -px_from_cny(10.0);
      vol += 3;
      MdRecord r{};
      r.inst = 0;
      r.recv_ts = at_time(9, 0, 0) + i * 300'000LL;
      r.exch_ts = r.recv_ts;
      r.last_px = last;
      r.day_volume = vol;
      r.open_interest = 50'000;
      for (int l = 0; l < kMaxLevels; ++l) {
        r.bid_px[l] = last - px_from_cny(10.0) * (l + 1);
        r.bid_qty[l] = up ? 50 : 10;           // imbalance agrees with the move
        r.ask_px[l] = last + px_from_cny(10.0) * (l + 1);
        r.ask_qty[l] = up ? 10 : 50;
      }
      w.append(r);
    }
    w.close();
  }

  TickReader rd;
  CHECK(rd.open(path));

  InstrumentTable table;
  table.add("cu2609", Venue::SHFE, Board::Future, px_from_cny(10.0), 1, 5,
            px_from_cny(78'000.0), 1'000);
  SiteConfig sc;
  sc.venue = Venue::SHFE;
  sc.hosted = {0};
  sc.day = 20'260'716;
  RiskLimits wide;
  wide.max_actions_per_sec = 1'000'000;
  wide.max_actions_per_day = 1'000'000'000;
  wide.duplicate_horizon = 0;
  sc.limits = wide;

  MomentumParams mp;
  mp.imb_gate_e4 = 1'000;
  using BtTestSite = Site<BacktestGateway, SnapMomentum>;
  BtTestSite site(table, sc, BtTestSite::Mode::Live,
                  {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})});
  BacktestGateway gw(table.size());
  site.set_gateway(&gw);

  PhaseChangeEvt ph{};
  ph.venue = Venue::SHFE;
  ph.phase = Phase::Continuous;
  ph.ts = at_time(9, 0);
  site.source(ph, ph.ts);

  std::uint64_t seq = 0;
  MdRecord rec;
  while (rd.next(rec)) {
    MdSnapshotEvt s{};
    s.feed_seq = ++seq;
    s.ts = rec.exch_ts;
    for (int l = 0; l < kMaxLevels; ++l) {
      s.bid_px[l] = rec.bid_px[l]; s.bid_qty[l] = rec.bid_qty[l];
      s.ask_px[l] = rec.ask_px[l]; s.ask_qty[l] = rec.ask_qty[l];
    }
    s.last_px = rec.last_px;
    s.day_volume = rec.day_volume;
    s.inst = 0;
    s.venue = Venue::SHFE;
    gw.set_book(0, rec.bid_px[0], rec.bid_qty[0], rec.ask_px[0], rec.ask_qty[0]);
    site.source(s, rec.exch_ts);
    if (!gw.drain().empty()) {
      auto reports = gw.drain();
      gw.clear();
      for (const auto& e : reports) site.source(e, rec.exch_ts);
    }
  }

  // The momentum strategy traded the trend through the real riskgate + PnL.
  CHECK(gw.fills() > 0);
  CHECK(site.tel().get("fills") > 0);
  CHECK(site.pnl().total_realized_e4(StrategyId{1}) != 0);
  // verified() is meaningful only after the whole stream was read (it checks
  // decoded == record_count); the backtest loop above consumed every record.
  CHECK(rd.verified());
  std::remove(path);
}

void test_tick_compare() {
  using namespace plat::rec;
  const char* pa = "plat_test_a.mdr";
  const char* pb = "plat_test_b.mdr";
  const char* pg = "plat_test_g.mdr";
  std::vector<MdrInstrument> dir(1);
  std::snprintf(dir[0].symbol, sizeof(dir[0].symbol), "cu2609");
  std::snprintf(dir[0].exchange, sizeof(dir[0].exchange), "SHFE");

  // Ten canonical ticks. A misses #4 (a UDP loss); B receives #6,#7 REORDERED
  // (arrival order swapped) and duplicates #8. The compare must repair B's
  // order, drop the dup, fill A's gap from B, and end with zero conflicts.
  std::vector<MdRecord> canon;
  for (int i = 0; i < 10; ++i)
    canon.push_back(mk_rec(0, at_time(9, 0, 0) + i * kNsPerMs,
                           px_from_cny(78'000.0) + i * px_from_cny(1.0), 1'000 + i,
                           px_from_cny(78'000.0), 5, px_from_cny(78'010.0), 7));
  {
    TickWriter w;
    w.open(pa, 20'260'715, "dc1", 5, dir);
    for (int i = 0; i < 10; ++i)
      if (i != 4) w.append(canon[i]);  // A drops #4
    w.close();
  }
  {
    TickWriter w;
    w.open(pb, 20'260'715, "dc2", 5, dir);
    for (int i = 0; i < 10; ++i) {
      if (i == 6) { w.append(canon[7]); w.append(canon[6]); }  // reordered pair
      else if (i == 7) continue;
      else { w.append(canon[i]); if (i == 8) w.append(canon[8]); }  // dup #8
    }
    w.close();
  }
  CompareStats s{};
  CHECK(compare_recordings(pa, pb, pg, s));
  CHECK(s.conflicts == 0);        // the datacenters agree on content
  CHECK(s.b_resorted >= 1);       // B's UDP reordering was detected + repaired
  CHECK(s.duplicates >= 1);       // B's duplicate dropped
  CHECK(s.b_only >= 1);           // A's gap (#4) filled from B's copy
  CHECK(s.golden == 10);          // the merge recovers all ten canonical ticks

  // The golden file is a clean, sealed, in-order recording of all ten.
  TickReader g;
  CHECK(g.open(pg));
  MdRecord out;
  std::size_t k = 0;
  Nanos last_exch = -1;
  while (g.next(out)) {
    CHECK(out.exch_ts >= last_exch);  // canonically ordered
    last_exch = out.exch_ts;
    ++k;
  }
  CHECK(k == 10 && g.verified());
  std::remove(pa);
  std::remove(pb);
  std::remove(pg);
}

