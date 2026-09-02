#pragma once
// plat/md/stock_book.hpp — the order-by-order stock book (§8.2): built
// entirely from 逐笔委托/逐笔成交; the 3-second exchange snapshot would only
// ever be an integrity check. Maintains exact price-time levels, the
// instrument's limit state, and queue-position estimates for our own resting
// orders (the input the T+0 and arb passive legs price with).
//
// Queue estimate mechanics: each add gets a per-level arrival ordinal. An own
// order's `ahead` starts as the level quantity in front of it and shrinks
// when (a) trades consume the level front, or (b) an earlier-ordinal order at
// the level cancels. Trades always consume from the front under price-time
// priority, so this is exact for a lossless feed, not a heuristic.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "plat/codec/msgs.hpp"
#include "plat/core/hash.hpp"
#include "plat/core/types.hpp"
#include "plat/ref/instrument.hpp"

namespace plat {

class StockBook {
 public:
  StockBook() = default;
  StockBook(const Instrument* in) : in_(in) {}

  struct Top {
    Price bid_px = 0, ask_px = 0;
    Qty bid_qty = 0, ask_qty = 0;
    Price last_px = 0;
    LimitState limit = LimitState::Normal;
  };

  // Returns true when top-of-book changed (the engine publishes BookTopEvt
  // only on change — §8.2).
  bool on_order(const MdOrderEvt& e) {
    last_evt_ts_ = e.ts;
    if (e.action == MdAction::Add) {
      Level& lvl = level(e.side, e.px);
      lvl.qty += e.qty;
      lvl.orders += 1;
      const std::uint64_t ordinal = ++lvl.next_ordinal;
      orders_.emplace(e.order_no, OrderRef{e.px, e.qty, ordinal, e.side});
      if (auto own = own_.find(e.order_no); own != own_.end()) {
        // Our own add arriving on the public feed: freeze the queue ahead.
        own->second.px = e.px;
        own->second.side = e.side;
        own->second.ordinal = ordinal;
        own->second.ahead = lvl.qty - e.qty;  // everything already at the level
      }
      return top_changed();
    }
    // Cancel: remove remaining qty from the level.
    auto it = orders_.find(e.order_no);
    if (it == orders_.end()) return false;  // unknown (pre-join history)
    reduce(it->second, it->second.qty, /*from_front=*/false, it->first);
    orders_.erase(it);
    own_.erase(e.order_no);
    return top_changed();
  }

  bool on_trade(const MdTradeEvt& e) {
    last_evt_ts_ = e.ts;
    last_px_ = e.px;
    consume(e.buy_no, e.qty);
    consume(e.sell_no, e.qty);
    return top_changed();
  }

  // Register an own order by its venue number (from the Ack), reconstructing
  // its queue position from the already-processed public add. Feed latency is
  // below exec-report latency at the same site, so the add always precedes
  // the ack; an order absent from the book is already gone (IOC) — tracking
  // it would only leak a placeholder.
  void track_own(std::uint64_t venue_no) {
    const auto it = orders_.find(venue_no);
    if (it == orders_.end()) return;
    const OrderRef& r = it->second;
    OwnRef own{};
    own.px = r.px;
    own.side = r.side;
    own.ordinal = r.ordinal;
    // Everything at the level with an earlier ordinal is ahead.
    Qty ahead = 0;
    for (const auto& [no, o] : orders_)
      if (o.side == r.side && o.px == r.px && o.ordinal < r.ordinal) ahead += o.qty;
    own.ahead = ahead;
    own_[venue_no] = own;
  }
  void untrack_own(std::uint64_t venue_no) { own_.erase(venue_no); }

  // Estimated quantity queued ahead of our resting order (-1 if unknown).
  [[nodiscard]] Qty queue_ahead(std::uint64_t venue_no) const {
    const auto it = own_.find(venue_no);
    return it == own_.end() ? -1 : it->second.ahead;
  }

  [[nodiscard]] Top top() const {
    Top t;
    for (auto it = bids_.begin(); it != bids_.end(); ++it) {
      if (it->second.qty > 0) {
        t.bid_px = it->first;
        t.bid_qty = it->second.qty;
        break;
      }
    }
    for (auto it = asks_.begin(); it != asks_.end(); ++it) {
      if (it->second.qty > 0) {
        t.ask_px = it->first;
        t.ask_qty = it->second.qty;
        break;
      }
    }
    t.last_px = last_px_ ? last_px_ : (in_ ? in_->prev_close : 0);
    if (in_) {
      if (t.bid_px == in_->upper_limit && t.ask_qty == 0) t.limit = LimitState::AtUpperLimit;
      else if (t.ask_px == in_->lower_limit && t.bid_qty == 0) t.limit = LimitState::AtLowerLimit;
    }
    return t;
  }

  [[nodiscard]] Nanos last_event_ts() const noexcept { return last_evt_ts_; }
  [[nodiscard]] bool quarantined() const noexcept { return quarantined_; }
  void set_quarantined(bool q) noexcept { quarantined_ = q; }

  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    for (const auto& [px, lvl] : bids_) {
      h.mix(px);
      h.mix(lvl.qty);
    }
    for (const auto& [px, lvl] : asks_) {
      h.mix(px);
      h.mix(lvl.qty);
    }
    h.mix(last_px_);
    // Quarantine is behavioral state (§8.1, review R1): recovery that
    // dropped it would trade on a book the uninterrupted run refused.
    h.mix(quarantined_);
    return h.value();
  }

  // Snapshot support (§11.1). Orders serialize in venue-number order so the
  // byte stream is deterministic; levels rebuild from the orders.
  void save(std::vector<std::uint8_t>& out) const {
    append(out, last_px_);
    append(out, last_evt_ts_);
    append(out, prev_top_);
    append(out, quarantined_);
    std::vector<std::pair<std::uint64_t, OrderRef>> rows(orders_.begin(), orders_.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    append(out, static_cast<std::uint64_t>(rows.size()));
    for (const auto& [no, r] : rows) {
      append(out, no);
      append(out, r);
    }
    append(out, static_cast<std::uint64_t>(own_.size()));
    for (const auto& [no, o] : own_) {
      append(out, no);
      append(out, o);
    }
  }
  std::size_t load(const std::uint8_t* p, std::size_t avail) {
    bids_.clear();
    asks_.clear();
    orders_.clear();
    own_.clear();
    std::size_t off = 0;
    off = read(p, off, avail, last_px_);
    off = read(p, off, avail, last_evt_ts_);
    off = read(p, off, avail, prev_top_);
    off = read(p, off, avail, quarantined_);
    std::uint64_t n = 0;
    off = read(p, off, avail, n);
    for (std::uint64_t i = 0; i < n; ++i) {
      std::uint64_t no = 0;
      OrderRef r{};
      off = read(p, off, avail, no);
      off = read(p, off, avail, r);
      orders_.emplace(no, r);
      Level& lvl = level(r.side, r.px);
      lvl.qty += r.qty;
      lvl.orders += 1;
      if (r.ordinal >= lvl.next_ordinal) lvl.next_ordinal = r.ordinal + 1;
    }
    off = read(p, off, avail, n);
    for (std::uint64_t i = 0; i < n; ++i) {
      std::uint64_t no = 0;
      OwnRef o{};
      off = read(p, off, avail, no);
      off = read(p, off, avail, o);
      own_[no] = o;
    }
    return off;
  }

 private:
  struct Level {
    Qty qty = 0;
    std::uint32_t orders = 0;
    std::uint64_t next_ordinal = 0;
  };
  struct OrderRef {
    Price px;
    Qty qty;
    std::uint64_t ordinal;
    Side side;
  };
  struct OwnRef {
    Price px = 0;
    Qty ahead = -1;
    std::uint64_t ordinal = 0;
    Side side = Side::Buy;
  };

  Level& level(Side s, Price px) { return s == Side::Buy ? bids_[px] : asks_[px]; }

  template <typename T>
  static void append(std::vector<std::uint8_t>& out, const T& v) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), b, b + sizeof(T));
  }
  template <typename T>
  static std::size_t read(const std::uint8_t* p, std::size_t off, std::size_t avail, T& v) {
    if (off + sizeof(T) <= avail) std::memcpy(&v, p + off, sizeof(T));
    return off + sizeof(T);
  }

  void consume(std::uint64_t order_no, Qty qty) {
    auto it = orders_.find(order_no);
    if (it == orders_.end()) return;  // aggressor fully filled before resting, or unknown
    const Qty take = qty < it->second.qty ? qty : it->second.qty;
    reduce(it->second, take, /*from_front=*/true, order_no);
    if (it->second.qty == 0) {
      orders_.erase(it);
      own_.erase(order_no);
    }
  }

  template <typename Map>
  void reduce_level(Map& side_map, const OrderRef& ref, Qty by) {
    auto lit = side_map.find(ref.px);
    if (lit != side_map.end()) {
      lit->second.qty -= by;
      if (ref.qty - by <= 0) lit->second.orders -= 1;
      if (lit->second.qty <= 0) side_map.erase(lit);
    }
  }

  void reduce(OrderRef& ref, Qty by, bool from_front, std::uint64_t order_no) {
    if (ref.side == Side::Buy) reduce_level(bids_, ref, by);
    else reduce_level(asks_, ref, by);
    // Own-order queue updates: front consumption reduces everyone behind;
    // a cancel reduces only those behind the cancelled order.
    for (auto& [no, own] : own_) {
      if (no == order_no || own.px != ref.px || own.side != ref.side) continue;
      if (from_front || own.ordinal > ref.ordinal) {
        own.ahead -= by;
        if (own.ahead < 0) own.ahead = 0;
      }
    }
    ref.qty -= by;
  }

  [[nodiscard]] bool top_changed() {
    const Top t = top();
    const bool changed = t.bid_px != prev_top_.bid_px || t.bid_qty != prev_top_.bid_qty ||
                         t.ask_px != prev_top_.ask_px || t.ask_qty != prev_top_.ask_qty ||
                         t.last_px != prev_top_.last_px || t.limit != prev_top_.limit;
    prev_top_ = t;
    return changed;
  }

  const Instrument* in_ = nullptr;
  std::map<Price, Level, std::greater<Price>> bids_;
  std::map<Price, Level> asks_;
  std::unordered_map<std::uint64_t, OrderRef> orders_;
  std::map<std::uint64_t, OwnRef> own_;  // ordered: deterministic hash/iteration
  Price last_px_ = 0;
  Nanos last_evt_ts_ = 0;
  Top prev_top_{};
  bool quarantined_ = false;
};

}  // namespace plat
