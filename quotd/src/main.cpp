// quotd harness. Four parts:
//
//   hot path : ns per on_tick (desired ladder + diff against working) with
//              the sim's fills landing between ticks — fenced rdtscp pairs.
//   skew     : the inventory experiment. Same seed, same informed flow;
//              skew on vs off. Skew shifts the loaded side out of
//              aggressors' reach, so it throttles accumulation — inventory
//              stddev and worst excursion drop, equity keeps the spread.
//   band     : the op-budget experiment. Hysteresis band 0..4 ticks: how
//              many venue ops does a tick of tolerance save, and what does
//              it cost in equity?
//   pull     : epoch mass-pull cycles (pull + resume + requote), bulk ns.
//              Plus the determinism replay.

#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>

#include "hft/common/bench.hpp"
#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/tsc_clock.hpp"
#include "qt/engine.hpp"
#include "qt/sim.hpp"

namespace {

using namespace qt;

struct Args {
  u64 steps = 5'000'000;
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
  const Opt opts[] = {{"--steps", &a.steps}, {"--seed", &a.seed}, {"--core", &core}};
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
      std::printf("quotd [--steps N] [--seed N] [--core N] [--no-pin] [--rt]\n");
      return false;
    }
  }
  a.core = static_cast<unsigned>(core);
  return true;
}

Cfg base_cfg() {
  Cfg c;
  c.half_spread = 2;
  c.level_step = 1;
  c.size = 100;
  c.levels = 4;
  c.band = 1;
  c.inv_bucket = 200;
  c.max_skew = 6;
  c.max_inv = 2000;
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return 1;
  std::printf("quotd: %llu sim steps, 4 levels/side, seed %llu\n\n",
              static_cast<unsigned long long>(a.steps),
              static_cast<unsigned long long>(a.seed));

  hft::TscClock::calibrate();
  if (a.pin && !hft::pin_current_thread(a.core)) std::puts("warn: pin failed");
  if (a.rt && !hft::boost_current_thread_priority()) std::puts("warn: rt priority failed");

  int rc = 0;

  // ---- part 1: the hot path (sim loop with timed on_tick) ----------------------
  {
    QuoteEngine eng(base_cfg());
    Rng rng(a.seed);
    Op buf[QuoteEngine::kMaxOps];
    hft::LatencyHistogram h;
    i64 fv = 100'000;
    i32 last_dir = 1;
    u64 ops = 0, quiet = 0;
    for (u64 t = 0; t < a.steps; ++t) {
      const u64 c0 = hft::rdtscp();
      std::atomic_signal_fence(std::memory_order_seq_cst);
      const unsigned n = eng.on_tick(fv, buf);
      std::atomic_signal_fence(std::memory_order_seq_cst);
      const u64 c1 = hft::rdtscp();
      h.record(c1 - c0);
      ops += n;
      quiet += n == 0;

      i32 move = 0;
      if (rng.below(100) < 30) move = rng.below(100) < 70 ? last_dir : -last_dir;
      if (rng.below(1000) < 300) {
        const bool informed = rng.below(100) < 60;
        const bool agg_sell = (informed && move != 0) ? move < 0 : rng.below(2) == 0;
        const u8 side = agg_sell ? 0 : 1;
        const i64 reach = 2 + static_cast<i64>(rng.below(3));
        for (u8 lvl = 0; lvl < eng.cfg().levels; ++lvl) {
          if (!eng.live(side, lvl)) continue;
          const i64 px = eng.px(side, lvl);
          if (side == 0 ? px >= fv - reach : px <= fv + reach)
            eng.on_fill(side, lvl, 25 + static_cast<i64>(rng.below(150)));
          break;
        }
      }
      fv += move;
      if (move != 0) last_dir = move;
    }
    auto ns = [](u64 cyc) {
      return static_cast<unsigned long long>(hft::TscClock::to_ns(cyc));
    };
    std::printf("hot path: on_tick = desired ladder + diff (ns, ~20 ns floor)\n"
                "  p50 %llu  p90 %llu  p99 %llu  p99.9 %llu  max %llu\n"
                "  venue ops %.1f/1k ticks | zero-op ticks %.1f%% | fills %llu (stale %llu)\n\n",
                ns(h.percentile(0.50)), ns(h.percentile(0.90)), ns(h.percentile(0.99)),
                ns(h.percentile(0.999)), ns(h.max()),
                1000.0 * static_cast<double>(ops) / static_cast<double>(a.steps),
                100.0 * static_cast<double>(quiet) / static_cast<double>(a.steps),
                static_cast<unsigned long long>(eng.fills()),
                static_cast<unsigned long long>(eng.stale_fills()));
  }

  // ---- part 2: the skew experiment ----------------------------------------------
  {
    std::printf("skew experiment: 2M steps, identical informed flow (60%%)\n");
    for (const i64 skew : {i64{0}, i64{6}}) {
      Cfg c = base_cfg();
      c.max_skew = skew;
      QuoteEngine eng(c);
      MMSim sim(a.seed + 1);
      const SimStats st = sim.run(eng, 2'000'000);
      std::printf("  skew %s : inventory std %6.0f, worst %5lld | equity %9lld | fills %llu\n",
                  skew ? "on " : "off", st.inv_std, static_cast<long long>(st.max_abs_inv),
                  static_cast<long long>(st.equity),
                  static_cast<unsigned long long>(st.fills));
    }
    std::printf("\n");
  }

  // ---- part 3: the hysteresis-band experiment -------------------------------------
  {
    std::printf("band experiment: 2M steps each, ops vs equity\n");
    for (const i64 band : {i64{0}, i64{1}, i64{2}, i64{4}}) {
      Cfg c = base_cfg();
      c.band = band;
      QuoteEngine eng(c);
      MMSim sim(a.seed + 2);
      const SimStats st = sim.run(eng, 2'000'000);
      std::printf("  band %lld : %7.1f ops/1k ticks | equity %9lld | fills %llu\n",
                  static_cast<long long>(band),
                  1000.0 * static_cast<double>(st.news + st.cancels) /
                      static_cast<double>(st.steps),
                  static_cast<long long>(st.equity),
                  static_cast<unsigned long long>(st.fills));
    }
    std::printf("\n");
  }

  // ---- part 4: epoch pull cycles + determinism -------------------------------------
  {
    QuoteEngine eng(base_cfg());
    Op buf[QuoteEngine::kMaxOps];
    eng.on_tick(100'000, buf);
    const u64 cycles = 1'000'000;
    const double pull_ns = hft::bench_ns_per_op(cycles, [&] {
      u64 acc = hft::bench_sink & 1;
      for (u64 i = 0; i < cycles; ++i) {
        acc += eng.pull_all(buf);      // O(1) decision + 8 cancels
        eng.resume();
        acc += eng.on_tick(100'000 + static_cast<i64>(i & 1), buf);  // requote 8
      }
      hft::bench_sink = acc;
    });
    std::printf("epoch pull: full pull+resume+requote cycle %.0f ns (8 cancels + 8 news)\n",
                pull_ns);

    auto run = [&] {
      QuoteEngine e(base_cfg());
      MMSim sim(a.seed + 3);
      sim.run(e, 1'000'000);
      return e.digest();
    };
    const u64 d1 = run(), d2 = run();
    std::printf("determinism replay: %s (digest %016llx)\n", d1 == d2 ? "PASS" : "FAIL",
                static_cast<unsigned long long>(d1));
    if (d1 != d2) rc = 1;
  }
  return rc;
}
