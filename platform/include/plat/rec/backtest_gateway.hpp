#pragma once
// plat/rec/backtest_gateway.hpp — the fill simulator that stands where the
// CTP counter stands in production. It satisfies the OrderGateway concept, so
// the backtest drives the SAME Site<...> and the SAME strategies as platd;
// only the venue is modelled instead of connected.
//
// Fill model (v1, deliberately simple and documented): the strategies send
// IOC orders that cross the spread, so a marketable order fills against the
// current top of book — min(order qty, touch qty) at the touch price — and
// any IOC remainder is cancelled. This is optimistic (assumes you take the
// touch with no queue/slippage); a more conservative model would add a
// slippage tick or a fill-probability. Fills are QUEUED here and drained by
// the driver after each snapshot dispatch, mirroring the live path
// (gateway → report ring → driver → site.source), so re-entrancy and
// ordering match production exactly.

#include <cstdint>
#include <vector>

#include "plat/codec/msgs.hpp"
#include "plat/core/types.hpp"

namespace plat::rec {

class BacktestGateway {
 public:
  explicit BacktestGateway(std::size_t instruments)
      : book_(instruments) {}

  // The driver refreshes the top of book before feeding each snapshot, so
  // place() models the fill against the market the strategy just saw.
  void set_book(InstrumentId inst, Price bid, Qty bid_qty, Price ask, Qty ask_qty) {
    if (inst < book_.size()) book_[inst] = {bid, bid_qty, ask, ask_qty};
  }

  // OrderGateway concept -------------------------------------------------
  void place(const NewOrderReq& r) {
    ExecReport ack = base(r);
    ack.type = ExecType::Ack;
    ack.venue_no = ++venue_seq_;
    ack.leaves = r.qty;
    pending_.push_back(ack);

    const Book b = r.inst < book_.size() ? book_[r.inst] : Book{};
    Qty fill_qty = 0;
    Price fill_px = r.px;
    if (r.side == Side::Buy && b.ask > 0 && r.px >= b.ask) {
      fill_qty = r.qty < b.ask_qty ? r.qty : b.ask_qty;
      fill_px = b.ask;
    } else if (r.side == Side::Sell && b.bid > 0 && r.px <= b.bid) {
      fill_qty = r.qty < b.bid_qty ? r.qty : b.bid_qty;
      fill_px = b.bid;
    }

    if (fill_qty > 0) {
      ExecReport fill = base(r);
      fill.type = ExecType::Fill;
      fill.venue_no = ack.venue_no;
      fill.px = fill_px;
      fill.qty = fill_qty;
      fill.cum = fill_qty;
      fill.leaves = r.qty - fill_qty;
      pending_.push_back(fill);
      ++fills_;
    }
    if (fill_qty < r.qty) {  // IOC: cancel the unfilled remainder
      ExecReport cxl = base(r);
      cxl.type = ExecType::Canceled;
      cxl.venue_no = ack.venue_no;
      cxl.cum = fill_qty;
      cxl.leaves = 0;
      pending_.push_back(cxl);
    }
  }

  void cancel(const CancelReq& c) {
    ExecReport cxl{};
    cxl.cl_id = c.cl_id;
    cxl.ts = c.ts;
    cxl.inst = c.inst;
    cxl.strategy = c.strategy;
    cxl.account = c.account;
    cxl.type = ExecType::Canceled;
    pending_.push_back(cxl);
  }

  [[nodiscard]] SessState state() const { return SessState::Up; }

  // Driver drains these into site.source() after each dispatch.
  [[nodiscard]] std::vector<ExecReport>& drain() { return pending_; }
  void clear() { pending_.clear(); }
  [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }

 private:
  struct Book {
    Price bid = 0;
    Qty bid_qty = 0;
    Price ask = 0;
    Qty ask_qty = 0;
  };
  static ExecReport base(const NewOrderReq& r) {
    ExecReport e{};
    e.cl_id = r.cl_id;
    e.ts = r.ts;
    e.trigger_ts = r.trigger_ts;
    e.px = r.px;
    e.inst = r.inst;
    e.strategy = r.strategy;
    e.account = r.account;
    e.side = r.side;
    e.offset = r.offset;
    return e;
  }

  std::vector<Book> book_;
  std::vector<ExecReport> pending_;
  std::uint64_t venue_seq_ = 0;
  std::uint64_t fills_ = 0;
};

}  // namespace plat::rec
