#pragma once
// Exact O(1) sliding windows over the last W events (W = power of two).
//
// RollingSum   : ring + running sum. push() is one subtract, one add, one
//                store — the sum is bit-identical to a recompute, always,
//                because everything is an integer.
// RollingExtrema : min AND max in amortized O(1) via two monotonic wedges
//                (deques on rings). The min-wedge holds indices of a
//                non-decreasing subsequence of the window: the front is the
//                window minimum; a new sample evicts everything it
//                dominates from the back (those elements could never be a
//                future minimum — they are older AND worse).
//
// Both are defined during warmup (fewer than W samples yet): they operate
// over "the last min(n, W) samples", matching the brute-force reference.

#include <cstdint>

#include "hft/common/platform.hpp"
#include "sg/types.hpp"

namespace sg {

template <unsigned W>
class RollingSum {
  static_assert((W & (W - 1)) == 0, "W must be a power of two");

 public:
  HFT_FORCE_INLINE i64 push(i64 v) noexcept {
    sum_ += v - buf_[n_ & (W - 1)];
    buf_[n_ & (W - 1)] = v;
    ++n_;
    return sum_;
  }
  [[nodiscard]] i64 sum() const noexcept { return sum_; }

 private:
  i64 buf_[W] = {};
  i64 sum_ = 0;
  u64 n_ = 0;
};

template <unsigned W>
class RollingExtrema {
  static_assert((W & (W - 1)) == 0, "W must be a power of two");

 public:
  HFT_FORCE_INLINE void push(i64 v) noexcept {
    const u64 i = n_++;
    // min wedge: retire the front if it fell out of the window (must happen
    // before the insert, or a strictly increasing run overflows the ring),
    // then evict from the back everything >= v (older and worse).
    if (min_h_ != min_t_ && mini_[min_h_ & kMask] + W <= i) ++min_h_;
    while (min_h_ != min_t_ && minv_[(min_t_ - 1) & kMask] >= v) --min_t_;
    minv_[min_t_ & kMask] = v;
    mini_[min_t_ & kMask] = i;
    ++min_t_;
    // max wedge, mirrored.
    if (max_h_ != max_t_ && maxi_[max_h_ & kMask] + W <= i) ++max_h_;
    while (max_h_ != max_t_ && maxv_[(max_t_ - 1) & kMask] <= v) --max_t_;
    maxv_[max_t_ & kMask] = v;
    maxi_[max_t_ & kMask] = i;
    ++max_t_;
  }

  [[nodiscard]] HFT_FORCE_INLINE i64 min() const noexcept { return minv_[min_h_ & kMask]; }
  [[nodiscard]] HFT_FORCE_INLINE i64 max() const noexcept { return maxv_[max_h_ & kMask]; }

 private:
  static constexpr u64 kMask = W - 1;  // wedge size never exceeds W
  i64 minv_[W] = {};
  u64 mini_[W] = {};
  i64 maxv_[W] = {};
  u64 maxi_[W] = {};
  u64 min_h_ = 0, min_t_ = 0, max_h_ = 0, max_t_ = 0;
  u64 n_ = 0;
};

}  // namespace sg
