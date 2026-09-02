#pragma once
// plat/replay/journal_dump.hpp — human-readable journal listing: one line
// per envelope — sequence, session time, Source/Derived, schema, and the
// payload fields an operator actually reads. The `.ref` sidecar (when
// present) resolves instrument ids to symbols. Works on sealed and crashed
// (unsealed) journals alike; this is `platd --dump`.

#include <cstdio>
#include <string>

#include "plat/bus/journal.hpp"
#include "plat/core/log.hpp"
#include "plat/core/time.hpp"
#include "plat/ref/instrument.hpp"
#include "plat/replay/sidecar.hpp"

namespace plat {

inline const char* schema_name(Schema s) {
  switch (s) {
    case Schema::MdOrder:      return "MdOrder";
    case Schema::MdTrade:      return "MdTrade";
    case Schema::MdSnapshot:   return "MdSnap";
    case Schema::BookTop:      return "BookTop";
    case Schema::PhaseChange:  return "Phase";
    case Schema::NewOrderReq:  return "NewOrder";
    case Schema::CancelReq:    return "Cancel";
    case Schema::ExecReport:   return "Exec";
    case Schema::RiskDeny:     return "RiskDeny";
    case Schema::HedgeIntent:  return "HedgeIntent";
    case Schema::BasisSummary: return "Basis";
    case Schema::KillSwitch:   return "Kill";
    case Schema::SessionState: return "Session";
    case Schema::Timer:        return "Timer";
    case Schema::HedgeDone:    return "HedgeDone";
    case Schema::FeedGap:      return "FeedGap";
  }
  return "?";
}

inline void dump_envelope(const Envelope& e, const InstrumentTable* table) {
  const auto sym = [&](InstrumentId i) -> const char* {
    return table && i < table->size() ? table->at(i).symbol : "?";
  };
  const auto tp = time_parts(e.ts);
  std::printf("%8llu %02d:%02d:%02d.%03d %c %-9s ",
              static_cast<unsigned long long>(e.seq), tp.hh, tp.mm, tp.ss, tp.ms,
              e.origin == Origin::Source ? 'S' : 'D', schema_name(e.schema));
  switch (e.schema) {
    case Schema::MdSnapshot: {
      const auto& m = e.as<MdSnapshotEvt>();
      std::printf("%-8s bid %.1f x%lld  ask %.1f x%lld  last %.1f  vol %lld  oi %lld",
                  sym(m.inst), px_to_cny(m.bid_px[0]), static_cast<long long>(m.bid_qty[0]),
                  px_to_cny(m.ask_px[0]), static_cast<long long>(m.ask_qty[0]),
                  px_to_cny(m.last_px), static_cast<long long>(m.day_volume),
                  static_cast<long long>(m.open_interest));
      break;
    }
    case Schema::NewOrderReq: {
      const auto& r = e.as<NewOrderReq>();
      std::printf("%-8s %s %s %lld @ %.1f %s  cl %llx  strat %u", sym(r.inst),
                  std::string(side_name(r.side)).c_str(),
                  r.offset == Offset::Open ? "Open" : "Close",
                  static_cast<long long>(r.qty), px_to_cny(r.px),
                  r.tif == Tif::Ioc ? "IOC" : "Day",
                  static_cast<unsigned long long>(r.cl_id), r.strategy);
      break;
    }
    case Schema::CancelReq: {
      const auto& c = e.as<CancelReq>();
      std::printf("%-8s cl %llx  strat %u", sym(c.inst),
                  static_cast<unsigned long long>(c.cl_id), c.strategy);
      break;
    }
    case Schema::ExecReport: {
      const auto& r = e.as<ExecReport>();
      static constexpr const char* kType[] = {"Ack", "Reject", "Fill", "Canceled", "CxlRej"};
      std::printf("%-8s %-8s %s %lld @ %.1f  cum %lld leaves %lld  cl %llx  strat %u",
                  sym(r.inst), kType[static_cast<int>(r.type)],
                  std::string(side_name(r.side)).c_str(), static_cast<long long>(r.qty),
                  px_to_cny(r.px), static_cast<long long>(r.cum),
                  static_cast<long long>(r.leaves),
                  static_cast<unsigned long long>(r.cl_id), r.strategy);
      break;
    }
    case Schema::RiskDeny: {
      const auto& d = e.as<RiskDeny>();
      std::printf("%-8s check #%d %s cl %llx  strat %u", sym(d.inst),
                  static_cast<int>(d.check), d.is_cancel ? "(cancel)" : "",
                  static_cast<unsigned long long>(d.cl_id), d.strategy);
      break;
    }
    case Schema::PhaseChange: {
      const auto& p = e.as<PhaseChangeEvt>();
      std::printf("%s -> %s", std::string(venue_name(p.venue)).c_str(),
                  std::string(phase_name(p.phase)).c_str());
      break;
    }
    case Schema::KillSwitch: {
      const auto& k = e.as<KillSwitchCmd>();
      static constexpr const char* kScope[] = {"Strategy", "Account", "Site", "Firm"};
      static constexpr const char* kAction[] = {"Halt", "Flatten", "Disarm"};
      std::printf("%s %s target %u  cmd_seq %llu", kScope[static_cast<int>(k.scope)],
                  kAction[static_cast<int>(k.action)], k.target,
                  static_cast<unsigned long long>(k.cmd_seq));
      break;
    }
    case Schema::SessionState: {
      static constexpr const char* kSess[] = {"Down", "LoggingIn", "Up", "Reconciling"};
      std::printf("%s", kSess[static_cast<int>(e.as<SessionStateEvt>().state)]);
      break;
    }
    case Schema::Timer: {
      const auto& t = e.as<TimerEvt>();
      std::printf("token %llu  strat %u", static_cast<unsigned long long>(t.token),
                  t.strategy);
      break;
    }
    case Schema::FeedGap:
      std::printf("lost %llu", static_cast<unsigned long long>(e.as<FeedGapEvt>().lost));
      break;
    case Schema::BookTop: {
      const auto& b = e.as<BookTopEvt>();
      std::printf("%-8s bid %.1f x%lld  ask %.1f x%lld", sym(b.inst), px_to_cny(b.bid_px),
                  static_cast<long long>(b.bid_qty), px_to_cny(b.ask_px),
                  static_cast<long long>(b.ask_qty));
      break;
    }
    default:
      break;  // remote/stock messages: header line suffices
  }
  std::printf("\n");
}

inline int dump_journal(const std::string& jnl_path, std::uint64_t limit) {
  JournalReader jr;
  if (!jr.open(jnl_path)) {
    logf("dump: cannot open journal %s\n", jnl_path.c_str());
    return 1;
  }
  InstrumentTable table;
  const bool have_ref = load_sidecar(jnl_path, table, /*quiet=*/true);
  std::printf("journal %s: day %u site %u, %llu envelopes, %s%s\n", jnl_path.c_str(),
              jr.header().day, jr.header().site,
              static_cast<unsigned long long>(jr.header().count),
              jr.sealed() ? "sealed" : "UNSEALED (crashed session)",
              jr.sealed() ? (jr.verified() ? ", checksum OK" : ", checksum FAILED") : "");
  std::printf("%8s %-12s %s %-9s payload\n", "seq", "time", "o", "schema");
  std::uint64_t shown = 0;
  for (const Envelope& e : jr.envelopes()) {
    if (limit && shown++ >= limit) {
      std::printf("... (%llu more; pass a larger count)\n",
                  static_cast<unsigned long long>(jr.envelopes().size() - limit));
      break;
    }
    dump_envelope(e, have_ref ? &table : nullptr);
  }
  return jr.sealed() && !jr.verified() ? 1 : 0;
}

}  // namespace plat
