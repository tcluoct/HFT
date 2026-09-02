// ticklab harness: three subcommands.
//
//   record    generate a synthetic session and capture it (write-path bench)
//   replay    as-fast decode bench + time-seek bench, or paced 1x/Nx replay
//             with a delivery-jitter histogram
//   backtest  run the strategy against the capture across a latency sweep --
//             the "alpha decays with latency" table -- plus determinism proof

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "tl/bt/strategy.hpp"
#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/tsc_clock.hpp"
#include "tl/data/recorder.hpp"
#include "tl/data/replayer.hpp"
#include "tl/sim/session_gen.hpp"

namespace {

using namespace tl;

bool parse_u64(const char* s, std::uint64_t& v) {
  const auto r = std::from_chars(s, s + std::strlen(s), v);
  return r.ec == std::errc{} && *r.ptr == '\0';
}

std::string comma(std::uint64_t v) {
  char raw[24];
  const auto r = std::to_chars(raw, raw + sizeof raw, v);
  std::string s(raw, r.ptr);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
  return s;
}

void usage() {
  std::printf(
      "ticklab record   --out FILE [--ticks N=10000000] [--symbols K=1] [--seed S=42]\n"
      "ticklab replay   --in FILE [--speed X]   (no --speed: as-fast decode + seek bench)\n"
      "ticklab backtest --in FILE [--latency-ns L] [--fee F]   (no L: latency sweep)\n"
      "common: [--rt] [--no-pin] [--core N=2]\n");
}

// ---------------------------------------------------------------- record ---
int cmd_record(int argc, char** argv) {
  const char* out = nullptr;
  std::uint64_t ticks = 10'000'000, nsym = 1, seed = 42;
  for (int i = 2; i < argc; ++i) {
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (!std::strcmp(argv[i], "--out")) out = next();
    else if (!std::strcmp(argv[i], "--ticks")) { const char* v = next(); if (!v || !parse_u64(v, ticks)) return 1; }
    else if (!std::strcmp(argv[i], "--symbols")) { const char* v = next(); if (!v || !parse_u64(v, nsym) || nsym < 1 || nsym > 64) return 1; }
    else if (!std::strcmp(argv[i], "--seed")) { const char* v = next(); if (!v || !parse_u64(v, seed)) return 1; }
  }
  if (!out) { usage(); return 1; }

  SessionGen gen(seed, static_cast<unsigned>(nsym), 10'000);
  Recorder rec(out, static_cast<std::uint16_t>(nsym));
  if (!rec.ok()) { std::printf("cannot open %s for writing\n", out); return 1; }

  const auto w0 = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < ticks; ++i) rec.add(gen.next());
  rec.close();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();

  const std::uint64_t bytes = rec.file_bytes();
  const double bpt = static_cast<double>(bytes) / static_cast<double>(ticks);
  std::printf("recorded %s ticks -> %s (%.1f MB) in %.3f s\n", comma(ticks).c_str(), out,
              static_cast<double>(bytes) / 1e6, secs);
  std::printf("  write rate  : %s ticks/s, %.0f MB/s (generate+encode+write)\n",
              comma(static_cast<std::uint64_t>(static_cast<double>(ticks) / secs)).c_str(),
              static_cast<double>(bytes) / 1e6 / secs);
  std::printf("  bytes/tick  : %.2f columnar-FOR vs %zu raw row  =  %.1fx compression\n", bpt,
              kRawRowBytes, static_cast<double>(kRawRowBytes) / bpt);
  return 0;
}

// ---------------------------------------------------------------- replay ---
int cmd_replay(int argc, char** argv) {
  const char* in = nullptr;
  double speed = 0;  // 0 = as-fast
  for (int i = 2; i < argc; ++i) {
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (!std::strcmp(argv[i], "--in")) in = next();
    else if (!std::strcmp(argv[i], "--speed")) { const char* v = next(); if (!v) return 1; speed = std::strtod(v, nullptr); }
  }
  if (!in) { usage(); return 1; }

  Replayer r;
  if (!r.open(in)) { std::printf("cannot open capture %s\n", in); return 1; }
  std::printf("capture: %s ticks, %zu chunks, %.1f MB, %u symbol(s), %.2f bytes/tick\n",
              comma(r.total_ticks()).c_str(), r.chunk_count(),
              static_cast<double>(r.file_bytes()) / 1e6, r.nsym(),
              static_cast<double>(r.file_bytes()) / static_cast<double>(r.total_ticks()));

  Tick t;
  if (speed <= 0) {
    // As-fast decode: two passes (first touches every page, second is warm).
    std::uint64_t checksum = 0, last_ts = 0;
    for (int pass = 1; pass <= 2; ++pass) {
      r.rewind();
      std::uint64_t n = 0;
      const auto w0 = std::chrono::steady_clock::now();
      while (r.next(t)) {
        checksum += t.ts_ns + static_cast<std::uint64_t>(t.px1) + t.q1;
        last_ts = t.ts_ns;
        ++n;
      }
      const double secs =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
      std::printf("  pass %d: %s ticks in %.3f s = %s ticks/s (%.0f MB/s decoded)\n", pass,
                  comma(n).c_str(), secs,
                  comma(static_cast<std::uint64_t>(static_cast<double>(n) / secs)).c_str(),
                  static_cast<double>(r.file_bytes()) / 1e6 / secs);
    }
    // Random time-seek bench across the session.
    std::uint64_t s = 0x243F6A8885A308D3ull, found = 0;
    const std::uint64_t kSeeks = 100'000;
    const auto w0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < kSeeks; ++i) {
      s ^= s << 13; s ^= s >> 7; s ^= s << 17;  // xorshift
      const std::uint64_t target = s % (last_ts + 1);
      if (r.seek_ts(target) && r.next(t)) { checksum += t.ts_ns; ++found; }
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    std::printf("  seek: %s random time-seeks in %.3f s = %s seeks/s (found %s)\n",
                comma(kSeeks).c_str(), secs,
                comma(static_cast<std::uint64_t>(static_cast<double>(kSeeks) / secs)).c_str(),
                comma(found).c_str());
    std::printf("  checksum %016llx\n", static_cast<unsigned long long>(checksum));
    return 0;
  }

  // Paced replay: deliver tick i at wall time (ts_i - ts_0)/speed after start,
  // spinning between deliveries; histogram of delivery lateness.
  hft::LatencyHistogram jitter;
  r.rewind();
  if (!r.next(t)) return 0;
  const std::uint64_t ts0 = t.ts_ns;
  const std::uint64_t c0 = hft::rdtsc();
  std::uint64_t n = 1, checksum = t.ts_ns;
  while (r.next(t)) {
    const std::uint64_t target =
        c0 + hft::TscClock::ns_to_cycles(
                 static_cast<std::uint64_t>(static_cast<double>(t.ts_ns - ts0) / speed));
    std::uint64_t now = hft::rdtsc();
    while (now < target) {
      hft::cpu_relax();
      now = hft::rdtsc();
    }
    jitter.record(hft::TscClock::to_ns(now - target));
    checksum += t.ts_ns;
    ++n;
  }
  const auto ns = [](std::uint64_t cyc) {
    return static_cast<unsigned long long>(cyc);
  };
  std::printf("  paced %.1fx: %s ticks delivered | lateness ns p50 %llu p99 %llu p99.9 %llu max %llu\n",
              speed, comma(n).c_str(), ns(jitter.percentile(0.50)), ns(jitter.percentile(0.99)),
              ns(jitter.percentile(0.999)), ns(jitter.max()));
  std::printf("  checksum %016llx\n", static_cast<unsigned long long>(checksum));
  return 0;
}

// -------------------------------------------------------------- backtest ---
int cmd_backtest(int argc, char** argv) {
  const char* in = nullptr;
  std::uint64_t latency = ~std::uint64_t{0};
  double fee = 0.0;
  for (int i = 2; i < argc; ++i) {
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (!std::strcmp(argv[i], "--in")) in = next();
    else if (!std::strcmp(argv[i], "--latency-ns")) { const char* v = next(); if (!v || !parse_u64(v, latency)) return 1; }
    else if (!std::strcmp(argv[i], "--fee")) { const char* v = next(); if (!v) return 1; fee = std::strtod(v, nullptr); }
  }
  if (!in) { usage(); return 1; }

  Replayer r;
  if (!r.open(in)) { std::printf("cannot open capture %s\n", in); return 1; }
  std::printf("backtest: %s ticks, %u symbol(s), fee %.4f ticks/share, fills capped at displayed size\n\n",
              comma(r.total_ticks()).c_str(), r.nsym(), fee);

  const std::uint64_t sweep[] = {0, 1'000, 5'000, 20'000, 100'000, 500'000};
  const bool single = latency != ~std::uint64_t{0};
  const std::uint64_t* lats = single ? &latency : sweep;
  const std::size_t nlat = single ? 1 : sizeof(sweep) / sizeof(sweep[0]);

  std::printf("latency      sent    fills  fill%%      volume        net PnL   PnL/fill  maxDD    sharpe\n");
  std::printf("------------------------------------------------------------------------------------------\n");
  double bench_secs = 0;
  std::uint64_t bench_ticks = 0;
  for (std::size_t i = 0; i < nlat; ++i) {
    r.rewind();
    ImbalanceTaker strat;
    BtContext::Params p;
    p.latency_ns = lats[i];
    p.fee_per_share = fee;
    p.nsym = r.nsym();
    const BtStats s = run_backtest(r, strat, p);
    std::printf("%7s us %8s %8s  %5.1f %11s %14.0f %10.2f %8.0f %8.2f\n",
                comma(lats[i] / 1000).c_str(), comma(s.sent).c_str(), comma(s.fills).c_str(),
                s.sent ? 100.0 * static_cast<double>(s.fills) / static_cast<double>(s.sent) : 0.0,
                comma(s.volume).c_str(), s.net_pnl,
                s.fills ? s.net_pnl / static_cast<double>(s.fills) : 0.0, s.max_dd, s.sharpe);
    bench_secs = s.secs;
    bench_ticks = s.ticks;
  }
  std::printf("\n(PnL in tick*share units: with a $0.01 tick, 10,000 = $100)\n");
  std::printf("backtest engine rate: %s ticks/s through strategy+fills\n",
              comma(static_cast<std::uint64_t>(static_cast<double>(bench_ticks) / bench_secs)).c_str());

  // Determinism: same capture + same latency twice => identical fill digests.
  BtContext::Params p;
  p.latency_ns = 20'000;
  p.fee_per_share = fee;
  p.nsym = r.nsym();
  r.rewind();
  ImbalanceTaker s1;
  const BtStats a = run_backtest(r, s1, p);
  r.rewind();
  ImbalanceTaker s2;
  const BtStats b = run_backtest(r, s2, p);
  const bool ok = a.digest == b.digest && a.net_pnl == b.net_pnl;
  std::printf("determinism: %s (fill digest %016llx %s %016llx, pnl %.2f vs %.2f)\n",
              ok ? "PASS" : "FAIL", static_cast<unsigned long long>(a.digest), ok ? "==" : "!=",
              static_cast<unsigned long long>(b.digest), a.net_pnl, b.net_pnl);
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 1;
  }
  bool rt = false, pin = true;
  std::uint64_t core = 2;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--rt")) rt = true;
    else if (!std::strcmp(argv[i], "--no-pin")) pin = false;
    else if (!std::strcmp(argv[i], "--core") && i + 1 < argc) parse_u64(argv[i + 1], core);
  }
  hft::TscClock::calibrate();
  if (pin) hft::pin_current_thread(static_cast<unsigned>(core));
  if (rt) hft::boost_current_thread_priority();

  if (!std::strcmp(argv[1], "record")) return cmd_record(argc, argv);
  if (!std::strcmp(argv[1], "replay")) return cmd_replay(argc, argv);
  if (!std::strcmp(argv[1], "backtest")) return cmd_backtest(argc, argv);
  usage();
  return 1;
}
