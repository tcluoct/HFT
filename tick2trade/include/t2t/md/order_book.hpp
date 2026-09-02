#pragma once
// Order-by-order (L3) limit order book, built for the feed-handler hot path.
//
// Low-latency techniques:
//  * flat price ladder: levels live in a plain array indexed by tick offset
//    from an anchor -> O(1) level access, no tree, no allocation, and the
//    touch area stays resident in L1/L2
//  * open-addressing order-id map with linear probing and SoA key/value
//    arrays: the key scan touches a dense cache line of keys only
//  * backward-shift deletion (no tombstones): performance cannot degrade
//    over a full trading day of adds/deletes
//  * zero allocation after construction; every structure is pre-sized
//  * best bid/ask maintained incrementally; on emptying the touch we scan
//    toward worse prices -- bounded, branch-predictable walk over the array

#include <array>
#include <cstdint>
#include <vector>

#include "hft/common/platform.hpp"
#include "t2t/proto/wire.hpp"

namespace t2t {

class OrderBook {
 public:
  static constexpr int kLadder = 1 << 13;  // 8192-tick band around the anchor
  static constexpr std::size_t kMapLog = 16;
  static constexpr std::size_t kMapCap = std::size_t{1} << kMapLog;  // 65536 orders

  struct Tob {
    Price bid_px = 0, ask_px = 0;
    std::uint64_t bid_qty = 0, ask_qty = 0;
    bool valid = false;
  };

  OrderBook() : keys_(kMapCap, 0), vals_(kMapCap) {}

  void set_anchor(Price mid) noexcept { anchor_ = mid - kLadder / 2; }

  // Applies one message. Returns true iff the top of book changed.
  HFT_FORCE_INLINE bool apply(const MdMsg& m) noexcept {
    switch (m.type) {
      case kMsgAdd:
        return add(m);
      case kMsgExecute:
      case kMsgCancel:
        return reduce(m.order_id, m.qty);
      case kMsgDelete:
        return remove(m.order_id);
      default:
        return false;
    }
  }

  [[nodiscard]] Tob tob() const noexcept {
    Tob t;
    if (best_bid_ >= 0 && best_ask_ < kLadder) {
      t.bid_px = anchor_ + best_bid_;
      t.bid_qty = bids_[static_cast<std::size_t>(best_bid_)].qty;
      t.ask_px = anchor_ + best_ask_;
      t.ask_qty = asks_[static_cast<std::size_t>(best_ask_)].qty;
      t.valid = true;
    }
    return t;
  }

 private:
  struct Level {
    std::uint64_t qty = 0;
    std::uint32_t count = 0;
  };
  struct OrderRec {
    Price price;
    Qty qty;
    std::uint8_t side;
  };

  bool add(const MdMsg& m) noexcept {
    const std::int64_t off = m.price - anchor_;
    if (off < 0 || off >= kLadder) [[unlikely]] return false;  // outside band
    if (!insert(m.order_id, OrderRec{m.price, m.qty, m.side})) [[unlikely]] return false;
    const int i = static_cast<int>(off);
    if (m.side == kBuy) {
      Level& lvl = bids_[static_cast<std::size_t>(i)];
      lvl.qty += m.qty;
      ++lvl.count;
      if (i > best_bid_) {
        best_bid_ = i;
        return true;
      }
      return i == best_bid_;
    }
    Level& lvl = asks_[static_cast<std::size_t>(i)];
    lvl.qty += m.qty;
    ++lvl.count;
    if (i < best_ask_) {
      best_ask_ = i;
      return true;
    }
    return i == best_ask_;
  }

  bool reduce(OrderId id, Qty by) noexcept {
    const std::size_t s = find(id);
    if (s == kNotFound) [[unlikely]] return false;
    return reduce_at(s, by);
  }

  bool remove(OrderId id) noexcept {
    const std::size_t s = find(id);
    if (s == kNotFound) [[unlikely]] return false;
    return reduce_at(s, vals_[s].qty);
  }

  bool reduce_at(std::size_t s, Qty by) noexcept {
    const std::uint8_t side = vals_[s].side;
    const int i = static_cast<int>(vals_[s].price - anchor_);
    const Qty dec = by < vals_[s].qty ? by : vals_[s].qty;
    vals_[s].qty -= dec;
    const bool gone = (vals_[s].qty == 0);

    Level& lvl = (side == kBuy) ? bids_[static_cast<std::size_t>(i)]
                                : asks_[static_cast<std::size_t>(i)];
    lvl.qty -= dec;
    if (gone) {
      --lvl.count;
      erase_at(s);
    }
    const bool was_top = (side == kBuy) ? (i == best_bid_) : (i == best_ask_);
    if (gone && lvl.count == 0 && was_top) advance_best(side);
    return was_top;
  }

  void advance_best(std::uint8_t side) noexcept {
    if (side == kBuy) {
      while (best_bid_ >= 0 && bids_[static_cast<std::size_t>(best_bid_)].count == 0) --best_bid_;
    } else {
      while (best_ask_ < kLadder && asks_[static_cast<std::size_t>(best_ask_)].count == 0)
        ++best_ask_;
    }
  }

  // ---- open-addressing order-id map (SoA, linear probing) ----
  static constexpr std::size_t kNotFound = ~std::size_t{0};
  static constexpr std::size_t kMapMask = kMapCap - 1;

  static HFT_FORCE_INLINE std::size_t hash(OrderId id) noexcept {
    // Fibonacci hashing: multiply spreads entropy, top bits index the table.
    return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> (64 - kMapLog));
  }

  [[nodiscard]] std::size_t find(OrderId id) const noexcept {
    std::size_t s = hash(id);
    for (;;) {
      const OrderId k = keys_[s];
      if (k == id) return s;
      if (k == 0) return kNotFound;
      s = (s + 1) & kMapMask;
    }
  }

  bool insert(OrderId id, OrderRec rec) noexcept {
    std::size_t s = hash(id);
    while (keys_[s] != 0) {
      if (keys_[s] == id) [[unlikely]] return false;  // duplicate id
      s = (s + 1) & kMapMask;
    }
    keys_[s] = id;
    vals_[s] = rec;
    return true;
  }

  // Backward-shift deletion keeps probe chains dense forever (no tombstones).
  void erase_at(std::size_t i) noexcept {
    std::size_t j = i;
    for (;;) {
      keys_[i] = 0;
      for (;;) {
        j = (j + 1) & kMapMask;
        if (keys_[j] == 0) return;
        const std::size_t ideal = hash(keys_[j]);
        // Movable iff j's probe path from its ideal slot passes through i.
        if (((j - ideal) & kMapMask) >= ((j - i) & kMapMask)) break;
      }
      keys_[i] = keys_[j];
      vals_[i] = vals_[j];
      i = j;
    }
  }

  Price anchor_ = 0;
  int best_bid_ = -1;
  int best_ask_ = kLadder;
  std::array<Level, kLadder> bids_{};
  std::array<Level, kLadder> asks_{};
  std::vector<OrderId> keys_;   // SoA: keys scanned separately from values
  std::vector<OrderRec> vals_;
};

}  // namespace t2t
