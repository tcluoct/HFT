#pragma once
// Synthetic market session generator, deterministic by seed.
//
// Crucially, the generated market has GENUINE short-horizon alpha: each
// symbol carries a slowly-flipping imbalance regime; displayed sizes tilt
// with the regime, and mid moves are biased in the regime's direction. An
// imbalance-taking strategy therefore has real edge — and that edge decays
// with reaction latency, because regimes flip and the mid drifts away. That
// is exactly the property the backtester's latency model exists to measure
// (a market with no alpha would show PnL ~ 0 at every latency and prove
// nothing).

#include <algorithm>
#include <cstdint>
#include <vector>

#include "hft/common/platform.hpp"
#include "tl/data/tick.hpp"

namespace tl {

class SessionGen {
 public:
  SessionGen(std::uint64_t seed, unsigned nsym, Price mid0)
      : s_(seed | 1), nsym_(nsym), mid0_(mid0) {
    st_.resize(nsym);
    for (auto& s : st_) s.mid = mid0;
  }

  Tick next() noexcept {
    const std::uint64_t r = rng();
    ts_ += 200 + (r & 1023);  // 200-1223 ns inter-tick gaps (~1.4M ticks/s)
    const SymbolId sym = static_cast<SymbolId>(bounded(r >> 40, nsym_));
    Sym& st = st_[sym];

    // Regime flip: ~once per 256 ticks. imb in [-3, +3].
    if (((r >> 12) & 0xFF) == 0)
      st.imb = static_cast<int>((r >> 20) % 7) - 3;

    Tick t{};
    t.ts_ns = ts_;
    t.symbol = sym;

    if (((r >> 32) & 0xFF) < 13) {  // ~5%: trade print at the touch
      t.kind = kTrade;
      const bool at_ask = ((r >> 45) & 1) != 0;
      t.px1 = at_ask ? st.mid + 1 : st.mid - 1;
      t.q1 = 100u * (1u + static_cast<Qty>((r >> 46) & 7));
      return t;
    }

    t.kind = kQuote;
    // ~3% of quotes move the mid; direction biased by the regime:
    // P(up) = (16 + 2*imb) / 32  ->  0.31 .. 0.69
    if (((r >> 48) & 0x1F) == 0) {
      const bool up = static_cast<int>((r >> 53) & 31) < (16 + 2 * st.imb);
      st.mid += up ? 1 : -1;
      st.mid = std::clamp(st.mid, mid0_ - 1500, mid0_ + 1500);
    }
    const Price ask_off = 1 + static_cast<Price>((r >> 58) & 1);  // spread 2-3 ticks
    t.px1 = st.mid - 1;
    t.px2 = st.mid + ask_off;
    // Displayed size tilts with the regime (this is the observable signal).
    const std::int64_t tilt = static_cast<std::int64_t>(st.imb) * 150;
    const std::int64_t bq = 300 + static_cast<std::int64_t>((r >> 33) & 255) + tilt;
    const std::int64_t aq = 300 + static_cast<std::int64_t>((r >> 25) & 255) - tilt;
    t.q1 = static_cast<Qty>(std::clamp<std::int64_t>(bq, 100, 1500));
    t.q2 = static_cast<Qty>(std::clamp<std::int64_t>(aq, 100, 1500));
    return t;
  }

 private:
  HFT_FORCE_INLINE std::uint64_t rng() noexcept {  // splitmix64
    std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  static HFT_FORCE_INLINE std::uint32_t bounded(std::uint64_t x, std::uint32_t n) noexcept {
    return static_cast<std::uint32_t>(((x & 0xFFFFFFFFull) * n) >> 32);
  }

  struct Sym {
    Price mid = 0;
    int imb = 0;
  };

  std::uint64_t s_;
  unsigned nsym_;
  Price mid0_;
  std::uint64_t ts_ = 0;
  std::vector<Sym> st_;
};

}  // namespace tl
