#pragma once
// Compile-time wiring: ring sizes and symbol capacity are constants so every
// index mask, array bound, and capacity check folds at compile time.

#include <cstddef>

#include "hft/common/spsc_ring.hpp"
#include "t2t/proto/wire.hpp"

namespace t2t {

inline constexpr unsigned kMaxSymbols = 4;

inline constexpr std::size_t kMdRingSize = 1 << 16;     // 64k * 32 B = 2 MiB
inline constexpr std::size_t kBookRingSize = 1 << 14;
inline constexpr std::size_t kOrderRingSize = 1 << 12;
inline constexpr std::size_t kExecRingSize = 1 << 12;

using MdRing = hft::SpscRing<MdMsg, kMdRingSize>;
using BookRing = hft::SpscRing<BookUpdate, kBookRingSize>;
using OrderRing = hft::SpscRing<NewOrder, kOrderRingSize>;
using ExecRing = hft::SpscRing<ExecReport, kExecRingSize>;

}  // namespace t2t
