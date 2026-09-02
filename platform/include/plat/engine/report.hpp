#pragma once
// plat/engine/report.hpp — the end-of-session report (§11.3, §14): one text
// artifact per site per day covering pre-flight, activity, risk denials by
// check, compliance counters vs the HFT thresholds, latency percentiles, and
// reconciliation results. Everything here is derived from stream-fed state,
// so the same report can be regenerated from the journal.

#include <format>
#include <string>

#include "plat/comp/compliance.hpp"
#include "plat/core/types.hpp"
#include "plat/engine/site.hpp"  // for SiteConfig consumers; works with any Site<...> instantiation

namespace plat {

template <typename SiteT>
std::string session_report(SiteT& site) {
  std::string r;
  const auto& cfg = site.config();
  r += std::format("=== session report — {} ({}) day {} ===\n", cfg.name,
                   venue_name(cfg.venue), cfg.day);

  const auto& pf = site.preflight_result();
  r += std::format("pre-flight: {}\n", pf.ok() ? "PASS" : "FAIL");

  auto& tel = site.tel();
  r += std::format("envelopes: {}  fills: {}  risk pass/deny: {}/{}\n",
                   tel.get("stream.envelopes"), tel.get("fills"), tel.get("risk.pass"),
                   tel.get("risk.deny"));

  // Risk denials by check id (§10.1 numbering).
  static constexpr const char* kCheckNames[] = {
      "-",          "phase",     "collar",    "size",      "exposure", "sellable",
      "margin",     "rate",      "cancel%",   "self-trade", "dup",      "stale",
      "kill"};
  for (int c = 1; c <= 12; ++c) {
    const auto n = tel.get("risk.deny." + std::to_string(c));
    if (n) r += std::format("  deny[{}] {}: {}\n", c, kCheckNames[c], n);
  }

  for (const auto& [acct, cc] : site.compliance().accounts()) {
    r += std::format(
        "compliance acct {}: orders {} cancels {} peak/s {} self-trades {} hft-hit {}\n", acct,
        cc.day_orders, cc.day_cancels, cc.peak_actions_per_sec, cc.self_trades,
        cc.hft_designation_hit ? "YES" : "no");
  }
  for (const auto& al : site.compliance().alerts()) {
    const auto tp = time_parts(al.ts);
    r += std::format("  alert {:02}:{:02}:{:02} acct {}: {}\n", tp.hh, tp.mm, tp.ss, al.account,
                     al.what);
  }

  for (const auto& [name, h] : tel.histos()) {
    if (h.count() == 0) continue;
    r += std::format("{}: n={} p50={} p99={} p99.9={} max={} (ns)\n", name, h.count(),
                     h.percentile(0.50), h.percentile(0.99), h.percentile(0.999), h.max());
  }

  const auto& eod = site.eod();
  if (eod.ran) {
    r += std::format("EOD recon: order breaks {}  fills {}  (venue buy/sell {}/{})\n",
                     eod.order_breaks, eod.fills_ok ? "OK" : "BREAK", eod.venue_buy,
                     eod.venue_sell);
  }
  return r;
}

// Per-strategy PnL block (shared fee library — same numbers a backtest of
// this day would print, §13.2).
template <typename SiteT>
std::string pnl_report(SiteT& site, StrategyId s, const char* label) {
  const auto& pnl = site.pnl();
  return std::format("strategy {} ({}): realized {:.2f} CNY, unrealized {:.2f}, fees {:.2f}\n",
                     s, label, px_to_cny(pnl.total_realized_e4(s)),
                     px_to_cny(pnl.total_unrealized_e4(s)), px_to_cny(pnl.total_fees_e4(s)));
}

}  // namespace plat
