#pragma once
// The 64-byte versioned record: one cache line = one stamp word + 56 bytes
// of payload. The stamp is a per-slot seqlock that also encodes *which*
// global sequence number the slot currently holds:
//
//   stamp = 0                     never written (fresh zero-filled memory)
//   stamp = 2*seq + 1  (odd)      write of `seq` in progress
//   stamp = 2*seq + 2  (even)     `seq` fully published
//
// Because the writer visits a slot with sequence numbers seq, seq+N,
// seq+2N, ... (N = ring capacity), stamps are strictly increasing per slot —
// there is no ABA. A reader expecting `seq` compares against 2*seq+2:
//
//   observed <  expected   not published yet — keep spinning
//   observed == expected   candidate — copy payload, re-check stamp
//   observed >  expected   the writer has lapped this reader; data is gone
//
// The payload is stored as seven relaxed std::atomic<uint64_t> words rather
// than raw bytes. On x86-64 a relaxed atomic store/load compiles to the same
// plain MOV as a non-atomic access — zero cost — but it makes the seqlock
// formally data-race-free under the C++ memory model instead of "works on
// this compiler" (Boehm, "Can seqlocks get along with programming language
// memory models?", MSPC 2012).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "hft/common/platform.hpp"

namespace sb {

inline constexpr std::size_t kSlotBytes = 64;
inline constexpr std::size_t kPayloadBytes = kSlotBytes - sizeof(std::uint64_t);  // 56
inline constexpr std::size_t kPayloadWords = kPayloadBytes / sizeof(std::uint64_t);

// Anything trivially copyable that fits in a slot can ride the bus.
template <typename T>
concept BusPayload = std::is_trivially_copyable_v<T> && sizeof(T) <= kPayloadBytes &&
                     alignof(T) <= alignof(std::uint64_t);

HFT_FORCE_INLINE constexpr std::uint64_t stamp_busy(std::uint64_t seq) noexcept {
  return 2 * seq + 1;
}
HFT_FORCE_INLINE constexpr std::uint64_t stamp_done(std::uint64_t seq) noexcept {
  return 2 * seq + 2;
}

struct alignas(kSlotBytes) Slot {
  std::atomic<std::uint64_t> stamp{0};
  std::atomic<std::uint64_t> word[kPayloadWords];
};

static_assert(sizeof(Slot) == kSlotBytes, "one slot must be exactly one cache line");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "shared-memory atomics must be lock-free: a lock-based fallback would put "
              "the lock in one process's address space only");

}  // namespace sb
