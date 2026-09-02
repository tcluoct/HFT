#pragma once
// plat/risk/kill_switch.hpp — the kill switch (§10.3): scoped halts
// (strategy / account / site / firm) with versioned commands. Split from
// riskgate.hpp for size only; it is riskgate check #12 and lives in the
// gate's state.

#include <cstdint>
#include <map>
#include <vector>

#include "plat/codec/msgs.hpp"
#include "plat/core/hash.hpp"
#include "plat/core/types.hpp"

namespace plat {

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
    const bool arm = c.action != KillAction::Disarm;
    switch (c.scope) {
      case KillScope::Firm:  firm_ = arm; break;
      case KillScope::Site:  site_ = arm; break;
      case KillScope::Account: account_[c.target] = arm; break;
      case KillScope::Strategy: strategy_[c.target] = arm; break;
    }
  }

  [[nodiscard]] std::uint64_t last_cmd_seq() const noexcept { return last_cmd_seq_; }
  [[nodiscard]] bool halted(AccountId a, StrategyId s) const {
    if (firm_ || site_) return true;
    if (account_.empty() && strategy_.empty()) [[likely]] return false;
    if (const auto it = account_.find(a); it != account_.end() && it->second) return true;
    if (const auto it = strategy_.find(s); it != strategy_.end() && it->second) return true;
    return false;
  }

  // Pre-flight probe (§11.3): is any scope armed at all?
  [[nodiscard]] bool any_armed() const {
    if (firm_ || site_) return true;
    for (const auto& [id, armed] : account_)
      if (armed) return true;
    for (const auto& [id, armed] : strategy_)
      if (armed) return true;
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
    for (const auto& [id, armed] : account_) {
      h.mix(id);
      h.mix(armed);
    }
    for (const auto& [id, armed] : strategy_) {
      h.mix(id);
      h.mix(armed);
    }
    return h.value();
  }
  template <typename AppendFn>
  void save(std::vector<std::uint8_t>& out, AppendFn&& append) const {
    append(out, firm_);
    append(out, site_);
    append(out, last_cmd_seq_);
    append(out, static_cast<std::uint64_t>(account_.size()));
    for (const auto& [id, armed] : account_) {
      append(out, id);
      append(out, armed);
    }
    append(out, static_cast<std::uint64_t>(strategy_.size()));
    for (const auto& [id, armed] : strategy_) {
      append(out, id);
      append(out, armed);
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
      bool armed{};
      off = read(p, off, avail, id);
      off = read(p, off, avail, armed);
      account_[id] = armed;
    }
    off = read(p, off, avail, n);
    for (std::uint64_t i = 0; i < n; ++i) {
      std::uint16_t id{};
      bool armed{};
      off = read(p, off, avail, id);
      off = read(p, off, avail, armed);
      strategy_[id] = armed;
    }
    return off;
  }

 private:
  bool firm_ = false;
  bool site_ = false;
  std::uint64_t last_cmd_seq_ = 0;
  std::map<std::uint16_t, bool> account_;   // cold: written by operator commands only
  std::map<std::uint16_t, bool> strategy_;
};

}  // namespace plat
