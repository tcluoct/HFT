// feedarb harness: streams tens of millions of impaired dual-line arrivals
// through the arbitrator on a pinned thread. The sim timeline (ns) drives
// loss/reorder/gap logic; real rdtscp pairs bracket each on_packet+poll to
// measure what arbitration itself costs. Generation is batched with a carry
// buffer so the arrival stream never has to fit in memory.

#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/tsc_clock.hpp"
#include "fa/core/arbiter.hpp"
#include "fa/sim/line_sim.hpp"

namespace {

using namespace fa;

struct Args {
  std::uint64_t seqs = 20'000'000;
  std::uint64_t warmup = 2'000'000;  // arrivals, not seqs
  std::uint64_t seed = 42;
  std::uint64_t drop_pm = 20;     // each line loses 2%
  std::uint64_t dup_pm = 10;      // 1% network dups
  std::uint64_t delay_a = 5'000;  // ns
  std::uint64_t delay_b = 7'000;  // ns (B path is 2us longer, like a real B line)
  std::uint64_t jitter = 2'000;   // ns
  std::uint64_t send_gap = 100;   // ns between venue sends (10M pkt/s)
  std::uint64_t timeout = 50'000; // ns before a hole is declared dead
  unsigned core = 2;
  bool pin = true;
  bool rt = false;
};

bool parse_u64(const char* s, std::uint64_t& v) {
  const auto r = std::from_chars(s, s + std::strlen(s), v);
  return r.ec == std::errc{} && *r.ptr == '\0';
}

void usage() {
  std::printf(
      "feedarb [options]\n"
      "  --seqs N       venue sequence numbers to send  (default 20000000)\n"
      "  --warmup N     warmup arrivals                 (default 2000000)\n"
      "  --seed N       impairment RNG seed             (default 42)\n"
      "  --drop-pm N    per-line loss, per mille        (default 20)\n"
      "  --dup-pm N     per-line duplication, per mille (default 10)\n"
      "  --delay-a N    line A propagation ns           (default 5000)\n"
      "  --delay-b N    line B propagation ns           (default 7000)\n"
      "  --jitter N     per-line jitter ns              (default 2000)\n"
      "  --send-gap N   venue inter-packet ns           (default 100)\n"
      "  --timeout N    gap declaration ns              (default 50000)\n"
      "  --core N / --no-pin / --rt\n");
}

bool parse_args(int argc, char** argv, Args& a) {
  struct Opt {
    const char* name;
    std::uint64_t* dst;
  };
  const Opt opts[] = {
      {"--seqs", &a.seqs},       {"--warmup", &a.warmup},   {"--seed", &a.seed},
      {"--drop-pm", &a.drop_pm}, {"--dup-pm", &a.dup_pm},   {"--delay-a", &a.delay_a},
      {"--delay-b", &a.delay_b}, {"--jitter", &a.jitter},   {"--send-gap", &a.send_gap},
      {"--timeout", &a.timeout},
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
    if (!std::strcmp(s, "--core")) {
      std::uint64_t n{};
      if (i + 1 >= argc || !parse_u64(argv[++i], n)) {
        usage();
        return false;
      }
      a.core = static_cast<unsigned>(n);
      continue;
    }
    bool hit = false;
    for (const auto& o : opts)
      if (!std::strcmp(s, o.name)) {
        if (i + 1 >= argc || !parse_u64(argv[++i], *o.dst)) {
          usage();
          return false;
        }
        hit = true;
        break;
      }
    if (!hit) {
      usage();
      return false;
    }
  }
  return true;
}

// The bench sink verifies every delivery and keeps only invariant counters —
// nothing here that a real book-builder wouldn't also have to do.
struct BenchSink {
  std::uint64_t delivered = 0;
  std::uint64_t bad_payload = 0;
  std::uint64_t out_of_order = 0;
  std::uint64_t last_seq = 0;

  void on_packet(const MdPacket& p) noexcept {
    bad_payload += (p.payload != mix64(p.seq));
    out_of_order += (p.seq <= last_seq);
    last_seq = p.seq;
    ++delivered;
  }
  void on_gap(std::uint64_t, std::uint64_t) noexcept {}
};

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return 1;

  std::printf(
      "feedarb: %llu seqs, loss %llu/1000 + dups %llu/1000 per line, "
      "A %lluns / B %lluns (+/-%lluns), send gap %lluns, gap timeout %lluns\n",
      static_cast<unsigned long long>(a.seqs), static_cast<unsigned long long>(a.drop_pm),
      static_cast<unsigned long long>(a.dup_pm), static_cast<unsigned long long>(a.delay_a),
      static_cast<unsigned long long>(a.delay_b), static_cast<unsigned long long>(a.jitter),
      static_cast<unsigned long long>(a.send_gap), static_cast<unsigned long long>(a.timeout));

  hft::TscClock::calibrate();
  if (a.pin && !hft::pin_current_thread(a.core)) std::puts("warn: pin failed");
  if (a.rt && !hft::boost_current_thread_priority()) std::puts("warn: rt priority failed");

  BenchSink sink;
  auto arb = std::make_unique<Arbiter<BenchSink, 1024>>(sink, a.timeout);
  LineSim sim(a.seed, {static_cast<std::uint32_t>(a.drop_pm), static_cast<std::uint32_t>(a.dup_pm), a.delay_a, a.jitter},
              {static_cast<std::uint32_t>(a.drop_pm), static_cast<std::uint32_t>(a.dup_pm), a.delay_b, a.jitter},
              a.send_gap);

  hft::LatencyHistogram lat;
  std::uint64_t fed = 0, busy_cycles = 0;

  constexpr std::uint64_t kBatch = 1'000'000;  // seqs per generation batch
  std::vector<Arrival> ev, carry;
  ev.reserve(2 * kBatch + (kBatch / 4));

  for (std::uint64_t first = 1; first <= a.seqs; first += kBatch) {
    const std::uint64_t last = std::min(first + kBatch, a.seqs + 1);
    ev.clear();
    ev.insert(ev.end(), carry.begin(), carry.end());
    carry.clear();
    sim.generate(first, last, ev);

    // Arrivals later than this can still be beaten by the next batch's
    // earliest send + min delay; hold them back.
    const std::uint64_t safe = last * a.send_gap;
    std::size_t i = 0;
    const std::uint64_t c0 = hft::rdtscp();
    for (; i < ev.size() && ev[i].when <= safe; ++i) {
      const Arrival& e = ev[i];
      const std::uint64_t t0 = hft::rdtscp();
      arb->on_packet(e.pkt, e.when);
      arb->poll(e.when);
      const std::uint64_t t1 = hft::rdtscp();
      if (fed >= a.warmup) lat.record(t1 - t0);
      ++fed;
    }
    busy_cycles += hft::rdtscp() - c0;
    carry.assign(ev.begin() + i, ev.end());
  }
  for (const Arrival& e : carry) {  // tail
    const std::uint64_t t0 = hft::rdtscp();
    arb->on_packet(e.pkt, e.when);
    arb->poll(e.when);
    busy_cycles += hft::rdtscp() - t0;
    lat.record(hft::rdtscp() - t0);
    ++fed;
  }
  std::uint64_t t_end = (a.seqs + 1) * a.send_gap;
  while (arb->state() == ArbState::kGapPending) {  // close trailing holes
    t_end += a.timeout + 1;
    arb->poll(t_end);
  }

  const auto& st = arb->stats();
  const double secs = static_cast<double>(busy_cycles) /
                      (hft::TscClock::cycles_per_ns() * 1e9);
  auto ns = [](std::uint64_t cyc) { return hft::TscClock::to_ns(cyc); };

  std::printf("\narbitrated %llu arrivals in %.2fs -> %.2fM pkt/s sustained\n",
              static_cast<unsigned long long>(fed), secs,
              static_cast<double>(fed) / secs / 1e6);
  std::printf("per-arrival cost (ns, rdtscp-bracketed; ~20 ns floor on this box)\n");
  std::printf("  p50 %llu  p90 %llu  p99 %llu  p99.9 %llu  max %llu\n",
              static_cast<unsigned long long>(ns(lat.percentile(0.50))),
              static_cast<unsigned long long>(ns(lat.percentile(0.90))),
              static_cast<unsigned long long>(ns(lat.percentile(0.99))),
              static_cast<unsigned long long>(ns(lat.percentile(0.999))),
              static_cast<unsigned long long>(ns(lat.max())));

  for (int l = 0; l < 2; ++l) {
    const LineStats& x = st.line[l];
    std::printf("line %c: received %llu, wins %llu (%.1f%%), dups %llu, stales %llu\n",
                'A' + l, static_cast<unsigned long long>(x.received),
                static_cast<unsigned long long>(x.firsts),
                100.0 * static_cast<double>(x.firsts) / static_cast<double>(st.delivered + st.gap_seqs),
                static_cast<unsigned long long>(x.dups),
                static_cast<unsigned long long>(x.stales));
  }
  std::printf("inter-line skew for raced seqs (sim ns): p50 %llu  p99 %llu\n",
              static_cast<unsigned long long>(arb->skew().percentile(0.5)),
              static_cast<unsigned long long>(arb->skew().percentile(0.99)));
  std::printf("delivered %llu | gaps %llu events / %llu seqs | window jumps %llu\n",
              static_cast<unsigned long long>(st.delivered),
              static_cast<unsigned long long>(st.gap_events),
              static_cast<unsigned long long>(st.gap_seqs),
              static_cast<unsigned long long>(st.window_jumps));
  std::printf("invariants: delivered+lost==%llu (%s) | out-of-order %llu | bad payload %llu\n",
              static_cast<unsigned long long>(a.seqs),
              (st.delivered + st.gap_seqs == a.seqs) ? "OK" : "VIOLATED",
              static_cast<unsigned long long>(sink.out_of_order),
              static_cast<unsigned long long>(sink.bad_payload));
  return (st.delivered + st.gap_seqs == a.seqs && !sink.out_of_order && !sink.bad_payload) ? 0 : 1;
}
