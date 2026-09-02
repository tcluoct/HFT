// platd_config.cpp — config-file → platform-struct mapping (see
// platd.example.cfg / platd.fleet.example.cfg for the key reference), plus
// the fleet-section machinery (`shardN.*` overlays and derived defaults).

#include <cstdio>
#include <cstring>

#include "plat/core/log.hpp"
#include "platd_app.hpp"
#include "strat_params.hpp"

using namespace plat;

namespace platd {

// The strategy/risk/fee param mapping lives in strat_params.hpp, shared with
// platbt so live and backtest build the identical strategies (§9); this file
// keeps only the platd-specific config (schedule, fleet sections).

// "HH:MM Phase, HH:MM Phase, ..." — falls back to the standard futures day
// session. Night sessions crossing midnight use 24h+ times (01:00 → 25:00).
PhaseSchedule schedule_from(const Config& c) {
  const auto items = c.list("phases");
  if (items.empty()) return futures_schedule();
  PhaseSchedule s;
  for (const auto& it : items) {
    int hh = 0, mm = 0;
    char name[32] = {};
    if (std::sscanf(it.c_str(), "%d:%d %31s", &hh, &mm, name) != 3) continue;
    Phase p = Phase::Continuous;
    if (std::strcmp(name, "OpenAuction") == 0) p = Phase::OpenAuction;
    else if (std::strcmp(name, "OpenAuctionLocked") == 0) p = Phase::OpenAuctionLocked;
    else if (std::strcmp(name, "Continuous") == 0) p = Phase::Continuous;
    else if (std::strcmp(name, "Break") == 0) p = Phase::Break;
    else if (std::strcmp(name, "CloseAuction") == 0) p = Phase::CloseAuction;
    else if (std::strcmp(name, "Closed") == 0) p = Phase::Closed;
    else continue;
    s.add(at_time(hh, mm), p);
  }
  return s.count ? s : futures_schedule();
}

bool require_keys(const Config& c) {
  static constexpr const char* kRequired[] = {
      "td_front", "md_front", "broker_id", "user_id", "password", "instruments"};
  bool ok = true;
  for (const char* k : kRequired) {
    // Present-but-EMPTY must fail too: the CTP api does not survive an empty
    // front address (null write inside the DLL), so catch it here with a
    // named key instead of there with a crash dump.
    if (c.str(k).empty()) {
      logf("config: missing required key `%s`\n", k);
      ok = false;
    }
  }
  return ok;
}

// full platd process — own core-pinned engine, own journal + replay drill,
// own CTP session (OrderRef scope is (FrontID, SessionID), so id spaces are
// disjoint) — which buys fault isolation: one shard crashing cannot take the
// fleet down. Assignment rule (§5 sharding): an instrument lives in exactly
// one shard; strategies follow their instruments.
//
// shard_config / fleet_shard_ids moved to fleet_cfg.hpp (shared with the
// platops web console, which mirrors every shard from the same fleet file).

void write_cmd_file(int shard, const std::string& cmd) {
  const std::string path = "shard" + std::to_string(shard) + ".cmd";
  if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(cmd.data(), 1, cmd.size(), f);
    std::fclose(f);
  }
}

}  // namespace platd
