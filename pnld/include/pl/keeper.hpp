#pragma once
// Keeper — the firm's position and P&L across all instruments, with the
// firm-level rollup maintained incrementally: every fill/mark applies a
// delta to the firm aggregates (position value, open cost, realized, fees),
// so "what is the firm's equity right now" is O(1) — no walk over books —
// and every delta is auditable against a brute-force recompute.
//
// Determinism: each fill folds (sym, pos, realized) into an FNV digest;
// identical tapes must produce identical digests.

#include <cstdint>
#include <memory>

#include "hft/common/platform.hpp"
#include "pl/book.hpp"
#include "pl/types.hpp"

namespace pl {

template <u32 NSym, unsigned kMaxLots = 2048>
class Keeper {
 public:
  using Book = PositionBook<kMaxLots>;

  Keeper() : books_(new Book[NSym]) {}

  bool on_fill(const Fill& f) noexcept {
    if (f.sym >= NSym || f.qty <= 0 || (f.side != 1 && f.side != -1)) return false;
    Book& b = books_[f.sym];
    const i64 pos0 = b.pos(), oc0 = b.open_cost(), wc0 = b.wac_cost();
    const i64 fr0 = b.fifo_realized(), wr0 = b.wac_realized();
    b.on_fill(f.side, f.qty, f.px, f.fee);
    firm_pos_mark_ += (b.pos() - pos0) * b.mark();
    firm_open_cost_ += b.open_cost() - oc0;
    firm_wac_cost_ += b.wac_cost() - wc0;
    firm_fifo_realized_ += b.fifo_realized() - fr0;
    firm_wac_realized_ += b.wac_realized() - wr0;
    firm_fees_ += f.fee;
    digest_ ^= (u64{f.sym} << 40) ^ static_cast<u64>(b.pos() * 0x9E3779B97F4A7C15ll) ^
               static_cast<u64>(b.fifo_realized());
    digest_ *= 0x100000001B3ull;
    return true;
  }

  bool on_mark(const Mark& m) noexcept {
    if (m.sym >= NSym) return false;
    Book& b = books_[m.sym];
    firm_pos_mark_ += b.pos() * (m.px - b.mark());
    b.on_mark(m.px);
    return true;
  }

  [[nodiscard]] const Book& book(u32 sym) const noexcept { return books_[sym]; }
  [[nodiscard]] i64 firm_fifo_realized() const noexcept { return firm_fifo_realized_; }
  [[nodiscard]] i64 firm_wac_realized() const noexcept { return firm_wac_realized_; }
  [[nodiscard]] i64 firm_fees() const noexcept { return firm_fees_; }
  [[nodiscard]] i64 firm_fifo_unrealized() const noexcept {
    return firm_pos_mark_ - firm_open_cost_;
  }
  [[nodiscard]] i64 firm_wac_unrealized() const noexcept {
    return firm_pos_mark_ - firm_wac_cost_;
  }
  [[nodiscard]] i64 firm_fifo_equity() const noexcept {
    return firm_fifo_realized_ - firm_fees_ + firm_fifo_unrealized();
  }
  [[nodiscard]] i64 firm_wac_equity() const noexcept {
    return firm_wac_realized_ - firm_fees_ + firm_wac_unrealized();
  }
  [[nodiscard]] u64 digest() const noexcept { return digest_; }

  // Brute-force recompute of every firm aggregate from the books; deep also
  // re-audits every book's lot structure. The incremental deltas must have
  // left the aggregates exactly equal to this.
  [[nodiscard]] bool audit(bool deep = false) const noexcept {
    i64 pm = 0, oc = 0, wc = 0, fr = 0, wr = 0, fe = 0;
    for (u32 s = 0; s < NSym; ++s) {
      const Book& b = books_[s];
      pm += b.pos() * b.mark();
      oc += b.open_cost();
      wc += b.wac_cost();
      fr += b.fifo_realized();
      wr += b.wac_realized();
      fe += b.fees();
      if (deep && !b.audit()) return false;
    }
    return pm == firm_pos_mark_ && oc == firm_open_cost_ && wc == firm_wac_cost_ &&
           fr == firm_fifo_realized_ && wr == firm_wac_realized_ && fe == firm_fees_;
  }

 private:
  std::unique_ptr<Book[]> books_;
  i64 firm_pos_mark_ = 0;   // sum over books of pos * mark
  i64 firm_open_cost_ = 0;  // FIFO open cost
  i64 firm_wac_cost_ = 0;   // WAC open cost
  i64 firm_fifo_realized_ = 0;
  i64 firm_wac_realized_ = 0;
  i64 firm_fees_ = 0;
  u64 digest_ = 0xCBF29CE484222325ull;
};

}  // namespace pl
