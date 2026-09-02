// Scenario tests for pnld. The interesting claims are accounting theorems,
// so the tests state them as such: FIFO and WAC disagree on attribution by
// construction (a hand-built case shows realized 500 vs 0), yet their
// equities are equal after every single event of a 200k-fill random tape —
// exactly, in integers, despite WAC's truncated division. The firm rollup's
// incremental deltas are audited against brute-force recomputes, and the
// whole run is digest-deterministic. No framework — a CHECK macro.

#include <cstdio>
#include <memory>
#include <vector>

#include "pl/book.hpp"
#include "pl/keeper.hpp"
#include "pl/sim.hpp"
#include "pl/types.hpp"

using namespace pl;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_fail;                                                     \
    }                                                               \
  } while (0)

void t_open_reduce() {
  std::puts("book: open, partial reduce, marks — the textbook case");
  PositionBook<64> b;
  b.on_fill(1, 100, 10, 0);   // buy 100 @ 10
  CHECK(b.pos() == 100 && b.open_cost() == 1000 && b.lot_count() == 1);
  b.on_fill(-1, 50, 12, 0);   // sell 50 @ 12
  CHECK(b.pos() == 50 && b.fifo_realized() == 100);  // 50 * (12 - 10)
  CHECK(b.open_cost() == 500 && b.lot_count() == 1);
  b.on_mark(12);
  CHECK(b.fifo_unrealized() == 100);  // 50 * (12 - 10)
  CHECK(b.fifo_equity() == 200 && b.wac_equity() == 200);  // single lot: agree
  CHECK(b.audit());
}

void t_fifo_vs_wac() {
  std::puts("book: FIFO and WAC split the same P&L differently, equity equal");
  PositionBook<64> b;
  b.on_fill(1, 100, 10, 0);  // lot 1
  b.on_fill(1, 100, 20, 0);  // lot 2
  b.on_fill(-1, 100, 15, 0); // sell against them
  CHECK(b.fifo_realized() == 500);  // consumed the @10 lot: 100 * (15-10)
  CHECK(b.wac_realized() == 0);     // avg cost 15: 100 * (15-15)
  b.on_mark(15);
  CHECK(b.fifo_unrealized() == -500);  // left holding the @20 lot at 15
  CHECK(b.wac_unrealized() == 0);
  CHECK(b.fifo_equity() == b.wac_equity() && b.fifo_equity() == 0);
  CHECK(b.audit());
}

void t_flip() {
  std::puts("book: crossing zero flips the position and starts fresh lots");
  PositionBook<64> b;
  b.on_fill(1, 100, 10, 0);
  b.on_fill(-1, 300, 12, 0);  // sell 300: close 100, go short 200
  CHECK(b.pos() == -200 && b.fifo_realized() == 200 && b.lot_count() == 1);
  CHECK(b.open_cost() == -2400);  // short 200 @ 12
  b.on_mark(11);
  CHECK(b.fifo_unrealized() == 200);  // short profits as it falls
  CHECK(b.fifo_equity() == 400 && b.wac_equity() == 400);
  b.on_fill(1, 200, 11, 0);  // buy back flat
  CHECK(b.pos() == 0 && b.lot_count() == 0 && b.open_cost() == 0);
  CHECK(b.fifo_realized() == 400 && b.fifo_equity() == 400);
  CHECK(b.audit());
}

void t_fees() {
  std::puts("book: fees reduce equity, never realized attribution");
  PositionBook<64> b;
  b.on_fill(1, 100, 10, 7);
  b.on_fill(-1, 100, 10, 3);
  CHECK(b.fifo_realized() == 0 && b.fees() == 10 && b.fifo_equity() == -10);
  CHECK(b.audit());
}

void t_deep_drain() {
  std::puts("book: one fill draining 1000 lots stays exact");
  auto b = std::make_unique<PositionBook<2048>>();
  i64 want_cost = 0;
  for (i64 i = 0; i < 1000; ++i) {
    b->on_fill(1, 1, 100 + (i % 50), 0);
    want_cost += 100 + (i % 50);
  }
  CHECK(b->lot_count() == 1000 && b->open_cost() == want_cost);
  const unsigned drained = b->on_fill(-1, 1000, 130, 0);
  CHECK(drained == 1000 && b->pos() == 0 && b->lot_count() == 0);
  CHECK(b->fifo_realized() == 1000 * 130 - want_cost);
  CHECK(b->audit());
}

void t_random_tape() {
  std::puts("keeper: 200k-fill tape — identity, rollup, and lots audited throughout");
  constexpr u32 kSyms = 64;
  auto k = std::make_unique<Keeper<kSyms, 2048>>();
  FlowSim<kSyms> sim(31);
  i64 truth_cash = 0;
  bool equity_ok = true, audits_ok = true;
  for (int i = 0; i < 200'000; ++i) {
    const Fill f = sim.next_fill();
    CHECK(k->on_fill(f));
    truth_cash += -f.side * f.qty * f.px - f.fee;
    if (i % 8 == 0) k->on_mark(sim.mark_of(static_cast<u32>(sim.rand_below(kSyms))));
    // The equity identity, both attributions, after every event.
    if (k->firm_fifo_equity() != k->firm_wac_equity()) equity_ok = false;
    if (i % 5000 == 0 && !k->audit(true)) audits_ok = false;
  }
  CHECK(equity_ok);
  CHECK(audits_ok);
  CHECK(k->audit(true));

  // Ground truth: equity == signed cash + position value, computed cold.
  i64 pos_value = 0;
  for (u32 s = 0; s < kSyms; ++s) pos_value += k->book(s).pos() * k->book(s).mark();
  CHECK(k->firm_fifo_equity() == truth_cash + pos_value);

  // Determinism: replay the same tape, same digest.
  auto k2 = std::make_unique<Keeper<kSyms, 2048>>();
  FlowSim<kSyms> sim2(31);
  for (int i = 0; i < 200'000; ++i) {
    k2->on_fill(sim2.next_fill());
    if (i % 8 == 0) k2->on_mark(sim2.mark_of(static_cast<u32>(sim2.rand_below(kSyms))));
  }
  CHECK(k2->digest() == k->digest());
}

void t_rejects() {
  std::puts("keeper: malformed fills rejected, state untouched");
  Keeper<4, 64> k;
  Fill f;
  f.sym = 9;
  f.qty = 1;
  CHECK(!k.on_fill(f));
  f.sym = 1;
  f.qty = 0;
  CHECK(!k.on_fill(f));
  f.qty = 1;
  f.side = 2;
  CHECK(!k.on_fill(f));
  CHECK(k.audit(true) && k.firm_fifo_equity() == 0);
}

}  // namespace

int main() {
  t_open_reduce();
  t_fifo_vs_wac();
  t_flip();
  t_fees();
  t_deep_drain();
  t_random_tape();
  t_rejects();
  if (g_fail == 0) {
    std::puts("\nall tests passed");
    return 0;
  }
  std::printf("\n%d CHECK(s) failed\n", g_fail);
  return 1;
}
