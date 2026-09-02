#pragma once
// Strategy layer.
//
// StrategyBase is a CRTP template: the event loop calls the concrete
// strategy through static dispatch, so there are no virtual calls, no
// indirect-branch mispredictions, and the strategy body inlines straight
// into the polling loop.
//
// ImbalanceTaker: when the touch is tight and one side of the book is much
// heavier than the other, take liquidity on the thin side (classic
// order-flow-imbalance signal, IOC only, position- and rate-limited).

#include <cstdint>

#include "hft/common/async_logger.hpp"
#include "hft/common/histogram.hpp"
#include "hft/common/run_control.hpp"
#include "hft/common/tsc_clock.hpp"
#include "t2t/config.hpp"

namespace t2t {

template <typename Derived>
class StrategyBase {
 public:
  StrategyBase(BookRing& book_in, ExecRing& exec_in, OrderRing& order_out, hft::LogQueue& log)
      : book_in_(book_in), exec_in_(exec_in), order_out_(order_out), log_(log) {}

  void run(hft::RunControl& rc) noexcept {
    bool measuring = false;
    std::uint64_t iter = 0;
    BookUpdate u;
    ExecReport er;
    for (;;) {
      if ((++iter & 0xFFFu) == 0) [[unlikely]] {
        const int ph = rc.phase.load(std::memory_order_relaxed);
        if (ph == hft::kStop) break;
        if (ph == hft::kMeasure && !measuring) {
          hist_.reset();
          events_ = sent_ = order_drops_ = 0;
          self().on_measure_start();
          measuring = true;
        }
      }
      while (exec_in_.try_pop(er)) self().on_exec(er);
      if (!book_in_.try_pop(u)) {
        hft::cpu_relax();
        continue;
      }
      self().on_book(u);
      const std::uint64_t t2 = hft::rdtscp();
      hist_.record(t2 - u.t1);  // decision latency incl. queue wait
      ++events_;
    }
  }

  [[nodiscard]] const hft::LatencyHistogram& hist() const noexcept { return hist_; }
  [[nodiscard]] std::uint64_t events() const noexcept { return events_; }
  [[nodiscard]] std::uint64_t sent() const noexcept { return sent_; }
  [[nodiscard]] std::uint64_t order_drops() const noexcept { return order_drops_; }

 protected:
  HFT_FORCE_INLINE Derived& self() noexcept { return *static_cast<Derived*>(this); }

  bool send(const BookUpdate& u, std::uint8_t side, Price px, Qty qty) noexcept {
    NewOrder o{};
    o.client_id = ++next_id_;
    o.t0 = u.t0;
    o.t1 = u.t1;
    o.t2 = hft::rdtscp();
    o.price = px;
    o.ref_px = (side == kBuy) ? u.ask_px : u.bid_px;
    o.qty = qty;
    o.symbol = u.symbol;
    o.side = side;
    if (!order_out_.try_push(o)) [[unlikely]] {
      ++order_drops_;
      return false;
    }
    ++sent_;
    log_.log("order id=%llu side=%llu px=%lld", o.client_id, side,
             static_cast<std::uint64_t>(px));
    return true;
  }

  BookRing& book_in_;
  ExecRing& exec_in_;
  OrderRing& order_out_;
  hft::LogQueue& log_;
  hft::LatencyHistogram hist_;
  std::uint64_t events_ = 0, sent_ = 0, order_drops_ = 0;
  OrderId next_id_ = 0;
};

class ImbalanceTaker final : public StrategyBase<ImbalanceTaker> {
 public:
  struct Params {
    std::uint64_t imb_num = 3, imb_den = 2;  // fire when heavy side >= 1.5x thin side
    Price max_spread = 4;                    // ticks
    std::int64_t pos_limit = 25'000;         // shares, absolute
    Qty clip = 100;                          // shares per order
    std::uint64_t cooldown_ns = 20'000;      // floor between orders
  };

  ImbalanceTaker(BookRing& b, ExecRing& e, OrderRing& o, hft::LogQueue& lg, Params p)
      : StrategyBase(b, e, o, lg),
        p_(p),
        cooldown_cycles_(static_cast<std::uint64_t>(static_cast<double>(p.cooldown_ns) *
                                                    hft::TscClock::cycles_per_ns())) {}

  HFT_FORCE_INLINE void on_book(const BookUpdate& u) noexcept {
    const Price spread = u.ask_px - u.bid_px;
    if (spread <= 0 || spread > p_.max_spread) return;  // locked/crossed/wide: stand down
    const std::uint64_t now = hft::rdtsc();
    if (now - last_send_ < cooldown_cycles_) return;

    if (u.bid_qty * p_.imb_den >= u.ask_qty * p_.imb_num) {          // heavy bid: lift ask
      if (pos_ + static_cast<std::int64_t>(p_.clip) <= p_.pos_limit &&
          send(u, kBuy, u.ask_px, p_.clip))
        last_send_ = now;
    } else if (u.ask_qty * p_.imb_den >= u.bid_qty * p_.imb_num) {   // heavy ask: hit bid
      if (pos_ - static_cast<std::int64_t>(p_.clip) >= -p_.pos_limit &&
          send(u, kSell, u.bid_px, p_.clip))
        last_send_ = now;
    }
  }

  HFT_FORCE_INLINE void on_exec(const ExecReport& r) noexcept {
    if (r.status == ExecStatus::Filled) {
      pos_ += (r.side == kBuy) ? static_cast<std::int64_t>(r.fill_qty)
                               : -static_cast<std::int64_t>(r.fill_qty);
      ++fills_;
    } else {
      ++kills_;
    }
  }

  void on_measure_start() noexcept { fills_ = kills_ = 0; }

  [[nodiscard]] std::int64_t position() const noexcept { return pos_; }
  [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }
  [[nodiscard]] std::uint64_t kills() const noexcept { return kills_; }

 private:
  Params p_;
  std::uint64_t cooldown_cycles_;
  std::uint64_t last_send_ = 0;
  std::int64_t pos_ = 0;
  std::uint64_t fills_ = 0, kills_ = 0;
};

}  // namespace t2t
