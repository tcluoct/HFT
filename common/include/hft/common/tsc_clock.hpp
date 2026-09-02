#pragma once
// Timestamping with the CPU's time-stamp counter (~6 ns to read, sub-ns
// resolution). Assumes invariant TSC (constant rate, synchronized across
// cores) which holds on every server/desktop x86 CPU from the last decade.
// See tick2trade docs/DESIGN.md §4 for the full rationale.
//
// Cross-core / cross-process deltas (reader rdtscp - writer rdtscp) are valid
// because the invariant TSC is a single package-wide counter started at reset;
// each process calibrates its own cycles-per-ns against steady_clock and the
// ratio is a hardware constant, so independently calibrated processes agree.

#include <chrono>
#include <cstdint>

#include "hft/common/platform.hpp"

namespace hft {

// Plain rdtsc: cheap, may be reordered by the CPU. Fine for coarse stamps.
HFT_FORCE_INLINE std::uint64_t rdtsc() noexcept { return __rdtsc(); }

// rdtscp: partially serialized (waits for prior instructions to retire).
// Use at measurement end-points so the stamp doesn't drift into the work.
HFT_FORCE_INLINE std::uint64_t rdtscp() noexcept {
  unsigned int aux;
  return __rdtscp(&aux);
}

class TscClock {
 public:
  // Measure TSC frequency against steady_clock. Call once at startup,
  // before any hot thread converts cycles to nanoseconds.
  static void calibrate(std::chrono::milliseconds window = std::chrono::milliseconds(300)) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const std::uint64_t c0 = rdtscp();
    const auto deadline = t0 + window;
    while (clock::now() < deadline) cpu_relax();
    const std::uint64_t c1 = rdtscp();
    const auto t1 = clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    cycles_per_ns_ = static_cast<double>(c1 - c0) / static_cast<double>(ns);
  }

  static double cycles_per_ns() noexcept { return cycles_per_ns_; }

  static std::uint64_t to_ns(std::uint64_t cycles) noexcept {
    return static_cast<std::uint64_t>(static_cast<double>(cycles) / cycles_per_ns_);
  }

  static std::uint64_t ns_to_cycles(std::uint64_t ns) noexcept {
    return static_cast<std::uint64_t>(static_cast<double>(ns) * cycles_per_ns_);
  }

 private:
  inline static double cycles_per_ns_ = 3.0;  // safe default until calibrated
};

}  // namespace hft
