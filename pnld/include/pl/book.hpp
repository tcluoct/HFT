#pragma once
// PositionBook — one instrument's position, lots, and P&L, maintained two
// ways at once:
//
//   FIFO  : open lots in a ring; a reducing fill consumes from the oldest
//           lot forward, realizing (px - lot.px) per share. This is the
//           attribution most tax/reg regimes and most trader intuition use.
//   WAC   : one number (wac_cost); a reducing fill removes a pro-rata slice,
//           truncated toward zero. Weighted-average-cost attribution.
//
// The two report DIFFERENT realized/unrealized splits — and identical
// equity, exactly, always:
//
//     equity := realized - fees + (pos * mark - open_cost)
//            == sum of signed trade cash - fees + pos * mark
//
// The identity holds for ANY cost-removal rule (even WAC's truncated
// division) because realized is defined as cash-in minus cost-removed and
// the removed cost leaves open_cost in the same expression: rounding moves
// P&L between realized and unrealized, never in or out of equity. The tests
// exploit that mercilessly.
//
// on_fill is O(lots consumed) — amortized O(1), since each lot is created
// once and consumed once. Everything else is O(1).

#include <cstdint>

#include "hft/common/platform.hpp"
#include "pl/types.hpp"

namespace pl {

template <unsigned kMaxLots = 4096>
class PositionBook {
  static_assert((kMaxLots & (kMaxLots - 1)) == 0, "kMaxLots must be a power of two");

 public:
  struct Lot {
    i64 qty;  // > 0; direction is the position's sign
    i64 px;
  };

  // Returns the number of lots fully consumed (bench: the drain cost).
  unsigned on_fill(i32 side, i64 qty, i64 px, i64 fee) noexcept {
    fees_ += fee;
    i64 remaining = qty;
    unsigned drained = 0;
    const i32 sign = pos_ >= 0 ? 1 : -1;

    if (pos_ != 0 && side != sign) {
      // Reducing (maybe through zero). WAC removes its pro-rata slice for
      // the whole reduce amount up front; FIFO walks lots.
      const i64 pos_abs = sign > 0 ? pos_ : -pos_;
      const i64 reduce = remaining < pos_abs ? remaining : pos_abs;
      const i64 wac_removed = wac_cost_ * reduce / pos_abs;  // truncates: dust
      wac_realized_ += -side * reduce * px - wac_removed;    // stays in wac_cost_
      wac_cost_ -= wac_removed;

      i64 left = reduce;
      while (left > 0) {
        Lot& l = lots_[head_ & (kMaxLots - 1)];
        const i64 q = left < l.qty ? left : l.qty;
        fifo_realized_ += q * (px - l.px) * sign;
        open_cost_ -= sign * q * l.px;
        pos_ -= sign * q;
        l.qty -= q;
        left -= q;
        if (l.qty == 0) {
          ++head_;
          ++drained;
        }
      }
      remaining -= reduce;
    }

    if (remaining > 0) {  // opening (or flipping through zero)
      if (tail_ - head_ == kMaxLots) {
        ++overflow_;  // configuration error: count it, drop the lot split
        Lot& l = lots_[(tail_ - 1) & (kMaxLots - 1)];
        // merge into newest lot only if same price (keeps exactness)…
        if (l.px == px) l.qty += remaining;
        // …otherwise the book is undersized for the flow; overflow_ flags it.
      } else {
        lots_[tail_ & (kMaxLots - 1)] = Lot{remaining, px};
        ++tail_;
      }
      pos_ += side * remaining;
      open_cost_ += side * remaining * px;
      wac_cost_ += side * remaining * px;
    }
    return drained;
  }

  void on_mark(i64 px) noexcept { mark_ = px; }

  [[nodiscard]] i64 pos() const noexcept { return pos_; }
  [[nodiscard]] i64 mark() const noexcept { return mark_; }
  [[nodiscard]] i64 fees() const noexcept { return fees_; }
  [[nodiscard]] i64 fifo_realized() const noexcept { return fifo_realized_; }
  [[nodiscard]] i64 wac_realized() const noexcept { return wac_realized_; }
  [[nodiscard]] i64 open_cost() const noexcept { return open_cost_; }
  [[nodiscard]] i64 wac_cost() const noexcept { return wac_cost_; }
  [[nodiscard]] i64 fifo_unrealized() const noexcept { return pos_ * mark_ - open_cost_; }
  [[nodiscard]] i64 wac_unrealized() const noexcept { return pos_ * mark_ - wac_cost_; }
  [[nodiscard]] i64 fifo_equity() const noexcept {
    return fifo_realized_ - fees_ + fifo_unrealized();
  }
  [[nodiscard]] i64 wac_equity() const noexcept {
    return wac_realized_ - fees_ + wac_unrealized();
  }
  [[nodiscard]] u32 lot_count() const noexcept { return tail_ - head_; }
  [[nodiscard]] u64 overflows() const noexcept { return overflow_; }
  [[nodiscard]] const Lot& lot(u32 i) const noexcept {  // i = 0 is oldest
    return lots_[(head_ + i) & (kMaxLots - 1)];
  }

  // Structural invariants, brute force: lot quantities sum to |pos|, lot
  // costs sum to open_cost, both attributions agree on equity.
  [[nodiscard]] bool audit() const noexcept {
    i64 q = 0, c = 0;
    const i32 sign = pos_ >= 0 ? 1 : -1;
    for (u32 i = 0; i < lot_count(); ++i) {
      if (lot(i).qty <= 0) return false;
      q += lot(i).qty;
      c += sign * lot(i).qty * lot(i).px;
    }
    const i64 pos_abs = sign > 0 ? pos_ : -pos_;
    return q == pos_abs && c == open_cost_ && fifo_equity() == wac_equity() &&
           overflow_ == 0;
  }

 private:
  Lot lots_[kMaxLots] = {};
  i64 pos_ = 0;
  i64 open_cost_ = 0;      // FIFO: signed cost of the open position
  i64 wac_cost_ = 0;       // WAC: signed cost of the open position
  i64 fifo_realized_ = 0;
  i64 wac_realized_ = 0;
  i64 fees_ = 0;
  i64 mark_ = 0;
  u32 head_ = 0, tail_ = 0;  // free-running lot ring cursors
  u64 overflow_ = 0;
};

}  // namespace pl
