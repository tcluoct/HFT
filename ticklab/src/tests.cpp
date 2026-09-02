// ticklab tests: codec round-trip, storage round-trip, time-seek correctness,
// the no-look-ahead fill rule, size capping, PnL accounting identity, and
// backtest determinism.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "tl/bt/strategy.hpp"
#include "tl/data/recorder.hpp"
#include "tl/data/replayer.hpp"
#include "tl/sim/session_gen.hpp"

using namespace tl;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_fail;                                                     \
    }                                                               \
  } while (0)

constexpr const char* kTmpFile = "tl_test_tmp.tkl";

void test_codec_roundtrip() {
  std::printf("codec round-trip (all widths incl. constant columns)\n");
  const std::size_t n = 1000;
  std::vector<std::uint64_t> in(n), out(n);
  std::uint64_t s = 987654321;
  auto rng = [&] { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };

  const std::uint64_t ranges[] = {0, 200, 60'000, 4'000'000'000ull, ~std::uint64_t{0} >> 1};
  for (const std::uint64_t range : ranges) {
    const std::uint64_t base = rng();
    for (auto& v : in) v = base / 2 + (range ? rng() % range : 0);
    std::vector<std::byte> enc;
    codec::encode_column(in.data(), n, enc);
    const std::byte* end = codec::decode_column(enc.data(), out.data(), n);
    CHECK(end == enc.data() + enc.size());
    CHECK(std::memcmp(in.data(), out.data(), n * 8) == 0);
  }
}

void test_storage_roundtrip() {
  std::printf("record -> replay round-trip is byte-exact\n");
  const std::uint64_t kN = 200'000;
  {
    SessionGen gen(7, 2, 10'000);
    Recorder rec(kTmpFile, 2);
    CHECK(rec.ok());
    for (std::uint64_t i = 0; i < kN; ++i) rec.add(gen.next());
  }
  Replayer r;
  CHECK(r.open(kTmpFile));
  CHECK(r.total_ticks() == kN);
  SessionGen gen(7, 2, 10'000);  // regenerate the identical stream
  Tick a, b;
  std::uint64_t n = 0;
  while (r.next(a)) {
    b = gen.next();
    if (a.ts_ns != b.ts_ns || a.symbol != b.symbol || a.kind != b.kind || a.px1 != b.px1 ||
        a.px2 != b.px2 || a.q1 != b.q1 || a.q2 != b.q2) {
      CHECK(false && "tick mismatch");
      break;
    }
    ++n;
  }
  CHECK(n == kN);
}

void test_seek() {
  std::printf("time-seek correctness\n");
  Replayer r;
  CHECK(r.open(kTmpFile));
  // Collect a few reference timestamps.
  Tick t;
  std::uint64_t ts_at_100k = 0, last_ts = 0, first_ts = 0, n = 0;
  r.rewind();
  while (r.next(t)) {
    if (n == 0) first_ts = t.ts_ns;
    if (n == 100'000) ts_at_100k = t.ts_ns;
    last_ts = t.ts_ns;
    ++n;
  }
  CHECK(r.seek_ts(ts_at_100k) && r.next(t) && t.ts_ns == ts_at_100k);
  CHECK(r.seek_ts(ts_at_100k - 1) && r.next(t) && t.ts_ns >= ts_at_100k - 1 &&
        t.ts_ns <= ts_at_100k);
  CHECK(r.seek_ts(0) && r.next(t) && t.ts_ns == first_ts);
  CHECK(!r.seek_ts(last_ts + 1));  // past end of session
}

Tick quote(std::uint64_t ts, Price bid, Price ask, Qty bq, Qty aq) {
  Tick t{};
  t.ts_ns = ts;
  t.kind = kQuote;
  t.px1 = bid;
  t.px2 = ask;
  t.q1 = bq;
  t.q2 = aq;
  return t;
}

void test_no_lookahead_fill() {
  std::printf("fills happen at effect time, not decision time (no look-ahead)\n");
  // Ask is 101 at decision time, moves to 105 before the order lands.
  {
    BtContext::Params p;
    p.latency_ns = 20'000;
    BtContext ctx(p);
    ctx.on_tick(quote(1'000, 99, 101, 500, 500));
    ctx.send_ioc(0, 0, 101, 100);                       // buy 101, effect at 21'000
    ctx.on_tick(quote(11'000, 99, 105, 500, 500));      // ask runs away first
    ctx.on_tick(quote(30'000, 99, 105, 500, 500));      // effect processed here
    CHECK(ctx.fills() == 0 && ctx.kills() == 1);        // MUST NOT fill at 101
  }
  // Same book, low latency: lands while 101 still stands.
  {
    BtContext::Params p;
    p.latency_ns = 5'000;
    BtContext ctx(p);
    ctx.on_tick(quote(1'000, 99, 101, 500, 500));
    ctx.send_ioc(0, 0, 101, 100);                       // effect at 6'000
    ctx.on_tick(quote(11'000, 99, 105, 500, 500));      // evaluated vs PRIOR book
    CHECK(ctx.fills() == 1 && ctx.position(0) == 100);
  }
}

void test_conservative_size_cap() {
  std::printf("fills capped at displayed size\n");
  BtContext::Params p;
  p.latency_ns = 1'000;
  BtContext ctx(p);
  ctx.on_tick(quote(1'000, 99, 101, 500, 50));   // only 50 shown at the ask
  ctx.send_ioc(0, 0, 101, 100);
  ctx.on_tick(quote(10'000, 99, 101, 500, 50));
  CHECK(ctx.fills() == 1);
  CHECK(ctx.position(0) == 50);                  // not 100
  CHECK(ctx.volume() == 50);
}

void test_pnl_identity() {
  std::printf("PnL accounting: cash + position*mid == equity\n");
  BtContext::Params p;
  p.latency_ns = 1'000;
  BtContext ctx(p);
  ctx.on_tick(quote(1'000, 100, 102, 500, 500));  // mid 101
  ctx.send_ioc(0, 0, 102, 100);
  ctx.on_tick(quote(10'000, 100, 102, 500, 500)); // fills 100 @ 102
  CHECK(ctx.fills() == 1);
  // equity = cash(-10200) + 100 * mid(101) = -100 (paid half-spread of 1 tick)
  CHECK(std::fabs(ctx.equity() - (-100.0)) < 1e-9);
}

void test_backtest_determinism() {
  std::printf("backtest determinism on a real capture\n");
  Replayer r;
  CHECK(r.open(kTmpFile));
  BtContext::Params p;
  p.latency_ns = 20'000;
  p.nsym = r.nsym();
  r.rewind();
  ImbalanceTaker s1;
  const BtStats a = run_backtest(r, s1, p);
  r.rewind();
  ImbalanceTaker s2;
  const BtStats b = run_backtest(r, s2, p);
  CHECK(a.fills == b.fills);
  CHECK(a.digest == b.digest);
  CHECK(a.net_pnl == b.net_pnl);
  CHECK(a.fills > 0);  // the strategy actually traded on this capture
}

}  // namespace

int main() {
  std::printf("ticklab tests\n=============\n");
  test_codec_roundtrip();
  test_storage_roundtrip();
  test_seek();
  test_no_lookahead_fill();
  test_conservative_size_cap();
  test_pnl_identity();
  test_backtest_determinism();
  std::remove(kTmpFile);
  if (g_fail == 0) {
    std::printf("=============\nALL TESTS PASSED\n");
    return 0;
  }
  std::printf("=============\n%d CHECK(S) FAILED\n", g_fail);
  return 1;
}
