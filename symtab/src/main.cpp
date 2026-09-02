// symtab harness. Three questions, three parts:
//
//   build  : what does constructing a minimal perfect hash over the whole
//            instrument universe cost at startup (and on every intraday
//            rebuild)?
//   lookup : what does resolving a wire symbol to a dense instrument id
//            cost, against the two honest baselines (the family's usual
//            open-addressing map, and std::unordered_map)? Measured three
//            ways: independent-stream throughput, a dependent chain (each
//            result feeds the next key, so lookups cannot overlap — true
//            latency), and the 16-wide prefetch-pipelined batch.
//   churn  : a writer relisting/adding symbols and swapping snapshots under
//            a reader doing full-rate lookups — what does the reader's tail
//            see, and does epoch reclamation actually free the snapshots?
//
// The lookup arrays are sized to be cache-resident (the real instrument
// master is: ~10k symbols = ~160 KB of slots), so the numbers isolate the
// algorithm, not DRAM.

#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hft/common/bench.hpp"
#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/tsc_clock.hpp"
#include "st/gen.hpp"
#include "st/mph.hpp"
#include "st/oa_map.hpp"
#include "st/table.hpp"

namespace {

using namespace st;

struct Args {
  u64 n = 8192;
  u64 lookups = 4'000'000;
  u64 swaps = 500;
  u64 swap_us = 1000;
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
      {"--n", &a.n},           {"--lookups", &a.lookups}, {"--swaps", &a.swaps},
      {"--swap-us", &a.swap_us}, {"--seed", &a.seed},     {"--core", &core},
  };
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
      std::printf("symtab [--n N] [--lookups N] [--swaps N] [--swap-us N] "
                  "[--seed N] [--core N] [--no-pin] [--rt]\n");
      return false;
    }
  }
  a.core = static_cast<unsigned>(core);
  return true;
}

using hft::bench_ns_per_op;
using hft::bench_sink;

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return 1;
  std::printf("symtab: universe %llu, %llu lookups/shape, %llu swaps @ %llu us, seed %llu\n\n",
              static_cast<unsigned long long>(a.n),
              static_cast<unsigned long long>(a.lookups),
              static_cast<unsigned long long>(a.swaps),
              static_cast<unsigned long long>(a.swap_us),
              static_cast<unsigned long long>(a.seed));

  hft::TscClock::calibrate();
  if (a.pin && !hft::pin_current_thread(a.core)) std::puts("warn: pin failed");
  if (a.rt && !hft::boost_current_thread_priority()) std::puts("warn: rt priority failed");

  const auto uni = gen_universe(a.n, a.seed);
  std::vector<Sym> keys;
  std::vector<u32> ids;
  for (const auto& ins : uni) {
    keys.push_back(ins.sym);
    ids.push_back(ins.id);
  }

  // ---- part 1: build --------------------------------------------------------
  Mph mph;
  {
    const u64 c0 = hft::rdtscp();
    if (!mph.build(keys, ids)) {
      std::puts("mph build FAILED");
      return 1;
    }
    const u64 c1 = hft::rdtscp();
    const auto& st = mph.stats();
    std::printf("build: %llu keys -> %llu slots (load 1.00), %llu buckets\n"
                "  %.2f ms | max bucket %u | displacement seeds: max %u, avg %.2f | "
                "attempt %u | digest %016llx\n\n",
                static_cast<unsigned long long>(a.n),
                static_cast<unsigned long long>(mph.size()),
                static_cast<unsigned long long>(mph.buckets()),
                hft::TscClock::to_ns(c1 - c0) / 1e6, st.max_bucket, st.max_seed,
                static_cast<double>(st.total_tries) / static_cast<double>(mph.buckets()),
                st.attempts, static_cast<unsigned long long>(mph.digest()));
  }

  OaMap oa;
  oa.build(keys, ids);
  std::unordered_map<u64, u32> um;
  um.reserve(a.n);
  for (u64 i = 0; i < a.n; ++i) um.emplace(keys[i].v, ids[i]);

  // ---- part 2: lookup shapes -------------------------------------------------
  {
    // Hit stream: random members. Miss stream: same shape, never members.
    Rng rng(a.seed + 1);
    std::vector<Sym> hits(a.lookups), misses(a.lookups);
    for (u64 i = 0; i < a.lookups; ++i) hits[i] = keys[rng.below(a.n)];
    for (u64 i = 0; i < a.lookups; ++i) {
      char buf[8];  // 7-letter tickers: valid shape, never generated members
      for (int j = 0; j < 7; ++j) buf[j] = static_cast<char>('A' + rng.below(26));
      misses[i] = Sym::from({buf, 7});
    }
    // Small cache-resident member pool for the dependent chain.
    std::vector<Sym> pool(4096);
    for (u64 i = 0; i < 4096; ++i) pool[i] = keys[rng.below(a.n)];

    auto through = [&](auto&& look, const std::vector<Sym>& stream) {
      return bench_ns_per_op(a.lookups, [&] {
        u64 sum = 0;
        for (const Sym& k : stream) sum += look(k);
        bench_sink = sum;
      });
    };
    auto chained = [&](auto&& look) {
      return bench_ns_per_op(a.lookups, [&] {
        u64 idx = 0, sum = 0;
        for (u64 i = 0; i < a.lookups; ++i) {
          const u64 id = look(pool[idx]);
          sum += id;
          idx = (idx + id + 1) & 4095;  // next key depends on this result
        }
        bench_sink = sum;
      });
    };

    auto mph_look = [&](Sym k) { return static_cast<u64>(mph.lookup(k)); };
    auto oa_look = [&](Sym k) { return static_cast<u64>(oa.lookup(k)); };
    auto um_look = [&](Sym k) {
      const auto it = um.find(k.v);
      return it == um.end() ? u64{Mph::kMiss} : u64{it->second};
    };

    std::printf("lookup (ns/op, best of 5 rounds of %lluM):\n"
                "                         mph      oa map   unordered_map\n",
                static_cast<unsigned long long>(a.lookups / 1'000'000));
    std::printf("  throughput, hits   %7.2f   %7.2f   %10.2f\n",
                through(mph_look, hits), through(oa_look, hits), through(um_look, hits));
    std::printf("  throughput, misses %7.2f   %7.2f   %10.2f\n",
                through(mph_look, misses), through(oa_look, misses),
                through(um_look, misses));
    std::printf("  dependent chain    %7.2f   %7.2f   %10.2f\n", chained(mph_look),
                chained(oa_look), chained(um_look));

    const double batch_ns = bench_ns_per_op(a.lookups, [&] {
      const u64 off = bench_sink & 15;  // round-dependent start: defeats cross-round CSE
      u64 sum = 0;
      u32 out[16];
      for (u64 i = off; i + 16 <= a.lookups; i += 16) {
        mph.lookup_batch<16>(&hits[i], out);
        for (int j = 0; j < 16; ++j) sum += out[j];
      }
      bench_sink = sum;
    });
    std::printf("  batch<16> prefetch %7.2f      -           -\n\n", batch_ns);
  }

  // ---- part 3: snapshot churn under load ------------------------------------
  {
    SymTable tab;
    if (!tab.load(uni)) {
      std::puts("table load FAILED");
      return 1;
    }
    std::atomic<bool> stop{false};
    std::atomic<u64> reader_misses{0};
    hft::LatencyHistogram reader_h;  // cycles per 64-lookup pinned batch

    std::thread reader([&] {
      if (a.pin) hft::pin_current_thread(a.core + 1);
      if (a.rt) hft::boost_current_thread_priority();
      auto& rd = tab.register_reader();
      Rng rng(a.seed + 2);
      std::vector<Sym> pool(4096);
      for (u64 i = 0; i < 4096; ++i) pool[i] = keys[rng.below(a.n)];
      u64 idx = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const u64 c0 = hft::rdtscp();
        const Snapshot* s = tab.pin(rd);
        u64 sum = 0;
        for (int i = 0; i < 64; ++i) {
          const u32 id = s->mph.lookup(pool[idx]);
          if (id == Mph::kMiss) reader_misses.fetch_add(1);
          sum += id;
          idx = (idx + 1) & 4095;
        }
        tab.unpin(rd);
        const u64 c1 = hft::rdtscp();
        bench_sink = sum;
        reader_h.record(c1 - c0);
      }
    });

    hft::LatencyHistogram swap_h;  // cycles per rebuild+publish
    const u64 gap_cycles = hft::TscClock::ns_to_cycles(a.swap_us * 1000);
    u64 next = hft::rdtscp() + gap_cycles;
    for (u64 i = 0; i < a.swaps; ++i) {
      while (hft::rdtscp() < next) hft::cpu_relax();
      next += gap_cycles;
      Instrument ins;
      char name[8] = {'Z', 'Z', 'Z', static_cast<char>('A' + (i / 676) % 26),
                      static_cast<char>('A' + (i / 26) % 26),
                      static_cast<char>('A' + i % 26), 0, 0};
      ins.sym = Sym::from({name, 6});
      ins.sym.v |= u64{0x7A} << 56;  // 8th byte 'z': outside the generator alphabet
      const u64 c0 = hft::rdtscp();
      if (!tab.add(ins)) {
        std::puts("intraday add FAILED");
        return 1;
      }
      const u64 c1 = hft::rdtscp();
      swap_h.record(c1 - c0);
    }
    stop.store(true);
    reader.join();
    tab.reclaim();

    auto ns = [](u64 cyc) {
      return static_cast<unsigned long long>(hft::TscClock::to_ns(cyc));
    };
    std::printf("churn: %llu intraday adds (clone + rebuild + publish) under a full-rate reader\n",
                static_cast<unsigned long long>(a.swaps));
    std::printf("  writer rebuild+publish (us): p50 %llu  p99 %llu  max %llu\n",
                ns(swap_h.percentile(0.50)) / 1000, ns(swap_h.percentile(0.99)) / 1000,
                ns(swap_h.max()) / 1000);
    std::printf("  reader 64-lookup batch (ns): p50 %llu  p99 %llu  p99.9 %llu  max %llu | "
                "batches %llu\n",
                ns(reader_h.percentile(0.50)), ns(reader_h.percentile(0.99)),
                ns(reader_h.percentile(0.999)), ns(reader_h.max()),
                static_cast<unsigned long long>(reader_h.count()));
    const bool ok = reader_misses.load() == 0 && tab.retired_pending() == 0 &&
                    tab.freed() == a.swaps;
    std::printf("  reader misses %llu | snapshots freed %llu/%llu, pending %llu -> %s\n",
                static_cast<unsigned long long>(reader_misses.load()),
                static_cast<unsigned long long>(tab.freed()),
                static_cast<unsigned long long>(a.swaps),
                static_cast<unsigned long long>(tab.retired_pending()),
                ok ? "OK" : "VIOLATED");
    return ok ? 0 : 1;
  }
}
