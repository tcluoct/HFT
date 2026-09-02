// sord harness. Three parts:
//
//   hot path : ns per route() decision — rank 16 venues (sorting network),
//              choose, largest-remainder split — per-call rdtscp pairs,
//              plus bulk network-vs-std::sort router duels at N=4/8/16.
//   quality  : the reason a cost model exists. 200k parent buys against 8
//              venues with true fees/latencies/fill ratios; the naive
//              router (ranks by displayed price only) and the model router
//              (knows the truth) face identical quotes and identical luck;
//              unfilled shares are chased at mid+3. Score: realized
//              slippage per share, in milli-ticks.
//   proof    : conservation audited on every route; digest replay.

#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>

#include "hft/common/bench.hpp"
#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/tsc_clock.hpp"
#include "sr/router.hpp"
#include "sr/sim.hpp"

namespace {

using namespace sr;

struct Args {
  u64 routes = 2'000'000;
  u64 parents = 200'000;
  u64 seed = 42;
  unsigned core = 2;
  bool pin = true;
  bool rt = false;
};

bool parse_u64(const char* s, u64& v) {
  const auto r = std::from_chars(s, s + std::strlen(s), v);
  return r.ec == std::errc{} && *r.ptr == '\0';
}

bool parse_args(int argc, char** argv, Args& a) {
  u64 core = a.core;
  struct Opt {
    const char* name;
    u64* dst;
  };
  const Opt opts[] = {
      {"--routes", &a.routes}, {"--parents", &a.parents}, {"--seed", &a.seed}, {"--core", &core}};
  for (int i = 1; i < argc; ++i) {
    const char* s = argv[i];
    if (!std::strcmp(s, "--no-pin")) {
      a.pin = false;
      continue;
    }
    if (!std::strcmp(s, "--rt")) {
      a.rt = true;
      continue;
    }
    bool hit = false;
    for (const auto& o : opts)
      if (!std::strcmp(s, o.name)) {
        if (i + 1 >= argc || !parse_u64(argv[++i], *o.dst)) return false;
        hit = true;
        break;
      }
    if (!hit) {
      std::printf("sord [--routes N] [--parents N] [--seed N] [--core N] [--no-pin] [--rt]\n");
      return false;
    }
  }
  a.core = static_cast<unsigned>(core);
  return true;
}

template <unsigned kNV, bool kStd>
void fill_router(Router<kNV, kStd>& r, SimRng& rng) {
  for (unsigned v = 0; v < kNV; ++v) {
    VenueCfg c;
    c.fee_mt = static_cast<i32>(rng.below(61)) - 30;
    c.lat_us = 5 + static_cast<u32>(rng.below(40));
    c.fill_pm = 550 + static_cast<u32>(rng.below(450));
    r.set_venue(v, c);
    const i64 px = 100'000 + static_cast<i64>(rng.below(3));
    r.set_quote(v, px - 1, 100 + static_cast<i64>(rng.below(2000)), px,
                100 + static_cast<i64>(rng.below(2000)));
  }
}

template <unsigned kNV, bool kStd>
double bulk_route_ns(u64 routes, u64 seed) {
  auto r = std::make_unique<Router<kNV, kStd>>();
  SimRng rng(seed);
  fill_router(*r, rng);
  return hft::bench_ns_per_op(routes, [&] {
    u64 acc = hft::bench_sink & 1;
    u64 s = 0x1234;
    for (u64 i = 0; i < routes; ++i) {
      // quotes move every route (cheap LCG, identical cost in both variants)
      s = s * 6364136223846793005ull + 1442695040888963407ull;
      const unsigned v = static_cast<unsigned>((s >> 33) % kNV);
      const i64 px = 100'000 + static_cast<i64>((s >> 20) % 3);
      r->set_quote(v, px - 1, 100 + static_cast<i64>((s >> 40) & 0x7FF), px,
                   100 + static_cast<i64>((s >> 45) & 0x7FF));
      const Plan p = r->route(1, 400 + static_cast<i64>(i & 0xFFF), 200'000);
      acc += p.routed;
      for (unsigned c = 0; c < p.nchild; ++c) r->complete(p.child[c].venue, p.child[c].qty);
    }
    hft::bench_sink = acc;
  });
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return 1;
  std::printf("sord: %llu routes (latency), %llu parents (quality), seed %llu\n\n",
              static_cast<unsigned long long>(a.routes),
              static_cast<unsigned long long>(a.parents),
              static_cast<unsigned long long>(a.seed));

  hft::TscClock::calibrate();
  if (a.pin && !hft::pin_current_thread(a.core)) std::puts("warn: pin failed");
  if (a.rt && !hft::boost_current_thread_priority()) std::puts("warn: rt priority failed");

  int rc = 0;

  // ---- part 1: the hot path ----------------------------------------------------
  {
    auto r = std::make_unique<Router<16>>();
    SimRng rng(a.seed);
    fill_router(*r, rng);
    hft::LatencyHistogram h;
    for (u64 i = 0; i < a.routes; ++i) {
      if ((i & 0xF) == 0) {  // quotes move between decisions (outside timing)
        const unsigned v = static_cast<unsigned>(rng.below(16));
        const i64 px = 100'000 + static_cast<i64>(rng.below(3));
        r->set_quote(v, px - 1, 100 + static_cast<i64>(rng.below(2000)), px,
                     100 + static_cast<i64>(rng.below(2000)));
      }
      const i64 qty = 400 + static_cast<i64>(rng.below(6000));
      const u64 c0 = hft::rdtscp();
      std::atomic_signal_fence(std::memory_order_seq_cst);
      const Plan p = r->route(1, qty, 200'000);
      std::atomic_signal_fence(std::memory_order_seq_cst);
      const u64 c1 = hft::rdtscp();
      h.record(c1 - c0);
      for (unsigned c = 0; c < p.nchild; ++c) r->complete(p.child[c].venue, p.child[c].qty);
    }
    auto ns = [](u64 cyc) {
      return static_cast<unsigned long long>(hft::TscClock::to_ns(cyc));
    };
    std::printf("hot path: route() over 16 venues (rank + split, ns, ~20 ns floor)\n"
                "  p50 %llu  p90 %llu  p99 %llu  p99.9 %llu  max %llu\n",
                ns(h.percentile(0.50)), ns(h.percentile(0.90)), ns(h.percentile(0.99)),
                ns(h.percentile(0.999)), ns(h.max()));
    std::printf("  bulk (ns/route)  network: N=4 %.0f  N=8 %.0f  N=16 %.0f\n",
                bulk_route_ns<4, false>(a.routes, a.seed + 1),
                bulk_route_ns<8, false>(a.routes, a.seed + 2),
                bulk_route_ns<16, false>(a.routes, a.seed + 3));
    std::printf("                  std::sort: N=4 %.0f  N=8 %.0f  N=16 %.0f\n\n",
                bulk_route_ns<4, true>(a.routes, a.seed + 1),
                bulk_route_ns<8, true>(a.routes, a.seed + 2),
                bulk_route_ns<16, true>(a.routes, a.seed + 3));
  }

  // ---- part 2: routing quality ---------------------------------------------------
  {
    auto naive = std::make_unique<Router<8>>();
    auto model = std::make_unique<Router<8>>();
    for (unsigned v = 0; v < 8; ++v) {
      naive->set_venue(v, VenueCfg{0, 0, 1000, 1'000'000});  // believes everyone
      model->set_venue(v, VenueCfg{kTruth[v].fee_mt, kTruth[v].lat_us, kTruth[v].fill_pm,
                                   1'000'000});
    }
    SimRng rng(a.seed + 10);
    i64 mid = 100'000;
    struct Tally {
      i64 slip_mt = 0, qty = 0, direct = 0, chased = 0, routed = 0;
    } tn, tm;
    bool conserve = true;
    i64 ask_px[8];

    for (u64 pid = 0; pid < a.parents; ++pid) {
      mid += static_cast<i64>(rng.below(3)) - 1;
      for (unsigned v = 0; v < 8; ++v) {
        ask_px[v] = mid + 1 + static_cast<i64>(rng.below(100) < 20 ? 1 : 0);
        const i64 disp = 100 + static_cast<i64>(rng.below(3000));
        naive->set_quote(v, mid - 1, disp, ask_px[v], disp);
        model->set_quote(v, mid - 1, disp, ask_px[v], disp);
      }
      const i64 qty = 500 + static_cast<i64>(rng.below(7500));

      auto run = [&](Router<8>& r, Tally& t) {
        const Plan p = r.route(1, qty, mid + 5);
        i64 sum = 0;
        for (unsigned c = 0; c < p.nchild; ++c) sum += p.child[c].qty;
        if (sum != p.routed || p.routed > qty) conserve = false;
        const Outcome o = settle<8>(a.seed, pid, p, qty, mid, ask_px);
        t.slip_mt += o.paid_mt - qty * mid * 1000;
        t.qty += qty;
        t.direct += o.direct;
        t.chased += o.chased;
        t.routed += p.routed;
        for (unsigned c = 0; c < p.nchild; ++c) r.complete(p.child[c].venue, p.child[c].qty);
      };
      run(*naive, tn);
      run(*model, tm);
    }

    auto row = [](const char* name, const Tally& t) {
      std::printf("  %-6s slippage %7.1f mt/share | direct fills %5.1f%% | chased %5.1f%%\n",
                  name, static_cast<double>(t.slip_mt) / static_cast<double>(t.qty),
                  100.0 * static_cast<double>(t.direct) / static_cast<double>(t.routed),
                  100.0 * static_cast<double>(t.chased) / static_cast<double>(t.qty));
    };
    std::printf("quality: %llu parent buys, identical quotes and luck (chase = mid+3)\n",
                static_cast<unsigned long long>(a.parents));
    row("naive", tn);
    row("model", tm);
    const double saved =
        static_cast<double>(tn.slip_mt - tm.slip_mt) / static_cast<double>(tm.qty);
    std::printf("  model saves %.1f milli-ticks/share | conservation %s\n\n", saved,
                conserve ? "OK" : "VIOLATED");
    if (!conserve) rc = 1;
  }

  // ---- part 3: determinism replay --------------------------------------------------
  {
    auto run = [&] {
      auto r = std::make_unique<Router<16>>();
      SimRng rng(a.seed + 20);
      fill_router(*r, rng);
      for (u64 i = 0; i < 500'000; ++i) {
        const Plan p = r->route(rng.below(2) ? 1 : -1, 1 + static_cast<i64>(rng.below(5000)),
                                rng.below(2) ? 200'000 : 1);
        for (unsigned c = 0; c < p.nchild; ++c) r->complete(p.child[c].venue, p.child[c].qty);
      }
      return r->digest();
    };
    const u64 d1 = run(), d2 = run();
    std::printf("determinism replay: %s (digest %016llx)\n", d1 == d2 ? "PASS" : "FAIL",
                static_cast<unsigned long long>(d1));
    if (d1 != d2) rc = 1;
  }
  return rc;
}
