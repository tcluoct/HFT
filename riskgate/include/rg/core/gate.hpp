#pragma once
// The pre-trade risk gate. Sits between strategy and the exchange gateway;
// every order pays for this check, so the whole rule set is evaluated as one
// branchless pass: each rule computes a bool, the bool selects a reject bit,
// the bits OR into a mask. The only data-dependent branch in check() is the
// final position commit — and order flow is overwhelmingly accepts, so it
// predicts.
//
// Threading contract (mirrors the venue-side matchd argument, firm-side):
//   * check() — one hot thread (the order path is serialized anyway)
//   * limits table — published from a control thread through per-symbol
//     seqlocks; check() never blocks on a reload
//   * kill switch — one relaxed atomic, flippable from any thread

#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

#include "hft/common/platform.hpp"
#include "rg/core/limits.hpp"
#include "rg/core/throttle.hpp"
#include "rg/core/types.hpp"

namespace rg {

template <std::size_t MaxSymbols>
class RiskGate {
 public:
  explicit RiskGate(const LimitsTable<MaxSymbols>& limits) : limits_(&limits) {}

  [[nodiscard]] TokenBucket& throttle() noexcept { return throttle_; }

  // Any thread, any time. THE red button.
  void set_kill(bool on) noexcept { kill_.store(on ? 1u : 0u, std::memory_order_relaxed); }

  // Hot path. Returns 0 to accept, else the OR of every violated rule's bit.
  // `now_tsc` is caller-stamped so the caller controls (and tests can pin)
  // the clock.
  std::uint32_t check(const OrderReq& o, std::uint64_t now_tsc) noexcept {
    const bool sym_in_range = o.symbol < MaxSymbols;
    const SymbolId sym = sym_in_range ? o.symbol : 0;
    const SymbolLimits lim = limits_->read(sym, &retries_);

    const std::int64_t sign = 1 - 2 * static_cast<std::int64_t>(std::to_underlying(o.side));
    const std::int64_t signed_qty = sign * static_cast<std::int64_t>(o.qty);
    const std::int64_t newpos = position_[sym] + signed_qty;

    std::uint32_t m = 0;
    m |= kill_.load(std::memory_order_relaxed) * kKillSwitch;
    m |= static_cast<std::uint32_t>(!sym_in_range | (lim.enabled == 0)) * kUnknownSymbol;
    m |= static_cast<std::uint32_t>(o.qty == 0) * kQtyZero;
    m |= static_cast<std::uint32_t>(o.qty > lim.max_order_qty) * kQtyTooLarge;
    m |= static_cast<std::uint32_t>(notional_exceeds(static_cast<std::uint64_t>(o.price),
                                                     o.qty, lim.max_order_notional)) *
         kNotionalTooLarge;
    m |= static_cast<std::uint32_t>(o.price < lim.band_lo) * kPriceBandLow;
    m |= static_cast<std::uint32_t>(o.price > lim.band_hi) * kPriceBandHigh;
    m |= static_cast<std::uint32_t>(newpos > lim.max_long_pos) * kPositionLong;
    m |= static_cast<std::uint32_t>(newpos < -lim.max_short_pos) * kPositionShort;

    // The throttle refills on every call but only spends a token on an
    // otherwise-clean order: a reject never reaches the wire, so it must not
    // eat wire budget.
    const bool clean = (m == 0);
    const bool token_ok = throttle_.try_consume(now_tsc, clean);
    m |= static_cast<std::uint32_t>(clean & !token_ok) * kThrottled;

    // Worst-case position model: reserve the full order at entry.
    position_[sym] += (m == 0) ? signed_qty : 0;
    return m;
  }

  // Order died upstream of a fill (exchange reject / cancel of the full
  // remainder): hand its position reservation back.
  void unwind(const OrderReq& o) noexcept {
    const std::int64_t sign = 1 - 2 * static_cast<std::int64_t>(std::to_underlying(o.side));
    position_[o.symbol] -= sign * static_cast<std::int64_t>(o.qty);
  }

  [[nodiscard]] std::int64_t position(SymbolId s) const noexcept { return position_[s]; }
  [[nodiscard]] std::uint64_t seqlock_retries() const noexcept { return retries_; }

 private:
  const LimitsTable<MaxSymbols>* limits_;
  TokenBucket throttle_;
  std::atomic<std::uint32_t> kill_{0};
  std::uint64_t retries_ = 0;
  std::array<std::int64_t, MaxSymbols> position_{};
};

}  // namespace rg
