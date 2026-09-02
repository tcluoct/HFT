#pragma once
// Order-rate throttle: token bucket in 32.32 fixed point, clocked directly
// by the TSC. Refill is a multiply-add on the cycle delta since the last
// call — no division, no floating point, no timer thread on the hot path.

#include <algorithm>
#include <cstdint>

#include "hft/common/platform.hpp"

namespace rg {

class TokenBucket {
 public:
  static constexpr std::uint64_t kOne = std::uint64_t{1} << 32;  // one token, 32.32

  // rate_fp = tokens per TSC cycle in 32.32 fixed point; burst = bucket size
  // in whole tokens. The bucket starts full.
  void configure_raw(std::uint64_t rate_fp, std::uint64_t burst) noexcept {
    rate_fp_ = rate_fp;
    cap_fp_ = burst * kOne;
    tokens_fp_ = cap_fp_;
    // Cap the refill delta at "fills the bucket from empty" so an idle gap
    // (or the very first call) cannot overflow the multiply.
    max_dt_ = rate_fp ? cap_fp_ / rate_fp + 1 : 0;
  }

  void configure(double orders_per_sec, std::uint64_t burst, double cycles_per_ns) noexcept {
    const double tokens_per_cycle = orders_per_sec / (cycles_per_ns * 1e9);
    configure_raw(static_cast<std::uint64_t>(tokens_per_cycle * static_cast<double>(kOne)),
                  burst);
  }

  // Refill from elapsed cycles, then take one token iff `want` and one is
  // available. Rejected-elsewhere orders call with want=false: they never
  // reach the wire, so they must not spend wire budget. Branchless.
  HFT_FORCE_INLINE bool try_consume(std::uint64_t now_tsc, bool want) noexcept {
    const std::uint64_t dt = std::min<std::uint64_t>(now_tsc - last_tsc_, max_dt_);
    last_tsc_ = now_tsc;
    tokens_fp_ = std::min(cap_fp_, tokens_fp_ + dt * rate_fp_);
    const bool ok = tokens_fp_ >= kOne;
    tokens_fp_ -= (static_cast<std::uint64_t>(ok & want)) << 32;
    return ok;
  }

  [[nodiscard]] std::uint64_t tokens() const noexcept { return tokens_fp_ >> 32; }

 private:
  std::uint64_t tokens_fp_ = 0;
  std::uint64_t cap_fp_ = 0;
  std::uint64_t rate_fp_ = 0;
  std::uint64_t max_dt_ = 0;
  std::uint64_t last_tsc_ = 0;
};

}  // namespace rg
