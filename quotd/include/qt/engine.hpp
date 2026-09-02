#pragma once
// QuoteEngine — keep a two-sided ladder of quotes centered on fair value,
// skewed by inventory, and move it with the *fewest* venue operations that
// queue-priority rules allow.
//
// The engine never "sends its quotes again". Each tick it computes the
// DESIRED ladder (fair value, half-spread, inventory skew from a clamped
// LUT, per-level step) and diffs it against the WORKING ladder (what is
// actually resting at the venue):
//
//   desired == working (within the hysteresis band)  ->  no op (keep queue!)
//   level gone / side over inventory limit           ->  cancel
//   fresh level                                      ->  new
//   price drifted beyond the band                    ->  cancel + new
//   quote eaten below half size                      ->  cancel + new (replenish)
//
// Queue position is the asset: a resting order keeps its place only if
// untouched (or amended DOWN in size, venue-permitting — modeled here as
// "keep"), so the band trades a little price staleness for a lot of queue
// priority and op budget.
//
// Mass pull: one epoch bump logically kills every resting quote — O(1) to
// DECIDE, before any cancel hits the wire. Fills that race the cancels
// reference an old epoch and are counted stale (inventory still moves —
// exposure is real — but the strategy knows they're from a dead regime).
// Physical cancels drain behind the decision.

#include <cstdint>

#include "hft/common/platform.hpp"

namespace qt {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

struct Cfg {
  i64 half_spread = 2;  // ticks
  i64 level_step = 1;   // ticks between ladder levels
  i64 size = 100;       // shares per level
  unsigned levels = 4;  // per side, <= kMaxLevels
  i64 band = 1;         // hysteresis: requote only if px drifts beyond this
  i64 inv_bucket = 200; // shares per skew step
  i64 max_skew = 6;     // ticks, LUT clamp
  i64 max_inv = 2000;   // hard limit: stop quoting the accumulating side
};

struct Op {
  enum Kind : u8 { kNew, kCancel };
  Kind kind;
  u8 side;   // 0 = bid, 1 = ask
  u8 level;
  i64 px;
  i64 qty;
};

class QuoteEngine {
 public:
  static constexpr unsigned kMaxLevels = 4;
  static constexpr unsigned kMaxOps = 4 * kMaxLevels;

  explicit QuoteEngine(const Cfg& cfg) : cfg_(cfg) {
    // Skew LUT: linear in inventory buckets, clamped at +-max_skew. Indexed
    // branchlessly with a clamped bucket, built once.
    for (i64 b = -kLutHalf; b <= kLutHalf; ++b) {
      i64 s = b;
      if (s > cfg_.max_skew) s = cfg_.max_skew;
      if (s < -cfg_.max_skew) s = -cfg_.max_skew;
      lut_[b + kLutHalf] = s;
    }
  }

  // Recompute desired ladder against fair value, emit the minimal op set.
  unsigned on_tick(i64 fv, Op* out) noexcept {
    if (pulled_) return 0;
    unsigned n = 0;
    const i64 skew = skew_ticks();
    const i64 center = fv - skew;
    for (u8 side = 0; side < 2; ++side) {
      const bool suppressed = side == 0 ? inv_ >= cfg_.max_inv : inv_ <= -cfg_.max_inv;
      for (u8 lvl = 0; lvl < cfg_.levels; ++lvl) {
        const i64 want_px = side == 0
                                ? center - cfg_.half_spread - lvl * cfg_.level_step
                                : center + cfg_.half_spread + lvl * cfg_.level_step;
        const i64 want_qty = suppressed ? 0 : cfg_.size;
        Quote& w = work_[side][lvl];
        if (want_qty == 0) {
          if (w.live) {
            out[n++] = Op{Op::kCancel, side, lvl, w.px, w.qty};
            w.live = false;
            ++cancels_;
          }
          continue;
        }
        if (w.live) {
          const i64 drift = w.px > want_px ? w.px - want_px : want_px - w.px;
          const bool starved = w.qty * 2 < cfg_.size;  // eaten below half
          if (drift <= cfg_.band && !starved) continue;  // keep the queue spot
          out[n++] = Op{Op::kCancel, side, lvl, w.px, w.qty};
          ++cancels_;
        }
        out[n++] = Op{Op::kNew, side, lvl, want_px, want_qty};
        w = Quote{want_px, want_qty, epoch_, true};
        ++news_;
      }
    }
    fold(n);
    return n;
  }

  // O(1) decision: everything resting is logically dead as of this bump.
  // The physical cancels follow (still N ops — but the *state* is already
  // safe: stale fills are recognized on arrival).
  unsigned pull_all(Op* out) noexcept {
    ++epoch_;
    pulled_ = true;
    unsigned n = 0;
    for (u8 side = 0; side < 2; ++side)
      for (u8 lvl = 0; lvl < cfg_.levels; ++lvl) {
        Quote& w = work_[side][lvl];
        if (!w.live) continue;
        out[n++] = Op{Op::kCancel, side, lvl, w.px, w.qty};
        w.live = false;
        ++cancels_;
      }
    fold(n);
    return n;
  }

  void resume() noexcept { pulled_ = false; }

  // A venue execution against our resting quote.
  void on_fill(u8 side, u8 level, i64 qty) noexcept {
    Quote& w = work_[side][level];
    if (qty > w.qty) qty = w.qty;
    if (qty <= 0) return;
    w.qty -= qty;
    if (w.qty == 0) w.live = false;
    inv_ += side == 0 ? qty : -qty;
    cash_ += side == 0 ? -w.px * qty : w.px * qty;
    ++fills_;
    if (w.epoch != epoch_) ++stale_fills_;
    digest_ ^= (u64{side} << 60) ^ (u64{level} << 50) ^ static_cast<u64>(w.px * 31 + qty);
    digest_ *= 0x100000001B3ull;
  }

  [[nodiscard]] i64 inventory() const noexcept { return inv_; }
  [[nodiscard]] i64 cash() const noexcept { return cash_; }
  [[nodiscard]] i64 equity(i64 fv) const noexcept { return cash_ + inv_ * fv; }
  [[nodiscard]] i64 skew_ticks() const noexcept {
    i64 b = inv_ / cfg_.inv_bucket;
    if (b > kLutHalf) b = kLutHalf;
    if (b < -kLutHalf) b = -kLutHalf;
    return lut_[b + kLutHalf];
  }
  [[nodiscard]] const Cfg& cfg() const noexcept { return cfg_; }
  [[nodiscard]] u64 news() const noexcept { return news_; }
  [[nodiscard]] u64 cancels() const noexcept { return cancels_; }
  [[nodiscard]] u64 fills() const noexcept { return fills_; }
  [[nodiscard]] u64 stale_fills() const noexcept { return stale_fills_; }
  [[nodiscard]] u64 digest() const noexcept { return digest_; }
  [[nodiscard]] bool live(u8 side, u8 lvl) const noexcept { return work_[side][lvl].live; }
  [[nodiscard]] i64 px(u8 side, u8 lvl) const noexcept { return work_[side][lvl].px; }
  [[nodiscard]] i64 qty(u8 side, u8 lvl) const noexcept { return work_[side][lvl].qty; }

 private:
  struct Quote {
    i64 px = 0;
    i64 qty = 0;
    u64 epoch = 0;
    bool live = false;
  };

  void fold(unsigned nops) noexcept {
    digest_ ^= nops * 0x9E3779B97F4A7C15ull;
    digest_ *= 0x100000001B3ull;
  }

  static constexpr i64 kLutHalf = 32;
  Cfg cfg_;
  Quote work_[2][kMaxLevels] = {};
  i64 lut_[2 * kLutHalf + 1] = {};
  i64 inv_ = 0;
  i64 cash_ = 0;
  u64 epoch_ = 1;
  bool pulled_ = false;
  u64 news_ = 0, cancels_ = 0, fills_ = 0, stale_fills_ = 0;
  u64 digest_ = 0xCBF29CE484222325ull;
};

}  // namespace qt
