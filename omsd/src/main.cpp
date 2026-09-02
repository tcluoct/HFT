// omsd harness. Two parts, mirroring how a real OMS is judged:
//
//   hot path : millions of simulated request/report events through the state
//              engine on one pinned core, rdtscp pairs around every event —
//              what does keeping the firm's order state cost per message?
//              (auto_prune on: terminal orders recycle, like an intraday OMS)
//   recon    : a 1M-order day with injected primary-path damage (dropped
//              reports, duplicated reports, alien drop-copy orders), then
//              end-of-day drop-copy reconciliation — how fast can you prove
//              your book against the venue's, and does it find EXACTLY the
//              injected damage? (auto_prune off: recon needs the corpses)
//
// Determinism is asserted at the end: the recon run is replayed from the
// same seed through a fresh engine and must produce an identical digest.

#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/tsc_clock.hpp"
#include "om/core/oms.hpp"
#include "om/core/recon.hpp"
#include "om/sim/venue_sim.hpp"

namespace {

using namespace om;

struct Args {
  u64 hot_orders = 3'000'000;
  u64 recon_orders = 1'000'000;
  u64 drop_pm = 5;
  u64 dup_pm = 3;
  u64 aliens = 100;
  u64 seed = 42;
  unsigned core = 2;
  bool pin = true;
  bool rt = false;
};

bool parse_u64(const char* s, u64& v) {
  const auto r = std::from_chars(s, s + std::strlen(s), v);
  return r.ec == std::errc{} && *r.ptr == '\0';
}

void usage() {
  std::printf(
      "omsd [options]\n"
      "  --hot N       hot-path orders                  (default 3000000)\n"
      "  --recon N     reconciliation-day orders        (default 1000000)\n"
      "  --drop-pm N   primary reports dropped, /1000   (default 5)\n"
      "  --dup-pm N    primary reports duplicated, /1000(default 3)\n"
      "  --aliens N    drop-copy-only phantom orders    (default 100)\n"
      "  --seed N / --core N / --no-pin / --rt\n");
}

bool parse_args(int argc, char** argv, Args& a) {
  u64 core = a.core;
  struct Opt {
    const char* name;
    u64* dst;
  };
  const Opt opts[] = {
      {"--hot", &a.hot_orders}, {"--recon", &a.recon_orders}, {"--drop-pm", &a.drop_pm},
      {"--dup-pm", &a.dup_pm},  {"--aliens", &a.aliens},      {"--seed", &a.seed},
      {"--core", &core},
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
  a.core = static_cast<unsigned>(core);
  return true;
}

void print_row(const char* label, const hft::LatencyHistogram& h) {
  auto ns = [](u64 cyc) {
    return static_cast<unsigned long long>(hft::TscClock::to_ns(cyc));
  };
  std::printf("  %-26s %10llu %8llu %8llu %8llu %8llu %9llu\n", label,
              static_cast<unsigned long long>(h.count()), ns(h.percentile(0.50)),
              ns(h.percentile(0.90)), ns(h.percentile(0.99)), ns(h.percentile(0.999)),
              ns(h.max()));
}

u64 g_last_digest = 0;

void print_counters(const Counters& c) {
  std::printf(
      "  verdicts: apply %llu | race %llu | heal %llu | stale %llu | venue-err %llu\n"
      "  dup execs dropped %llu | late intents %llu | unknown reports %llu\n",
      static_cast<unsigned long long>(c.verdicts[unsigned(Verdict::kApply)]),
      static_cast<unsigned long long>(c.verdicts[unsigned(Verdict::kRace)]),
      static_cast<unsigned long long>(c.verdicts[unsigned(Verdict::kHeal)]),
      static_cast<unsigned long long>(c.verdicts[unsigned(Verdict::kStale)]),
      static_cast<unsigned long long>(c.verdicts[unsigned(Verdict::kVenueErr)]),
      static_cast<unsigned long long>(c.dup_execs),
      static_cast<unsigned long long>(c.late_intent),
      static_cast<unsigned long long>(c.unknown_order));
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return 1;
  std::printf("omsd: hot %llu orders | recon %llu orders, drops %llu/1000, dups %llu/1000, "
              "aliens %llu, seed %llu\n\n",
              static_cast<unsigned long long>(a.hot_orders),
              static_cast<unsigned long long>(a.recon_orders),
              static_cast<unsigned long long>(a.drop_pm),
              static_cast<unsigned long long>(a.dup_pm),
              static_cast<unsigned long long>(a.aliens),
              static_cast<unsigned long long>(a.seed));

  hft::TscClock::calibrate();
  if (a.pin && !hft::pin_current_thread(a.core)) std::puts("warn: pin failed");
  if (a.rt && !hft::boost_current_thread_priority()) std::puts("warn: rt priority failed");

  int rc = 0;

  // ---- part 1: the hot path -----------------------------------------------
  {
    SimCfg cfg;
    cfg.orders = a.hot_orders;
    cfg.seed = a.seed;
    VenueSim sim(cfg, false, false);
    auto oms = std::make_unique<Oms<1u << 21, 1024>>();
    // Stragglers arrive at most maxlife + 2*wire after an order goes
    // terminal; twice that is a comfortable grace period.
    oms->prune_grace_ns = 2 * (cfg.maxlife_ns + 2 * cfg.wire_ns);

    hft::LatencyHistogram req_h, rep_h;
    u64 busy = 0, peak_open = 0;
    run_tape(sim, cfg, [&](const TapeItem& it) {
      const u64 t0 = hft::rdtscp();
      if (it.is_req) oms->on_request(it.req, it.t);
      else oms->on_report(it.rep, it.t);
      const u64 t1 = hft::rdtscp();
      (it.is_req ? req_h : rep_h).record(t1 - t0);
      busy += t1 - t0;
      if (oms->open_orders() > peak_open) peak_open = oms->open_orders();
    });

    const u64 events = req_h.count() + rep_h.count();
    const double secs = static_cast<double>(busy) / (hft::TscClock::cycles_per_ns() * 1e9);
    std::printf("hot path: %llu events (%llu requests, %llu reports), grace-pruned\n",
                static_cast<unsigned long long>(events),
                static_cast<unsigned long long>(req_h.count()),
                static_cast<unsigned long long>(rep_h.count()));
    std::printf("  per event (ns, rdtscp-bracketed; ~20 ns floor)   count      p50      p90      p99    p99.9       max\n");
    print_row("request (new/cxl/rpl)", req_h);
    print_row("report (ack/fill/...)", rep_h);
    std::printf("  engine-busy throughput %.2fM events/s | peak open orders %llu\n",
                static_cast<double>(events) / secs / 1e6,
                static_cast<unsigned long long>(peak_open));
    print_counters(oms->counters());
    const bool clean = oms->counters().verdicts[unsigned(Verdict::kVenueErr)] == 0 &&
                       oms->counters().verdicts[unsigned(Verdict::kHeal)] == 0 &&
                       oms->counters().unknown_order == 0;
    const bool audit = oms->audit_open_interest();
    std::printf("  invariants: clean venue %s | open-interest audit %s\n\n",
                clean ? "OK" : "VIOLATED", audit ? "OK" : "VIOLATED");
    if (!clean || !audit) rc = 1;
  }

  // ---- part 2: the reconciliation day ---------------------------------------
  {
    SimCfg cfg;
    cfg.orders = a.recon_orders;
    cfg.seed = a.seed + 1;
    cfg.drop_pm = static_cast<u32>(a.drop_pm);
    cfg.dup_pm = static_cast<u32>(a.dup_pm);
    cfg.alien_orders = a.aliens;

    auto run_day = [&](u64* digest_out) {
      VenueSim sim(cfg, true, true);
      auto oms = std::make_unique<Oms<1u << 21, 1024>>();
      run_tape(sim, cfg, [&](const TapeItem& it) {
        if (it.is_req) oms->on_request(it.req);
        else oms->on_report(it.rep);
      });
      sim.generate_aliens();
      if (digest_out != nullptr) {  // determinism replay: digest only
        *digest_out = oms->digest();
        return true;
      }

      // Fold the drop-copy, then reconcile — both timed.
      auto recon = std::make_unique<Recon<1u << 21>>();
      const u64 c0 = hft::rdtscp();
      for (const ExecReport& e : sim.dropcopy()) recon->on_dropcopy(e);
      const u64 c1 = hft::rdtscp();
      std::vector<u8> flagged(cfg.orders + 1, 0);
      u64 phantom_cb = 0;
      const Findings f = recon->compare(*oms, [&](u64 cl_id, u32) {
        if (cl_id > cfg.orders) ++phantom_cb;
        else flagged[cl_id] = 1;
      });
      const u64 c2 = hft::rdtscp();

      u64 false_pos = 0, missed_fills = 0, dropped_orders = 0, dropped_fill_orders = 0;
      for (u64 id = 1; id <= cfg.orders; ++id) {
        const Truth& t = sim.truth()[id - 1];
        dropped_orders += t.dropped_any;
        dropped_fill_orders += t.dropped_fill;
        if (flagged[id] && !t.dropped_any) ++false_pos;
        if (t.dropped_fill && !flagged[id]) ++missed_fills;
      }

      std::printf("recon day: %llu orders, %llu drop-copy reports\n",
                  static_cast<unsigned long long>(cfg.orders),
                  static_cast<unsigned long long>(sim.dropcopy().size()));
      std::printf("  injected: %llu dropped primary reports (%llu orders, %llu losing fills), "
                  "%llu dups, %llu aliens\n",
                  static_cast<unsigned long long>(sim.dropped()),
                  static_cast<unsigned long long>(dropped_orders),
                  static_cast<unsigned long long>(dropped_fill_orders),
                  static_cast<unsigned long long>(sim.duped()),
                  static_cast<unsigned long long>(cfg.alien_orders));
      std::printf("  fold %.1f ms (%.1fM reports/s) + compare %.1f ms = %.1f ms end-to-end\n",
                  hft::TscClock::to_ns(c1 - c0) / 1e6,
                  static_cast<double>(sim.dropcopy().size()) /
                      (static_cast<double>(hft::TscClock::to_ns(c1 - c0)) / 1e9) / 1e6,
                  hft::TscClock::to_ns(c2 - c1) / 1e6, hft::TscClock::to_ns(c2 - c0) / 1e6);
      std::printf("  findings: %llu flagged (%llu state, %llu cum, %llu qty) + %llu phantom | "
                  "stuck %llu\n",
                  static_cast<unsigned long long>(f.flagged_orders),
                  static_cast<unsigned long long>(f.state_mismatch),
                  static_cast<unsigned long long>(f.cum_mismatch),
                  static_cast<unsigned long long>(f.qty_mismatch),
                  static_cast<unsigned long long>(f.phantom),
                  static_cast<unsigned long long>(f.stuck));
      print_counters(oms->counters());

      const bool exact = false_pos == 0 && missed_fills == 0 && f.phantom == cfg.alien_orders &&
                         f.stuck == 0 && oms->counters().dup_execs == sim.duped();
      std::printf("  exactness: false accusations %llu | lost fills missed %llu | "
                  "phantoms %llu/%llu | dups absorbed %llu/%llu -> %s\n",
                  static_cast<unsigned long long>(false_pos),
                  static_cast<unsigned long long>(missed_fills),
                  static_cast<unsigned long long>(f.phantom),
                  static_cast<unsigned long long>(cfg.alien_orders),
                  static_cast<unsigned long long>(oms->counters().dup_execs),
                  static_cast<unsigned long long>(sim.duped()), exact ? "OK" : "VIOLATED");
      if (digest_out == nullptr) g_last_digest = oms->digest();
      return exact;
    };

    if (!run_day(nullptr)) rc = 1;

    u64 replay_digest = 0;
    run_day(&replay_digest);
    const bool det = replay_digest == g_last_digest;
    std::printf("  determinism replay: %s (digest %016llx)\n", det ? "PASS" : "FAIL",
                static_cast<unsigned long long>(replay_digest));
    if (!det) rc = 1;
  }

  return rc;
}
