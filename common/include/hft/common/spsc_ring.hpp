#pragma once
// Wait-free single-producer/single-consumer ring buffer.
// For interprocess use, construct the ring in shared storage before either
// endpoint attaches. Each endpoint owns its cursor and cached peer cursor.
// One slot is reserved to distinguish full from empty.
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
  static_assert(std::atomic<std::size_t>::is_always_lock_free);

 public:
  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer only. The consumer can only free space, so a successful
  // check remains valid until the producer pushes.
  [[nodiscard]] HFT_FORCE_INLINE bool can_push() noexcept {
    return has_room((head_.load(std::memory_order_relaxed) + 1) & kMask);
  }

  // Producer only.
  [[nodiscard]] HFT_FORCE_INLINE bool try_push(const T& v) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) & kMask;
    if (!has_room(next)) return false;
    slots_[head] = v;
    head_.store(next, std::memory_order_release);  // publish the slot write
    return true;
  }

  // Consumer only. The slot is held until pop(), allowing a consumer to
  // defer an item without advancing the queue.
  [[nodiscard]] HFT_FORCE_INLINE const T* front() noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == cached_head_) [[unlikely]] {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == cached_head_) return nullptr;
    }
    return &slots_[tail];
  }

  // Consumer only. Requires a successful front(). Invalidates its pointer.
  HFT_FORCE_INLINE void pop() noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    tail_.store((tail + 1) & kMask, std::memory_order_release);  // free the slot
  }

  [[nodiscard]] HFT_FORCE_INLINE bool try_pop(T& out) noexcept {
    const T* slot = front();
    if (!slot) return false;
    out = *slot;
    pop();
    return true;
  }

 private:
  static constexpr std::size_t kMask = Capacity - 1;

  [[nodiscard]] HFT_FORCE_INLINE bool has_room(std::size_t next) noexcept {
    if (next == cached_tail_) [[unlikely]]
      cached_tail_ = tail_.load(std::memory_order_acquire);
    return next != cached_tail_;
  }

  std::array<T, Capacity> slots_{};

  // Producer-owned line: index it writes + its cached view of the consumer.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_{0};

  // Consumer-owned line.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_{0};
};

}  // namespace hft
