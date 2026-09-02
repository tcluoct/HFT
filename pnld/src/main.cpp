// pnld harness. Three parts:
//
//   hot path : 10M fills (plus a mark every 8) across 1024 symbols through
//              the full Keeper — fill cost includes both attributions and
//              the firm rollup deltas. rdtscp pairs per event.
//   churn    : the two structural worst cases in isolation, bulk-timed —
//              flip churn (every fill crosses zero: close + flip + open)
//              and a single fill draining 100k lots (the FIFO walk).
//   proof    : end-of-run audits — incremental firm aggregates vs cold
//              recompute, FIFO equity == WAC equity == cash + position
//              value (the identity), digest replay.

#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>

#include "hft/common/bench.hpp"
#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/tsc_clock.hpp"
#include "pl/keeper.hpp"
#include "pl/sim.hpp"

namespace {

using namespace pl;

struct Args {
  u64 fills = 10'000'000;
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
  const Opt opts[] = {{"--fills", &a.fills}, {"--seed", &a.seed}, {"--core", &core}};
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
      std::printf("pnld [--fills N] [--seed N] [--core N] [--no-pin] [--rt]\n");
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
  std::printf("pnld: %llu fills, 1024 symbols, mark every 8 fills, seed %llu\n\n",
              static_cast<unsigned long long>(a.fills),
              static_cast<unsigned long long>(a.seed));

  hft::TscClock::calibrate();
  if (a.pin && !hft::pin_current_thread(a.core)) std::puts("warn: pin failed");
  if (a.rt && !hft::boost_current_thread_priority()) std::puts("warn: rt priority failed");

  int rc = 0;
  constexpr u32 kSyms = 1024;

  // ---- part 1: the hot path --------------------------------------------------
  u64 first_digest = 0;
  {
    auto keeper = std::make_unique<Keeper<kSyms, 2048>>();
    FlowSim<kSyms> sim(a.seed);
    hft::LatencyHistogram fill_h, mark_h;
    i64 truth_cash = 0;
    u64 busy = 0, max_lots = 0;
    for (u64 i = 0; i < a.fills; ++i) {
      const Fill f = sim.next_fill();
      truth_cash += -f.side * f.qty * f.px - f.fee;
      const u64 c0 = hft::rdtscp();
      keeper->on_fill(f);
      const u64 c1 = hft::rdtscp();
      fill_h.record(c1 - c0);
      busy += c1 - c0;
      if (i % 8 == 0) {
        const Mark m = sim.mark_of(static_cast<u32>(sim.rand_below(kSyms)));
        const u64 m0 = hft::rdtscp();
        keeper->on_mark(m);
        const u64 m1 = hft::rdtscp();
        mark_h.record(m1 - m0);
        busy += m1 - m0;
      }
      if (keeper->book(f.sym).lot_count() > max_lots)
        max_lots = keeper->book(f.sym).lot_count();
    }

    auto ns = [](u64 cyc) {
      return static_cast<unsigned long long>(hft::TscClock::to_ns(cyc));
    };
    const double secs = static_cast<double>(busy) / (hft::TscClock::cycles_per_ns() * 1e9);
    std::printf("hot path (ns, rdtscp pairs, ~20 ns floor)   count      p50   p90   p99  p99.9     max\n");
    std::printf("  fill (both attributions + rollup) %10llu   %6llu %5llu %5llu %6llu %7llu\n",
                static_cast<unsigned long long>(fill_h.count()),
                ns(fill_h.percentile(0.50)), ns(fill_h.percentile(0.90)),
                ns(fill_h.percentile(0.99)), ns(fill_h.percentile(0.999)),
                ns(fill_h.max()));
    std::printf("  mark (O(1) firm delta)             %10llu   %6llu %5llu %5llu %6llu %7llu\n",
                static_cast<unsigned long long>(mark_h.count()),
                ns(mark_h.percentile(0.50)), ns(mark_h.percentile(0.90)),
                ns(mark_h.percentile(0.99)), ns(mark_h.percentile(0.999)),
                ns(mark_h.max()));
    std::printf("  engine-busy throughput %.1fM events/s | peak lots on one book %llu\n",
                static_cast<double>(fill_h.count() + mark_h.count()) / secs / 1e6,
                static_cast<unsigned long long>(max_lots));

    // ---- part 3 (proof) uses this run's end state ----------------------------
    i64 pos_value = 0;
    for (u32 s = 0; s < kSyms; ++s)
      pos_value += keeper->book(s).pos() * keeper->book(s).mark();
    const bool rollup = keeper->audit(true);
    const bool identity = keeper->firm_fifo_equity() == keeper->firm_wac_equity() &&
                          keeper->firm_fifo_equity() == truth_cash + pos_value;
    std::printf("  firm equity (tick-shares): fifo %lld == wac %lld == cash+posval %lld\n",
                static_cast<long long>(keeper->firm_fifo_equity()),
                static_cast<long long>(keeper->firm_wac_equity()),
                static_cast<long long>(truth_cash + pos_value));
    std::printf("  realized split: fifo %lld / wac %lld (attribution differs, equity identical)\n",
                static_cast<long long>(keeper->firm_fifo_realized()),
                static_cast<long long>(keeper->firm_wac_realized()));
    std::printf("  invariants: rollup+lots audit %s | equity identity %s | digest %016llx\n\n",
                rollup ? "OK" : "VIOLATED", identity ? "OK" : "VIOLATED",
                static_cast<unsigned long long>(keeper->digest()));
    if (!rollup || !identity) rc = 1;
    first_digest = keeper->digest();
  }

  // ---- part 2: structural worst cases -----------------------------------------
  {
    // Flip churn: every fill closes the whole position and reopens the other
    // way — close + flip + open on every single event.
    auto b = std::make_unique<PositionBook<64>>();
    const u64 flips = 2'000'000;
    const double flip_ns = hft::bench_ns_per_op(flips, [&] {
      i32 side = 1;
      for (u64 i = 0; i < flips; ++i) {
        b->on_fill(side, 600, 100'000 + static_cast<i64>(i & 0xFF), 1);
        side = -side;
      }
      hft::bench_sink = static_cast<u64>(b->fifo_realized());
    });

    // Deep drain: 100k one-share lots consumed by a single opposite fill.
    auto big = std::make_unique<PositionBook<1u << 17>>();
    for (i64 i = 0; i < 100'000; ++i) big->on_fill(1, 1, 90'000 + (i % 977), 0);
    const u64 d0 = hft::rdtscp();
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const unsigned drained = big->on_fill(-1, 100'000, 95'000, 0);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const u64 d1 = hft::rdtscp();
    const bool drain_ok = drained == 100'000 && big->pos() == 0 && big->audit();

    std::printf("worst cases:\n"
                "  flip churn (close+flip+open per fill)  %.1f ns/fill\n"
                "  100k-lot drain in one fill             %.2f ms total, %.1f ns/lot -> %s\n\n",
                flip_ns, hft::TscClock::to_ns(d1 - d0) / 1e6,
                static_cast<double>(hft::TscClock::to_ns(d1 - d0)) / 100'000.0,
                drain_ok ? "OK" : "VIOLATED");
    if (!drain_ok) rc = 1;
  }

  // ---- determinism replay ------------------------------------------------------
  {
    auto keeper = std::make_unique<Keeper<kSyms, 2048>>();
    FlowSim<kSyms> sim(a.seed);
    for (u64 i = 0; i < a.fills; ++i) {
      keeper->on_fill(sim.next_fill());
      if (i % 8 == 0) keeper->on_mark(sim.mark_of(static_cast<u32>(sim.rand_below(kSyms))));
    }
    const bool det = keeper->digest() == first_digest;
    std::printf("determinism replay: %s (digest %016llx)\n", det ? "PASS" : "FAIL",
                static_cast<unsigned long long>(keeper->digest()));
    if (!det) rc = 1;
  }
  return rc;
}
