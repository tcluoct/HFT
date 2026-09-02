#pragma once
// Fixed-capacity order pool with a freelist and slotmap-style handles.
//
// The technique: an exchange order id is not a random number — the engine
// *assigns* it, so it can encode the pool slot directly:
//
//     OrderId = (generation << kSlotBits) | slot
//
// Cancel/replace lookup is then: mask off the slot, compare the generation.
// O(1), branch-predictable, ZERO hash map — and stale handles (cancel racing
// a fill, duplicate cancel) are *detected*, not silently mismatched, because
// freeing a slot bumps its generation. This is the slotmap / generational-
// index pattern; the same idea powers entity ids in game engines.
//
// Orders link into price-level FIFOs through 32-bit prev/next indices —
// intrusive lists with no per-node allocation and half the footprint of
// pointer links.

#include <cstdint>
#include <vector>

#include "me/core/types.hpp"

namespace me {

struct Order {
  Token token;
  Price price;
  Qty leaves;
  std::uint32_t prev, next;  // intrusive FIFO links within a price level
  std::uint32_t gen;         // bumped on free/replace: invalidates old handles
  ParticipantId participant;
  SymbolId symbol;
  std::uint8_t side;
  std::uint8_t tif;
  std::uint8_t live;
  std::uint8_t pad;
};
static_assert(sizeof(Order) == 40);

class OrderPool {
 public:
  static constexpr unsigned kSlotBits = 24;  // up to 16M concurrent orders
  static constexpr std::uint64_t kSlotMask = (std::uint64_t{1} << kSlotBits) - 1;

  explicit OrderPool(std::uint32_t capacity) : slots_(capacity) {
    // Chain every slot into the freelist.
    for (std::uint32_t i = 0; i < capacity; ++i) slots_[i].next = i + 1;
    slots_[capacity - 1].next = kNil;
    free_head_ = 0;
  }

  [[nodiscard]] std::uint32_t alloc() noexcept {
    const std::uint32_t s = free_head_;
    if (s == kNil) [[unlikely]] return kNil;
    free_head_ = slots_[s].next;
    slots_[s].live = 1;
    if (++live_ > high_water_) high_water_ = live_;
    return s;
  }

  void free(std::uint32_t s) noexcept {
    Order& o = slots_[s];
    o.live = 0;
    ++o.gen;               // every handle ever issued for this slot is now stale
    o.next = free_head_;
    free_head_ = s;
    --live_;
  }

  // Issued to the client; also used internally for market-data ids.
  [[nodiscard]] OrderId make_id(std::uint32_t s) const noexcept {
    return (static_cast<std::uint64_t>(slots_[s].gen) << kSlotBits) | s;
  }

  // Returns the slot, or kNil if the handle is stale/unknown. O(1), no hash.
  [[nodiscard]] std::uint32_t resolve(OrderId id) const noexcept {
    const std::uint32_t s = static_cast<std::uint32_t>(id & kSlotMask);
    if (s >= slots_.size()) [[unlikely]] return kNil;
    const Order& o = slots_[s];
    if (!o.live || o.gen != static_cast<std::uint32_t>(id >> kSlotBits)) return kNil;
    return s;
  }

  // Invalidate the current handle while keeping the slot live (used by
  // replace: the replaced order gets a fresh id in the same slot).
  void bump_gen(std::uint32_t s) noexcept { ++slots_[s].gen; }

  [[nodiscard]] Order& operator[](std::uint32_t s) noexcept { return slots_[s]; }
  [[nodiscard]] const Order& operator[](std::uint32_t s) const noexcept { return slots_[s]; }

  [[nodiscard]] std::uint32_t live_count() const noexcept { return live_; }
  [[nodiscard]] std::uint32_t high_water() const noexcept { return high_water_; }

 private:
  std::vector<Order> slots_;
  std::uint32_t free_head_ = kNil;
  std::uint32_t live_ = 0;
  std::uint32_t high_water_ = 0;
};

}  // namespace me
