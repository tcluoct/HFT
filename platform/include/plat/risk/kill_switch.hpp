#pragma once
// plat/risk/kill_switch.hpp — the kill switch (§10.3): scoped HALT / FLATTEN /
// DISARM with versioned commands. Split from riskgate.hpp for size only; it
// is riskgate check #12 and lives in the gate's state.
//
// Two armed modes, deliberately distinct:
//
//   Halt    — no orders at all. Operators use it to stop a misbehaving site.
//   Flatten — reduce-only: resting orders are cancelled and the site closes
//             out its positions (site_dispatch.hpp), while *opening* orders
//             stay refused until the scope is disarmed. This is the emergency
//             exit, so riskgate lets reduce-only orders through the kill gate
//             (and through the throttles that would otherwise block a close),
//             but never an order that adds risk.
//
// The mode is per scope and the most restrictive scope wins: any HALT in
// effect beats a FLATTEN, which beats OFF.

#include <cstdint>
#include <map>
#include <vector>

#include "plat/codec/msgs.hpp"
#include "plat/core/hash.hpp"
#include "plat/core/types.hpp"

namespace plat {

enum class KillMode : std::uint8_t { Off = 0, Flatten = 1, Halt = 2 };

class KillSwitch {
 public:
  void apply(const KillSwitchCmd& c) {
    // Versioned commands (§10.3, review R1): a stale or replayed command —
    // e.g. redelivered over the bridge after a partition — must not override
    // a newer one. cmd_seq 0 is unversioned and always applies.
    if (c.cmd_seq != 0) {
      if (c.cmd_seq <= last_cmd_seq_) return;
      last_cmd_seq_ = c.cmd_seq;
    }
    const KillMode m = c.action == KillAction::Disarm    ? KillMode::Off
                       : c.action == KillAction::Flatten ? KillMode::Flatten
                                                         : KillMode::Halt;
    switch (c.scope) {
      case KillScope::Firm:  firm_ = m; break;
      case KillScope::Site:  site_ = m; break;
      case KillScope::Account: account_[c.target] = m; break;
      case KillScope::Strategy: strategy_[c.target] = m; break;
    }
  }

  [[nodiscard]] std::uint64_t last_cmd_seq() const noexcept { return last_cmd_seq_; }

  // Effective mode for one order's (account, strategy) — the most restrictive
  // armed scope wins.
  [[nodiscard]] KillMode mode(AccountId a, StrategyId s) const {
    KillMode m = KillMode::Off;
    merge(m, firm_);
    merge(m, site_);
    if (const auto it = account_.find(a); it != account_.end()) merge(m, it->second);
    if (const auto it = strategy_.find(s); it != strategy_.end()) merge(m, it->second);
    return m;
  }

  [[nodiscard]] bool halted(AccountId a, StrategyId s) const {
    return mode(a, s) != KillMode::Off;
  }

  // Pre-flight probe (§11.3): is any scope armed at all?
  [[nodiscard]] bool any_armed() const {
    if (firm_ != KillMode::Off || site_ != KillMode::Off) return true;
    for (const auto& [id, m] : account_)
      if (m != KillMode::Off) return true;
    for (const auto& [id, m] : strategy_)
      if (m != KillMode::Off) return true;
    return false;
  }

  // Armed state is restrictive state: a riskgate restart that dropped it
  // would silently disarm a halt (§10.3, review R1) — so it snapshots and
  // hashes with the rest of the gate.
  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    h.mix(firm_);
    h.mix(site_);
    h.mix(last_cmd_seq_);
    for (const auto& [id, m] : account_) {
      h.mix(id);
      h.mix(m);
    }
    for (const auto& [id, m] : strategy_) {
      h.mix(id);
      h.mix(m);
    }
    return h.value();
  }
  template <typename AppendFn>
  void save(std::vector<std::uint8_t>& out, AppendFn&& append) const {
    append(out, firm_);
    append(out, site_);
    append(out, last_cmd_seq_);
    append(out, static_cast<std::uint64_t>(account_.size()));
    for (const auto& [id, m] : account_) {
      append(out, id);
      append(out, m);
    }
    append(out, static_cast<std::uint64_t>(strategy_.size()));
    for (const auto& [id, m] : strategy_) {
      append(out, id);
      append(out, m);
    }
  }
  template <typename ReadFn>
  std::size_t load(const std::uint8_t* p, std::size_t off, std::size_t avail, ReadFn&& read) {
    account_.clear();
    strategy_.clear();
    off = read(p, off, avail, firm_);
    off = read(p, off, avail, site_);
    off = read(p, off, avail, last_cmd_seq_);
    std::uint64_t n = 0;
    off = read(p, off, avail, n);
    for (std::uint64_t i = 0; i < n; ++i) {
      std::uint16_t id{};
      KillMode m{};
      off = read(p, off, avail, id);
      off = read(p, off, avail, m);
      account_[id] = m;
    }
    off = read(p, off, avail, n);
    for (std::uint64_t i = 0; i < n; ++i) {
      std::uint16_t id{};
      KillMode m{};
      off = read(p, off, avail, id);
      off = read(p, off, avail, m);
      strategy_[id] = m;
    }
    return off;
  }

 private:
  static void merge(KillMode& m, KillMode x) noexcept {
    if (x == KillMode::Halt) m = KillMode::Halt;
    else if (x == KillMode::Flatten && m != KillMode::Halt) m = KillMode::Flatten;
  }

  KillMode firm_ = KillMode::Off;
  KillMode site_ = KillMode::Off;
  std::uint64_t last_cmd_seq_ = 0;
  std::map<std::uint16_t, KillMode> account_;   // cold: operator commands only
  std::map<std::uint16_t, KillMode> strategy_;
};

}  // namespace plat
