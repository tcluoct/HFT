#pragma once
// Feed handler: pops raw market-data messages off the "wire" ring, applies
// them to per-symbol books, and publishes normalized top-of-book updates.
// Busy-spins on a pinned core; PAUSE on empty polls. Records decode+book
// latency (t1 - t0) for every message.

#include <array>
#include <cstdint>

#include "hft/common/async_logger.hpp"
#include "hft/common/histogram.hpp"
#include "hft/common/run_control.hpp"
#include "hft/common/tsc_clock.hpp"
#include "t2t/config.hpp"
#include "t2t/md/order_book.hpp"

namespace t2t {

class FeedHandler {
 public:
  FeedHandler(MdRing& in, BookRing& out, hft::LogQueue& log, Price mid0, unsigned nsym)
      : in_(in), out_(out), log_(log), nsym_(nsym) {
    for (auto& b : books_) b.set_anchor(mid0);
  }

  void run(hft::RunControl& rc) noexcept {
    bool measuring = false;
    std::uint64_t iter = 0;
    MdMsg m;
    for (;;) {
      if ((++iter & 0xFFFu) == 0) [[unlikely]] {
        const int ph = rc.phase.load(std::memory_order_relaxed);
        if (ph == hft::kStop) break;
        if (ph == hft::kMeasure && !measuring) {
          hist_.reset();
          msgs_ = updates_ = dropped_updates_ = 0;
          measuring = true;
        }
      }
      if (!in_.try_pop(m)) {
        hft::cpu_relax();
        continue;
      }
      if (m.symbol >= nsym_) [[unlikely]] continue;

      OrderBook& book = books_[m.symbol];
      const bool tob_changed = book.apply(m);
      const std::uint64_t t1 = hft::rdtscp();
      hist_.record(t1 - m.ts_origin);
      ++msgs_;

      if (tob_changed) {
        const OrderBook::Tob tob = book.tob();
        if (tob.valid) [[likely]] {
          const BookUpdate u{m.ts_origin, t1,          tob.bid_px, tob.ask_px,
                             tob.bid_qty, tob.ask_qty, m.symbol};
          // Full downstream ring => drop and count. The real-world answer is
          // conflation (keep only the latest state per symbol).
          if (out_.try_push(u)) [[likely]]
            ++updates_;
          else
            ++dropped_updates_;
        }
      }
    }
  }

  [[nodiscard]] const hft::LatencyHistogram& hist() const noexcept { return hist_; }
  [[nodiscard]] std::uint64_t msgs() const noexcept { return msgs_; }
  [[nodiscard]] std::uint64_t updates() const noexcept { return updates_; }
  [[nodiscard]] std::uint64_t dropped_updates() const noexcept { return dropped_updates_; }

 private:
  MdRing& in_;
  BookRing& out_;
  hft::LogQueue& log_;
  unsigned nsym_;
  std::array<OrderBook, kMaxSymbols> books_;
  hft::LatencyHistogram hist_;
  std::uint64_t msgs_ = 0, updates_ = 0, dropped_updates_ = 0;
};

}  // namespace t2t
