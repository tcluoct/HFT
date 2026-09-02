#pragma once
// plat/strat/indicators.hpp — bar-cadence indicator toolkit shared by
// strategies, live and backtest alike. Integer fixed-point throughout, same
// rationale as signals.hpp: indicators feed deterministic replay, and floats
// accumulate differently across optimization levels — 1e-4 fixed point does
// not.
//
// These run at bar cadence (once per closed KBar, i.e. seconds to minutes),
// not on the tick path, so Rolling favors POD simplicity over algorithmic
// cleverness: min/max/variance are O(window) scans of a small ring, not
// monotonic-deque machinery. Everything is trivially copyable with explicit
// padding, so strategies mix indicators into state_hash() and memcpy them
// through save()/load().
//
// Donchian channels come free: Rolling over bar highs/lows, max()/min().

#include <cstdint>
#include <type_traits>

#include "plat/core/types.hpp"
#include "plat/strat/kbar.hpp"
#include "plat/strat/signals.hpp"

namespace plat {

// Integer square root (floor). Bit-by-bit restoring method: exact, branchy
// but bar-cadence only (stddev, not tick path).
constexpr std::int64_t isqrt64(std::int64_t v) noexcept {
  if (v <= 0) return 0;
  auto x = static_cast<std::uint64_t>(v);
  std::uint64_t r = 0, bit = 1ull << 62;
  while (bit > x) bit >>= 2;
  while (bit) {
    if (x >= r + bit) {
      x -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return static_cast<std::int64_t>(r);
}

// Rolling window of int64 samples (prices, volumes, e4 signals) with a
// running sum. Capacity N is compile-time; the effective window may be any
// 1..N at construction. [0] is the newest sample.
template <std::size_t N>
class Rolling {
  static_assert(N > 0);

 public:
  constexpr Rolling() noexcept = default;  // window = N
  explicit constexpr Rolling(std::uint32_t window) noexcept
      : window_(window >= 1 && window <= N ? window : static_cast<std::uint32_t>(N)) {}

  void push(std::int64_t v) noexcept {
    if (count_ >= window_) sum_ -= buf_[(count_ - window_) % N];
    buf_[count_ % N] = v;
    sum_ += v;
    ++count_;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return count_ < window_ ? static_cast<std::size_t>(count_) : window_;
  }
  [[nodiscard]] bool full() const noexcept { return count_ >= window_; }
  [[nodiscard]] std::uint32_t window() const noexcept { return window_; }
  [[nodiscard]] const std::int64_t& operator[](std::size_t i) const noexcept {
    return buf_[(count_ - 1 - i) % N];
  }

  [[nodiscard]] std::int64_t sum() const noexcept { return sum_; }
  [[nodiscard]] std::int64_t mean() const noexcept {
    const auto n = static_cast<std::int64_t>(size());
    return n ? sum_ / n : 0;
  }
  [[nodiscard]] std::int64_t min() const noexcept {
    std::int64_t m = size() ? (*this)[0] : 0;
    for (std::size_t i = 1; i < size(); ++i)
      if ((*this)[i] < m) m = (*this)[i];
    return m;
  }
  [[nodiscard]] std::int64_t max() const noexcept {
    std::int64_t m = size() ? (*this)[0] : 0;
    for (std::size_t i = 1; i < size(); ++i)
      if ((*this)[i] > m) m = (*this)[i];
    return m;
  }

  // Population variance around the integer mean. Centering before squaring
  // keeps the squares in deviation range (ticks, not absolute price), so a
  // 78'000-CNY copper price over a 20-bar window cannot overflow int64 the
  // way a naive sum-of-squares would.
  [[nodiscard]] std::int64_t variance() const noexcept {
    const auto n = static_cast<std::int64_t>(size());
    if (n == 0) return 0;
    const std::int64_t m = mean();
    std::int64_t acc = 0;
    for (std::size_t i = 0; i < size(); ++i) {
      const std::int64_t d = (*this)[i] - m;
      acc += d * d;
    }
    return acc / n;
  }
  [[nodiscard]] std::int64_t stddev() const noexcept { return isqrt64(variance()); }

 private:
  std::int64_t buf_[N]{};
  std::int64_t sum_ = 0;
  std::uint64_t count_ = 0;
  std::uint32_t window_ = static_cast<std::uint32_t>(N);
  std::uint32_t pad_ = 0;
};
static_assert(std::is_trivially_copyable_v<Rolling<4>>);

// Average true range, Wilder smoothing (alpha 1/n) over closed bars. The
// first bar has no previous close, so its TR is just high−low.
class Atr {
 public:
  constexpr Atr() noexcept = default;  // n = 14
  explicit constexpr Atr(std::int64_t n) noexcept : ema_(1, n > 0 ? n : 14) {}

  std::int64_t update(const KBar& b) noexcept {
    std::int64_t tr = b.high - b.low;
    if (has_prev_) {
      const std::int64_t hc = b.high > prev_close_ ? b.high - prev_close_ : prev_close_ - b.high;
      const std::int64_t lc = b.low > prev_close_ ? b.low - prev_close_ : prev_close_ - b.low;
      if (hc > tr) tr = hc;
      if (lc > tr) tr = lc;
    }
    prev_close_ = b.close;
    has_prev_ = 1;
    return ema_.update(tr);
  }

  [[nodiscard]] std::int64_t value() const noexcept { return ema_.value(); }
  [[nodiscard]] bool primed() const noexcept { return ema_.primed(); }

 private:
  EmaE4 ema_{1, 14};
  Price prev_close_ = 0;
  std::uint8_t has_prev_ = 0;
  std::uint8_t pad_[7]{};
};
static_assert(std::is_trivially_copyable_v<Atr>);

// Relative strength index, Wilder smoothing, reported in 1e-4 of full scale:
// value() ∈ [0, 10'000], so the classic 70/30 bands sit at 7'000/3'000.
class Rsi {
 public:
  constexpr Rsi() noexcept = default;  // n = 14
  explicit constexpr Rsi(std::int64_t n) noexcept
      : up_(1, n > 0 ? n : 14), dn_(1, n > 0 ? n : 14) {}

  std::int64_t update(Price close) noexcept {
    if (has_prev_) {
      const Price d = close - prev_close_;
      up_.update(d > 0 ? d : 0);
      dn_.update(d < 0 ? -d : 0);
    }
    prev_close_ = close;
    has_prev_ = 1;
    return value();
  }

  // Neutral 5'000 until a direction exists (flat series divides 0 by 0).
  [[nodiscard]] std::int64_t value() const noexcept {
    const std::int64_t sum = up_.value() + dn_.value();
    return sum ? up_.value() * 10'000 / sum : 5'000;
  }
  [[nodiscard]] bool primed() const noexcept { return up_.primed(); }

 private:
  EmaE4 up_{1, 14};
  EmaE4 dn_{1, 14};
  Price prev_close_ = 0;
  std::uint8_t has_prev_ = 0;
  std::uint8_t pad_[7]{};
};
static_assert(std::is_trivially_copyable_v<Rsi>);

// MACD over closes, classic EMA convention alpha = 2/(n+1); defaults 12/26/9.
// All three lines are in price units (1e-4 CNY).
class Macd {
 public:
  constexpr Macd() noexcept : Macd(12, 26, 9) {}
  constexpr Macd(std::int64_t fast_n, std::int64_t slow_n, std::int64_t signal_n) noexcept
      : fast_(2, fast_n + 1), slow_(2, slow_n + 1), sig_(2, signal_n + 1) {}

  std::int64_t update(Price close) noexcept {
    macd_ = fast_.update(close) - slow_.update(close);
    sig_.update(macd_);
    return macd_;
  }

  [[nodiscard]] std::int64_t macd() const noexcept { return macd_; }
  [[nodiscard]] std::int64_t signal() const noexcept { return sig_.value(); }
  [[nodiscard]] std::int64_t histogram() const noexcept { return macd_ - sig_.value(); }
  [[nodiscard]] bool primed() const noexcept { return sig_.primed(); }

 private:
  EmaE4 fast_{2, 13};
  EmaE4 slow_{2, 27};
  EmaE4 sig_{2, 10};
  std::int64_t macd_ = 0;
};
static_assert(std::is_trivially_copyable_v<Macd>);

// Volume-weighted average price accumulator (session or bar scope — caller
// resets at the boundary it wants). Σpx·qty stays inside int64 for a full
// day of any listed instrument; a 100'000-CNY price (1e9 e4) needs ~9e9 lots
// to overflow.
class Vwap {
 public:
  void add(Price px, Qty qty) noexcept {
    pxq_ += px * qty;
    qty_ += qty;
  }
  [[nodiscard]] Price value() const noexcept { return qty_ ? pxq_ / qty_ : 0; }
  [[nodiscard]] Qty volume() const noexcept { return qty_; }
  void reset() noexcept { pxq_ = qty_ = 0; }

 private:
  std::int64_t pxq_ = 0;
  Qty qty_ = 0;
};
static_assert(std::is_trivially_copyable_v<Vwap>);

}  // namespace plat
