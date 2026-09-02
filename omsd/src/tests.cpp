// Scenario tests for the OMS. Every crossing the verdict matrix claims to
// handle is constructed here explicitly — fill-vs-cancel, replace-vs-fill,
// heals for lost acks, venue contract violations, duplicate reports — and
// each asserts both the resulting state AND the exposure accounting, because
// an OMS that gets the state right but leaks open interest is still wrong.
// Then the sim-driven sweeps: brute-force audit of the exposure invariant,
// digest determinism, and drop-copy reconciliation that must find exactly
// the injected discrepancies. No test framework — a CHECK macro.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "om/core/oms.hpp"
#include "om/core/recon.hpp"
#include "om/core/types.hpp"
#include "om/sim/venue_sim.hpp"

using namespace om;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_fail;                                                     \
    }                                                               \
  } while (0)

using SmallOms = Oms<1u << 10, 16>;

u64 g_exec = 0;

Request rq_new(u64 id, u32 qty, u8 side = 0, u32 sym = 1) {
  Request r{};
  r.cl_id = id;
  r.qty = qty;
  r.px = 1000;
  r.sym = sym;
  r.type = ReqType::kNew;
  r.side = side;
  return r;
}
Request rq_cxl(u64 id) {
  Request r{};
  r.cl_id = id;
  r.type = ReqType::kCancel;
  return r;
}
Request rq_rpl(u64 id, u32 qty) {
  Request r{};
  r.cl_id = id;
  r.qty = qty;
  r.type = ReqType::kReplace;
  return r;
}
ExecReport rp(u64 id, Ev ev, u32 qty = 0) {
  ExecReport e{};
  e.cl_id = id;
  e.exec_id = ++g_exec;
  e.qty = qty;
  e.px = 1000;
  e.ev = ev;
  return e;
}

void t_happy_fill() {
  std::puts("lifecycle: new -> ack -> partial fill -> fill -> Filled");
  SmallOms oms;
  oms.on_request(rq_new(1, 300));
  CHECK(oms.find(1)->st == St::kPendingNew);
  CHECK(oms.book(1).open_buy == 300);  // exposure from the moment of intent
  oms.on_report(rp(1, Ev::kAckNew, 300));
  CHECK(oms.find(1)->st == St::kLive);
  CHECK(oms.book(1).open_buy == 300);
  oms.on_report(rp(1, Ev::kFill, 100));
  CHECK(oms.find(1)->st == St::kLive && oms.find(1)->cum == 100);
  CHECK(oms.book(1).open_buy == 200 && oms.book(1).position == 100);
  oms.on_report(rp(1, Ev::kFill, 200));
  CHECK(oms.find(1)->st == St::kFilled);
  CHECK(oms.book(1).open_buy == 0 && oms.book(1).position == 300);
  CHECK(oms.audit_open_interest());
  CHECK(oms.counters().verdicts[unsigned(Verdict::kVenueErr)] == 0);
}

void t_cancel_path() {
  std::puts("lifecycle: cancel releases exposure");
  SmallOms oms;
  oms.on_request(rq_new(1, 500, /*side=*/1));
  oms.on_report(rp(1, Ev::kAckNew, 500));
  oms.on_request(rq_cxl(1));
  CHECK(oms.find(1)->st == St::kPendingCancel);
  CHECK(oms.book(1).open_sell == 500);  // still reserved while in flight
  oms.on_report(rp(1, Ev::kAckCancel));
  CHECK(oms.find(1)->st == St::kCanceled);
  CHECK(oms.book(1).open_sell == 0);
  CHECK(oms.audit_open_interest());
}

void t_replace_conservative_exposure() {
  std::puts("replace: exposure holds max(old,new) leaves until resolved");
  SmallOms oms;
  oms.on_request(rq_new(1, 300));
  oms.on_report(rp(1, Ev::kAckNew, 300));
  oms.on_request(rq_rpl(1, 600));  // upsize
  CHECK(oms.find(1)->st == St::kPendingReplace);
  CHECK(oms.book(1).open_buy == 600);  // conservative: the venue may say yes
  oms.on_report(rp(1, Ev::kAckReplace, 600));
  CHECK(oms.find(1)->st == St::kLive && oms.find(1)->qty == 600);
  CHECK(oms.book(1).open_buy == 600);

  oms.on_request(rq_rpl(1, 100));  // downsize: old qty still possible
  CHECK(oms.book(1).open_buy == 600);
  oms.on_report(rp(1, Ev::kAckReplace, 100));
  CHECK(oms.book(1).open_buy == 100 && oms.find(1)->qty == 100);
  CHECK(oms.audit_open_interest());
}

void t_fill_vs_cancel_race() {
  std::puts("race: fill completes while our cancel is in flight");
  SmallOms oms;
  oms.on_request(rq_new(1, 200));
  oms.on_report(rp(1, Ev::kAckNew, 200));
  oms.on_request(rq_cxl(1));
  oms.on_report(rp(1, Ev::kFill, 200));  // the venue filled before canceling
  CHECK(oms.find(1)->st == St::kFilled);
  CHECK(oms.book(1).position == 200 && oms.book(1).open_buy == 0);
  oms.on_report(rp(1, Ev::kRejCancel));  // "too late" — stale, no effect
  CHECK(oms.find(1)->st == St::kFilled);
  CHECK(oms.counters().verdicts[unsigned(Verdict::kRace)] >= 1);
  CHECK(oms.counters().verdicts[unsigned(Verdict::kStale)] >= 1);
  CHECK(oms.counters().verdicts[unsigned(Verdict::kVenueErr)] == 0);
}

void t_replace_vs_fill_race() {
  std::puts("race: order fills to OLD qty while replace is in flight");
  SmallOms oms;
  oms.on_request(rq_new(1, 300));
  oms.on_report(rp(1, Ev::kAckNew, 300));
  oms.on_request(rq_rpl(1, 600));
  oms.on_report(rp(1, Ev::kFill, 300));  // old qty exhausted at the venue
  // Not Filled yet: the venue might still say yes to 600.
  CHECK(oms.find(1)->st == St::kPendingReplace);
  CHECK(oms.book(1).open_buy == 300);  // 600 possible - 300 done
  oms.on_report(rp(1, Ev::kRejReplace));  // venue: order was already gone
  CHECK(oms.find(1)->st == St::kFilled);
  CHECK(oms.book(1).open_buy == 0);
  CHECK(oms.audit_open_interest());
}

void t_replace_then_more_fills() {
  std::puts("race: replace acked mid-flow, fills continue to new qty");
  SmallOms oms;
  oms.on_request(rq_new(1, 300));
  oms.on_report(rp(1, Ev::kAckNew, 300));
  oms.on_request(rq_rpl(1, 500));
  oms.on_report(rp(1, Ev::kFill, 200));
  oms.on_report(rp(1, Ev::kAckReplace, 500));
  CHECK(oms.find(1)->st == St::kLive && oms.find(1)->qty == 500);
  oms.on_report(rp(1, Ev::kFill, 300));
  CHECK(oms.find(1)->st == St::kFilled && oms.find(1)->cum == 500);
  CHECK(oms.book(1).open_buy == 0 && oms.book(1).position == 500);
}

void t_heal_lost_ack() {
  std::puts("heal: fill before ack-new implies the ack was dropped");
  SmallOms oms;
  oms.on_request(rq_new(1, 200));
  oms.on_report(rp(1, Ev::kFill, 50));  // never saw AckNew
  CHECK(oms.find(1)->st == St::kLive && oms.find(1)->cum == 50);
  CHECK(oms.counters().verdicts[unsigned(Verdict::kHeal)] == 1);
  CHECK(oms.book(1).open_buy == 150 && oms.book(1).position == 50);

  oms.on_request(rq_new(2, 100));
  oms.on_report(rp(2, Ev::kAckCancel));  // cancel-ack while pending-new
  CHECK(oms.find(2)->st == St::kCanceled);
  CHECK(oms.book(1).open_buy == 150);  // order 2's reserve fully released
  CHECK(oms.audit_open_interest());
}

void t_venue_contract_violations() {
  std::puts("venue errors: fill-after-cancel and overfill are never applied");
  SmallOms oms;
  oms.on_request(rq_new(1, 100));
  oms.on_report(rp(1, Ev::kAckNew, 100));
  oms.on_request(rq_cxl(1));
  oms.on_report(rp(1, Ev::kAckCancel));
  oms.on_report(rp(1, Ev::kFill, 100));  // fill AFTER cancel-ack
  CHECK(oms.find(1)->st == St::kCanceled);
  CHECK(oms.book(1).position == 0);  // the bogus fill moved nothing
  CHECK(oms.counters().verdicts[unsigned(Verdict::kVenueErr)] == 1);
  CHECK(oms.counters().overfill_qty == 100);

  oms.on_request(rq_new(2, 100));
  oms.on_report(rp(2, Ev::kAckNew, 100));
  oms.on_report(rp(2, Ev::kFill, 100));
  oms.on_report(rp(2, Ev::kFill, 50));  // overfill
  CHECK(oms.find(2)->st == St::kFilled && oms.find(2)->cum == 100);
  CHECK(oms.book(1).position == 100);
  CHECK(oms.counters().verdicts[unsigned(Verdict::kVenueErr)] == 2);
}

void t_dup_exec_dropped() {
  std::puts("dedup: a duplicated exec report is applied once");
  SmallOms oms;
  oms.on_request(rq_new(1, 100));
  oms.on_report(rp(1, Ev::kAckNew, 100));
  ExecReport fill = rp(1, Ev::kFill, 60);
  oms.on_report(fill);
  oms.on_report(fill);  // network dup: same exec_id
  CHECK(oms.find(1)->cum == 60);
  CHECK(oms.counters().dup_execs == 1);
  CHECK(oms.book(1).position == 60);
}

void t_late_intent_and_unknowns() {
  std::puts("edges: late cancel intent, unknown ids");
  SmallOms oms;
  oms.on_request(rq_new(1, 100));
  oms.on_report(rp(1, Ev::kAckNew, 100));
  oms.on_report(rp(1, Ev::kFill, 100));
  oms.on_request(rq_cxl(1));  // strategy cancels a filled order
  CHECK(oms.counters().late_intent == 1);
  CHECK(oms.find(1)->st == St::kFilled);
  oms.on_report(rp(1, Ev::kRejCancel));  // venue agrees: too late
  CHECK(oms.find(1)->st == St::kFilled);

  oms.on_request(rq_cxl(99));  // never sent
  CHECK(oms.counters().local_unknown == 1);
  oms.on_report(rp(98, Ev::kFill, 10));  // venue talks about a stranger
  CHECK(oms.counters().unknown_order == 1);
}

// ---- sim sweeps ---------------------------------------------------------------

void t_sim_invariants_and_determinism() {
  std::puts("sweep: 50k simulated orders — audit every 100k events, digest replay");
  SimCfg cfg;
  cfg.orders = 50'000;
  auto run = [&](u64& digest, Counters& out) {
    VenueSim sim(cfg, false, false);
    auto oms = std::make_unique<Oms<1u << 17, 256>>();
    u64 events = 0;
    bool audits_ok = true;
    run_tape(sim, cfg, [&](const TapeItem& it) {
      if (it.is_req) oms->on_request(it.req);
      else oms->on_report(it.rep);
      if (++events % 100'000 == 0) audits_ok = audits_ok && oms->audit_open_interest();
    });
    CHECK(audits_ok);
    CHECK(oms->audit_open_interest());
    digest = oms->digest();
    out = oms->counters();
  };
  u64 d1 = 0, d2 = 0;
  Counters c1, c2;
  run(d1, c1);
  run(d2, c2);
  CHECK(d1 == d2);  // same tape, same fold, same digest
  CHECK(c1.verdicts[unsigned(Verdict::kVenueErr)] == 0);  // clean venue
  CHECK(c1.verdicts[unsigned(Verdict::kHeal)] == 0);      // nothing dropped
  CHECK(c1.dup_execs == 0);
  CHECK(c1.unknown_order == 0);
  CHECK(c1.verdicts[unsigned(Verdict::kRace)] > 0);  // crossings DID happen
  std::printf("  races handled: %llu, stale answers: %llu\n",
              static_cast<unsigned long long>(c1.verdicts[unsigned(Verdict::kRace)]),
              static_cast<unsigned long long>(c1.verdicts[unsigned(Verdict::kStale)]));
}

void t_recon_clean_is_silent() {
  std::puts("recon: with nothing dropped, 20k orders reconcile to zero findings");
  SimCfg cfg;
  cfg.orders = 20'000;
  VenueSim sim(cfg, true, true);
  auto oms = std::make_unique<Oms<1u << 16, 256>>();
  run_tape(sim, cfg, [&](const TapeItem& it) {
    if (it.is_req) oms->on_request(it.req);
    else oms->on_report(it.rep);
  });
  auto recon = std::make_unique<Recon<1u << 16>>();
  for (const ExecReport& e : sim.dropcopy()) recon->on_dropcopy(e);
  const Findings f = recon->compare(*oms, [](u64, u32) {});
  CHECK(f.checked == cfg.orders);
  CHECK(f.flagged_orders == 0);
  CHECK(f.phantom == 0 && f.state_mismatch == 0 && f.cum_mismatch == 0 && f.qty_mismatch == 0);
  CHECK(f.stuck == 0);
}

void t_recon_finds_exactly_the_damage() {
  std::puts("recon: injected drops/dups/aliens are found — nothing else");
  SimCfg cfg;
  cfg.orders = 20'000;
  cfg.drop_pm = 5;   // 0.5% of primary reports vanish
  cfg.dup_pm = 3;    // 0.3% arrive twice
  cfg.alien_orders = 7;
  VenueSim sim(cfg, true, true);
  auto oms = std::make_unique<Oms<1u << 16, 256>>();
  run_tape(sim, cfg, [&](const TapeItem& it) {
    if (it.is_req) oms->on_request(it.req);
    else oms->on_report(it.rep);
  });
  sim.generate_aliens();

  auto recon = std::make_unique<Recon<1u << 16>>();
  for (const ExecReport& e : sim.dropcopy()) recon->on_dropcopy(e);
  std::vector<u8> flagged(cfg.orders + 1, 0);
  u64 phantoms_seen = 0;
  const Findings f = recon->compare(*oms, [&](u64 cl_id, u32 mask) {
    if (cl_id > cfg.orders) {
      ++phantoms_seen;
      CHECK((mask & kFindPhantom) != 0);
    } else {
      flagged[cl_id] = 1;
    }
  });

  CHECK(f.phantom == cfg.alien_orders && phantoms_seen == cfg.alien_orders);
  CHECK(f.stuck == 0);  // the request path is lossless (sesslink's job)
  CHECK(oms->counters().dup_execs == sim.duped());  // every dup was absorbed

  // No false accusations: every flagged order really had a primary drop.
  // No missed positions: every order with a dropped FILL is flagged.
  u64 false_pos = 0, missed_fill_flags = 0, dropped_orders = 0, dropped_fill_orders = 0;
  const auto& truth = sim.truth();
  for (u64 id = 1; id <= cfg.orders; ++id) {
    const Truth& t = truth[id - 1];
    dropped_orders += t.dropped_any;
    dropped_fill_orders += t.dropped_fill;
    if (flagged[id] && !t.dropped_any) ++false_pos;
    if (t.dropped_fill && !flagged[id]) ++missed_fill_flags;
  }
  CHECK(false_pos == 0);
  CHECK(missed_fill_flags == 0);
  CHECK(f.flagged_orders > 0);
  std::printf(
      "  dropped reports %llu over %llu orders (%llu with a lost fill) -> "
      "%llu flagged: %llu state, %llu cum, %llu qty, %llu phantom\n",
      static_cast<unsigned long long>(sim.dropped()),
      static_cast<unsigned long long>(dropped_orders),
      static_cast<unsigned long long>(dropped_fill_orders),
      static_cast<unsigned long long>(f.flagged_orders),
      static_cast<unsigned long long>(f.state_mismatch),
      static_cast<unsigned long long>(f.cum_mismatch),
      static_cast<unsigned long long>(f.qty_mismatch),
      static_cast<unsigned long long>(f.phantom));
}

}  // namespace

int main() {
  t_happy_fill();
  t_cancel_path();
  t_replace_conservative_exposure();
  t_fill_vs_cancel_race();
  t_replace_vs_fill_race();
  t_replace_then_more_fills();
  t_heal_lost_ack();
  t_venue_contract_violations();
  t_dup_exec_dropped();
  t_late_intent_and_unknowns();
  t_sim_invariants_and_determinism();
  t_recon_clean_is_silent();
  t_recon_finds_exactly_the_damage();

  if (g_fail == 0) {
    std::puts("\nALL TESTS PASSED");
    return 0;
  }
  std::printf("\n%d CHECK(S) FAILED\n", g_fail);
  return 1;
}
