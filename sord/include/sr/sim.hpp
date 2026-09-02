#pragma once
// RouteSim — the quality experiment. Eight venues with TRUE fee schedules,
// latencies, and fill probabilities; quotes jitter around a random-walk mid;
// parents are buys of random size. Two routers face identical quotes and
// identical per-(parent,venue) fill randomness:
//
//   naive : believes all venues are equal (fee 0, latency 0, fill 100%) —
//           so it ranks by displayed price alone, like a first-cut SOR
//   model : knows the truth (calibrated fees/latency/fill ratios)
//
// Unfilled shares get chased at mid+3 plus a taker fee — the realistic
// price of routing to a venue that doesn't fill you. The score is realized
// slippage per share vs the decision-time mid, in milli-ticks.

#include <cstdint>

#include "sr/router.hpp"

namespace sr {

struct VenueTruth {
  i32 fee_mt;
  u32 lat_us;
  u32 fill_pm;
};

inline constexpr VenueTruth kTruth[8] = {
    {+30, 5, 950},   // premium taker: expensive, fast, reliable
    {+5, 10, 900},   // workhorse
    {-15, 30, 550},  // inverted venue: rebate, slow, unreliable
    {0, 15, 850},
    {+10, 8, 920},
    {-5, 40, 650},   // rebate, flaky
    {+20, 6, 940},
    {0, 25, 750},
};

struct SimRng {
  u64 s;
  explicit SimRng(u64 seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  u64 next() noexcept {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1Dull;
  }
  u64 below(u64 n) noexcept { return next() % n; }
};

inline u64 outcome_mix(u64 seed, u64 parent, u64 venue) noexcept {
  u64 x = seed ^ (parent * 0x9E3779B97F4A7C15ull) ^ (venue * 0xC2B2AE3D27D4EB4Full);
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}

// One parent's realized outcome for a given plan against the truth.
// Deterministic in (seed, parent_id, venue): both routers face the same luck.
struct Outcome {
  i64 paid_mt = 0;    // all-in cash paid, milli-ticks * shares
  i64 direct = 0;     // shares filled where routed
  i64 chased = 0;     // shares re-routed at the chase price
};

template <unsigned kNV>
inline Outcome settle(u64 seed, u64 parent_id, const Plan& p, i64 qty, i64 mid,
                      const i64* ask_px) {
  Outcome o;
  for (unsigned i = 0; i < p.nchild; ++i) {
    const unsigned v = p.child[i].venue;
    const i64 q = p.child[i].qty;
    const bool full = outcome_mix(seed, parent_id, v) % 1000 < kTruth[v].fill_pm;
    const i64 filled = full ? q : q / 2;  // misses usually mean partials
    o.direct += filled;
    o.paid_mt += filled * (ask_px[v] * 1000 + kTruth[v].fee_mt);
    o.chased += q - filled;
  }
  o.chased += qty - p.routed;  // capacity shortfall is chased too
  o.paid_mt += o.chased * ((mid + 3) * 1000 + 10);
  return o;
}

}  // namespace sr
