// tests_signals.cpp — k-bar aggregation and the bar-cadence indicator
// toolkit: bucket alignment, snapshot volume differencing, midnight wrap,
// ring history, and the integer indicator math.

#include "plat/strat/indicators.hpp"
#include "plat/strat/kbar.hpp"
#include "tests_util.hpp"

// ------------------------------------------------------------------ k-bars

void test_kbar() {
  KBarBuilder b(60 * kNsPerSec);  // 1-minute bars

  // Three trades inside 09:30 — no close, working bar aggregates.
  CHECK(!b.on_trade(at_time(9, 30, 5), px_from_cny(100.0), 2));
  CHECK(!b.on_trade(at_time(9, 30, 20), px_from_cny(101.0), 1));
  CHECK(!b.on_trade(at_time(9, 30, 59), px_from_cny(99.5), 3));
  CHECK(b.working());
  CHECK(b.working_bar().start == at_time(9, 30));
  CHECK(b.working_bar().open == px_from_cny(100.0));
  CHECK(b.working_bar().high == px_from_cny(101.0));
  CHECK(b.working_bar().low == px_from_cny(99.5));
  CHECK(b.working_bar().close == px_from_cny(99.5));
  CHECK(b.working_bar().volume == 6 && b.working_bar().ticks == 3);

  // First trade of 09:31 closes the 09:30 bar and opens the new one.
  CHECK(b.on_trade(at_time(9, 31, 0), px_from_cny(100.5), 4));
  CHECK(b.closed().start == at_time(9, 30));
  CHECK(b.closed().close == px_from_cny(99.5) && b.closed().volume == 6);
  CHECK(b.working_bar().start == at_time(9, 31));
  CHECK(b.working_bar().open == px_from_cny(100.5) && b.working_bar().volume == 4);

  // Quiet gap (session break): next trade lands at 09:35 — the 09:31 bar
  // closes, no synthetic empty bars in between.
  CHECK(b.on_trade(at_time(9, 35, 30), px_from_cny(100.0), 1));
  CHECK(b.closed().start == at_time(9, 31));
  CHECK(b.working_bar().start == at_time(9, 35));

  // px == 0 never touches the bar.
  CHECK(!b.on_trade(at_time(9, 35, 40), 0, 9));
  CHECK(b.working_bar().volume == 1 && b.working_bar().ticks == 1);

  // Force-close at session end.
  CHECK(b.close_working());
  CHECK(b.closed().start == at_time(9, 35) && !b.working());
  CHECK(!b.close_working());  // nothing left to close

  // Night session wrapping midnight: session time jumps backwards, which
  // must close the working bar rather than extend it.
  KBarBuilder n(60 * kNsPerSec);
  CHECK(!n.on_trade(at_time(23, 59, 50), px_from_cny(500.0), 1));
  CHECK(n.on_trade(at_time(0, 0, 10), px_from_cny(501.0), 1));
  CHECK(n.closed().start == at_time(23, 59));
  CHECK(n.working_bar().start == at_time(0, 0));
}

void test_kbar_snapshot() {
  KBarSeries<4> s(60 * kNsPerSec);

  // Pre-trade snapshot (last_px 0): never touches the bar, but it pins the
  // cumulative-volume baseline at 0 so the opening auction is not lost.
  CHECK(!s.on_snapshot(at_time(9, 29, 59), 0, 0, 0));
  CHECK(!s.working());

  // First traded snapshot: the 1'000 lots since the pre-open baseline are
  // real (auction) volume and land in the bar.
  CHECK(!s.on_snapshot(at_time(9, 30, 0, 500 * kNsPerMs), px_from_cny(78'000.0), 1'000, 50));
  CHECK(s.working_bar().volume == 1'000 && s.working_bar().open_interest == 50);

  // Subsequent snapshots contribute the day-volume delta.
  CHECK(!s.on_snapshot(at_time(9, 30, 1), px_from_cny(78'010.0), 1'005, 52));
  CHECK(s.working_bar().volume == 1'005);
  // A regressed cumulative volume (feed hiccup) clamps to zero, not negative.
  CHECK(!s.on_snapshot(at_time(9, 30, 2), px_from_cny(78'010.0), 900, 52));
  CHECK(s.working_bar().volume == 1'005);
  CHECK(!s.on_snapshot(at_time(9, 30, 3), px_from_cny(77'990.0), 910, 53));
  CHECK(s.working_bar().volume == 1'015);
  CHECK(s.working_bar().high == px_from_cny(78'010.0));
  CHECK(s.working_bar().low == px_from_cny(77'990.0));

  // Boundary snapshot closes 09:30 into the ring; its own delta belongs to
  // the new 09:31 bar.
  CHECK(s.on_snapshot(at_time(9, 31, 0), px_from_cny(78'020.0), 930, 55));
  CHECK(s.size() == 1);
  CHECK(s[0].start == at_time(9, 30) && s[0].volume == 1'015 && s[0].open_interest == 53);
  CHECK(s.working_bar().start == at_time(9, 31) && s.working_bar().volume == 20);

  // A builder started mid-session cannot know how much of the cumulative
  // count is history: its first snapshot only primes the baseline.
  KBarBuilder mid(60 * kNsPerSec);
  CHECK(!mid.on_snapshot(at_time(10, 0, 0), px_from_cny(78'000.0), 500, 0));
  CHECK(mid.working_bar().volume == 0);

  // MdSnapshotEvt overload feeds the same path.
  MdSnapshotEvt evt = snap(0, 1, at_time(9, 32, 0), 78'020.0, 78'030.0, 10, 10, 940);
  evt.open_interest = 56;
  CHECK(s.on_snapshot(evt));
  CHECK(s.size() == 2 && s[0].start == at_time(9, 31) && s[1].start == at_time(9, 30));

  // Ring history wraps at capacity: oldest bars fall off, total() keeps counting.
  for (int m = 0; m < 6; ++m)
    s.on_snapshot(at_time(9, 33 + m, 0), px_from_cny(78'000.0), 950 + m, 56);
  CHECK(s.size() == 4 && s.total() == 8);
  CHECK(s[0].start == at_time(9, 37));  // newest closed
  CHECK(s[3].start == at_time(9, 34));  // oldest retained
}

// -------------------------------------------------------------- indicators

void test_indicators() {
  CHECK(isqrt64(0) == 0 && isqrt64(-5) == 0 && isqrt64(1) == 1);
  CHECK(isqrt64(3) == 1 && isqrt64(4) == 2 && isqrt64(10) == 3);
  CHECK(isqrt64(1'000'000'000'000'000'000) == 1'000'000'000);

  Rolling<8> r(4);
  CHECK(r.size() == 0 && !r.full() && r.mean() == 0 && r.variance() == 0);
  for (std::int64_t v = 1; v <= 6; ++v) r.push(v);  // window holds 3,4,5,6
  CHECK(r.full() && r.size() == 4);
  CHECK(r.sum() == 18 && r.mean() == 4);
  CHECK(r.min() == 3 && r.max() == 6);
  CHECK(r[0] == 6 && r[3] == 3);

  Rolling<8> rv(4);
  for (std::int64_t v : {2, 4, 6, 8}) rv.push(v);
  CHECK(rv.variance() == 5 && rv.stddev() == 2);  // mean 5, Σd²=20, /4
  Rolling<8> rc(4);
  for (int i = 0; i < 4; ++i) rc.push(7);
  CHECK(rc.variance() == 0 && rc.stddev() == 0);

  // Centered variance survives absolute-price magnitudes that would
  // overflow a naive sum of squares (78'000 CNY in e4 = 7.8e8).
  Rolling<32> big(20);
  for (int i = 0; i < 20; ++i) big.push(px_from_cny(78'000.0) + i * 100);
  CHECK(big.variance() > 0 && big.stddev() < px_from_cny(1.0));

  // ATR with n=1 (no smoothing): pure true range.
  Atr atr(1);
  KBar b1{};
  b1.high = 110; b1.low = 100; b1.close = 105;
  CHECK(atr.update(b1) == 10);  // first bar: high − low
  KBar b2{};
  b2.high = 120; b2.low = 115; b2.close = 118;
  CHECK(atr.update(b2) == 15);  // gap up: |high − prev close| dominates
  CHECK(atr.primed() && atr.value() == 15);

  // RSI with n=1: direction of the latest delta saturates the scale.
  Rsi rsi(1);
  CHECK(rsi.update(100) == 5'000);  // no direction yet — neutral
  CHECK(rsi.update(105) == 10'000);
  CHECK(rsi.update(103) == 0);
  Rsi rsi3(3);
  rsi3.update(100);
  rsi3.update(106);            // avg up 6, avg dn 0
  CHECK(rsi3.update(103) > 5'000);  // up 6→4, dn 0→1: still net up

  // MACD fast n=1 (ema = latest close), slow n=3 (alpha 1/2), signal n=3.
  Macd macd(1, 3, 3);
  macd.update(100);
  CHECK(macd.macd() == 0 && macd.signal() == 0 && macd.histogram() == 0);
  macd.update(104);  // fast 104, slow 102 → macd 2; signal 0 + (2−0)/2 = 1
  CHECK(macd.macd() == 2 && macd.signal() == 1 && macd.histogram() == 1);

  Vwap vw;
  CHECK(vw.value() == 0);
  vw.add(100, 2);
  vw.add(110, 3);
  CHECK(vw.value() == 106 && vw.volume() == 5);
  vw.reset();
  CHECK(vw.value() == 0 && vw.volume() == 0);
}
