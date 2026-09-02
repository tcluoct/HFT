// Scenario tests for sigcalc. The engine's promise is exactness: every
// windowed feature equals a from-scratch recompute over the raw history
// (integers make that a bit-for-bit claim, not an epsilon claim), the two
// EWMA backends are indistinguishable on every step, and the whole run is
// digest-deterministic. So the reference implementations here are the
// dumbest possible loops over stored history, and the comparisons are ==.
// No framework — a CHECK macro.

#include <cstdio>
#include <vector>

#include "sg/engine.hpp"
#include "sg/ewma.hpp"
#include "sg/sim.hpp"
#include "sg/types.hpp"
#include "sg/windows.hpp"

using namespace sg;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_fail;                                                     \
    }                                                               \
  } while (0)

void t_rolling_sum() {
  std::puts("rolling sum: == brute-force recompute, incl. warmup");
  RollingSum<8> rs;
  std::vector<i64> hist;
  Rng rng(1);
  bool ok = true;
  for (int i = 0; i < 5000; ++i) {
    const i64 v = static_cast<i64>(rng.below(2001)) - 1000;
    hist.push_back(v);
    const i64 got = rs.push(v);
    i64 want = 0;
    const std::size_t from = hist.size() > 8 ? hist.size() - 8 : 0;
    for (std::size_t j = from; j < hist.size(); ++j) want += hist[j];
    if (got != want) ok = false;
  }
  CHECK(ok);
}

template <unsigned W>
bool wedge_matches(u64 seed, int steps, bool monotone_up, bool monotone_down) {
  RollingExtrema<W> ex;
  std::vector<i64> hist;
  Rng rng(seed);
  for (int i = 0; i < steps; ++i) {
    i64 v;
    if (monotone_up) v = i;
    else if (monotone_down) v = -i;
    else v = static_cast<i64>(rng.below(2001)) - 1000;
    hist.push_back(v);
    ex.push(v);
    const std::size_t from = hist.size() > W ? hist.size() - W : 0;
    i64 mn = hist[from], mx = hist[from];
    for (std::size_t j = from; j < hist.size(); ++j) {
      if (hist[j] < mn) mn = hist[j];
      if (hist[j] > mx) mx = hist[j];
    }
    if (ex.min() != mn || ex.max() != mx) return false;
  }
  return true;
}

void t_wedge() {
  std::puts("wedges: min/max == brute force (random, and monotone runs 3W long)");
  CHECK(wedge_matches<8>(2, 20000, false, false));
  CHECK(wedge_matches<64>(3, 20000, false, false));
  CHECK(wedge_matches<8>(0, 24, true, false));   // strictly rising: ring stress
  CHECK(wedge_matches<8>(0, 24, false, true));   // strictly falling
  CHECK(wedge_matches<256>(4, 4000, false, false));
}

void t_ewma_backends() {
  std::puts("ewma lattice: AVX2 == scalar bit-for-bit on every step");
  EwmaLattice a, b;
  Rng rng(5);
  i64 x = 400'000 << 16;
  bool equal = true;
  for (int i = 0; i < 200'000; ++i) {
    x += (static_cast<i64>(rng.below(2001)) - 1000) << 8;
    a.update_scalar(x);
    b.update_avx2(x);
    for (unsigned l = 0; l < EwmaLattice::kLanes; ++l)
      if (a.m[l] != b.m[l]) equal = false;
  }
  CHECK(equal);

  // Convergence: constant input pulls every lane to within 2^k of it.
  EwmaLattice c;
  const i64 target = 123'456'789;
  for (int i = 0; i < 100'000; ++i) c.update_scalar(target);
  bool close = true;
  for (unsigned l = 0; l < EwmaLattice::kLanes; ++l)
    if (target - c.m[l] >= (i64{1} << EwmaLattice::kShift[l]) || target - c.m[l] < 0)
      close = false;
  CHECK(close);
}

void t_microprice_hand() {
  std::puts("microprice/imbalance: hand-checked cases");
  SignalEngine<8, false> eng;
  Tick t;
  t.bid_px = 100;
  t.ask_px = 101;
  t.bid_qty = 10;
  t.ask_qty = 10;
  const Features f = eng.on_tick(t);  // copy: on_tick returns a live reference
  CHECK(f.microprice_q16 == (201 << 16) / 2);  // balanced book: mid, 100.5
  CHECK(f.imbalance_q16 == 0);

  t.bid_qty = 30;  // heavy bid pushes the microprice toward the ask
  t.ask_qty = 10;
  const Features g = eng.on_tick(t);
  CHECK(g.microprice_q16 == ((100 * 10 + 101 * 30) << 16) / 40);
  CHECK(g.microprice_q16 > f.microprice_q16);
  CHECK(g.imbalance_q16 == (20 << 16) / 40);
}

// Reference recompute of every windowed feature from raw tick history.
template <unsigned W>
bool engine_matches_reference(u64 seed, int steps) {
  SignalEngine<W, false> eng;
  MarketSim sim(seed);
  std::vector<Tick> hist;
  for (int i = 0; i < steps; ++i) {
    const Tick t = sim.next();
    hist.push_back(t);
    const Features f = eng.on_tick(t);
    if (i > 600 && i % 97 != 0) continue;  // early events exhaustively, then sampled

    const std::size_t n = hist.size();
    const std::size_t from = n > W ? n - W : 0;
    i64 ofi = 0, drift = 0, rvar = 0, tflow = 0;
    i64 mn = hist[from].bid_px + hist[from].ask_px, mx = mn;
    for (std::size_t j = from; j < n; ++j) {
      const Tick& c = hist[j];
      const i64 mid = c.bid_px + c.ask_px;
      if (j > 0) {
        const Tick& p = hist[j - 1];
        i64 e = 0;
        if (c.bid_px >= p.bid_px) e += c.bid_qty;
        if (c.bid_px <= p.bid_px) e -= p.bid_qty;
        if (c.ask_px <= p.ask_px) e -= c.ask_qty;
        if (c.ask_px >= p.ask_px) e += p.ask_qty;
        ofi += e;
        const i64 d = mid - (p.bid_px + p.ask_px);
        drift += d;
        rvar += d * d;
      }
      tflow += c.trade_side * c.trade_qty;
      if (mid < mn) mn = mid;
      if (mid > mx) mx = mid;
    }
    const Tick& c = hist.back();
    const i64 den = c.bid_qty + c.ask_qty;
    const i64 mid = c.bid_px + c.ask_px;
    const i64 mp = ((c.bid_px * c.ask_qty + c.ask_px * c.bid_qty) << 16) / den;
    const i64 imb = ((c.bid_qty - c.ask_qty) << 16) / den;
    const i64 rp = mx > mn ? (((mid - mn) << 16) / (mx - mn)) * 2 - (i64{1} << 16) : 0;
    if (f.ofi_w != ofi || f.drift_w != drift || f.rvar_w != rvar || f.tflow_w != tflow)
      return false;
    if (f.microprice_q16 != mp || f.imbalance_q16 != imb || f.range_pos_q16 != rp)
      return false;
  }
  return true;
}

void t_engine_reference() {
  std::puts("engine: every feature == brute-force recompute over history");
  CHECK(engine_matches_reference<256>(7, 50'000));
  CHECK(engine_matches_reference<16>(8, 20'000));
}

void t_digests() {
  std::puts("engine: AVX2/scalar digests equal; runs deterministic");
  SignalEngine<256, true> avx;
  SignalEngine<256, false> sca;
  {
    MarketSim s1(11), s2(11);
    for (int i = 0; i < 200'000; ++i) avx.on_tick(s1.next());
    for (int i = 0; i < 200'000; ++i) sca.on_tick(s2.next());
  }
  CHECK(avx.digest() == sca.digest());

  SignalEngine<256, true> again;
  MarketSim s3(11);
  for (int i = 0; i < 200'000; ++i) again.on_tick(s3.next());
  CHECK(again.digest() == avx.digest());

  SignalEngine<256, true> other;
  MarketSim s4(12);
  for (int i = 0; i < 200'000; ++i) other.on_tick(s4.next());
  CHECK(other.digest() != avx.digest());
}

}  // namespace

int main() {
  t_rolling_sum();
  t_wedge();
  t_ewma_backends();
  t_microprice_hand();
  t_engine_reference();
  t_digests();
  if (g_fail == 0) {
    std::puts("\nall tests passed");
    return 0;
  }
  std::printf("\n%d CHECK(s) failed\n", g_fail);
  return 1;
}
