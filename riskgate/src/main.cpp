// riskgate harness: streams synthetic order flow through the gate on a
// pinned thread, histograms per-check latency separately for the accept and
// reject paths, and (optionally) hammers the limits table from a control
// thread while measuring — the hot-reload cost is a measured number here,
// not a claim.

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/tsc_clock.hpp"
#include "rg/core/gate.hpp"

namespace {

using namespace rg;

constexpr std::size_t kMaxSymbols = 4096;

struct Args {
  std::uint64_t orders = 20'000'000;
  std::uint64_t warmup = 2'000'000;
  std::uint64_t seed = 42;
  unsigned nsym = 256;
  unsigned core = 2;
  unsigned reject_pct = 5;   // % of flow generated out-of-band
  unsigned reload_us = 0;    // 0 = no control-plane republishing
  bool pin = true;
  bool rt = false;
};

bool parse_u64(const char* s, std::uint64_t& v) {
  const auto r = std::from_chars(s, s + std::strlen(s), v);
  return r.ec == std::errc{} && *r.ptr == '\0';
}

void usage() {
  std::printf(
      "riskgate [options]\n"
      "  --orders N      measured checks               (default 20000000)\n"
      "  --warmup N      warmup checks                 (default 2000000)\n"
      "  --seed N        flow RNG seed                 (default 42)\n"
      "  --symbols N     symbols in play, 1..%zu      (default 256)\n"
      "  --reject-pct N  %% of flow priced out of band  (default 5)\n"
      "  --reload-us N   republish limits every N us from a control thread\n"
      "                  while measuring (0 = off)     (default 0)\n"
      "  --core N        core for the gate thread      (default 2)\n"
      "  --no-pin / --rt\n",
      kMaxSymbols);
}

bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const char* s = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    std::uint64_t n{};
    if (!std::strcmp(s, "--orders")) {
      const char* v = next();
      if (!v || !parse_u64(v, a.orders)) return false;
    } else if (!std::strcmp(s, "--warmup")) {
      const char* v = next();
      if (!v || !parse_u64(v, a.warmup)) return false;
    } else if (!std::strcmp(s, "--seed")) {
      const char* v = next();
      if (!v || !parse_u64(v, a.seed)) return false;
    } else if (!std::strcmp(s, "--symbols")) {
      const char* v = next();
      if (!v || !parse_u64(v, n) || n < 1 || n > kMaxSymbols) return false;
      a.nsym = static_cast<unsigned>(n);
    } else if (!std::strcmp(s, "--reject-pct")) {
      const char* v = next();
      if (!v || !parse_u64(v, n) || n > 100) return false;
      a.reject_pct = static_cast<unsigned>(n);
    } else if (!std::strcmp(s, "--reload-us")) {
      const char* v = next();
      if (!v || !parse_u64(v, n)) return false;
      a.reload_us = static_cast<unsigned>(n);
    } else if (!std::strcmp(s, "--core")) {
      const char* v = next();
      if (!v || !parse_u64(v, n)) return false;
      a.core = static_cast<unsigned>(n);
    } else if (!std::strcmp(s, "--no-pin")) {
      a.pin = false;
    } else if (!std::strcmp(s, "--rt")) {
      a.rt = true;
    } else {
      usage();
      return false;
    }
  }
  return true;
}

// xorshift64*: one mul + shifts per draw, deterministic across runs.
struct Rng {
  std::uint64_t s;
  std::uint64_t next() noexcept {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1Dull;
  }
};

SymbolLimits bench_limits() {
  return SymbolLimits{
      .max_order_qty = 1'000,
      .max_order_notional = 50'000'000,
      .band_lo = 9'000,
      .band_hi = 11'000,
      .max_long_pos = 2'000'000'000,  // wide: the run measures the check, not the walk
      .max_short_pos = 2'000'000'000,
      .enabled = 1,
      .pad_ = 0,
  };
}

void print_hist(const char* name, const hft::LatencyHistogram& h) {
  auto ns = [](std::uint64_t cyc) { return hft::TscClock::to_ns(cyc); };
  std::printf("%-14s %9llu checks   p50 %5llu  p90 %5llu  p99 %5llu  p99.9 %6llu  max %9llu ns\n",
              name, static_cast<unsigned long long>(h.count()),
              static_cast<unsigned long long>(ns(h.percentile(0.50))),
              static_cast<unsigned long long>(ns(h.percentile(0.90))),
              static_cast<unsigned long long>(ns(h.percentile(0.99))),
              static_cast<unsigned long long>(ns(h.percentile(0.999))),
              static_cast<unsigned long long>(ns(h.max())));
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return 1;

  std::printf("riskgate: %llu checks (%llu warmup), %u symbols, %u%% out-of-band, reload %s\n",
              static_cast<unsigned long long>(a.orders),
              static_cast<unsigned long long>(a.warmup), a.nsym, a.reject_pct,
              a.reload_us ? "on" : "off");

  hft::TscClock::calibrate();

  auto table = std::make_unique<LimitsTable<kMaxSymbols>>();
  const SymbolLimits base = bench_limits();
  for (unsigned s = 0; s < a.nsym; ++s) table->publish(s, base);

  auto gate = std::make_unique<RiskGate<kMaxSymbols>>(*table);
  // Wire budget wide open: the throttle refill math still runs on every
  // check (it's part of the cost being measured), it just never rejects.
  gate->throttle().configure(1e12, 1'000'000, hft::TscClock::cycles_per_ns());

  // Optional control plane: republish one random symbol's limits every
  // reload_us microseconds, band nudged ±5 ticks (harmless to the flow).
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> reloads{0};
  std::thread control;
  if (a.reload_us) {
    control = std::thread([&] {
      hft::pin_current_thread(a.core + 1);
      Rng rng{a.seed ^ 0x9E3779B97F4A7C15ull};
      const auto period = std::chrono::microseconds(a.reload_us);
      auto next_pub = std::chrono::steady_clock::now();
      while (!stop.load(std::memory_order_relaxed)) {
        next_pub += period;
        while (std::chrono::steady_clock::now() < next_pub) hft::cpu_relax();
        auto l = base;
        const auto r = rng.next();
        l.band_lo = base.band_lo - static_cast<std::int64_t>(r % 6);
        l.band_hi = base.band_hi + static_cast<std::int64_t>((r >> 8) % 6);
        table->publish(static_cast<SymbolId>((r >> 16) % a.nsym), l);
        reloads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  if (a.pin && !hft::pin_current_thread(a.core)) std::puts("warn: pin failed");
  if (a.rt && !hft::boost_current_thread_priority()) std::puts("warn: rt priority failed");

  hft::LatencyHistogram accept_h, reject_h;
  std::uint64_t reject_counts[16] = {};
  std::uint64_t accepts = 0, rejects = 0;

  Rng rng{a.seed};
  const std::uint64_t total = a.warmup + a.orders;
  for (std::uint64_t i = 0; i < total; ++i) {
    const std::uint64_t r = rng.next();
    OrderReq o;
    o.symbol = static_cast<SymbolId>(r % a.nsym);
    o.side = static_cast<Side>((r >> 20) & 1);
    o.qty = 1 + ((r >> 24) % 100);
    const bool bad = ((r >> 32) % 100) < a.reject_pct;
    o.price = bad ? (base.band_hi + 1 + static_cast<std::int64_t>((r >> 40) % 100))
                  : (base.band_lo + static_cast<std::int64_t>((r >> 40) % 2'001));

    const std::uint64_t t0 = hft::rdtscp();
    const std::uint32_t m = gate->check(o, t0);
    const std::uint64_t t1 = hft::rdtscp();

    if (i >= a.warmup) {
      (m ? reject_h : accept_h).record(t1 - t0);
      if (m) {
        ++rejects;
        for (unsigned b = 0; b < 16; ++b)
          if (m & (1u << b)) ++reject_counts[b];
      } else {
        ++accepts;
      }
    }
  }

  stop.store(true);
  if (control.joinable()) control.join();

  std::puts("\nper-check latency (ns, rdtscp-bracketed; ~20 ns floor on this box)");
  print_hist("accept path", accept_h);
  if (reject_h.count()) print_hist("reject path", reject_h);

  static constexpr const char* kBitNames[16] = {
      "kill_switch", "unknown_symbol", "qty_zero",      "qty_too_large",
      "notional",    "band_low",       "band_high",     "position_long",
      "position_short", "throttled",   "?", "?", "?", "?", "?", "?"};
  std::printf("\naccepted %llu, rejected %llu\n", static_cast<unsigned long long>(accepts),
              static_cast<unsigned long long>(rejects));
  for (unsigned b = 0; b < 16; ++b)
    if (reject_counts[b])
      std::printf("  %-15s %llu\n", kBitNames[b],
                  static_cast<unsigned long long>(reject_counts[b]));

  std::printf("seqlock retries: %llu, limit reloads while measuring: %llu\n",
              static_cast<unsigned long long>(gate->seqlock_retries()),
              static_cast<unsigned long long>(reloads.load()));
  return 0;
}
