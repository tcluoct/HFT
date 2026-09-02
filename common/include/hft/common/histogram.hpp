#pragma once
// Log-linear latency histogram (HdrHistogram-style) with a fixed footprint
// and O(1) record(). Exact below 32 units, <= ~3% relative error above.
// Hot threads record raw TSC cycle deltas (no float math on the hot path);
// conversion to nanoseconds happens once, at report time.

#include <array>
#include <bit>
#include <cstdint>

namespace hft {

class LatencyHistogram {
 public:
  static constexpr unsigned kSubBits = 5;
  static constexpr unsigned kSub = 1u << kSubBits;                  // 32
  static constexpr unsigned kBuckets = (64 - kSubBits + 1) * kSub;  // 1920

  void record(std::uint64_t v) noexcept {
    ++buckets_[index_of(v)];
    ++count_;
    if (v > max_) max_ = v;
    if (v < min_) min_ = v;
  }

  void reset() noexcept {
    buckets_.fill(0);
    count_ = 0;
    max_ = 0;
    min_ = ~std::uint64_t{0};
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
  [[nodiscard]] std::uint64_t min() const noexcept { return count_ ? min_ : 0; }

  [[nodiscard]] std::uint64_t percentile(double q) const noexcept {
    if (count_ == 0) return 0;
    std::uint64_t target = static_cast<std::uint64_t>(q * static_cast<double>(count_) + 0.5);
    if (target < 1) target = 1;
    if (target > count_) target = count_;
    std::uint64_t acc = 0;
    for (unsigned i = 0; i < kBuckets; ++i) {
      acc += buckets_[i];
      if (acc >= target) return value_of(i);
    }
    return max_;
  }

 private:
  static unsigned index_of(std::uint64_t v) noexcept {
    if (v < kSub) return static_cast<unsigned>(v);
    const unsigned msb = 63u - static_cast<unsigned>(std::countl_zero(v));
    const unsigned group = msb - kSubBits + 1;
    const unsigned sub = static_cast<unsigned>((v >> (msb - kSubBits)) & (kSub - 1));
    return group * kSub + sub;
  }

  static std::uint64_t value_of(unsigned i) noexcept {
    if (i < kSub) return i;
    const unsigned group = i / kSub;
    const unsigned sub = i % kSub;
    const unsigned shift = group - 1;
    const std::uint64_t base = std::uint64_t{1} << (group + kSubBits - 1);
    return base + (static_cast<std::uint64_t>(sub) << shift) + ((std::uint64_t{1} << shift) >> 1);
  }

  std::array<std::uint64_t, kBuckets> buckets_{};
  std::uint64_t count_ = 0;
  std::uint64_t max_ = 0;
  std::uint64_t min_ = ~std::uint64_t{0};
};

}  // namespace hft
