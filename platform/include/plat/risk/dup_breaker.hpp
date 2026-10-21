#pragma once
// plat/risk/dup_breaker.hpp — riskgate check #10, the duplicate/burst
// breaker: an identical order (account, strategy, instrument, side, price,
// qty) repeated inside the repeat-fire horizon is a strategy stuck in a
// loop, not intent. Fixed 16-slot ring, no allocation. Split from
// riskgate.hpp for size only; it lives in the gate's state.

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "plat/codec/msgs.hpp"
#include "plat/core/hash.hpp"
#include "plat/core/types.hpp"

namespace plat {

class DupBreaker {
 public:
  static constexpr std::uint32_t kCap = 16;

  DupBreaker(std::uint32_t window, Nanos horizon)
      : window_(window > kCap ? kCap : window), horizon_(horizon) {}

  [[nodiscard]] bool is_duplicate(const NewOrderReq& r, Nanos now) const noexcept {
    for (std::uint32_t k = 0; k < count_; ++k) {
      const Recent& d = at(k);
      if (now - d.ts > horizon_) continue;
      if (d.inst == r.inst && d.px == r.px && d.qty == r.qty && d.side == r.side &&
          d.strategy == r.strategy && d.account == r.account)
        return true;
    }
    return false;
  }
  void remember(const NewOrderReq& r, Nanos now) noexcept {
    push({r.inst, r.px, r.qty, r.strategy, r.account, r.side, now});
  }

  void mix_into(StateHash& h) const {
    for (std::uint32_t k = 0; k < count_; ++k) {  // field-wise: Recent has padding
      const Recent& d = at(k);
      h.mix(d.inst);
      h.mix(d.px);
      h.mix(d.qty);
      h.mix(d.strategy);
      h.mix(d.account);
      h.mix(d.side);
      h.mix(d.ts);
    }
  }
  template <typename AppendFn>
  void save(std::vector<std::uint8_t>& out, AppendFn&& append) const {
    append(out, static_cast<std::uint64_t>(count_));
    for (std::uint32_t k = 0; k < count_; ++k) append(out, at(k));
  }
  template <typename ReadFn>
  std::size_t load(const std::uint8_t* p, std::size_t off, std::size_t avail, ReadFn&& read) {
    head_ = count_ = 0;
    std::uint64_t n = 0;
    off = read(p, off, avail, n);
    for (std::uint64_t i = 0; i < n; ++i) {
      Recent r{};
      off = read(p, off, avail, r);
      push(r);
    }
    return off;
  }

 private:
  struct Recent {
    InstrumentId inst;
    Price px;
    Qty qty;
    StrategyId strategy;
    AccountId account;
    Side side;
    Nanos ts;
  };

  [[nodiscard]] const Recent& at(std::uint32_t k) const noexcept {
    return ring_[(head_ + k) % kCap];
  }
  void push(const Recent& r) noexcept {
    if (window_ == 0) return;
    if (count_ == window_) {
      head_ = (head_ + 1) % kCap;  // evict oldest
      --count_;
    }
    // Zero the slot first: a plain struct copy may carry the temporary's
    // indeterminate padding into the snapshot bytes save() writes.
    Recent& slot = ring_[(head_ + count_++) % kCap];
    std::memset(&slot, 0, sizeof(slot));
    slot.inst = r.inst;
    slot.px = r.px;
    slot.qty = r.qty;
    slot.strategy = r.strategy;
    slot.account = r.account;
    slot.side = r.side;
    slot.ts = r.ts;
  }

  std::uint32_t window_;
  Nanos horizon_;
  std::array<Recent, kCap> ring_{};
  std::uint32_t head_ = 0;
  std::uint32_t count_ = 0;
};

}  // namespace plat
