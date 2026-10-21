#pragma once
// platops_json.hpp — mirror state → one JSON document for the web UI.
// Hand-rolled writer (cold path, no dependency); everything money is
// converted e4 → CNY here so the page never sees internal units.

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "plat/core/time.hpp"
#include "platops_app.hpp"

namespace platops {

inline const char* risk_check_name(RiskCheck c) {
  switch (c) {
    case RiskCheck::PhaseGate: return "PhaseGate";
    case RiskCheck::PriceCollar: return "PriceCollar";
    case RiskCheck::SizeNotional: return "SizeNotional";
    case RiskCheck::Exposure: return "Exposure";
    case RiskCheck::SellableInventory: return "Sellable";
    case RiskCheck::Margin: return "Margin";
    case RiskCheck::RateLimit: return "RateLimit";
    case RiskCheck::CancelRatio: return "CancelRatio";
    case RiskCheck::SelfTrade: return "SelfTrade";
    case RiskCheck::DuplicateBurst: return "Duplicate";
    case RiskCheck::StaleData: return "StaleData";
    case RiskCheck::KillSwitch: return "KillSwitch";
    case RiskCheck::TickSize: return "TickSize";
    case RiskCheck::LossLimit: return "LossLimit";
    case RiskCheck::InstOpenLimit: return "InstOpenLimit";
    case RiskCheck::InstCancelLimit: return "InstCancelLimit";
    case RiskCheck::CancelTarget: return "CancelTarget";
    case RiskCheck::StockQty: return "StockQty";
    case RiskCheck::PriceCage: return "PriceCage";
    case RiskCheck::StBuyLimit: return "StBuyLimit";
    case RiskCheck::Concentration: return "Concentration";
  }
  return "?";
}

inline const char* offset_name(Offset o) {
  switch (o) {
    case Offset::Open: return "Open";
    case Offset::CloseToday: return "CloseTd";
    case Offset::CloseYesterday: return "CloseYd";
  }
  return "?";
}

inline const char* order_state_name(OrderState s) {
  switch (s) {
    case OrderState::PendingNew: return "Pending";
    case OrderState::Live: return "Live";
    case OrderState::Filled: return "Filled";
    case OrderState::Canceled: return "Canceled";
    case OrderState::Rejected: return "Rejected";
  }
  return "?";
}

// ---- tiny JSON appender -----------------------------------------------

inline void jstr(std::string& o, std::string_view s) {
  o += '"';
  for (const char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  o += '"';
}
inline void jkey(std::string& o, const char* k) { jstr(o, k); o += ':'; }
inline void jcny(std::string& o, std::int64_t e4) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", px_to_cny(e4));
  o += buf;
}
inline void jpx(std::string& o, Price px) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4f", px_to_cny(px));
  o += buf;
}
inline void jts(std::string& o, Nanos t) {
  const TimeParts p = time_parts(t);
  char buf[24];
  std::snprintf(buf, sizeof(buf), "\"%02d:%02d:%02d.%03d\"", p.hh, p.mm, p.ss, p.ms);
  o += buf;
}
inline void jhex(std::string& o, std::uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "\"%llx\"", static_cast<unsigned long long>(v));
  o += buf;
}

// ---- per-shard sections -------------------------------------------------

inline void json_strategies(std::string& o, MirrorShard& sh) {
  MirrorSite* site = sh.site();
  o += '[';
  bool first = true;
  for (const auto& s : sh.specs()) {
    if (!first) o += ',';
    first = false;
    o += '{';
    jkey(o, "id"); o += std::to_string(s.id); o += ',';
    jkey(o, "name"); jstr(o, s.name); o += ',';
    jkey(o, "type"); jstr(o, Pack::kind_name(s.kind)); o += ',';
    jkey(o, "halted");
    o += site && site->risk().kill().halted(s.account, s.id) ? "true" : "false";
    o += ',';
    jkey(o, "realized"); jcny(o, site ? site->pnl().total_realized_e4(s.id) : 0); o += ',';
    jkey(o, "unrealized"); jcny(o, site ? site->pnl().total_unrealized_e4(s.id) : 0); o += ',';
    jkey(o, "fees"); jcny(o, site ? site->pnl().total_fees_e4(s.id) : 0);
    o += '}';
  }
  // Ledger-only ids (carried book of a strategy not configured this session).
  if (site) {
    std::vector<StrategyId> extra;
    site->pnl().for_each([&](const PosKey& k, const Position&) {
      for (const auto& s : sh.specs())
        if (s.id == k.strategy) return;
      for (const StrategyId id : extra)
        if (id == k.strategy) return;
      extra.push_back(k.strategy);
    });
    for (const StrategyId id : extra) {
      if (!first) o += ',';
      first = false;
      o += '{';
      jkey(o, "id"); o += std::to_string(id); o += ',';
      jkey(o, "name"); jstr(o, "(carried)"); o += ',';
      jkey(o, "type"); jstr(o, "-"); o += ',';
      jkey(o, "halted"); o += "false"; o += ',';
      jkey(o, "realized"); jcny(o, site->pnl().total_realized_e4(id)); o += ',';
      jkey(o, "unrealized"); jcny(o, site->pnl().total_unrealized_e4(id)); o += ',';
      jkey(o, "fees"); jcny(o, site->pnl().total_fees_e4(id));
      o += '}';
    }
  }
  o += ']';
}

inline void json_positions(std::string& o, MirrorShard& sh) {
  o += '[';
  MirrorSite* site = sh.site();
  if (site) {
    bool first = true;
    site->pnl().for_each([&](const PosKey& k, const Position& p) {
      if (p.net() == 0 && p.total == 0 && p.long_yd + p.long_td + p.short_yd + p.short_td == 0)
        return;
      if (!first) o += ',';
      first = false;
      o += '{';
      jkey(o, "strategy"); o += std::to_string(k.strategy); o += ',';
      jkey(o, "symbol"); jstr(o, sh.symbol(k.inst)); o += ',';
      jkey(o, "long_yd"); o += std::to_string(p.long_yd); o += ',';
      jkey(o, "long_td"); o += std::to_string(p.long_td); o += ',';
      jkey(o, "short_yd"); o += std::to_string(p.short_yd); o += ',';
      jkey(o, "short_td"); o += std::to_string(p.short_td); o += ',';
      jkey(o, "net"); o += std::to_string(p.net()); o += ',';
      jkey(o, "long_cost"); jpx(o, p.long_cost_e4); o += ',';
      jkey(o, "short_cost"); jpx(o, p.short_cost_e4); o += ',';
      jkey(o, "mark"); jpx(o, site->pnl().mark_for(k.inst)); o += ',';
      jkey(o, "unrealized"); jcny(o, site->pnl().unrealized_e4(k, p)); o += ',';
      jkey(o, "realized"); jcny(o, p.realized_e4);
      o += '}';
    });
  }
  o += ']';
}

inline void json_orders(std::string& o, MirrorShard& sh, Nanos now) {
  o += '[';
  MirrorSite* site = sh.site();
  if (site) {
    bool first = true;
    for (InstrumentId i = 0; i < sh.table().size(); ++i) {
      for (const OmsOrder* ord : site->oms().open_orders(i)) {
        if (!first) o += ',';
        first = false;
        o += '{';
        jkey(o, "cl_id"); jhex(o, ord->cl_id); o += ',';
        jkey(o, "symbol"); jstr(o, sh.symbol(ord->inst)); o += ',';
        jkey(o, "strategy"); o += std::to_string(ord->strategy); o += ',';
        jkey(o, "side"); jstr(o, side_name(ord->side)); o += ',';
        jkey(o, "offset"); jstr(o, offset_name(ord->offset)); o += ',';
        jkey(o, "px"); jpx(o, ord->px); o += ',';
        jkey(o, "qty"); o += std::to_string(ord->qty); o += ',';
        jkey(o, "leaves"); o += std::to_string(ord->leaves); o += ',';
        jkey(o, "state"); jstr(o, order_state_name(ord->state)); o += ',';
        jkey(o, "age_s");
        o += std::to_string(now > ord->create_ts ? (now - ord->create_ts) / kNsPerSec : 0);
        o += '}';
      }
    }
  }
  o += ']';
}

inline void json_feeds(std::string& o, MirrorShard& sh) {
  jkey(o, "fills");
  o += '[';
  bool first = true;
  for (const FillRow& f : sh.fills()) {
    if (!first) o += ',';
    first = false;
    o += '{';
    jkey(o, "ts"); jts(o, f.ts); o += ',';
    jkey(o, "symbol"); jstr(o, sh.symbol(f.inst)); o += ',';
    jkey(o, "strategy"); o += std::to_string(f.strategy); o += ',';
    jkey(o, "side"); jstr(o, side_name(f.side)); o += ',';
    jkey(o, "offset"); jstr(o, offset_name(f.offset)); o += ',';
    jkey(o, "px"); jpx(o, f.px); o += ',';
    jkey(o, "qty"); o += std::to_string(f.qty);
    o += '}';
  }
  o += "],";
  jkey(o, "denies");
  o += '[';
  first = true;
  for (const DenyRow& d : sh.denies()) {
    if (!first) o += ',';
    first = false;
    o += '{';
    jkey(o, "ts"); jts(o, d.ts); o += ',';
    jkey(o, "symbol"); jstr(o, sh.symbol(d.inst)); o += ',';
    jkey(o, "strategy"); o += std::to_string(d.strategy); o += ',';
    jkey(o, "check"); jstr(o, platops::risk_check_name(d.check)); o += ',';
    jkey(o, "cancel"); o += d.is_cancel ? "true" : "false";
    o += '}';
  }
  o += "],";
  jkey(o, "ops");
  o += '[';
  first = true;
  for (const OpsRow& r : sh.ops_log()) {
    if (!first) o += ',';
    first = false;
    o += '{';
    jkey(o, "ts"); jts(o, r.ts); o += ',';
    jkey(o, "text"); jstr(o, r.text);
    o += '}';
  }
  o += ']';
}

inline void json_risk(std::string& o, MirrorShard& sh) {
  MirrorSite* site = sh.site();
  o += '{';
  jkey(o, "pass"); o += std::to_string(site ? site->tel().get("risk.pass") : 0); o += ',';
  jkey(o, "deny"); o += std::to_string(site ? site->tel().get("risk.deny") : 0); o += ',';
  jkey(o, "margin_used"); jcny(o, site ? site->pnl().margin_used_e4(sh.account()) : 0); o += ',';
  jkey(o, "margin_limit"); jcny(o, site ? site->config().limits.max_margin_e4 : 0); o += ',';
  jkey(o, "any_armed");
  o += site && site->risk().kill().any_armed() ? "true" : "false";
  o += ',';
  jkey(o, "checks");
  o += '{';
  bool first = true;
  for (int c = 1; c <= static_cast<int>(RiskCheck::TickSize); ++c) {
    const std::uint64_t n =
        site ? site->tel().get("risk.deny." + std::to_string(c)) : 0;
    if (n == 0) continue;
    if (!first) o += ',';
    first = false;
    jkey(o, platops::risk_check_name(static_cast<RiskCheck>(c)));
    o += std::to_string(n);
  }
  o += "}}";
}

inline void json_alerts(std::string& o, MirrorShard& sh, Nanos now) {
  o += '[';
  bool first = true;
  const auto add = [&](const std::string& s) {
    if (!first) o += ',';
    first = false;
    jstr(o, s);
  };
  if (!sh.specs_ok()) add("strategy config INVALID (fail closed)");
  MirrorSite* site = sh.site();
  if (!sh.tail().exists()) add("waiting for journal " + sh.jnl_path());
  else if (!site) add("journal present, sidecar missing — mirror not built");
  if (site) {
    const bool live = !sh.tail().header().sealed;
    if (site->risk().kill().any_armed()) add("kill switch ARMED");
    if (live && sh.phase() == Phase::Continuous && sh.last_md_ts() &&
        now - sh.last_md_ts() > 5 * kNsPerSec)
      add("market data stale " + std::to_string((now - sh.last_md_ts()) / kNsPerSec) + "s");
    if (live && sh.last_env_ts() && sh.phase() == Phase::Continuous &&
        now - sh.last_env_ts() > 10 * kNsPerSec)
      add("journal stale " + std::to_string((now - sh.last_env_ts()) / kNsPerSec) + "s");
    std::size_t recent = 0;
    for (const DenyRow& d : sh.denies())
      if (now - d.ts < 60 * kNsPerSec) ++recent;
    if (recent >= 10) add("risk deny burst: " + std::to_string(recent) + " in 60s");
  }
  o += ']';
}

// ---- the document -------------------------------------------------------

inline std::string build_state_json(std::vector<std::unique_ptr<MirrorShard>>& shards,
                                    Nanos now) {
  std::string o;
  o.reserve(64 * 1024);
  o += '{';
  jkey(o, "now"); jts(o, now); o += ',';
  jkey(o, "shards");
  o += '[';
  bool first = true;
  for (auto& shp : shards) {
    MirrorShard& sh = *shp;
    if (!first) o += ',';
    first = false;
    MirrorSite* site = sh.site();
    o += '{';
    jkey(o, "name"); jstr(o, sh.name()); o += ',';
    jkey(o, "journal"); jstr(o, sh.jnl_path()); o += ',';
    jkey(o, "day"); o += std::to_string(sh.tail().exists() ? sh.tail().header().day : 0);
    o += ',';
    jkey(o, "status");
    jstr(o, !sh.tail().exists() ? "waiting"
             : !site            ? "no-sidecar"
             : sh.tail().header().sealed ? "sealed" : "live");
    o += ',';
    jkey(o, "phase"); jstr(o, site ? phase_name(sh.phase()) : "-"); o += ',';
    jkey(o, "session");
    o += std::to_string(site ? static_cast<int>(site->session_state()) : 0);
    o += ',';
    jkey(o, "envelopes"); o += std::to_string(sh.envelopes()); o += ',';
    jkey(o, "md_age_ms");
    o += std::to_string(sh.last_md_ts() && now > sh.last_md_ts()
                            ? (now - sh.last_md_ts()) / kNsPerMs
                            : -1);
    o += ',';
    jkey(o, "strategies"); json_strategies(o, sh); o += ',';
    jkey(o, "positions"); json_positions(o, sh); o += ',';
    jkey(o, "orders"); json_orders(o, sh, now); o += ',';
    json_feeds(o, sh); o += ',';
    jkey(o, "risk"); json_risk(o, sh); o += ',';
    jkey(o, "alerts"); json_alerts(o, sh, now);
    o += '}';
  }
  o += "]}";
  return o;
}

}  // namespace platops
