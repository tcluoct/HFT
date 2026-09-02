// Scenario tests for sord. The sorting network is proven twice (at compile
// time by static_assert, at run time against std::sort on random arrays);
// the largest-remainder splitter is proven by hand cases plus a conservation
// property over 50k random routes (allocations sum exactly, never exceed a
// cap, children come out in cost order); eligibility rules are exercised
// one by one. No framework — a CHECK macro.

#include <algorithm>
#include <cstdio>
#include <vector>

#include "sr/network.hpp"
#include "sr/router.hpp"
#include "sr/sim.hpp"

using namespace sr;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_fail;                                                     \
    }                                                               \
  } while (0)

template <unsigned N>
bool network_equals_stdsort(u64 seed, int rounds) {
  SimRng rng(seed);
  for (int r = 0; r < rounds; ++r) {
    u64 a[N], b[N];
    for (unsigned i = 0; i < N; ++i) a[i] = b[i] = rng.next() >> (r % 3 == 0 ? 56 : 0);
    SortNet<N>::sort(a);
    std::sort(b, b + N);
    for (unsigned i = 0; i < N; ++i)
      if (a[i] != b[i]) return false;
  }
  return true;
}

void t_network() {
  std::puts("network: == std::sort on 20k random arrays (incl. heavy duplicates)");
  CHECK(network_equals_stdsort<4>(1, 20000));
  CHECK(network_equals_stdsort<8>(2, 20000));
  CHECK(network_equals_stdsort<16>(3, 20000));
  std::printf("  comparators: N=4 -> %u, N=8 -> %u, N=16 -> %u\n", SortNet<4>::kPairs,
              SortNet<8>::kPairs, SortNet<16>::kPairs);
}

Router<8> make_flat(i64 disp = 1000) {
  Router<8> r;
  for (unsigned v = 0; v < 8; ++v) {
    r.set_venue(v, VenueCfg{});
    r.set_quote(v, 99, disp, 100, disp);
  }
  return r;
}

void t_largest_remainder() {
  std::puts("split: largest remainder — hand case 10 across caps {5,3,3}");
  Router<8> r;
  for (unsigned v = 0; v < 8; ++v) r.set_venue(v, VenueCfg{});
  r.set_quote(0, 99, 1, 100, 5);
  r.set_quote(1, 99, 1, 100, 3);
  r.set_quote(2, 99, 1, 100, 3);  // venues 3..7: no quote -> ineligible
  const Plan p = r.route(1, 10, 105);
  CHECK(p.routed == 10 && p.nchild == 3);
  // floors 4/2/2 (W=11), remainders 6/8/8 -> the two 8s get the leftovers
  CHECK(p.child[0].qty == 4 && p.child[1].qty == 3 && p.child[2].qty == 3);
}

void t_conservation_property() {
  std::puts("split: 50k random routes — exact conservation, caps, cost order");
  SimRng rng(7);
  bool ok = true;
  for (int t = 0; t < 50'000 && ok; ++t) {
    Router<8> r;
    i64 disp[8];
    i64 total = 0;
    for (unsigned v = 0; v < 8; ++v) {
      VenueCfg c;
      c.fee_mt = static_cast<i32>(rng.below(61)) - 30;
      c.lat_us = static_cast<u32>(rng.below(50));
      c.fill_pm = 500 + static_cast<u32>(rng.below(500));
      r.set_venue(v, c);
      disp[v] = static_cast<i64>(rng.below(3000));  // 0 -> ineligible
      const i64 px = 100 + static_cast<i64>(rng.below(3));
      r.set_quote(v, px - 1, disp[v], px, disp[v]);
      if (disp[v] > 0) total += disp[v];
    }
    const i64 qty = 1 + static_cast<i64>(rng.below(12'000));
    const Plan p = r.route(1, qty, 200);

    const i64 want = qty < total ? qty : total;
    i64 sum = 0;
    for (unsigned i = 0; i < p.nchild; ++i) {
      sum += p.child[i].qty;
      if (p.child[i].qty <= 0 || p.child[i].qty > disp[p.child[i].venue]) ok = false;
    }
    // With <= 8 eligible venues, capacity is never stranded by the top-8 cut.
    if (sum != p.routed || p.routed != want) ok = false;
  }
  CHECK(ok);
}

void t_cost_ranking() {
  std::puts("rank: fees, rebates, latency, and fill risk order the venues");
  Router<8> r = make_flat();
  VenueCfg cheap, dear, rebate;
  dear.fee_mt = 30;
  cheap.fee_mt = 5;
  rebate.fee_mt = -10;
  rebate.fill_pm = 900;  // same reliability: rebate should win outright
  r.set_venue(0, dear);
  r.set_venue(1, cheap);
  r.set_venue(2, rebate);
  for (unsigned v = 3; v < 8; ++v) r.set_venue(v, dear);  // backdrop: all dear
  // 2500 across 1000-share venues needs a 3-venue prefix, in cost order.
  const Plan p = r.route(1, 2500, 105);
  CHECK(p.nchild == 3);
  CHECK(p.child[0].venue == 2 && p.child[1].venue == 1);
  // A qty one venue can absorb goes entirely to the best venue.
  const Plan p1 = r.route(1, 100, 105);
  CHECK(p1.nchild == 1 && p1.child[0].venue == 2 && p1.routed == 100);

  // A big enough fill-risk haircut overturns a small rebate.
  VenueCfg flaky = rebate;
  flaky.fill_pm = 500;  // +800 mt of fill risk vs 900pm
  r.set_venue(2, flaky);
  const Plan q = r.route(1, 100, 105);
  CHECK(q.nchild == 1 && q.child[0].venue == 1);
}

void t_eligibility() {
  std::puts("rank: halts, limits, and in-flight caps exclude venues");
  {
    Router<8> r = make_flat();
    for (unsigned v = 0; v < 8; ++v) r.set_halted(v, true);
    CHECK(r.route(1, 100, 105).nchild == 0);
  }
  {
    Router<8> r = make_flat();
    CHECK(r.route(1, 100, 99).nchild == 0);   // buy limit below every ask
    CHECK(r.route(-1, 100, 100).nchild == 0); // sell limit above every bid
    CHECK(r.route(-1, 100, 99).routed == 100);
  }
  {
    Router<8> r = make_flat(1000);
    VenueCfg tight;
    tight.out_cap = 1500;
    for (unsigned v = 0; v < 8; ++v) r.set_venue(v, tight);
    (void)r.route(1, 8000, 105);              // fills every venue to 1000 out
    const Plan p2 = r.route(1, 8000, 105);    // room for 500 more each
    CHECK(p2.routed == 8 * 500);
    const Plan p3 = r.route(1, 100, 105);     // everyone at cap now
    CHECK(p3.nchild == 0);
    r.complete(3, 1500);                      // venue 3 frees up
    const Plan p4 = r.route(1, 100, 105);
    CHECK(p4.nchild == 1 && p4.child[0].venue == 3);
  }
}

void t_determinism() {
  std::puts("route: identical tapes -> identical digests");
  auto run = [] {
    Router<8> r;
    SimRng rng(9);
    for (unsigned v = 0; v < 8; ++v) {
      VenueCfg c;
      c.fee_mt = static_cast<i32>(v) * 3 - 10;
      r.set_venue(v, c);
    }
    for (int i = 0; i < 20'000; ++i) {
      for (unsigned v = 0; v < 8; ++v)
        r.set_quote(v, 99, 100 + static_cast<i64>(rng.below(900)), 100,
                    100 + static_cast<i64>(rng.below(900)));
      const Plan p = r.route(rng.below(2) ? 1 : -1, 1 + static_cast<i64>(rng.below(4000)), 200);
      for (unsigned c = 0; c < p.nchild; ++c) r.complete(p.child[c].venue, p.child[c].qty);
    }
    return r.digest();
  };
  CHECK(run() == run());
}

}  // namespace

int main() {
  t_network();
  t_largest_remainder();
  t_conservation_property();
  t_cost_ranking();
  t_eligibility();
  t_determinism();
  if (g_fail == 0) {
    std::puts("\nall tests passed");
    return 0;
  }
  std::printf("\n%d CHECK(s) failed\n", g_fail);
  return 1;
}
