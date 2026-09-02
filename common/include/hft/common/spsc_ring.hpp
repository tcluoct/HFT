#pragma once
// Wait-free single-producer/single-consumer ring buffer.
//
// Low-latency techniques used here:
//  * power-of-two capacity  -> index wrap is a mask, not a modulo
//  * acquire/release atomics-> exactly the synchronization needed, no more
//  * per-owner cache lines  -> producer and consumer state never false-share
//  * cached counterpart idx -> cross-core cache-coherence traffic happens only
//    when the ring *looks* full/empty, not on every push/pop (rigtorp trick)
//  * trivially-copyable T   -> memcpy semantics, no constructors on hot path

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <type_traits>

#include "hft/common/platform.hpp"

namespace hft {

template <typename T, std::size_t Capacity>
  requires std::is_trivially_copyable_v<T> && (std::has_single_bit(Capacity))
class SpscRing {
 public:
  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer thread only.
  [[nodiscard]] HFT_FORCE_INLINE bool try_push(const T& v) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) & kMask;
    if (next == cached_tail_) [[unlikely]] {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (next == cached_tail_) return false;  // truly full
    }
    slots_[head] = v;
    head_.store(next, std::memory_order_release);  // publish the slot write
    return true;
  }

  // Consumer thread only.
  [[nodiscard]] HFT_FORCE_INLINE bool try_pop(T& out) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == cached_head_) [[unlikely]] {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == cached_head_) return false;  // truly empty
    }
    out = slots_[tail];
    tail_.store((tail + 1) & kMask, std::memory_order_release);  // free the slot
    return true;
  }

 private:
  static constexpr std::size_t kMask = Capacity - 1;

  std::array<T, Capacity> slots_{};

  // Producer-owned line: index it writes + its cached view of the consumer.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_{0};

  // Consumer-owned line.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_{0};
};

}  // namespace hft
