// Scenario tests for quotd. The engine's contract is behavioral, so each
// rule gets a constructed scene: the ladder shape, hysteresis keeping queue
// position, the replenish threshold, the inventory hard stop, self-match
// impossibility across an inventory sweep, the epoch pull with a racing
// stale fill, and hand-checked spread capture. No framework — a CHECK macro.

#include <cstdio>

#include "qt/engine.hpp"
#include "qt/sim.hpp"

using namespace qt;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_fail;                                                     \
    }                                                               \
  } while (0)

Cfg base_cfg() {
  Cfg c;
  c.half_spread = 2;
  c.level_step = 1;
  c.size = 100;
  c.levels = 4;
  c.band = 1;
  return c;
}

void t_ladder() {
  std::puts("ladder: 4x2 quotes around fair value; identical tick -> zero ops");
  QuoteEngine e(base_cfg());
  Op buf[QuoteEngine::kMaxOps];
  const unsigned n = e.on_tick(100'000, buf);
  CHECK(n == 8);
  for (unsigned i = 0; i < n; ++i) CHECK(buf[i].kind == Op::kNew && buf[i].qty == 100);
  CHECK(e.px(0, 0) == 99'998 && e.px(0, 3) == 99'995);   // bids step down
  CHECK(e.px(1, 0) == 100'002 && e.px(1, 3) == 100'005); // asks step up
  CHECK(e.on_tick(100'000, buf) == 0);  // nothing changed, nothing sent
}

void t_band() {
  std::puts("hysteresis: 1-tick drift keeps every queue spot, 2 ticks requotes");
  QuoteEngine e(base_cfg());
  Op buf[QuoteEngine::kMaxOps];
  e.on_tick(100'000, buf);
  CHECK(e.on_tick(100'001, buf) == 0);   // within band: hold the line
  CHECK(e.px(0, 0) == 99'998);           // still the original quote
  const unsigned n = e.on_tick(100'002, buf);
  CHECK(n == 16);  // beyond band: every level cancel+new
  CHECK(e.px(0, 0) == 100'000 && e.px(1, 0) == 100'004);
}

void t_skew_lut() {
  std::puts("skew: linear in buckets, clamped, symmetric, zero when flat");
  Cfg c = base_cfg();
  c.inv_bucket = 200;
  c.max_skew = 6;
  QuoteEngine e(c);
  Op buf[QuoteEngine::kMaxOps];
  e.on_tick(100'000, buf);
  CHECK(e.skew_ticks() == 0);
  e.on_fill(0, 0, 100);  // +100 shares: under one bucket
  CHECK(e.skew_ticks() == 0);
  e.on_fill(0, 0, 0);  // no-op fill ignored
  QuoteEngine e2(c);
  e2.on_tick(100'000, buf);
  for (int i = 0; i < 5; ++i) {  // 5 x 100 = 500 long = 2 buckets
    e2.on_fill(0, 0, 100);
    e2.on_tick(100'000, buf);
  }
  CHECK(e2.inventory() == 500 && e2.skew_ticks() == 2);
  CHECK(e2.px(0, 0) == 99'996 && e2.px(1, 0) == 100'000);  // ladder shifted down 2
}

void t_smp() {
  std::puts("self-match: bid < ask across an inventory and fair-value sweep");
  Cfg c = base_cfg();
  QuoteEngine e(c);
  Op buf[QuoteEngine::kMaxOps];
  Rng rng(3);
  bool ok = true;
  i64 fv = 100'000;
  for (int i = 0; i < 10'000; ++i) {
    fv += static_cast<i64>(rng.below(7)) - 3;
    e.on_tick(fv, buf);
    const u8 s = static_cast<u8>(rng.below(2));
    for (u8 l = 0; l < 4; ++l)
      if (e.live(s, l)) {
        e.on_fill(s, l, 25 + static_cast<i64>(rng.below(100)));
        break;
      }
    if (e.live(0, 0) && e.live(1, 0) && e.px(0, 0) >= e.px(1, 0)) ok = false;
  }
  CHECK(ok);
}

void t_replenish() {
  std::puts("replenish: keep above half size, replace below, renew when eaten");
  QuoteEngine e(base_cfg());
  Op buf[QuoteEngine::kMaxOps];
  e.on_tick(100'000, buf);
  e.on_fill(0, 0, 30);  // 70 left: >= half, keep the spot
  CHECK(e.on_tick(100'000, buf) == 0 && e.qty(0, 0) == 70);
  e.on_fill(0, 0, 30);  // 40 left: starved
  unsigned n = e.on_tick(100'000, buf);
  CHECK(n == 2 && buf[0].kind == Op::kCancel && buf[1].kind == Op::kNew);
  CHECK(e.qty(0, 0) == 100);
  e.on_fill(0, 0, 100);  // fully eaten: dead quote
  CHECK(!e.live(0, 0));
  n = e.on_tick(100'000, buf);
  CHECK(n == 1 && buf[0].kind == Op::kNew && e.live(0, 0));
}

void t_max_inv() {
  std::puts("hard stop: the loaded side stops quoting, resumes on unwind");
  Cfg c = base_cfg();
  c.max_inv = 300;
  c.max_skew = 0;  // isolate the hard stop from the skew
  QuoteEngine e(c);
  Op buf[QuoteEngine::kMaxOps];
  e.on_tick(100'000, buf);
  e.on_fill(0, 0, 100);
  e.on_fill(0, 1, 100);
  e.on_fill(0, 2, 100);  // inv 300 == max
  const unsigned n = e.on_tick(100'000, buf);
  CHECK(e.inventory() == 300);
  bool bids_gone = true;
  for (u8 l = 0; l < 4; ++l)
    if (e.live(0, l)) bids_gone = false;
  CHECK(bids_gone);
  bool asks_live = true;
  for (u8 l = 0; l < 4; ++l)
    if (!e.live(1, l)) asks_live = false;
  CHECK(asks_live && n >= 1);
  e.on_fill(1, 0, 100);  // sell some: inv 200 < max
  e.on_tick(100'000, buf);
  CHECK(e.live(0, 0));
}

void t_pull() {
  std::puts("epoch pull: one bump kills the book; racing fills flagged stale");
  QuoteEngine e(base_cfg());
  Op buf[QuoteEngine::kMaxOps];
  e.on_tick(100'000, buf);
  const unsigned n = e.pull_all(buf);
  CHECK(n == 8);
  for (unsigned i = 0; i < n; ++i) CHECK(buf[i].kind == Op::kCancel);
  CHECK(e.on_tick(100'000, buf) == 0);  // pulled: engine stays silent
  // A fill races the cancel to the venue: exposure is real, regime is dead.
  e.on_fill(0, 0, 50);
  CHECK(e.inventory() == 50 && e.stale_fills() == 1);
  e.resume();
  CHECK(e.on_tick(100'000, buf) == 8);  // full fresh ladder, new epoch
  e.on_fill(1, 0, 50);
  CHECK(e.stale_fills() == 1);  // current-epoch fill is not stale
}

void t_capture() {
  std::puts("capture: buy the bid, sell the ask, pocket the spread");
  QuoteEngine e(base_cfg());
  Op buf[QuoteEngine::kMaxOps];
  e.on_tick(100'000, buf);
  e.on_fill(0, 0, 100);  // buy 100 @ 99,998
  e.on_fill(1, 0, 100);  // sell 100 @ 100,002
  CHECK(e.inventory() == 0);
  CHECK(e.cash() == 100 * 4);  // 2 * half_spread * qty
  CHECK(e.equity(100'000) == 400);
}

void t_determinism() {
  std::puts("sim: identical seeds -> identical digests and stats");
  Cfg c = base_cfg();
  QuoteEngine a(c), b(c);
  MMSim s1(77), s2(77);
  const SimStats ra = s1.run(a, 200'000);
  const SimStats rb = s2.run(b, 200'000);
  CHECK(a.digest() == b.digest());
  CHECK(ra.equity == rb.equity && ra.fills == rb.fills);
  CHECK(ra.fills > 1000);  // the sim actually trades
}

}  // namespace

int main() {
  t_ladder();
  t_band();
  t_skew_lut();
  t_smp();
  t_replenish();
  t_max_inv();
  t_pull();
  t_capture();
  t_determinism();
  if (g_fail == 0) {
    std::puts("\nall tests passed");
    return 0;
  }
  std::printf("\n%d CHECK(s) failed\n", g_fail);
  return 1;
}
