#pragma once
// Deterministic fill/mark flow generator. Flow is inventory-mean-reverting
// (a market maker's is: the more long you are, the more you sell), which
// keeps positions and lot counts inside the magnitude contract of
// types.hpp while still crossing zero constantly. Same seed -> same tape.

#include <cstdint>
#include <vector>

#include "pl/types.hpp"

namespace pl {

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

template <u32 NSym>
class FlowSim {
  static_assert((NSym & (NSym - 1)) == 0, "NSym must be a power of two");

 public:
  explicit FlowSim(u64 seed) : rng_(seed), px_(NSym), pos_(NSym, 0) {
    for (u32 s = 0; s < NSym; ++s) px_[s] = 90'000 + static_cast<i64>(rng_.below(20'000));
  }

  Fill next_fill() noexcept {
    Fill f;
    f.sym = static_cast<u32>(rng_.below(NSym));
    i64& px = px_[f.sym];
    px += static_cast<i64>(rng_.below(21)) - 10;  // per-symbol walk
    if (px < 1000) px = 1000;
    // Mean-reverting side: lean against the current inventory.
    const i64 p = pos_[f.sym];
    const u64 sell_pct = p > 0 ? 60 : (p < 0 ? 40 : 50);
    f.side = rng_.below(100) < sell_pct ? -1 : 1;
    f.qty = 1 + static_cast<i64>(rng_.below(1000));
    f.px = px;
    f.fee = f.qty / 10 + 1;
    pos_[f.sym] += f.side * f.qty;
    return f;
  }

  Mark mark_of(u32 sym) const noexcept { return Mark{sym, px_[sym]}; }
  u64 rand_below(u64 n) noexcept { return rng_.below(n); }

 private:
  Rng rng_;
  std::vector<i64> px_;
  std::vector<i64> pos_;
};

}  // namespace pl
