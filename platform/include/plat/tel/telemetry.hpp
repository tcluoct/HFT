#pragma once
// plat/tel/telemetry.hpp — counters and latency histograms (§14, §12.2).
// Recording is O(1) increments on preallocated storage (a shm page in
// production; a struct here); exporting/formatting happens off-path. The
// histogram is log2-bucketed with linear sub-buckets — coarse HDR — which is
// enough to regression-track p50/p99/p99.9 without heap or floating point on
// the record path.

#include <array>
#include <bit>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace plat {

class LatencyHisto {
 public:
  static constexpr int kLog2Buckets = 40;  // up to ~2^40 ns ≈ 18 min
  static constexpr int kSub = 8;           // linear sub-buckets per octave

  void record(std::uint64_t ns) noexcept {
    ++count_;
    sum_ += ns;
    if (ns > max_) max_ = ns;
    // Octave via bit_width (one LZCNT) instead of a shift loop — same
    // bucketing: b = ⌊log2(ns)⌋ − log2(kSub) for ns ≥ kSub, clamped.
    int b = ns < kSub ? 0 : static_cast<int>(std::bit_width(ns)) - 3;
    if (b > kLog2Buckets - 1) b = kLog2Buckets - 1;
    const auto sub = static_cast<int>(b == 0 ? ns : (ns >> (b - 1)) - kSub);
    ++buckets_[static_cast<std::size_t>(b * kSub + (sub < kSub ? sub : kSub - 1))];
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
  [[nodiscard]] std::uint64_t mean() const noexcept { return count_ ? sum_ / count_ : 0; }

  // Percentile by bucket walk; returns the bucket's upper-bound estimate.
  [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
    if (count_ == 0) return 0;
    const auto target = static_cast<std::uint64_t>(p * static_cast<double>(count_));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
      seen += buckets_[i];
      if (seen > target) {
        const int b = static_cast<int>(i) / kSub;
        const int sub = static_cast<int>(i) % kSub;
        const std::uint64_t bound =
            b == 0 ? static_cast<std::uint64_t>(sub)
                   : (static_cast<std::uint64_t>(kSub + sub + 1) << (b - 1)) - 1;
        return bound < max_ ? bound : max_;  // bucket bound can overshoot the max
      }
    }
    return max_;
  }

 private:
  std::array<std::uint64_t, static_cast<std::size_t>(kLog2Buckets* kSub)> buckets_{};
  std::uint64_t count_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t max_ = 0;
};

// Named counters + named histograms. Registration (string-keyed) happens at
// wiring time and returns a stable slot reference; the hot path increments
// through that reference — no strings, no lookups, no allocation (the shm
// counter-page discipline of §14, in-process form).
class Telemetry {
 public:
  // Register-or-fetch a counter slot. Call at setup; keep the reference.
  std::uint64_t& counter(const std::string& name) {
    const auto it = index_.find(name);
    if (it != index_.end()) return slots_[it->second];
    index_.emplace(name, slots_.size());
    slots_.push_back(0);  // deque: references stay valid across growth
    return slots_.back();
  }
  LatencyHisto& histo(const std::string& name) { return histos_[name]; }

  // Report-side reads (cold).
  [[nodiscard]] std::uint64_t get(const std::string& name) const {
    const auto it = index_.find(name);
    return it == index_.end() ? 0 : slots_[it->second];
  }
  [[nodiscard]] std::map<std::string, std::uint64_t> counters() const {
    std::map<std::string, std::uint64_t> out;
    for (const auto& [n, i] : index_) out.emplace(n, slots_[i]);
    return out;
  }
  [[nodiscard]] const std::map<std::string, LatencyHisto>& histos() const { return histos_; }

 private:
  std::map<std::string, std::size_t> index_;
  std::deque<std::uint64_t> slots_;
  std::map<std::string, LatencyHisto> histos_;
};

}  // namespace plat
