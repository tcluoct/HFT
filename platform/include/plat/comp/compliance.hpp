#pragma once
// plat/comp/compliance.hpp — the programmatic-trading compliance engine
// (§10.2). A cold-path consumer of the same canonical stream that
// reconstructs, per account, the counters the exchanges compute under the
// 2024–2025 programmatic-trading regime:
//
//   · order actions (news + cancels) per second and per day — the HFT
//     designation thresholds are ≥300/s or ≥20,000/day (§4.4);
//   · cancel ratio;
//   · self-trade incidence (same-account crosses observed in fills).
//
// Alerts fire at a configurable fraction of the exchange thresholds so the
// desk hears about an approach *before* the venue does. The EOD report is
// derived entirely from stream data — reproducible from the journal alone.

#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "plat/codec/msgs.hpp"
#include "plat/core/hash.hpp"
#include "plat/core/time.hpp"
#include "plat/core/types.hpp"

namespace plat {

struct ComplianceThresholds {
  std::uint32_t hft_actions_per_sec = 300;   // regulatory definition
  std::uint32_t hft_actions_per_day = 20'000;
  std::uint32_t alert_pct = 80;              // alert at this % of threshold
  std::uint32_t cancel_ratio_alert_pct = 70;
  std::uint32_t cancel_ratio_min_actions = 200;
};

struct AccountCounters {
  std::uint32_t day_orders = 0;
  std::uint32_t day_cancels = 0;
  std::uint32_t peak_actions_per_sec = 0;
  std::uint32_t self_trades = 0;
  std::uint32_t risk_denials = 0;
  bool hft_designation_hit = false;  // crossed a regulatory threshold today
};

class ComplianceEngine {
 public:
  explicit ComplianceEngine(ComplianceThresholds t = {}) : thr_(t) {}

  void on_new(const NewOrderReq& r) { note_action(r.account, r.ts, /*cancel=*/false); }
  void on_cancel(const CancelReq& c) { note_action(c.account, c.ts, /*cancel=*/true); }
  void on_deny(const RiskDeny& d) { ++acct_[d.account].risk_denials; }

  // Fill-side surveillance: a self-trade shows up as one venue trade whose
  // two exec reports share an account. The engine sees each side's report;
  // pairing by (venue trade) is approximated by matching opposing fills with
  // identical (inst, px, qty, ts) — exact for the sim venue's report stream.
  void on_exec(const ExecReport& e) {
    if (e.type != ExecType::Fill) return;
    for (auto it = pending_fills_.begin(); it != pending_fills_.end(); ++it) {
      if (it->ts == e.ts && it->inst == e.inst && it->px == e.px && it->qty == e.qty &&
          it->side != e.side) {
        if (it->account == e.account) ++acct_[e.account].self_trades;
        pending_fills_.erase(it);
        return;
      }
    }
    pending_fills_.push_back({e.ts, e.px, e.qty, e.inst, e.account, e.side});
    while (pending_fills_.size() > 64) pending_fills_.pop_front();
  }

  struct Alert {
    AccountId account;
    Nanos ts;
    std::string what;
  };
  [[nodiscard]] const std::deque<Alert>& alerts() const noexcept { return alerts_; }
  [[nodiscard]] const AccountCounters& account(AccountId a) { return acct_[a]; }
  [[nodiscard]] const std::map<AccountId, AccountCounters>& accounts() const { return acct_; }

  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    for (const auto& [a, c] : acct_) {
      h.mix(a);
      h.mix(c.day_orders);
      h.mix(c.day_cancels);
      h.mix(c.peak_actions_per_sec);
      h.mix(c.self_trades);
      h.mix(c.hft_designation_hit);
    }
    return h.value();
  }

  // Snapshot (§11.1): the rolling windows and unpaired fills are behavioral
  // state — a recovery that dropped them would compute different per-second
  // peaks and self-trade counts than the uninterrupted run, and the recovery
  // hash test would (did) catch it.
  void save(std::vector<std::uint8_t>& out) const {
    append(out, static_cast<std::uint64_t>(acct_.size()));
    for (const auto& [a, c] : acct_) {
      append(out, a);
      append(out, c);
      const auto wit = windows_.find(a);
      const std::uint64_t wn = wit == windows_.end() ? 0 : wit->second.size();
      append(out, wn);
      if (wit != windows_.end())
        for (const Nanos t : wit->second) append(out, t);
    }
    append(out, static_cast<std::uint64_t>(pending_fills_.size()));
    for (const auto& f : pending_fills_) append(out, f);
  }
  std::size_t load(const std::uint8_t* p, std::size_t avail) {
    acct_.clear();
    windows_.clear();
    pending_fills_.clear();
    std::uint64_t n = 0;
    std::size_t off = read(p, 0, avail, n);
    for (std::uint64_t i = 0; i < n; ++i) {
      AccountId a{};
      AccountCounters c{};
      off = read(p, off, avail, a);
      off = read(p, off, avail, c);
      std::uint64_t wn = 0;
      off = read(p, off, avail, wn);
      auto& win = windows_[a];
      for (std::uint64_t j = 0; j < wn; ++j) {
        Nanos t{};
        off = read(p, off, avail, t);
        win.push_back(t);
      }
      acct_[a] = c;
    }
    off = read(p, off, avail, n);
    for (std::uint64_t i = 0; i < n; ++i) {
      PendingFill f{};
      off = read(p, off, avail, f);
      pending_fills_.push_back(f);
    }
    return off;
  }

 private:
  void note_action(AccountId a, Nanos ts, bool cancel) {
    AccountCounters& c = acct_[a];
    if (cancel) ++c.day_cancels;
    else ++c.day_orders;

    auto& win = windows_[a];
    win.push_back(ts);
    while (!win.empty() && ts - win.front() >= kNsPerSec) win.pop_front();
    const auto per_sec = static_cast<std::uint32_t>(win.size());
    if (per_sec > c.peak_actions_per_sec) c.peak_actions_per_sec = per_sec;

    const std::uint32_t day_actions = c.day_orders + c.day_cancels;
    if (per_sec >= thr_.hft_actions_per_sec || day_actions >= thr_.hft_actions_per_day)
      c.hft_designation_hit = true;

    if (per_sec * 100 >= thr_.hft_actions_per_sec * thr_.alert_pct)
      alert(a, ts, "order-rate approaching HFT threshold");
    if (day_actions * 100 == thr_.hft_actions_per_day * thr_.alert_pct)
      alert(a, ts, "daily actions approaching HFT threshold");
    if (day_actions >= thr_.cancel_ratio_min_actions &&
        c.day_cancels * 100 >= day_actions * thr_.cancel_ratio_alert_pct)
      alert(a, ts, "cancel ratio elevated");
  }

  template <typename T>
  static void append(std::vector<std::uint8_t>& out, const T& v) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), b, b + sizeof(T));
  }
  template <typename T>
  static std::size_t read(const std::uint8_t* p, std::size_t off, std::size_t avail, T& v) {
    if (off + sizeof(T) <= avail) std::memcpy(&v, p + off, sizeof(T));
    return off + sizeof(T);
  }

  void alert(AccountId a, Nanos ts, const char* what) {
    // One alert per (account, message) per session — alarms that repeat
    // every event train operators to ignore them.
    for (const auto& al : alerts_)
      if (al.account == a && al.what == what) return;
    alerts_.push_back({a, ts, what});
  }

  ComplianceThresholds thr_;
  std::map<AccountId, AccountCounters> acct_;
  std::map<AccountId, std::deque<Nanos>> windows_;
  std::deque<Alert> alerts_;

  struct PendingFill {
    Nanos ts;
    Price px;
    Qty qty;
    InstrumentId inst;
    AccountId account;
    Side side;
  };
  std::deque<PendingFill> pending_fills_;
};

}  // namespace plat
