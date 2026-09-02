#pragma once
// Exchange-side limit order book: a flat price ladder where each level is an
// intrusive FIFO of orders (head = oldest = first to trade). Price-time
// priority falls out of the structure: price priority from the ladder scan
// direction, time priority from always inserting at the tail and always
// matching from the head.

#include <array>
#include <cstdint>

#include "me/core/order_pool.hpp"
#include "me/core/types.hpp"

namespace me {

class Book {
 public:
  static constexpr int kLadder = 1 << 13;  // 8192-tick band

  struct Level {
    std::uint32_t head = kNil;  // oldest order (matches first)
    std::uint32_t tail = kNil;  // newest order (inserts here)
    std::uint64_t qty = 0;
    std::uint32_t count = 0;
  };

  void set_anchor(Price mid) noexcept { anchor_ = mid - kLadder / 2; }

  [[nodiscard]] bool in_band(Price px) const noexcept {
    const std::int64_t off = px - anchor_;
    return off >= 0 && off < kLadder;
  }

  [[nodiscard]] bool has_bid() const noexcept { return best_bid_ >= 0; }
  [[nodiscard]] bool has_ask() const noexcept { return best_ask_ < kLadder; }
  [[nodiscard]] Price bid_px() const noexcept { return anchor_ + best_bid_; }
  [[nodiscard]] Price ask_px() const noexcept { return anchor_ + best_ask_; }
  [[nodiscard]] std::uint32_t bid_head() const noexcept {
    return bids_[static_cast<std::size_t>(best_bid_)].head;
  }
  [[nodiscard]] std::uint32_t ask_head() const noexcept {
    return asks_[static_cast<std::size_t>(best_ask_)].head;
  }

  // Insert at the tail of the order's price level: newest = last to match.
  void insert(OrderPool& pool, std::uint32_t s) noexcept {
    Order& o = pool[s];
    const int i = idx(o.price);
    Level& L = lvl(o.side, i);
    o.prev = L.tail;
    o.next = kNil;
    if (L.tail != kNil)
      pool[L.tail].next = s;
    else
      L.head = s;
    L.tail = s;
    L.qty += o.leaves;
    ++L.count;
    if (o.side == kBuy) {
      if (i > best_bid_) best_bid_ = i;
    } else {
      if (i < best_ask_) best_ask_ = i;
    }
  }

  // Unlink an order; advances the best price if its level just emptied.
  void remove(OrderPool& pool, std::uint32_t s) noexcept {
    Order& o = pool[s];
    const int i = idx(o.price);
    Level& L = lvl(o.side, i);
    if (o.prev != kNil)
      pool[o.prev].next = o.next;
    else
      L.head = o.next;
    if (o.next != kNil)
      pool[o.next].prev = o.prev;
    else
      L.tail = o.prev;
    L.qty -= o.leaves;
    --L.count;
    if (L.count == 0) {
      if (o.side == kBuy && i == best_bid_) {
        while (best_bid_ >= 0 && bids_[static_cast<std::size_t>(best_bid_)].count == 0)
          --best_bid_;
      } else if (o.side == kSell && i == best_ask_) {
        while (best_ask_ < kLadder && asks_[static_cast<std::size_t>(best_ask_)].count == 0)
          ++best_ask_;
      }
    }
  }

  // Partial fill / in-place qty reduction (keeps time priority).
  void reduce(OrderPool& pool, std::uint32_t s, Qty dec) noexcept {
    Order& o = pool[s];
    lvl(o.side, idx(o.price)).qty -= dec;
    o.leaves -= dec;
  }

  // FOK pre-check: total opposite-side qty crossable at `limit` or better.
  // Stops as soon as `need` is reached; only runs for FOK orders.
  [[nodiscard]] std::uint64_t crossable_qty(std::uint8_t aggr_side, Price limit,
                                            std::uint64_t need) const noexcept {
    std::uint64_t acc = 0;
    if (aggr_side == kBuy) {
      for (int i = best_ask_; i < kLadder; ++i) {
        if (anchor_ + i > limit) break;
        acc += asks_[static_cast<std::size_t>(i)].qty;
        if (acc >= need) break;
      }
    } else {
      for (int i = best_bid_; i >= 0; --i) {
        if (anchor_ + i < limit) break;
        acc += bids_[static_cast<std::size_t>(i)].qty;
        if (acc >= need) break;
      }
    }
    return acc;
  }

 private:
  [[nodiscard]] int idx(Price px) const noexcept { return static_cast<int>(px - anchor_); }
  [[nodiscard]] Level& lvl(std::uint8_t side, int i) noexcept {
    return (side == kBuy) ? bids_[static_cast<std::size_t>(i)]
                          : asks_[static_cast<std::size_t>(i)];
  }

  Price anchor_ = 0;
  int best_bid_ = -1;
  int best_ask_ = kLadder;
  std::array<Level, kLadder> bids_{};
  std::array<Level, kLadder> asks_{};
};

}  // namespace me
