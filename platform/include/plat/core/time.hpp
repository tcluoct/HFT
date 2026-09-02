#pragma once
// plat/core/time.hpp — session-time helpers. All platform time is Nanos since
// midnight of the trading day; in simulation the scheduler owns "now", in
// production the PTP-disciplined hft::clock facade would (§6.4). Components
// only ever see time as event timestamps — that is the determinism contract.

#include <cstdint>

#include "plat/core/types.hpp"

namespace plat {

inline constexpr Nanos kNsPerSec = 1'000'000'000ll;
inline constexpr Nanos kNsPerMs  = 1'000'000ll;
inline constexpr Nanos kNsPerUs  = 1'000ll;

constexpr Nanos at_time(int hh, int mm, int ss = 0, Nanos ns = 0) noexcept {
  return ((static_cast<Nanos>(hh) * 3600 + static_cast<Nanos>(mm) * 60 + ss) * kNsPerSec) + ns;
}

// hh:mm:ss.mmm for reports/logs.
struct TimeParts {
  int hh, mm, ss, ms;
};
constexpr TimeParts time_parts(Nanos t) noexcept {
  const auto total_ms = t / kNsPerMs;
  return {static_cast<int>(total_ms / 3'600'000),
          static_cast<int>((total_ms / 60'000) % 60),
          static_cast<int>((total_ms / 1'000) % 60),
          static_cast<int>(total_ms % 1'000)};
}

}  // namespace plat
