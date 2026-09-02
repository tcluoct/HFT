// Scenario tests for the A/B arbitrator. The contract under test: the sink
// sees every venue seq exactly once, in order, either as a packet or inside
// a declared gap — under loss, duplication, reordering, dead lines, and
// window overflow. Plus a 200k-seq adversarial sweep asserting the exact
// coverage invariant. No test framework — a CHECK macro.

#include <cstdio>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "fa/core/arbiter.hpp"
#include "fa/sim/line_sim.hpp"

using namespace fa;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_fail;                                                     \
    }                                                               \
  } while (0)

struct RecSink {
  std::vector<std::uint64_t> seqs;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> gaps;
  std::uint64_t bad_payload = 0;
  std::uint64_t empty_gaps = 0;
  std::uint64_t disordered_gaps = 0;
  std::uint64_t last_gap_hi = 0;

  void on_packet(const MdPacket& p) noexcept {
    if (p.payload != mix64(p.seq)) ++bad_payload;
    seqs.push_back(p.seq);
  }
  void on_gap(std::uint64_t lo, std::uint64_t hi) noexcept {
    if (lo >= hi) {
      if (empty_gaps < 5)
        std::printf("    !! empty/inverted gap (%llu,%llu) after %zu deliveries\n",
                    static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi),
                    seqs.size());
      ++empty_gaps;
    }
    if (lo < last_gap_hi) {
      if (disordered_gaps < 5)
        std::printf("    !! out-of-order gap (%llu,%llu) after gap ending %llu\n",
                    static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi),
                    static_cast<unsigned long long>(last_gap_hi));
      ++disordered_gaps;
    }
    last_gap_hi = hi;
    gaps.emplace_back(lo, hi);
  }
};

using Arb = Arbiter<RecSink, 1024>;

// Feed a simulated arrival stream; poll rides along on arrival time.
void feed(Arb& a, const std::vector<Arrival>& evs) {
  for (const auto& e : evs) {
    a.on_packet(e.pkt, e.when);
    a.poll(e.when);
  }
}

// End of stream: keep advancing the sim clock until every open hole has
// timed out (each poll resolves one hole and re-arms for the next).
void flush(Arb& a, std::uint64_t t, std::uint64_t timeout) {
  while (a.state() == ArbState::kGapPending) {
    t += timeout + 1;
    a.poll(t);
  }
}

bool in_order_1_to_n(const std::vector<std::uint64_t>& v, std::uint64_t n) {
  if (v.size() != n) return false;
  for (std::uint64_t i = 0; i < n; ++i)
    if (v[i] != i + 1) return false;
  return true;
}

// Every seq in [1, n] appears exactly once: as a delivered packet or inside
// a gap range. Delivered must be strictly increasing.
bool exact_coverage(const RecSink& s, std::uint64_t n) {
  auto bad = [&](std::size_t i, std::uint64_t expect, std::uint64_t seq, std::size_t gi) {
    std::printf("    coverage break at delivered[%zu]: expect %llu, seq %llu, gap[%zu]=%s\n",
                i, static_cast<unsigned long long>(expect),
                static_cast<unsigned long long>(seq), gi,
                gi < s.gaps.size()
                    ? (std::to_string(s.gaps[gi].first) + "," + std::to_string(s.gaps[gi].second))
                          .c_str()
                    : "none");
    return false;
  };
  std::size_t gi = 0;
  std::uint64_t expect = 1;
  for (std::size_t i = 0; i <= s.seqs.size(); ++i) {
    const std::uint64_t seq = (i < s.seqs.size()) ? s.seqs[i] : n + 1;
    while (expect < seq) {
      if (gi >= s.gaps.size() || s.gaps[gi].first != expect) return bad(i, expect, seq, gi);
      expect = s.gaps[gi].second;
      ++gi;
      if (expect > seq) return bad(i, expect, seq, gi - 1);
    }
    if (expect != seq) return bad(i, expect, seq, gi);
    expect = seq + 1;
  }
  return expect == n + 2 && gi == s.gaps.size();  // consumed everything
}

MdPacket pkt(std::uint64_t seq, std::uint32_t line) {
  return MdPacket{seq, 0, mix64(seq), line, 0};
}

void t_clean_lines() {
  std::puts("clean lines: in-order delivery, twins dropped, skew measured");
  RecSink s;
  Arb a(s, 1'000'000);
  LineSim sim(7, {0, 0, 100, 0}, {0, 0, 150, 0}, 1'000);  // A 100ns, B 150ns, no jitter
  std::vector<Arrival> ev;
  sim.generate(1, 1'001, ev);
  feed(a, ev);
  CHECK(in_order_1_to_n(s.seqs, 1'000));
  CHECK(s.gaps.empty());
  CHECK(s.bad_payload == 0);
  CHECK(a.stats().line[0].firsts == 1'000);  // A always wins by 50ns
  CHECK(a.stats().line[1].dups == 1'000);
  CHECK(a.skew().percentile(0.5) == 50);
  CHECK(a.skew().max() == 50);
}

void t_b_rescues_a() {
  std::puts("A drops, B fills every hole: complete stream, zero gaps");
  RecSink s;
  Arb a(s, 1'000'000);
  LineSim sim(11, {50, 0, 100, 0}, {0, 0, 150, 0}, 1'000);  // A loses 5%
  std::vector<Arrival> ev;
  sim.generate(1, 5'001, ev);
  feed(a, ev);
  flush(a, ev.back().when, 1'000'000);
  CHECK(in_order_1_to_n(s.seqs, 5'000));
  CHECK(s.gaps.empty());
  CHECK(a.stats().line[1].firsts > 0);  // B won exactly the seqs A dropped
  CHECK(a.stats().line[1].firsts == 5'000 - a.stats().line[0].firsts);
}

void t_dead_line() {
  std::puts("one line completely dead: the other carries the stream");
  RecSink s;
  Arb a(s, 1'000'000);
  LineSim sim(13, {0, 0, 100, 0}, {1'000, 0, 150, 0}, 1'000);  // B drops 100%
  std::vector<Arrival> ev;
  sim.generate(1, 2'001, ev);
  feed(a, ev);
  CHECK(in_order_1_to_n(s.seqs, 2'000));
  CHECK(s.gaps.empty());
  CHECK(a.stats().line[1].received == 0);
}

void t_both_drop_gap_declared() {
  std::puts("both lines lose a seq: gap declared after timeout, exact range");
  RecSink s;
  Arb a(s, 100);  // 100ns gap timeout
  a.on_packet(pkt(1, 0), 10);
  a.on_packet(pkt(2, 0), 20);
  a.on_packet(pkt(4, 0), 40);  // 3 never arrives on either line
  a.on_packet(pkt(5, 0), 50);
  a.poll(139);  // deadline armed at 40+100
  CHECK(s.seqs.size() == 2);  // still waiting for 3
  CHECK(a.state() == ArbState::kGapPending);
  a.poll(141);
  CHECK((s.gaps == std::vector<std::pair<std::uint64_t, std::uint64_t>>{{3, 4}}));
  CHECK((s.seqs == std::vector<std::uint64_t>{1, 2, 4, 5}));
  CHECK(a.state() == ArbState::kSynced);
  CHECK(a.stats().gap_seqs == 1);
}

void t_reorder_within_window() {
  std::puts("jitter reordering inside the window resolves silently");
  RecSink s;
  Arb a(s, 1'000'000);
  for (auto [seq, t] : {std::pair<std::uint64_t, std::uint64_t>{1, 10},
                        {3, 20},
                        {2, 30},
                        {5, 40},
                        {4, 50}}) {
    a.on_packet(pkt(seq, 0), t);
    a.poll(t);
  }
  CHECK(in_order_1_to_n(s.seqs, 5));
  CHECK(s.gaps.empty());
  CHECK(a.stats().gap_events == 0);
}

void t_stale_and_dup() {
  std::puts("twin of a delivered seq is a dup; ancient copy is a stale");
  RecSink s;
  Arb a(s, 1'000'000);
  a.on_packet(pkt(1, 0), 10);
  a.on_packet(pkt(1, 1), 15);  // twin after delivery, still in the seen ring
  CHECK(a.stats().line[1].dups == 1);
  // Push the window far past seq 1, recycling its ring entry...
  for (std::uint64_t q = 2; q <= Arb::kWindow + 2; ++q) a.on_packet(pkt(q, 0), 20 + q);
  a.on_packet(pkt(1, 1), 9'999);  // ...now the late copy is merely stale
  CHECK(a.stats().line[1].stales == 1);
  CHECK(s.seqs.size() == Arb::kWindow + 2);  // delivered exactly once each
}

void t_window_overflow_fast_forward() {
  std::puts("seq beyond the window: fast-forward, gaps reported, resync");
  RecSink s;
  Arb a(s, 100);
  a.on_packet(pkt(1, 0), 10);
  a.on_packet(pkt(2'000, 0), 20);  // outage: 2..1999 vanished
  CHECK(a.stats().window_jumps == 1);
  a.poll(121);  // remaining hole (977..1999) times out
  flush(a, 121, 100);
  CHECK((s.seqs == std::vector<std::uint64_t>{1, 2'000}));
  CHECK(a.stats().gap_seqs == 1'998);  // 2..1999 inclusive
  CHECK(exact_coverage(s, 2'000));
}

void t_races_split() {
  std::puts("equal jitter: wins split, stream still perfect");
  RecSink s;
  Arb a(s, 1'000'000);
  LineSim sim(17, {0, 0, 100, 400}, {0, 0, 100, 400}, 1'000);
  std::vector<Arrival> ev;
  sim.generate(1, 10'001, ev);
  feed(a, ev);
  CHECK(in_order_1_to_n(s.seqs, 10'000));
  const auto& st = a.stats();
  CHECK(st.line[0].firsts + st.line[1].firsts == 10'000);
  CHECK(st.line[0].firsts > 3'000);  // symmetric race: neither side sweeps
  CHECK(st.line[1].firsts > 3'000);
}

void t_determinism() {
  std::puts("same seed, same everything: delivered digest and stats match");
  auto run = [](RecSink& s) {
    Arb a(s, 20'000);
    LineSim sim(99, {30, 20, 5'000, 2'000}, {10, 10, 7'000, 2'000}, 100);
    std::vector<Arrival> ev;
    sim.generate(1, 50'001, ev);
    feed(a, ev);
    flush(a, ev.back().when, 20'000);
    return a.stats();
  };
  RecSink s1, s2;
  const auto st1 = run(s1);
  const auto st2 = run(s2);
  CHECK(s1.seqs == s2.seqs);
  CHECK(s1.gaps == s2.gaps);
  CHECK(st1.delivered == st2.delivered);
  CHECK(st1.gap_seqs == st2.gap_seqs);
  CHECK(st1.line[0].firsts == st2.line[0].firsts);
}

void t_adversarial_coverage() {
  std::puts("200k-seq adversarial sweep: exact coverage, zero corruption");
  RecSink s;
  Arb a(s, 20'000);  // 20us timeout vs 5us jitter
  LineSim sim(1234, {100, 20, 5'000, 5'000}, {100, 20, 7'000, 5'000}, 100);
  constexpr std::uint64_t kN = 200'000;
  std::vector<Arrival> ev;
  sim.generate(1, kN + 1, ev);
  // Audited feed: catch the first packet that corrupts internal state.
  std::uint64_t audit_fails = 0;
  for (std::size_t i = 0; i < ev.size(); ++i) {
    const auto& e = ev[i];
    a.on_packet(e.pkt, e.when);
    a.poll(e.when);
    if (const char* why = a.audit(); why && audit_fails++ == 0)
      std::printf("    !! audit fail at arrival %zu (seq %llu line %u when %llu, "
                  "next %llu): %s\n",
                  i, static_cast<unsigned long long>(e.pkt.seq), e.pkt.line,
                  static_cast<unsigned long long>(e.when),
                  static_cast<unsigned long long>(a.next_expected()), why);
  }
  CHECK(audit_fails == 0);
  flush(a, ev.back().when, 20'000);
  const auto& st = a.stats();
  std::printf("    delivered %llu, gaps %llu (%llu seqs), dups %llu, jumps %llu\n",
              static_cast<unsigned long long>(st.delivered),
              static_cast<unsigned long long>(st.gap_events),
              static_cast<unsigned long long>(st.gap_seqs),
              static_cast<unsigned long long>(st.line[0].dups + st.line[1].dups),
              static_cast<unsigned long long>(st.window_jumps));
  CHECK(st.delivered + st.gap_seqs == kN);
  CHECK(s.bad_payload == 0);
  CHECK(exact_coverage(s, kN));
  CHECK(st.gap_seqs > 0);  // 10% x 10% loss: both-lines holes must exist
  for (std::size_t i = 1; i < s.seqs.size(); ++i)  // strictly increasing
    if (s.seqs[i] <= s.seqs[i - 1]) {
      CHECK(false);
      break;
    }
}

}  // namespace

int main() {
  std::puts("feedarb scenario tests");
  std::puts("======================");
  t_clean_lines();
  t_b_rescues_a();
  t_dead_line();
  t_both_drop_gap_declared();
  t_reorder_within_window();
  t_stale_and_dup();
  t_window_overflow_fast_forward();
  t_races_split();
  t_determinism();
  t_adversarial_coverage();
  std::puts("======================");
  if (g_fail == 0) {
    std::puts("ALL TESTS PASSED");
    return 0;
  }
  std::printf("%d CHECKS FAILED\n", g_fail);
  return 1;
}
