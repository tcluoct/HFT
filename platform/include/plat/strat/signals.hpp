#pragma once
// plat/strat/signals.hpp — signal primitives (sigcalc graduation, §9.2).
// Integer fixed-point throughout: signals feed the deterministic replay
// machinery, and floating point accumulates differently under different
// optimization levels — 1e-4 fixed point does not.

#include <cstdint>

#include "plat/core/types.hpp"

namespace plat {

// Exponential moving average over 1e-4 fixed-point samples with rational
// alpha = num/den (e.g., 1/8).
class EmaE4 {
 public:
  constexpr EmaE4() noexcept = default;  // alpha 1/1: pass-through until configured
  constexpr EmaE4(std::int64_t num, std::int64_t den) noexcept : num_(num), den_(den) {}

  constexpr std::int64_t update(std::int64_t sample_e4) noexcept {
    if (!primed_) {
      value_ = sample_e4;
      primed_ = true;
    } else {
      value_ += (sample_e4 - value_) * num_ / den_;
    }
    return value_;
  }

  [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool primed() const noexcept { return primed_; }
  constexpr void reset() noexcept {
    value_ = 0;
    primed_ = false;
  }

 private:
  std::int64_t num_ = 1, den_ = 1;
  std::int64_t value_ = 0;
  bool primed_ = false;
};

// L1 order-book imbalance in 1e-4: (bid−ask)/(bid+ask) ∈ [−10000, +10000].
constexpr std::int64_t imbalance_e4(Qty bid_qty, Qty ask_qty) noexcept {
  const Qty sum = bid_qty + ask_qty;
  return sum ? (bid_qty - ask_qty) * 10'000 / sum : 0;
}

}  // namespace plat
