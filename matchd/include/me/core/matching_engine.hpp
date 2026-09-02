#pragma once
// The matching core.
//
// Deliberately SINGLE-THREADED. This is how real exchanges work (NASDAQ,
// CME partition symbols across engine instances, each one single-threaded)
// and it is a latency/correctness technique in its own right:
//   * no locks, no atomics, no memory-ordering reasoning inside the core;
//   * given the same inbound sequence it produces byte-identical outbound
//     events — which makes journaling, replay, testing, and hot-standby
//     failover (ship the journal, replay it) trivial;
//   * fairness is the *sequence*, decided once at the front door.
//
// Outbound events go through a compile-time Sink template parameter: the
// event handlers inline into the matching loop, no virtual dispatch, and the
// core does zero serialization or I/O.

#include <array>
#include <cstdint>

#include "me/core/book.hpp"
#include "me/core/order_pool.hpp"
#include "me/core/types.hpp"
#include "me/proto/ouch.hpp"

namespace me {

// NOTE: this object embeds the full price ladders (~1.6 MB with 4 books).
// Always allocate it on the heap (make_unique) — it will overflow a default
// 1 MB Windows thread stack.
template <class Sink>
class MatchingEngine {
 public:
  static constexpr unsigned kMaxSymbols = 4;

  struct Params {
    unsigned nsym = 1;
    Price mid0 = 10'000;
    std::uint32_t pool_capacity = 1u << 20;
    Qty max_qty = 1'000'000;
    bool self_match_prevention = true;  // cancel-resting STP policy
  };

  MatchingEngine(Sink& sink, Params p) : sink_(sink), pool_(p.pool_capacity), p_(p) {
    for (auto& b : books_) b.set_anchor(p.mid0);
  }

  void process(const InMsg& m) noexcept {
    switch (m.type) {
      case kMsgEnter:   enter(m.enter); break;
      case kMsgCancel:  cancel(m.cancel); break;
      case kMsgReplace: replace(m.replace); break;
      default: break;  // unknown type: ignored (a session layer would NAK)
    }
  }

  // Touch introspection (single-threaded: safe for the harness/generator).
  [[nodiscard]] bool has_bid(unsigned sym) const noexcept { return books_[sym].has_bid(); }
  [[nodiscard]] bool has_ask(unsigned sym) const noexcept { return books_[sym].has_ask(); }
  [[nodiscard]] Price bid_px(unsigned sym) const noexcept { return books_[sym].bid_px(); }
  [[nodiscard]] Price ask_px(unsigned sym) const noexcept { return books_[sym].ask_px(); }

  // Counters.
  [[nodiscard]] std::uint64_t accepts() const noexcept { return accepts_; }
  [[nodiscard]] std::uint64_t trades() const noexcept { return trades_; }
  [[nodiscard]] std::uint64_t traded_qty() const noexcept { return traded_qty_; }
  [[nodiscard]] std::uint64_t user_cancels() const noexcept { return user_cancels_; }
  [[nodiscard]] std::uint64_t ioc_kills() const noexcept { return ioc_kills_; }
  [[nodiscard]] std::uint64_t fok_kills() const noexcept { return fok_kills_; }
  [[nodiscard]] std::uint64_t stp_cancels() const noexcept { return stp_cancels_; }
  [[nodiscard]] std::uint64_t replaces() const noexcept { return replaces_; }
  [[nodiscard]] std::uint64_t rejects() const noexcept { return rejects_; }
  [[nodiscard]] const OrderPool& pool() const noexcept { return pool_; }

 private:
  void enter(const EnterOrder& in) noexcept {
    if (in.qty == 0 || in.qty > p_.max_qty) [[unlikely]] {
      reject(in.participant, in.token, RejectReason::BadQty);
      return;
    }
    if (in.symbol >= p_.nsym) [[unlikely]] {
      reject(in.participant, in.token, RejectReason::BadSymbol);
      return;
    }
    Book& b = books_[in.symbol];
    if (!b.in_band(in.price)) [[unlikely]] {
      reject(in.participant, in.token, RejectReason::BadPrice);
      return;
    }
    const std::uint32_t s = pool_.alloc();
    if (s == kNil) [[unlikely]] {
      reject(in.participant, in.token, RejectReason::PoolFull);
      return;
    }

    Order& o = pool_[s];
    o.token = in.token;
    o.price = in.price;
    o.leaves = in.qty;
    o.participant = in.participant;
    o.symbol = in.symbol;
    o.side = in.side;
    o.tif = in.tif;

    // Real-exchange flow: accept (and assign the id) BEFORE matching, so
    // executions can reference the aggressor's id.
    const OrderId oid = pool_.make_id(s);
    ++accepts_;
    sink_.on_accepted(in.participant, in.token, oid, in.side, in.price, in.qty);

    if (in.tif == kTifFok) {
      // All-or-nothing: pre-scan crossable liquidity, kill without touching
      // the book if insufficient. (Counts all liquidity, including own —
      // STP interaction is simplified; see README.)
      if (b.crossable_qty(in.side, in.price, in.qty) < in.qty) {
        ++fok_kills_;
        sink_.on_canceled(in.participant, in.token, oid, in.qty, CancelReason::Fok);
        pool_.free(s);
        return;
      }
    }
    match_and_rest(s, oid);
  }

  // Shared by enter and replace: match aggressively, then rest / kill the
  // remainder according to TIF.
  void match_and_rest(std::uint32_t s, OrderId oid) noexcept {
    Order& o = pool_[s];
    Book& b = books_[o.symbol];
    const std::uint8_t side = o.side;

    while (o.leaves != 0) {
      if (side == kBuy) {
        if (!b.has_ask() || o.price < b.ask_px()) break;
      } else {
        if (!b.has_bid() || o.price > b.bid_px()) break;
      }
      const std::uint32_t rs = (side == kBuy) ? b.ask_head() : b.bid_head();
      Order& r = pool_[rs];
      const OrderId rid = pool_.make_id(rs);

      if (p_.self_match_prevention && r.participant == o.participant) [[unlikely]] {
        // Cancel-resting policy: the older own order is pulled, the
        // aggressor keeps matching. (Cancel-newest is the other common one.)
        ++stp_cancels_;
        b.remove(pool_, rs);
        sink_.on_canceled(r.participant, r.token, rid, r.leaves, CancelReason::SelfMatch);
        sink_.on_md_delete(rid);
        pool_.free(rs);
        continue;
      }

      const Qty tq = o.leaves < r.leaves ? o.leaves : r.leaves;
      const Price px = r.price;  // trades print at the RESTING price
      o.leaves -= tq;
      ++trades_;
      traded_qty_ += tq;
      sink_.on_exec(o.participant, o.token, oid, tq, px);
      sink_.on_exec(r.participant, r.token, rid, tq, px);
      sink_.on_md_exec(rid, tq, px);

      if (tq == r.leaves) {  // resting order exhausted (r.leaves still == tq here:
        b.remove(pool_, rs); //  remove() uses it to adjust the level aggregate)
        pool_.free(rs);
      } else {
        b.reduce(pool_, rs, tq);
      }
    }

    if (o.leaves != 0) {
      if (o.tif == kTifIoc || o.tif == kTifFok) {
        // FOK remainder can only happen if STP consumed pre-counted
        // liquidity mid-match; both TIFs kill the remainder.
        ++ioc_kills_;
        sink_.on_canceled(o.participant, o.token, oid, o.leaves, CancelReason::Ioc);
        pool_.free(s);
      } else {
        b.insert(pool_, s);
        sink_.on_md_add(oid, side, o.price, o.leaves);
      }
    } else {
      pool_.free(s);
    }
  }

  void cancel(const CancelOrder& in) noexcept {
    const std::uint32_t s = pool_.resolve(in.order_id);
    if (s == kNil) [[unlikely]] {  // stale handle: already filled/canceled
      reject(in.participant, in.token, RejectReason::UnknownOrder);
      return;
    }
    Order& o = pool_[s];
    books_[o.symbol].remove(pool_, s);
    ++user_cancels_;
    sink_.on_canceled(o.participant, o.token, in.order_id, o.leaves, CancelReason::User);
    sink_.on_md_delete(in.order_id);
    pool_.free(s);
  }

  void replace(const ReplaceOrder& in) noexcept {
    const std::uint32_t s = pool_.resolve(in.order_id);
    if (s == kNil) [[unlikely]] {
      reject(in.participant, 0, RejectReason::UnknownOrder);
      return;
    }
    Order& o = pool_[s];
    if (in.new_qty == 0 || in.new_qty > p_.max_qty) [[unlikely]] {
      reject(in.participant, o.token, RejectReason::BadQty);
      return;  // replace rejected: the original order stands untouched
    }
    Book& b = books_[o.symbol];
    if (!b.in_band(in.new_price)) [[unlikely]] {
      reject(in.participant, o.token, RejectReason::BadPrice);
      return;
    }
    ++replaces_;

    if (in.new_price == o.price && in.new_qty <= o.leaves) {
      // Qty decrease at the same price: time priority is PRESERVED
      // (the real exchange rule), so reduce in place.
      const Qty dec = o.leaves - in.new_qty;
      if (dec != 0) b.reduce(pool_, s, dec);
      sink_.on_replaced(o.participant, o.token, in.order_id, in.order_id, o.price, o.leaves);
      if (dec != 0) sink_.on_md_reduce(in.order_id, dec);
      return;
    }

    // Price change or qty increase: priority is LOST. Leave the book, take a
    // fresh id (the old handle becomes stale), and re-run the matching flow —
    // the new price may cross.
    b.remove(pool_, s);
    sink_.on_md_delete(in.order_id);
    pool_.bump_gen(s);
    o.price = in.new_price;
    o.leaves = in.new_qty;
    o.tif = kTifDay;
    const OrderId nid = pool_.make_id(s);
    sink_.on_replaced(o.participant, o.token, in.order_id, nid, o.price, o.leaves);
    match_and_rest(s, nid);
  }

  void reject(ParticipantId part, Token token, RejectReason r) noexcept {
    ++rejects_;
    sink_.on_rejected(part, token, r);
  }

  Sink& sink_;
  OrderPool pool_;
  Params p_;
  std::array<Book, kMaxSymbols> books_;
  std::uint64_t accepts_ = 0, trades_ = 0, traded_qty_ = 0;
  std::uint64_t user_cancels_ = 0, ioc_kills_ = 0, fok_kills_ = 0, stp_cancels_ = 0;
  std::uint64_t replaces_ = 0, rejects_ = 0;
};

}  // namespace me
