#pragma once
// plat/risk/session_guard.hpp — the risk desk's session-scoped state behind
// riskgate checks #13–#16: daily stop-loss latches, the exchanges'
// per-contract daily counters (日内开仓限额, 单合约日撤单), and cancels in
// flight. Split from riskgate.hpp for size only, like kill_switch.hpp; it
// lives in the gate's state, snapshots and hashes with it.
//
// Every field is restrictive state: a recovery that dropped a stop-loss
// latch would reopen a stopped-out book, one that dropped the per-contract
// counters would let the day's 500th cancel through twice.

#include <cstdint>
#include <vector>

#include "plat/codec/msgs.hpp"
#include "plat/core/hash.hpp"
#include "plat/core/types.hpp"

namespace plat {

class SessionGuard {
 public:
  explicit SessionGuard(std::size_t n_inst = 0) : n_inst_(n_inst) {}

  // #13 — latches are session-long: once stopped, a mark bounce back above
  // the line does not reopen the scope.
  [[nodiscard]] bool loss_latched(AccountId a, StrategyId s) const noexcept {
    if (loss_accounts_.empty() && loss_strategies_.empty()) [[likely]] return false;
    return contains(loss_accounts_, a) || contains(loss_strategies_, s);
  }
  void latch_strategy(StrategyId s) {
    if (!contains(loss_strategies_, s)) loss_strategies_.push_back(s);
  }
  void latch_account(AccountId a) {
    if (!contains(loss_accounts_, a)) loss_accounts_.push_back(a);
  }

  // #14/#15/#19 per (account, contract) daily counters. `opened` counts
  // position-opening fills: futures Open, stock buys.
  [[nodiscard]] Qty opened(AccountId a, InstrumentId i) const noexcept {
    const Acct* x = find(a);
    return x ? x->opened[i] : 0;
  }
  [[nodiscard]] std::uint32_t cancels(AccountId a, InstrumentId i) const noexcept {
    const Acct* x = find(a);
    return x ? x->cancels[i] : 0;
  }

  // #16: is a cancel for this order already in flight (and not yet past the
  // retry window, after which a lost cancel may be re-sent)?
  [[nodiscard]] bool cancel_in_flight(OrderId id, Nanos now, Nanos retry) const noexcept {
    for (const auto& p : pend_cxl_)
      if (p.id == id) return now - p.ts < retry;
    return false;
  }

  void on_cancel_sent(AccountId a, InstrumentId i, OrderId id, Nanos now) {
    ++acct(a).cancels[i];
    for (auto& p : pend_cxl_)
      if (p.id == id) {
        p.ts = now;  // a retry after the window restarts it
        return;
      }
    pend_cxl_.push_back({id, now});
  }

  // Report feed: opening fills count toward 开仓限额 / the ST buy cap —
  // every one on the account, foreign ones too (the exchange doesn't care
  // which process sent the order); a terminal report or cancel verdict
  // resolves an in-flight cancel.
  void on_exec(const ExecReport& e, bool stock) {
    if (e.type == ExecType::Fill && (stock ? e.side == Side::Buy : e.offset == Offset::Open))
      acct(e.account).opened[e.inst] += e.qty;
    const bool done = e.type == ExecType::Canceled || e.type == ExecType::CancelReject ||
                      e.type == ExecType::Reject || (e.type == ExecType::Fill && e.leaves == 0);
    if (!done || pend_cxl_.empty()) [[likely]] return;
    for (std::size_t k = 0; k < pend_cxl_.size(); ++k)
      if (pend_cxl_[k].id == e.cl_id) {
        pend_cxl_[k] = pend_cxl_.back();
        pend_cxl_.pop_back();
        return;
      }
  }

  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    for (const Acct& x : accts_) {
      h.mix(x.account);
      for (InstrumentId i = 0; i < x.opened.size(); ++i) {
        if (!x.opened[i] && !x.cancels[i]) continue;
        h.mix(i);
        h.mix(x.opened[i]);
        h.mix(x.cancels[i]);
      }
    }
    for (const StrategyId s : loss_strategies_) h.mix(s);
    for (const AccountId a : loss_accounts_) h.mix(a);
    for (const auto& p : pend_cxl_) {
      h.mix(p.id);
      h.mix(p.ts);
    }
    return h.value();
  }

  template <typename AppendFn>
  void save(std::vector<std::uint8_t>& out, AppendFn&& append) const {
    append(out, static_cast<std::uint64_t>(accts_.size()));
    for (const Acct& x : accts_) {
      append(out, x.account);
      std::uint64_t touched = 0;
      for (InstrumentId i = 0; i < x.opened.size(); ++i) touched += x.opened[i] || x.cancels[i];
      append(out, touched);
      for (InstrumentId i = 0; i < x.opened.size(); ++i) {
        if (!x.opened[i] && !x.cancels[i]) continue;
        append(out, i);
        append(out, x.opened[i]);
        append(out, x.cancels[i]);
      }
    }
    append(out, static_cast<std::uint64_t>(loss_strategies_.size()));
    for (const StrategyId s : loss_strategies_) append(out, s);
    append(out, static_cast<std::uint64_t>(loss_accounts_.size()));
    for (const AccountId a : loss_accounts_) append(out, a);
    append(out, static_cast<std::uint64_t>(pend_cxl_.size()));
    for (const auto& p : pend_cxl_) append(out, p);
  }
  template <typename ReadFn>
  std::size_t load(const std::uint8_t* p, std::size_t off, std::size_t avail, ReadFn&& read) {
    accts_.clear();
    loss_strategies_.clear();
    loss_accounts_.clear();
    pend_cxl_.clear();
    std::uint64_t n = 0;
    off = read(p, off, avail, n);
    for (std::uint64_t k = 0; k < n; ++k) {
      AccountId a{};
      off = read(p, off, avail, a);
      Acct& x = acct(a);
      std::uint64_t touched = 0;
      off = read(p, off, avail, touched);
      for (std::uint64_t j = 0; j < touched; ++j) {
        InstrumentId i{};
        Qty opened{};
        std::uint32_t cancels{};
        off = read(p, off, avail, i);
        off = read(p, off, avail, opened);
        off = read(p, off, avail, cancels);
        if (i < x.opened.size()) {
          x.opened[i] = opened;
          x.cancels[i] = cancels;
        }
      }
    }
    off = read(p, off, avail, n);
    for (std::uint64_t k = 0; k < n; ++k) {
      StrategyId s{};
      off = read(p, off, avail, s);
      loss_strategies_.push_back(s);
    }
    off = read(p, off, avail, n);
    for (std::uint64_t k = 0; k < n; ++k) {
      AccountId a{};
      off = read(p, off, avail, a);
      loss_accounts_.push_back(a);
    }
    off = read(p, off, avail, n);
    for (std::uint64_t k = 0; k < n; ++k) {
      PendingCancel pc{};
      off = read(p, off, avail, pc);
      pend_cxl_.push_back(pc);
    }
    return off;
  }

 private:
  // Dense-id per-contract arrays per account, sized at first touch (cold);
  // a site trades a handful of accounts, so lookup is a short linear scan.
  struct Acct {
    AccountId account = 0;
    std::vector<Qty> opened;
    std::vector<std::uint32_t> cancels;
  };
  struct PendingCancel {
    OrderId id;
    Nanos ts;
  };

  template <typename V, typename T>
  [[nodiscard]] static bool contains(const V& v, T x) noexcept {
    for (const auto& e : v)
      if (e == x) return true;
    return false;
  }
  [[nodiscard]] const Acct* find(AccountId a) const noexcept {
    for (const Acct& x : accts_)
      if (x.account == a) return &x;
    return nullptr;
  }
  Acct& acct(AccountId a) {
    for (Acct& x : accts_)
      if (x.account == a) [[likely]] return x;
    Acct& x = accts_.emplace_back();
    x.account = a;
    x.opened.assign(n_inst_, 0);
    x.cancels.assign(n_inst_, 0);
    return x;
  }

  std::size_t n_inst_;
  std::vector<Acct> accts_;
  std::vector<StrategyId> loss_strategies_;
  std::vector<AccountId> loss_accounts_;
  std::vector<PendingCancel> pend_cxl_;  // cancels sent, no terminal report yet
};

}  // namespace plat
