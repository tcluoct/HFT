#pragma once
// Deterministic market simulator: a random-walk mid with calm/hot volatility
// regimes, a mostly-one-tick spread that occasionally widens, stochastic
// touch sizes, and aggressor trades biased in the direction of recent moves.
// Same seed -> same tick stream, bit for bit (the digest tests rely on it).

#include <cstdint>

#include "sg/types.hpp"

namespace sg {

struct Rng {
  u64 s;
  explicit Rng(u64 seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  u64 next() noexcept {  // xorshift64*
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1Dull;
  }
  u64 below(u64 n) noexcept { return next() % n; }
};

class MarketSim {
 public:
  explicit MarketSim(u64 seed) : rng_(seed) {}

  Tick next() noexcept {
    // Volatility regime flips roughly every 4096 events.
    if (rng_.below(4096) == 0) hot_ = !hot_;
    const u64 move_pm = hot_ ? 500 : 120;  // per-mille chance the mid steps

    if (rng_.below(1000) < move_pm) {
      const i64 dir = (rng_.next() & 1) ? 1 : -1;
      bid_ += dir;
      if (bid_ < 1000) bid_ = 1000;  // floor, keeps prices positive
      last_dir_ = dir;
    }
    spread_ = (rng_.below(100) < 92) ? 1 : 1 + static_cast<i64>(rng_.below(3));

    Tick t;
    t.bid_px = bid_;
    t.ask_px = bid_ + spread_;
    t.bid_qty = 1 + static_cast<i64>(rng_.below(2000));
    t.ask_qty = 1 + static_cast<i64>(rng_.below(2000));
    if (rng_.below(100) < 25) {  // a quarter of events carry a trade
      t.trade_qty = 1 + static_cast<i64>(rng_.below(500));
      // Aggressors lean the way the price has been going (weak signal).
      const bool with_flow = rng_.below(100) < 60;
      t.trade_side = with_flow ? static_cast<i32>(last_dir_)
                               : static_cast<i32>(-last_dir_);
    }
    return t;
  }

 private:
  Rng rng_;
  i64 bid_ = 200'000;
  i64 spread_ = 1;
  i64 last_dir_ = 1;
  bool hot_ = false;
};

}  // namespace sg
