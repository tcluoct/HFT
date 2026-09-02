#pragma once
// Router — turn one parent order into child orders across venues.
//
// Ranking: every venue gets an all-in cost per share in milli-ticks —
//
//     cost = price-preference*1000 + taker fee (- rebate)
//          + latency penalty (adverse selection while in flight)
//          + fill-risk penalty ((1000 - fill_pm) * weight)
//
// — packed with its venue id into one u64 rank key (`cost << 8 | venue`),
// ineligible venues forced to ~0. The keys run through a compile-time
// Batcher sorting network: branch-free, data-independent time, no pointer
// chasing — ranking is pure register work.
//
// Splitting: parent qty is allocated across the best venues pro-rata to
// displayed size by the LARGEST-REMAINDER method: floor(R * w_i / W) each,
// then leftover shares go to the largest remainders one by one. Integer
// exact: allocations always sum to exactly min(qty, total capacity), no
// share created or lost, every allocation within its venue cap.
//
// The router tracks in-flight exposure per venue (out/out_cap) and refuses
// venues that are halted, quote-less, price-worse-than-limit, or full.

#include <cstdint>

#include "hft/common/platform.hpp"
#include "sr/network.hpp"

namespace sr {

struct VenueCfg {
  i32 fee_mt = 0;      // per-share taker fee in milli-ticks (negative = rebate)
  u32 lat_us = 10;     // observed one-way latency
  u32 fill_pm = 900;   // historical fill ratio, per mille
  i64 out_cap = 1'000'000;  // max in-flight shares
};

struct Child {
  u16 venue = 0;
  i64 qty = 0;
};

struct Plan {
  unsigned nchild = 0;
  i64 routed = 0;  // == sum of child qty == min(parent qty, capacity)
  Child child[8];
};

template <unsigned kNV = 16, bool kUseStdSort = false>
class Router {
  static_assert(kNV >= 2 && (kNV & (kNV - 1)) == 0 && kNV <= 256);

 public:
  static constexpr i64 kPxCeil = i64{1} << 22;  // px normalizer for sells
  static constexpr i32 kLatMtPerUs = 2;         // adverse selection weight
  static constexpr i32 kFillRiskMt = 2;         // per-mille-unfilled weight

  void set_venue(unsigned v, const VenueCfg& c) noexcept { cfg_[v] = c; }
  void set_quote(unsigned v, i64 bid_px, i64 bid_q, i64 ask_px, i64 ask_q) noexcept {
    bid_px_[v] = bid_px;
    bid_q_[v] = bid_q;
    ask_px_[v] = ask_px;
    ask_q_[v] = ask_q;
  }
  void set_halted(unsigned v, bool h) noexcept { halted_[v] = h; }
  void complete(unsigned v, i64 qty) noexcept { out_[v] -= qty; }
  [[nodiscard]] i64 outstanding(unsigned v) const noexcept { return out_[v]; }
  [[nodiscard]] u64 digest() const noexcept { return digest_; }

  Plan route(i32 side, i64 qty, i64 limit_px) noexcept {
    // ---- rank: one u64 key per venue --------------------------------------
    u64 keys[kNV];
    for (unsigned v = 0; v < kNV; ++v) {
      const i64 px = side > 0 ? ask_px_[v] : bid_px_[v];
      const i64 disp = side > 0 ? ask_q_[v] : bid_q_[v];
      const bool px_ok = side > 0 ? px <= limit_px : px >= limit_px;
      const bool ok = !halted_[v] && disp > 0 && out_[v] < cfg_[v].out_cap && px_ok;
      const i64 pref = side > 0 ? px : kPxCeil - px;
      const i64 cost = pref * 1000 + cfg_[v].fee_mt +
                       static_cast<i64>(cfg_[v].lat_us) * kLatMtPerUs +
                       static_cast<i64>(1000 - cfg_[v].fill_pm) * kFillRiskMt;
      keys[v] = ok ? (static_cast<u64>(cost) << 8) | v : ~u64{0};
    }
    if constexpr (kUseStdSort) std::sort(keys, keys + kNV);
    else SortNet<kNV>::sort(keys);

    // ---- choose up to 8 venues, capacity-capped ----------------------------
    Plan p;
    unsigned n = 0;
    i64 cap[8];
    u16 ven[8];
    i64 total_cap = 0;
    for (unsigned i = 0; i < kNV && n < 8; ++i) {
      if (keys[i] == ~u64{0}) break;
      const unsigned v = static_cast<unsigned>(keys[i] & 0xFF);
      const i64 disp = side > 0 ? ask_q_[v] : bid_q_[v];
      const i64 room = cfg_[v].out_cap - out_[v];
      const i64 c = disp < room ? disp : room;
      if (c <= 0) continue;
      ven[n] = static_cast<u16>(v);
      cap[n] = c;
      total_cap += c;
      ++n;
    }
    // Minimal ranked prefix that covers the quantity: the split never
    // touches a venue a better one could have absorbed (this is what makes
    // ranking matter; spraying everything pro-rata would ignore it).
    unsigned k = 0;
    i64 cum = 0;
    while (k < n && cum < qty) cum += cap[k++];
    n = k;
    total_cap = cum;
    const i64 route_qty = qty < total_cap ? qty : total_cap;
    if (n == 0 || route_qty <= 0) return p;

    // ---- largest-remainder split, weights = caps ---------------------------
    i64 alloc[8], rem[8];
    i64 given = 0;
    for (unsigned i = 0; i < n; ++i) {
      alloc[i] = route_qty * cap[i] / total_cap;  // floor: alloc <= cap
      rem[i] = route_qty * cap[i] % total_cap;
      given += alloc[i];
    }
    for (i64 left = route_qty - given; left > 0; --left) {
      int best = -1;
      for (unsigned i = 0; i < n; ++i)
        if (alloc[i] < cap[i] && (best < 0 || rem[i] > rem[best])) best = static_cast<int>(i);
      ++alloc[best];  // capacity >= route_qty guarantees best exists
      rem[best] = -1;
    }

    for (unsigned i = 0; i < n; ++i) {
      if (alloc[i] == 0) continue;
      p.child[p.nchild++] = Child{ven[i], alloc[i]};
      p.routed += alloc[i];
      out_[ven[i]] += alloc[i];
      digest_ ^= (u64{ven[i]} << 48) ^ static_cast<u64>(alloc[i]);
      digest_ *= 0x100000001B3ull;
    }
    return p;
  }

 private:
  VenueCfg cfg_[kNV] = {};
  i64 bid_px_[kNV] = {}, ask_px_[kNV] = {};
  i64 bid_q_[kNV] = {}, ask_q_[kNV] = {};
  i64 out_[kNV] = {};
  bool halted_[kNV] = {};
  u64 digest_ = 0xCBF29CE484222325ull;
};

}  // namespace sr
