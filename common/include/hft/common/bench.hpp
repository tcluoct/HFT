#pragma once
// Bulk micro-benchmark helper: run a closure R times between rdtscp pairs,
// report ns-per-op of the fastest round (least descheduling noise).
//
// The signal fences are load-bearing: MSVC's __rdtscp intrinsic is NOT a
// compiler barrier, and under LTO whole measured loops get scheduled outside
// the rdtscp pair and read as 0 ns. The fences forbid that reordering.
// Feed results into `sink` (volatile) so the work cannot be dead-code
// eliminated; read `sink` at the start of a round to defeat cross-round CSE.

#include <atomic>
#include <cstdint>

#include "hft/common/tsc_clock.hpp"

namespace hft {

inline volatile std::uint64_t bench_sink = 0;

template <typename F>
double bench_ns_per_op(std::uint64_t ops, F&& fn, int rounds = 5) {
  double best = 1e30;
  for (int r = 0; r < rounds; ++r) {
    const std::uint64_t c0 = rdtscp();
    std::atomic_signal_fence(std::memory_order_seq_cst);
    fn();
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const std::uint64_t c1 = rdtscp();
    const double ns =
        static_cast<double>(TscClock::to_ns(c1 - c0)) / static_cast<double>(ops);
    if (ns < best) best = ns;
  }
  return best;
}

}  // namespace hft
