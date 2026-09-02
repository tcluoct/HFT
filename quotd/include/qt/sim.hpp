#pragma once
// MMSim — the market a quote engine actually faces: a fair value that runs
// (momentum), aggressors that arrive at random — 60% of them informed, i.e.
// they trade the direction the price is about to move — and fills that
// happen only when our quote is within the aggressor's reach. That last
// part is what makes inventory skew WORK: shifting the ladder away from
// fair value on the loaded side moves that side out of most aggressors'
// range, throttling accumulation instead of just repricing it.
//
// Everything is deterministic in the seed; runs fold into the engine digest.

#include <cmath>
#include <cstdint>

#include "qt/engine.hpp"

namespace qt {

struct Rng {
  u64 s;
  explicit Rng(u64 seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  u64 next() noexcept {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1Dull;
  }
  u64 below(u64 n) noexcept { return next() % n; }
};

struct SimStats {
  double inv_mean = 0, inv_std = 0;
  i64 max_abs_inv = 0;
  i64 equity = 0;
  u64 fills = 0, news = 0, cancels = 0;
  u64 steps = 0;
};

class MMSim {
 public:
  MMSim(u64 seed, u32 agg_pm = 300, u32 informed_pct = 60)
      : rng_(seed), agg_pm_(agg_pm), informed_pct_(informed_pct) {}

  SimStats run(QuoteEngine& eng, u64 steps) {
    Op buf[QuoteEngine::kMaxOps];
    i64 fv = 100'000;
    i32 last_dir = 1;
    i64 inv_sum = 0;
    i64 inv_sumsq = 0;
    i64 max_abs = 0;

    for (u64 t = 0; t < steps; ++t) {
      eng.on_tick(fv, buf);

      // This step's move (applied after trading): 30% of steps move, with
      // momentum — 70% continue the last direction. Informed flow peeks.
      i32 move = 0;
      if (rng_.below(100) < 30)
        move = rng_.below(100) < 70 ? last_dir : -last_dir;

      if (rng_.below(1000) < agg_pm_) {
        const bool informed = rng_.below(100) < informed_pct_;
        bool agg_sell;
        if (informed && move != 0) agg_sell = move < 0;  // sell before the drop
        else agg_sell = rng_.below(2) == 0;
        const u8 side = agg_sell ? 0 : 1;  // sells hit our bid
        const i64 reach = 2 + static_cast<i64>(rng_.below(3));  // limit depth 2..4
        for (u8 lvl = 0; lvl < eng.cfg().levels; ++lvl) {
          if (!eng.live(side, lvl)) continue;
          const i64 px = eng.px(side, lvl);
          const bool crossable = side == 0 ? px >= fv - reach : px <= fv + reach;
          if (crossable) eng.on_fill(side, lvl, 25 + static_cast<i64>(rng_.below(150)));
          break;  // aggressors trade the best level only
        }
      }

      fv += move;
      if (move != 0) last_dir = move;

      const i64 inv = eng.inventory();
      inv_sum += inv;
      inv_sumsq += inv * inv;
      const i64 a = inv < 0 ? -inv : inv;
      if (a > max_abs) max_abs = a;
    }

    SimStats st;
    st.steps = steps;
    st.inv_mean = static_cast<double>(inv_sum) / static_cast<double>(steps);
    const double var = static_cast<double>(inv_sumsq) / static_cast<double>(steps) -
                       st.inv_mean * st.inv_mean;
    st.inv_std = var > 0 ? std::sqrt(var) : 0;
    st.max_abs_inv = max_abs;
    st.equity = eng.equity(fv);
    st.fills = eng.fills();
    st.news = eng.news();
    st.cancels = eng.cancels();
    return st;
  }

 private:
  Rng rng_;
  u32 agg_pm_;
  u32 informed_pct_;
};

}  // namespace qt
