#pragma once
// Shared run-phase switch. Hot loops sample it with a relaxed load once every
// 4096 iterations (amortized control-plane check) so steady-state iterations
// pay nothing for coordination.

#include <atomic>

namespace hft {

enum Phase : int { kWarmup = 0, kMeasure = 1, kStop = 2 };

struct RunControl {
  std::atomic<int> phase{kWarmup};
};

}  // namespace hft
