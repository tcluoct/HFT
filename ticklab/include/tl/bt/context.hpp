#pragma once
// Backtest execution context: per-symbol BBO state, a latency-modeled order
// queue, a fill simulator, and PnL accounting.
//
// THE rule that makes a backtest honest — no look-ahead:
//
//   an order sent at decision time T takes effect at T + latency, and it
//   fills against the book state prevailing JUST BEFORE the first tick with
//   ts >= T + latency — never against the book the strategy was looking at
//   when it decided.
//
// Mechanically: pending orders are processed at the top of on_tick(), BEFORE
// the new tick updates the book. Fill quantity is capped at the displayed
// size by default (conservative). Filling at decision-time prices — the
// classic backtesting bug — reliably manufactures alpha that does not exist.

#include <cmath>
#include <cstdint>
#include <vector>

#include "hft/common/platform.hpp"
#include "tl/data/tick.hpp"

namespace tl {

class BtContext {
 public:
  struct Params {
    std::uint64_t latency_ns = 20'000;  // decision -> exchange effect
    bool conservative_size = true;      // cap fills at displayed size
    double fee_per_share = 0.0;         // in price-tick units per share
    unsigned nsym = 1;
  };

  struct Bbo {
    Price bid_px = 0, ask_px = 0;
    Qty bid_qty = 0, ask_qty = 0;
    bool valid = false;
  };

  explicit BtContext(Params p) : p_(p) {
    bbo_.resize(p.nsym);
    pos_.resize(p.nsym, 0);
    pending_.reserve(1 << 12);
    curve_.reserve(1 << 12);
  }

  // --- engine side -----------------------------------------------------
  HFT_FORCE_INLINE void on_tick(const Tick& t) noexcept {
    process_pending(t.ts_ns);  // BEFORE the book updates: no look-ahead
    if (t.kind == kQuote) {
      Bbo& b = bbo_[t.symbol];
      b.bid_px = t.px1;
      b.ask_px = t.px2;
      b.bid_qty = t.q1;
      b.ask_qty = t.q2;
      b.valid = true;
    }
    now_ = t.ts_ns;
    if ((++applied_ & 0xFFFFu) == 0) curve_.push_back(equity());
  }

  void finish() noexcept {
    process_pending(~std::uint64_t{0});
    curve_.push_back(equity());
  }

  // --- strategy side -----------------------------------------------------
  void send_ioc(SymbolId sym, std::uint8_t side, Price px, Qty qty) noexcept {
    pending_.push_back(Pending{now_ + p_.latency_ns, px, qty, sym, side});
    ++sent_;
  }
  [[nodiscard]] const Bbo& bbo(SymbolId sym) const noexcept { return bbo_[sym]; }
  [[nodiscard]] std::int64_t position(SymbolId sym) const noexcept { return pos_[sym]; }
  [[nodiscard]] std::uint64_t now() const noexcept { return now_; }

  // --- results (tick*share units; with a $0.01 tick, 100 = $1 per share·100) --
  [[nodiscard]] double equity() const noexcept {
    double e = cash_;
    for (unsigned i = 0; i < p_.nsym; ++i) {
      const Bbo& b = bbo_[i];
      if (b.valid && pos_[i] != 0)
        e += static_cast<double>(pos_[i]) *
             (static_cast<double>(b.bid_px + b.ask_px) / 2.0);
    }
    return e;
  }

  [[nodiscard]] std::uint64_t sent() const noexcept { return sent_; }
  [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }
  [[nodiscard]] std::uint64_t kills() const noexcept { return kills_; }
  [[nodiscard]] std::uint64_t volume() const noexcept { return volume_; }
  [[nodiscard]] double fees() const noexcept { return fees_; }
  [[nodiscard]] std::uint64_t digest() const noexcept { return dig_; }
  [[nodiscard]] const std::vector<double>& equity_curve() const noexcept { return curve_; }

  [[nodiscard]] double max_drawdown() const noexcept {
    double peak = -1e300, dd = 0;
    for (const double e : curve_) {
      if (e > peak) peak = e;
      if (peak - e > dd) dd = peak - e;
    }
    return dd;
  }

  // Naive per-sample Sharpe (mean/std of equity increments, scaled by
  // sqrt(#samples)) — a shape indicator, not an annualized figure.
  [[nodiscard]] double sharpe() const noexcept {
    if (curve_.size() < 3) return 0;
    double mean = 0;
    const std::size_t n = curve_.size() - 1;
    for (std::size_t i = 1; i < curve_.size(); ++i) mean += curve_[i] - curve_[i - 1];
    mean /= static_cast<double>(n);
    double var = 0;
    for (std::size_t i = 1; i < curve_.size(); ++i) {
      const double d = (curve_[i] - curve_[i - 1]) - mean;
      var += d * d;
    }
    var /= static_cast<double>(n);
    if (var <= 0) return 0;
    return mean / std::sqrt(var) * std::sqrt(static_cast<double>(n));
  }

 private:
  struct Pending {
    std::uint64_t effect_ts;
    Price px;
    Qty qty;
    SymbolId sym;
    std::uint8_t side;  // 0 buy, 1 sell
  };

  void process_pending(std::uint64_t upto) noexcept {
    while (head_ < pending_.size() && pending_[head_].effect_ts <= upto) {
      eval(pending_[head_]);
      ++head_;
    }
    if (head_ != 0 && head_ == pending_.size()) {  // compact when drained
      pending_.clear();
      head_ = 0;
    }
  }

  void eval(const Pending& o) noexcept {
    const Bbo& b = bbo_[o.sym];
    if (!b.valid) {
      ++kills_;
      return;
    }
    if (o.side == 0) {  // buy IOC: marketable if limit >= prevailing ask
      if (b.ask_px <= o.px && b.ask_qty > 0) {
        const Qty fq = (p_.conservative_size && b.ask_qty < o.qty) ? b.ask_qty : o.qty;
        fill(o, fq, b.ask_px);
      } else {
        ++kills_;
      }
    } else {  // sell IOC
      if (b.bid_px >= o.px && b.bid_qty > 0) {
        const Qty fq = (p_.conservative_size && b.bid_qty < o.qty) ? b.bid_qty : o.qty;
        fill(o, fq, b.bid_px);
      } else {
        ++kills_;
      }
    }
  }

  void fill(const Pending& o, Qty fq, Price px) noexcept {
    ++fills_;
    volume_ += fq;
    const double amt = static_cast<double>(px) * static_cast<double>(fq);
    if (o.side == 0) {
      pos_[o.sym] += fq;
      cash_ -= amt;
    } else {
      pos_[o.sym] -= fq;
      cash_ += amt;
    }
    const double fee = p_.fee_per_share * static_cast<double>(fq);
    fees_ += fee;
    cash_ -= fee;
    dig_ = (dig_ ^ o.effect_ts) * 0x100000001b3ull;
    dig_ = (dig_ ^ static_cast<std::uint64_t>(px)) * 0x100000001b3ull;
    dig_ = (dig_ ^ fq) * 0x100000001b3ull;
    dig_ = (dig_ ^ ((static_cast<std::uint64_t>(o.sym) << 8) | o.side)) * 0x100000001b3ull;
  }

  Params p_;
  std::vector<Bbo> bbo_;
  std::vector<std::int64_t> pos_;
  std::vector<Pending> pending_;
  std::size_t head_ = 0;
  std::uint64_t now_ = 0;
  std::uint64_t applied_ = 0;
  std::uint64_t sent_ = 0, fills_ = 0, kills_ = 0, volume_ = 0;
  double cash_ = 0, fees_ = 0;
  std::uint64_t dig_ = 0xcbf29ce484222325ull;
  std::vector<double> curve_;
};

}  // namespace tl
