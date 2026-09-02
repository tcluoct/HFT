#pragma once
// plat/strat/kbar.hpp — k-bar (OHLCV candle) aggregation for bar-driven
// strategies. Strategies own their builders and feed them from on_snapshot /
// on_trade, so bars are a pure function of the event stream — identical in
// live trading and in platbt replaying the same events, no engine wiring.
//
// Two feed modes, one bar shape:
//   on_snapshot — futures L1 (250–500 ms cadence): last_px drives OHLC and
//     the venue's *cumulative* day volume is differenced into per-bar volume
//     (the CTP convention; vnpy's BarGenerator does the same).
//   on_trade — stock 逐笔成交: price/qty per print.
//
// Bars are clock-aligned buckets of session time (ts − ts % period), closed
// lazily by the first event at or beyond the bar's end — there are no timers
// and no synthetic empty bars, so quiet periods (session breaks, illiquid
// contracts) simply skip buckets and replay stays deterministic. A night
// session crossing midnight makes ts jump backwards; that also closes the
// working bar. close_working() force-closes on phase change / session end.
//
// Everything here is trivially copyable with explicit padding, so strategies
// mix it into state_hash() and memcpy it through save()/load() directly.

#include <cstdint>
#include <type_traits>

#include "plat/codec/msgs.hpp"
#include "plat/core/time.hpp"
#include "plat/core/types.hpp"

namespace plat {

struct KBar {
  Nanos start = 0;  // aligned bucket open time (ns since trading-day midnight)
  Price open = 0;
  Price high = 0;
  Price low = 0;
  Price close = 0;
  Qty volume = 0;         // traded within the bar
  Qty open_interest = 0;  // last OI seen in the bar (futures; 0 on trade feeds)
  std::uint32_t ticks = 0;  // events aggregated into the bar
  std::uint32_t pad_ = 0;
};
static_assert(std::is_trivially_copyable_v<KBar> && sizeof(KBar) == 64);

class KBarBuilder {
 public:
  constexpr KBarBuilder() noexcept = default;  // 1-minute bars
  explicit constexpr KBarBuilder(Nanos period) noexcept
      : period_(period > 0 ? period : 60 * kNsPerSec) {}

  // Trade-driven update. Returns true when this event closed the previous
  // bar — read it via closed() before the next update.
  bool on_trade(Nanos ts, Price px, Qty qty) noexcept {
    if (px == 0) return false;
    const bool c = roll(ts);
    apply(ts, px, qty);
    return c;
  }

  // Snapshot-driven update: day_volume is the venue's cumulative count and
  // is differenced here (first snapshot primes the baseline, contributing
  // zero). last_px == 0 (no trade yet) never touches the bar, but the
  // volume baseline still tracks the venue.
  bool on_snapshot(Nanos ts, Price last_px, Qty day_volume, Qty oi = 0) noexcept {
    const Qty d = vol_primed_ && day_volume > day_vol_ ? day_volume - day_vol_ : 0;
    day_vol_ = day_volume;
    vol_primed_ = 1;
    if (last_px == 0) return false;
    const bool c = roll(ts);
    apply(ts, last_px, d);
    cur_.open_interest = oi;
    return c;
  }
  bool on_snapshot(const MdSnapshotEvt& s) noexcept {
    return on_snapshot(s.ts, s.last_px, s.day_volume, s.open_interest);
  }

  // Force-close the working bar (phase change / session end). Returns true
  // when there was a working bar; it is then available via closed().
  bool close_working() noexcept {
    if (cur_.ticks == 0) return false;
    closed_ = cur_;
    cur_ = KBar{};
    return true;
  }

  [[nodiscard]] const KBar& closed() const noexcept { return closed_; }
  [[nodiscard]] bool working() const noexcept { return cur_.ticks != 0; }
  [[nodiscard]] const KBar& working_bar() const noexcept { return cur_; }
  [[nodiscard]] Nanos period() const noexcept { return period_; }

 private:
  // Close the working bar if ts left its bucket — either forward past the
  // end, or backwards (night session wrapping midnight resets session time).
  bool roll(Nanos ts) noexcept {
    if (cur_.ticks == 0 || (ts < cur_.start + period_ && ts >= cur_.start)) return false;
    closed_ = cur_;
    cur_ = KBar{};
    return true;
  }

  void apply(Nanos ts, Price px, Qty qty) noexcept {
    if (cur_.ticks == 0) {
      cur_.start = ts - ts % period_;
      cur_.open = cur_.high = cur_.low = px;
    } else {
      if (px > cur_.high) cur_.high = px;
      if (px < cur_.low) cur_.low = px;
    }
    cur_.close = px;
    cur_.volume += qty;
    ++cur_.ticks;
  }

  Nanos period_ = 60 * kNsPerSec;
  KBar cur_{};
  KBar closed_{};
  Qty day_vol_ = 0;         // last cumulative day volume seen
  std::uint8_t vol_primed_ = 0;
  std::uint8_t pad_[7]{};
};
static_assert(std::is_trivially_copyable_v<KBarBuilder>);

// Fixed-capacity history of closed bars. [0] is the most recently closed,
// [size()-1] the oldest still retained; total() counts every bar ever pushed.
template <std::size_t N>
class BarRing {
  static_assert(N > 0);

 public:
  void push(const KBar& b) noexcept {
    bars_[count_ % N] = b;
    ++count_;
  }
  [[nodiscard]] std::size_t size() const noexcept {
    return count_ < N ? static_cast<std::size_t>(count_) : N;
  }
  [[nodiscard]] std::uint64_t total() const noexcept { return count_; }
  [[nodiscard]] const KBar& operator[](std::size_t i) const noexcept {
    return bars_[(count_ - 1 - i) % N];
  }

 private:
  KBar bars_[N]{};
  std::uint64_t count_ = 0;
};
static_assert(std::is_trivially_copyable_v<BarRing<4>>);

// Builder + history composed — the shape most strategies want per
// instrument: feed events in, look back over the last N closed bars.
template <std::size_t N>
class KBarSeries {
 public:
  KBarSeries() = default;
  explicit KBarSeries(Nanos period) : b_(period) {}

  bool on_trade(Nanos ts, Price px, Qty qty) {
    return push_if(b_.on_trade(ts, px, qty));
  }
  bool on_snapshot(Nanos ts, Price last_px, Qty day_volume, Qty oi = 0) {
    return push_if(b_.on_snapshot(ts, last_px, day_volume, oi));
  }
  bool on_snapshot(const MdSnapshotEvt& s) { return push_if(b_.on_snapshot(s)); }
  bool close_working() { return push_if(b_.close_working()); }

  [[nodiscard]] std::size_t size() const noexcept { return ring_.size(); }
  [[nodiscard]] std::uint64_t total() const noexcept { return ring_.total(); }
  [[nodiscard]] const KBar& operator[](std::size_t i) const noexcept { return ring_[i]; }
  [[nodiscard]] bool working() const noexcept { return b_.working(); }
  [[nodiscard]] const KBar& working_bar() const noexcept { return b_.working_bar(); }
  [[nodiscard]] Nanos period() const noexcept { return b_.period(); }

 private:
  bool push_if(bool closed) {
    if (closed) ring_.push(b_.closed());
    return closed;
  }

  KBarBuilder b_;
  BarRing<N> ring_;
};
static_assert(std::is_trivially_copyable_v<KBarSeries<4>>);

}  // namespace plat
