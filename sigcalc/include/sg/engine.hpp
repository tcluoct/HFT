#pragma once
// SignalEngine — one symbol's microstructure feature vector, updated per
// market-data event in O(1) with no allocation, no floats, no branches
// beyond the wedge's amortized pops.
//
// Features (window W events, default 256):
//   microprice   size-weighted touch price (Stoikov): where the next trade
//                "should" print given queue sizes
//   imbalance    (bid_qty - ask_qty) / (bid_qty + ask_qty)
//   OFI          Cont-Kukanov-Stoikov order-flow imbalance, summed over W:
//                quote changes translated into signed depth added/removed
//   tflow        signed traded volume over W (buy-aggressor positive)
//   drift/rvar   sum and sum-of-squares of mid changes over W (momentum and
//                realized variance, unnormalized: W is a compile-time
//                constant, so normalization is a report-time division)
//   range_pos    where mid sits inside its rolling [min, max] (wedges)
//   mom          fast EWMA minus slow EWMA of mid
//   score        fixed integer weights over the above — "the alpha"
//
// Everything is exact integer arithmetic; the engine folds every score into
// an FNV digest so two runs (or the scalar and AVX2 backends) can be proven
// identical, bit for bit.

#include <cstdint>

#include "hft/common/platform.hpp"
#include "sg/ewma.hpp"
#include "sg/types.hpp"
#include "sg/windows.hpp"

namespace sg {

template <unsigned W = 256, bool kAvx2 = true>
class SignalEngine {
 public:
  HFT_FORCE_INLINE const Features& on_tick(const Tick& t) noexcept {
    const i64 mid_x2 = t.bid_px + t.ask_px;  // mid in half-ticks: no /2, exact

    // Microprice and top imbalance straight from the touch.
    const i64 den = t.bid_qty + t.ask_qty;  // >= 2 by contract
    f_.microprice_q16 = ((t.bid_px * t.ask_qty + t.ask_px * t.bid_qty) << 16) / den;
    f_.imbalance_q16 = ((t.bid_qty - t.ask_qty) << 16) / den;

    // OFI event term (Cont et al.): depth interpreted as added to / pulled
    // from the bid and ask given how each side's price moved.
    i64 e = 0;
    if (have_prev_) {
      if (t.bid_px >= prev_bid_px_) e += t.bid_qty;
      if (t.bid_px <= prev_bid_px_) e -= prev_bid_qty_;
      if (t.ask_px <= prev_ask_px_) e -= t.ask_qty;
      if (t.ask_px >= prev_ask_px_) e += prev_ask_qty_;
    }
    f_.ofi_w = ofi_.push(e);

    const i64 d = have_prev_ ? mid_x2 - prev_mid_x2_ : 0;
    f_.drift_w = drift_.push(d);
    f_.rvar_w = rvar_.push(d * d);
    f_.tflow_w = tflow_.push(t.trade_side * t.trade_qty);

    ext_.push(mid_x2);
    const i64 lo = ext_.min(), hi = ext_.max();
    f_.range_pos_q16 = hi > lo ? (((mid_x2 - lo) << 16) / (hi - lo)) * 2 - (i64{1} << 16)
                               : 0;  // [-1, 1], 0 when the window is flat

    if constexpr (kAvx2) lattice_.update_avx2(mid_x2 << 16);
    else lattice_.update_scalar(mid_x2 << 16);
    f_.mom_q16 = (lattice_.fastest() - lattice_.slowest()) >> 1;  // half-ticks -> ticks

    // The alpha: small integer weights, one multiply-accumulate per term.
    f_.score = 3 * f_.mom_q16 + 2 * f_.imbalance_q16 + (f_.ofi_w << 4) +
               (f_.tflow_w << 3) + (f_.drift_w << 8) + f_.range_pos_q16;

    // Fold the score into the run digest (determinism / backend-equality proof).
    digest_ ^= static_cast<u64>(f_.score);
    digest_ *= 0x100000001B3ull;

    prev_bid_px_ = t.bid_px;
    prev_ask_px_ = t.ask_px;
    prev_bid_qty_ = t.bid_qty;
    prev_ask_qty_ = t.ask_qty;
    prev_mid_x2_ = mid_x2;
    have_prev_ = true;
    return f_;
  }

  [[nodiscard]] const Features& features() const noexcept { return f_; }
  [[nodiscard]] u64 digest() const noexcept { return digest_; }

 private:
  Features f_{};
  RollingSum<W> ofi_, drift_, rvar_, tflow_;
  RollingExtrema<W> ext_;
  EwmaLattice lattice_;
  i64 prev_bid_px_ = 0, prev_ask_px_ = 0;
  i64 prev_bid_qty_ = 0, prev_ask_qty_ = 0;
  i64 prev_mid_x2_ = 0;
  bool have_prev_ = false;
  u64 digest_ = 0xCBF29CE484222325ull;
};

}  // namespace sg
