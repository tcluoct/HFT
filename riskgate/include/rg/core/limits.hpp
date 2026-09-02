#pragma once
// Per-symbol risk limits and the seqlock-published table they live in.
// The control plane (ops desk, config service) republishes entries while the
// hot path keeps checking orders — no lock, no stall, no torn limits.

#include <array>
#include <cstdint>

#include "hft/common/platform.hpp"
#include "rg/core/seqlock.hpp"
#include "rg/core/types.hpp"

namespace rg {

// 56 bytes: fits with its seqlock version in one cache line, so a limits
// read touches exactly one line.
struct SymbolLimits {
  Qty max_order_qty;
  std::uint64_t max_order_notional;  // tick·share units
  PriceTicks band_lo, band_hi;       // inclusive fat-finger price collar
  std::int64_t max_long_pos;         // >= 0
  std::int64_t max_short_pos;        // magnitude, >= 0: breach iff pos < -max_short_pos
  std::uint32_t enabled;             // 0 = symbol not tradable
  std::uint32_t pad_;
};
static_assert(sizeof(SymbolLimits) == 56);

template <std::size_t MaxSymbols>
class LimitsTable {
 public:
  static constexpr std::size_t capacity() noexcept { return MaxSymbols; }

  // Control-plane thread only.
  void publish(SymbolId s, const SymbolLimits& l) noexcept { slots_[s].lk.publish(l); }

  // Hot path.
  SymbolLimits read(SymbolId s, std::uint64_t* retries = nullptr) const noexcept {
    return slots_[s].lk.read(retries);
  }

 private:
  struct alignas(hft::kCacheLine) Slot {
    Seqlock<SymbolLimits> lk;
  };
  static_assert(sizeof(Slot) == hft::kCacheLine);

  std::array<Slot, MaxSymbols> slots_{};
};

}  // namespace rg
