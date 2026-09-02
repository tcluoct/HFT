#pragma once
// Exchange simulator: plays the role of the venue.
//
//  * generates an order-by-order market data stream (adds / deletes /
//    executes around a slowly random-walking mid), paced by TSC to a target
//    message rate
//  * consumes IOC orders from the gateway, fills them against its own mirror
//    book's touch, and emits execution reports
//  * stamps t0 on market data at "the wire" and t4 on order arrival --
//    (t4 - t0) is the wire-to-wire tick-to-trade latency this project exists
//    to measure
//
// Hot-path RNG is splitmix64 (a few arithmetic ops); bounded picks use
// Lemire's multiply-shift reduction instead of the modulo operator.

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "hft/common/async_logger.hpp"
#include "hft/common/histogram.hpp"
#include "hft/common/run_control.hpp"
#include "hft/common/tsc_clock.hpp"
#include "t2t/config.hpp"
#include "t2t/md/order_book.hpp"

namespace t2t {

class ExchangeSim {
 public:
  struct Params {
    std::uint64_t rate = 1'000'000;  // md msgs/sec; 0 = as fast as possible
    Price mid0 = 10'000;
    std::uint64_t seed = 42;
    unsigned nsym = 2;
  };

  // Keep the resting book thin: a sparse touch flickers between imbalance
  // states, which is what gives the strategy realistic firing opportunities.
  static constexpr std::size_t kMaxLive = 512;  // live orders per symbol

  ExchangeSim(MdRing& md_out, OrderRing& ord_in, ExecRing& exec_out, hft::LogQueue& log, Params p)
      : md_out_(md_out), ord_in_(ord_in), exec_out_(exec_out), log_(log), p_(p),
        rng_s_(p.seed | 1) {
    for (unsigned i = 0; i < kMaxSymbols; ++i) {
      sym_[i].mid = p.mid0;
      sym_[i].live.reserve(kMaxLive);  // pre-size: no allocation on the hot path
      books_[i].set_anchor(p.mid0);
    }
  }

  void run(hft::RunControl& rc) noexcept {
    const std::uint64_t cycles_per_msg =
        p_.rate ? static_cast<std::uint64_t>(hft::TscClock::cycles_per_ns() * 1e9 /
                                             static_cast<double>(p_.rate))
                : 0;
    std::uint64_t next_emit = hft::rdtsc();
    bool measuring = false;
    std::uint64_t iter = 0;

    for (;;) {
      if ((++iter & 0xFFFu) == 0) [[unlikely]] {
        const int ph = rc.phase.load(std::memory_order_relaxed);
        if (ph == hft::kStop) break;
        if (ph == hft::kMeasure && !measuring) {
          t2t_hist_.reset();
          md_sent_ = md_drops_ = orders_ = fills_ = kills_ = exec_drops_ = 0;
          measuring = true;
        }
      }

      // Order entry first: the latency-critical direction has priority.
      NewOrder o;
      while (ord_in_.try_pop(o)) {
        const std::uint64_t t4 = hft::rdtscp();
        t2t_hist_.record(t4 - o.t0);  // wire-to-wire tick-to-trade
        ++orders_;

        ExecReport rep{o.client_id, 0, 0, o.symbol, o.side, ExecStatus::Killed};
        const OrderBook::Tob tob = books_[o.symbol].tob();
        if (tob.valid && o.side == kBuy && o.price >= tob.ask_px) {
          rep.status = ExecStatus::Filled;
          rep.fill_qty = o.qty;
          rep.fill_px = tob.ask_px;
          ++fills_;
        } else if (tob.valid && o.side == kSell && o.price <= tob.bid_px) {
          rep.status = ExecStatus::Filled;
          rep.fill_qty = o.qty;
          rep.fill_px = tob.bid_px;
          ++fills_;
        } else {
          ++kills_;
        }
        if (!exec_out_.try_push(rep)) [[unlikely]] ++exec_drops_;
      }

      // Market data generation, TSC-paced to the target rate.
      if (cycles_per_msg == 0) {
        emit_one();
        continue;
      }
      const std::uint64_t now = hft::rdtsc();
      if (now >= next_emit) {
        emit_one();
        next_emit += cycles_per_msg;
        if (now > next_emit + (cycles_per_msg << 10)) next_emit = now;  // resync after stall
      } else {
        hft::cpu_relax();
      }
    }
  }

  [[nodiscard]] const hft::LatencyHistogram& tick_to_trade_hist() const noexcept { return t2t_hist_; }
  [[nodiscard]] std::uint64_t md_sent() const noexcept { return md_sent_; }
  [[nodiscard]] std::uint64_t md_drops() const noexcept { return md_drops_; }
  [[nodiscard]] std::uint64_t orders_seen() const noexcept { return orders_; }
  [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }
  [[nodiscard]] std::uint64_t kills() const noexcept { return kills_; }
  [[nodiscard]] std::uint64_t exec_drops() const noexcept { return exec_drops_; }

 private:
  struct SimOrder {
    OrderId id;
    Price px;
    Qty qty;
    std::uint8_t side;
  };
  struct SymState {
    Price mid = 0;
    std::vector<SimOrder> live;
  };

  HFT_FORCE_INLINE std::uint64_t rng() noexcept {  // splitmix64
    std::uint64_t z = (rng_s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  // Lemire multiply-shift: maps a 32-bit random x into [0, n) without modulo.
  static HFT_FORCE_INLINE std::uint32_t bounded(std::uint64_t x32, std::uint32_t n) noexcept {
    return static_cast<std::uint32_t>(((x32 & 0xFFFFFFFFull) * n) >> 32);
  }

  void emit_one() noexcept {
    const std::uint64_t r = rng();
    const SymbolId sym = static_cast<SymbolId>(bounded(r, p_.nsym));
    const std::uint32_t roll = bounded(r >> 32, 100);
    SymState& st = sym_[sym];
    const bool full = st.live.size() >= kMaxLive;

    MdMsg m{};
    m.symbol = sym;

    if (roll < 55 && !full) {  // add liquidity
      const std::uint8_t side = static_cast<std::uint8_t>((r >> 48) & 1);
      const Price off = static_cast<Price>((r >> 50) & 7);
      const Price px = (side == kBuy) ? st.mid - 1 - off : st.mid + 1 + off;
      const Qty qty = 100u * (1u + static_cast<Qty>((r >> 53) & 7));
      m.type = kMsgAdd;
      m.side = side;
      m.qty = qty;
      m.order_id = ++next_id_;
      m.price = px;
      st.live.push_back(SimOrder{m.order_id, px, qty, side});
    } else if (!st.live.empty() && (roll < 80 || full)) {  // delete an order
      const std::uint64_t r2 = rng();
      const std::size_t k = bounded(r2, static_cast<std::uint32_t>(st.live.size()));
      SimOrder& so = st.live[k];
      m.type = kMsgDelete;
      m.side = so.side;
      m.qty = so.qty;
      m.order_id = so.id;
      m.price = so.px;
      so = st.live.back();  // swap-pop: O(1) removal
      st.live.pop_back();
    } else if (!st.live.empty()) {  // partial/total execution
      const std::uint64_t r2 = rng();
      const std::size_t k = bounded(r2, static_cast<std::uint32_t>(st.live.size()));
      SimOrder& so = st.live[k];
      const Qty ex = so.qty < 100u ? so.qty : 100u;
      m.type = kMsgExecute;
      m.side = so.side;
      m.qty = ex;
      m.order_id = so.id;
      m.price = so.px;
      so.qty -= ex;
      if (so.qty == 0) {
        so = st.live.back();
        st.live.pop_back();
      }
    } else {
      return;  // nothing sensible to emit this slot
    }

    books_[sym].apply(m);  // keep the fill-matching mirror in sync (not timed)

    if ((++emitted_ & 0xFFu) == 0) {  // slow random walk of the mid
      st.mid += ((r >> 60) & 1) ? 1 : -1;
      st.mid = std::clamp(st.mid, p_.mid0 - 1500, p_.mid0 + 1500);
    }

    m.ts_origin = hft::rdtscp();  // t0: stamped at "the wire", last thing before send
    if (md_out_.try_push(m)) [[likely]]
      ++md_sent_;
    else
      ++md_drops_;
  }

  MdRing& md_out_;
  OrderRing& ord_in_;
  ExecRing& exec_out_;
  hft::LogQueue& log_;
  Params p_;
  std::uint64_t rng_s_;
  OrderId next_id_ = 0;
  std::uint64_t emitted_ = 0;
  std::array<SymState, kMaxSymbols> sym_{};
  std::array<OrderBook, kMaxSymbols> books_;
  hft::LatencyHistogram t2t_hist_;
  std::uint64_t md_sent_ = 0, md_drops_ = 0;
  std::uint64_t orders_ = 0, fills_ = 0, kills_ = 0, exec_drops_ = 0;
};

}  // namespace t2t
