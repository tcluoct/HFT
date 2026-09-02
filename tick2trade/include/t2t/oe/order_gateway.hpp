#pragma once
// Order gateway: the last hop before the exchange. Runs mandatory pre-trade
// risk checks (a regulatory requirement -- SEC 15c3-5 in the US) as a handful
// of integer compares, stamps t3, and forwards to the exchange session.
// Checks are ordered so the common (pass) path is straight-line code.

#include <cstdint>

#include "hft/common/async_logger.hpp"
#include "hft/common/histogram.hpp"
#include "hft/common/run_control.hpp"
#include "hft/common/tsc_clock.hpp"
#include "t2t/config.hpp"

namespace t2t {

class OrderGateway {
 public:
  struct Risk {
    Qty max_qty = 1000;                     // max shares per order
    Price px_min = 0, px_max = 0;           // fat-finger price collar
    std::uint64_t max_total = 10'000'000;   // session kill switch
    std::uint64_t min_gap_ns = 5'000;       // rate throttle between orders
  };

  OrderGateway(OrderRing& in, OrderRing& out, hft::LogQueue& log, Risk r)
      : in_(in),
        out_(out),
        log_(log),
        risk_(r),
        min_gap_cycles_(static_cast<std::uint64_t>(static_cast<double>(r.min_gap_ns) *
                                                   hft::TscClock::cycles_per_ns())) {}

  void run(hft::RunControl& rc) noexcept {
    bool measuring = false;
    std::uint64_t iter = 0;
    NewOrder o;
    for (;;) {
      if ((++iter & 0xFFFu) == 0) [[unlikely]] {
        const int ph = rc.phase.load(std::memory_order_relaxed);
        if (ph == hft::kStop) break;
        if (ph == hft::kMeasure && !measuring) {
          hist_.reset();
          sent_ = rejects_ = throttled_ = drops_ = 0;
          measuring = true;
        }
      }
      if (!in_.try_pop(o)) {
        hft::cpu_relax();
        continue;
      }

      if (o.qty == 0 || o.qty > risk_.max_qty || o.price < risk_.px_min ||
          o.price > risk_.px_max) [[unlikely]] {
        ++rejects_;
        log_.log("REJECT id=%llu px=%lld qty=%llu", o.client_id,
                 static_cast<std::uint64_t>(o.price), o.qty);
        continue;
      }
      if (total_sent_ >= risk_.max_total) [[unlikely]] {  // kill switch tripped
        ++rejects_;
        continue;
      }

      o.t3 = hft::rdtscp();
      if (o.t3 - last_send_ < min_gap_cycles_) [[unlikely]] {
        ++throttled_;
        continue;
      }
      hist_.record(o.t3 - o.t2);
      if (!out_.try_push(o)) [[unlikely]] {
        ++drops_;
        continue;
      }
      last_send_ = o.t3;
      ++sent_;
      ++total_sent_;
    }
  }

  [[nodiscard]] const hft::LatencyHistogram& hist() const noexcept { return hist_; }
  [[nodiscard]] std::uint64_t sent() const noexcept { return sent_; }
  [[nodiscard]] std::uint64_t rejects() const noexcept { return rejects_; }
  [[nodiscard]] std::uint64_t throttled() const noexcept { return throttled_; }
  [[nodiscard]] std::uint64_t drops() const noexcept { return drops_; }

 private:
  OrderRing& in_;
  OrderRing& out_;
  hft::LogQueue& log_;
  Risk risk_;
  std::uint64_t min_gap_cycles_;
  std::uint64_t last_send_ = 0;
  hft::LatencyHistogram hist_;
  std::uint64_t sent_ = 0, rejects_ = 0, throttled_ = 0, drops_ = 0;
  std::uint64_t total_sent_ = 0;  // survives measurement reset: kill switch is per-session
};

}  // namespace t2t
