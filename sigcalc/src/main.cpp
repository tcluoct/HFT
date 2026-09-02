// sigcalc harness. Three parts:
//
//   hot path : per-event rdtscp pairs around the full feature computation
//              (microprice, imbalance, OFI, trade flow, drift/rvar, wedge
//              extrema, 8-horizon EWMA lattice, score) streamed from the
//              simulator — what does a complete per-tick signal update cost?
//   duel     : scalar vs AVX2 backends, bulk-timed over the same pre-built
//              tick tape, digests asserted equal — what does vectorizing
//              the lattice buy inside a real (mostly non-vector) update?
//              Plus the lattice alone, where the 8 lanes are the whole op.
//   parts    : each component in isolation (bulk, fenced) — where do the
//              nanoseconds go?
//
// The engine is exact integer arithmetic end to end; the digest printed at
// the end is reproducible across runs, seeds permitting.

#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "hft/common/bench.hpp"
#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/tsc_clock.hpp"
#include "sg/engine.hpp"
#include "sg/sim.hpp"

namespace {

using namespace sg;

struct Args {
  u64 events = 10'000'000;
  u64 tape = 1'000'000;
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
      {"--events", &a.events}, {"--tape", &a.tape}, {"--seed", &a.seed}, {"--core", &core}};
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
      std::printf("sigcalc [--events N] [--tape N] [--seed N] [--core N] [--no-pin] [--rt]\n");
      return false;
    }
  }
  a.core = static_cast<unsigned>(core);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return 1;
  std::printf("sigcalc: %llu streamed events, %llu-tick duel tape, window 256, seed %llu\n\n",
              static_cast<unsigned long long>(a.events),
              static_cast<unsigned long long>(a.tape),
              static_cast<unsigned long long>(a.seed));

  hft::TscClock::calibrate();
  if (a.pin && !hft::pin_current_thread(a.core)) std::puts("warn: pin failed");
  if (a.rt && !hft::boost_current_thread_priority()) std::puts("warn: rt priority failed");

  // ---- part 1: the hot path --------------------------------------------------
  {
    auto eng = std::make_unique<SignalEngine<256, true>>();
    MarketSim sim(a.seed);
    hft::LatencyHistogram h;
    u64 busy = 0;
    for (u64 i = 0; i < a.events; ++i) {
      const Tick t = sim.next();
      const u64 c0 = hft::rdtscp();
      eng->on_tick(t);
      const u64 c1 = hft::rdtscp();
      h.record(c1 - c0);
      busy += c1 - c0;
    }
    auto ns = [](u64 cyc) {
      return static_cast<unsigned long long>(hft::TscClock::to_ns(cyc));
    };
    const double secs = static_cast<double>(busy) / (hft::TscClock::cycles_per_ns() * 1e9);
    std::printf("hot path: full feature vector per event (ns, rdtscp pairs, ~20 ns floor)\n"
                "  count %llu | p50 %llu  p90 %llu  p99 %llu  p99.9 %llu  max %llu\n"
                "  engine-busy throughput %.1fM events/s | digest %016llx\n\n",
                static_cast<unsigned long long>(h.count()), ns(h.percentile(0.50)),
                ns(h.percentile(0.90)), ns(h.percentile(0.99)), ns(h.percentile(0.999)),
                ns(h.max()), static_cast<double>(a.events) / secs / 1e6,
                static_cast<unsigned long long>(eng->digest()));
  }

  // ---- part 2: scalar vs AVX2 -------------------------------------------------
  {
    std::vector<Tick> tape(a.tape);
    MarketSim sim(a.seed + 1);
    for (u64 i = 0; i < a.tape; ++i) tape[i] = sim.next();

    auto sca = std::make_unique<SignalEngine<256, false>>();
    auto avx = std::make_unique<SignalEngine<256, true>>();
    const double sca_ns = hft::bench_ns_per_op(a.tape, [&] {
      for (const Tick& t : tape) sca->on_tick(t);
      hft::bench_sink = static_cast<u64>(sca->features().score);
    });
    const double avx_ns = hft::bench_ns_per_op(a.tape, [&] {
      for (const Tick& t : tape) avx->on_tick(t);
      hft::bench_sink = static_cast<u64>(avx->features().score);
    });

    // Lattice alone: 8 EWMAs is the whole operation.
    EwmaLattice lat_s, lat_v;
    const u64 lat_ops = 32'000'000;
    const double lat_sca = hft::bench_ns_per_op(lat_ops, [&] {
      i64 x = static_cast<i64>(hft::bench_sink & 0xFFFF) + (400'000 << 16);
      for (u64 i = 0; i < lat_ops; ++i) {
        lat_s.update_scalar(x);
        x += static_cast<i64>((i * 0x9E3779B9u) & 0x1FF) - 256;
      }
      hft::bench_sink = static_cast<u64>(lat_s.slowest());
    });
    const double lat_avx = hft::bench_ns_per_op(lat_ops, [&] {
      i64 x = static_cast<i64>(hft::bench_sink & 0xFFFF) + (400'000 << 16);
      for (u64 i = 0; i < lat_ops; ++i) {
        lat_v.update_avx2(x);
        x += static_cast<i64>((i * 0x9E3779B9u) & 0x1FF) - 256;
      }
      hft::bench_sink = static_cast<u64>(lat_v.slowest());
    });

    const bool same = sca->digest() == avx->digest();
    std::printf("duel (ns/event, best of 5 x %lluM-tick tape):\n"
                "  full engine   scalar %6.2f | avx2 %6.2f  (%.2fx)\n"
                "  lattice only  scalar %6.2f | avx2 %6.2f  (%.2fx)   [32M updates]\n"
                "  backend digests %s\n\n",
                static_cast<unsigned long long>(a.tape / 1'000'000), sca_ns, avx_ns,
                sca_ns / avx_ns, lat_sca, lat_avx, lat_sca / lat_avx,
                same ? "EQUAL" : "DIVERGED");
    if (!same) return 1;
  }

  // ---- part 3: where the nanoseconds go ---------------------------------------
  {
    const u64 ops = 8'000'000;
    Rng rng(a.seed + 2);
    std::vector<i64> vals(1u << 16);
    for (auto& v : vals) v = static_cast<i64>(rng.below(2001)) - 1000;
    std::vector<Tick> tape(1u << 16);
    MarketSim sim(a.seed + 3);
    for (auto& t : tape) t = sim.next();

    RollingSum<256> rsum;
    const double sum_ns = hft::bench_ns_per_op(ops, [&] {
      i64 acc = 0;
      for (u64 i = 0; i < ops; ++i) acc += rsum.push(vals[i & 0xFFFF]);
      hft::bench_sink = static_cast<u64>(acc);
    });

    RollingExtrema<256> ext;
    const double ext_ns = hft::bench_ns_per_op(ops, [&] {
      i64 acc = 0;
      for (u64 i = 0; i < ops; ++i) {
        ext.push(vals[i & 0xFFFF] + static_cast<i64>(i & 0x3F));
        acc += ext.min() + ext.max();
      }
      hft::bench_sink = static_cast<u64>(acc);
    });

    const double mp_ns = hft::bench_ns_per_op(ops, [&] {
      i64 acc = 0;
      for (u64 i = 0; i < ops; ++i) {
        const Tick& t = tape[i & 0xFFFF];
        const i64 den = t.bid_qty + t.ask_qty;
        acc += ((t.bid_px * t.ask_qty + t.ask_px * t.bid_qty) << 16) / den +
               ((t.bid_qty - t.ask_qty) << 16) / den;
      }
      hft::bench_sink = static_cast<u64>(acc);
    });

    std::printf("parts (ns/op, best of 5 x 8M):\n"
                "  rolling sum push        %6.2f\n"
                "  wedge push + min + max  %6.2f\n"
                "  microprice + imbalance  %6.2f   (the two divides)\n",
                sum_ns, ext_ns, mp_ns);
  }
  return 0;
}
