#pragma once
// plat/engine/ops_cmds.hpp — extended operator verbs, shared by the stdin
// console and the cmd-file channel (which the platops web UI drives). Every
// verb that changes engine state is injected as a journaled SOURCE envelope:
// manual interventions are risk-checked, replayable, and land in the audit
// record exactly like anything else that crosses the deterministic boundary.
//
//   param <strat_id> <key> <int_value>
//       live re-tune of one whitelisted strategy parameter (config key
//       names; timeout_ms in ms, prices in e4, the rest plain ints)
//   order <strat_id> <symbol> buy|sell open|close|closetoday <px_cny> <lots>
//       operator order under a strategy's book, full riskgate applies
//   cancel <cl_id>
//       pull one working order (decimal or 0x-hex client order id)
//   transfer <symbol> <from_id> <to_id> long|short <lots>
//       bookkeeping transfer between strategy books (no exchange action)

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

#include "plat/codec/msgs.hpp"
#include "plat/core/log.hpp"
#include "plat/core/types.hpp"

namespace plat {

// Returns true if the line was one of the extended verbs (handled + logged,
// even when its arguments were rejected); false lets the caller fall through
// to its own verbs / help text.
template <typename SiteT>
bool apply_ops_command(SiteT& site, AccountId account, const std::string& line, Nanos now) {
  std::istringstream ss(line);
  std::string verb;
  ss >> verb;

  if (verb == "param") {
    std::uint32_t id = 0;
    std::string key;
    std::int64_t value = 0;
    if (!(ss >> id >> key >> value) || id == 0 || key.empty() ||
        key.size() >= kParamKeyLen) {
      logf("[ops] usage: param <strat_id> <key> <int_value>\n");
      return true;
    }
    ParamUpdateEvt u{};
    u.ts = now;
    u.value = value;
    u.strategy = static_cast<StrategyId>(id);
    std::memcpy(u.key, key.c_str(), key.size() + 1);
    site.source(u, now);
    logf("[ops] param: strategy %u %s = %lld\n", u.strategy, u.key,
         static_cast<long long>(value));
    return true;
  }

  if (verb == "order") {
    std::uint32_t id = 0;
    std::string sym, side_s, off_s;
    double px_cny = 0.0;
    std::int64_t lots = 0;
    // id 0 is the unattributed carry-in book — an operator may close it by hand.
    if (!(ss >> id >> sym >> side_s >> off_s >> px_cny >> lots) || lots <= 0 ||
        px_cny <= 0.0 || (side_s != "buy" && side_s != "sell") ||
        (off_s != "open" && off_s != "close" && off_s != "closetoday")) {
      logf("[ops] usage: order <strat_id> <symbol> buy|sell open|close|closetoday "
           "<px_cny> <lots>\n");
      return true;
    }
    const InstrumentId inst = site.table().find(sym);
    if (inst == kNoInstrument) {
      logf("[ops] order: `%s` is not hosted at this site\n", sym.c_str());
      return true;
    }
    ManualOrderCmd m{};
    m.ts = now;
    m.px = px_from_cny(px_cny);
    m.qty = lots;
    m.inst = inst;
    m.strategy = static_cast<StrategyId>(id);
    m.account = account;
    m.side = side_s == "buy" ? Side::Buy : Side::Sell;
    m.offset = off_s == "open"    ? Offset::Open
               : off_s == "close" ? Offset::CloseYesterday
                                  : Offset::CloseToday;
    site.source(m, now);
    logf("[ops] order: strategy %u %s %s %s %.4f x %lld (risk-checked) cl_id=0x%016llx\n",
         m.strategy, side_s.c_str(), off_s.c_str(), sym.c_str(), px_cny,
         static_cast<long long>(lots),
         static_cast<unsigned long long>(site.last_manual_order_cl_id()));
    // Cold path (operator command), so report a risk denial right here
    // rather than leaving it for the next `status`.
    if (const RiskDeny& d = site.last_deny(); d.check != RiskCheck{} &&
                                              d.cl_id == site.last_manual_order_cl_id())
      logf("[ops] order REJECTED by risk check `%s` — not sent to the counter\n",
           risk_check_name(d.check));
    return true;
  }

  if (verb == "cancel") {
    std::string id_s;
    if (!(ss >> id_s)) {
      logf("[ops] usage: cancel <cl_id>\n");
      return true;
    }
    OrderId cl_id = kNoOrder;
    try {
      cl_id = std::stoull(id_s, nullptr, 0);  // decimal or 0x-hex
    } catch (...) {}
    if (cl_id == kNoOrder) {
      logf("[ops] cancel: bad order id `%s`\n", id_s.c_str());
      return true;
    }
    ManualCancelCmd c{};
    c.ts = now;
    c.cl_id = cl_id;
    site.source(c, now);
    logf("[ops] cancel: %016llx requested\n", static_cast<unsigned long long>(cl_id));
    return true;
  }

  if (verb == "transfer") {
    std::string sym, side_s;
    std::uint32_t from = 0, to = 0;
    std::int64_t lots = 0;
    if (!(ss >> sym >> from >> to >> side_s >> lots) || from == 0 || to == 0 ||
        from == to || lots <= 0 || (side_s != "long" && side_s != "short")) {
      logf("[ops] usage: transfer <symbol> <from_id> <to_id> long|short <lots>\n");
      return true;
    }
    const InstrumentId inst = site.table().find(sym);
    if (inst == kNoInstrument) {
      logf("[ops] transfer: `%s` is not hosted at this site\n", sym.c_str());
      return true;
    }
    PositionTransferCmd t{};
    t.ts = now;
    t.qty = lots;
    t.inst = inst;
    t.from_strategy = static_cast<StrategyId>(from);
    t.to_strategy = static_cast<StrategyId>(to);
    t.account = account;
    t.side = side_s == "long" ? Side::Buy : Side::Sell;
    site.source(t, now);
    logf("[ops] transfer: %s %s %lld lots strategy %u -> %u\n", sym.c_str(), side_s.c_str(),
         static_cast<long long>(lots), from, to);
    return true;
  }

  return false;
}

}  // namespace plat
