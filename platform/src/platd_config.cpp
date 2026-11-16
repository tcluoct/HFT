// platd_config.cpp — config-file → platform-struct mapping (see
// platd.example.cfg / platd.fleet.example.cfg for the key reference), plus
// the fleet-section machinery (`shardN.*` overlays and derived defaults).

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "plat/core/log.hpp"
#include "platd_app.hpp"

using namespace plat;

namespace platd {

// Risk/fee config mapping lives in plat/engine/params.hpp (owned by the
// platform), the strategy lineup in desk/pack.hpp (owned by the desk both
// platd and platbt link) — neither is duplicated here. This file keeps only
// the platd-specific config (schedule, fleet sections).

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

bool is_trading_day(const std::string& path, std::uint32_t day) {
  std::ifstream f(path);
  if (!f) {
    logf("[platd] trade_calendar `%s` cannot be opened — refusing to guess\n", path.c_str());
    return false;
  }
  std::string line;
  bool listed = false;
  while (std::getline(f, line)) {
    const auto hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    const auto b = line.find_first_not_of(" \t\r");
    if (b == std::string::npos) continue;
    const auto e = line.find_last_not_of(" \t\r");
    line = line.substr(b, e - b + 1);
    const auto dash = line.find('-');
    const std::string lhs = line.substr(0, dash);
    const std::string rhs = dash == std::string::npos ? lhs : line.substr(dash + 1);
    std::uint32_t lo = 0, hi = 0;
    if (lhs.size() != 8 || rhs.size() != 8) {
      logf("[platd] trade_calendar: expected yyyymmdd or yyyymmdd-yyyymmdd, got `%s`\n",
           line.c_str());
      continue;
    }
    try {
      lo = static_cast<std::uint32_t>(std::stoul(lhs));
      hi = static_cast<std::uint32_t>(std::stoul(rhs));
    } catch (...) {
      logf("[platd] trade_calendar: ignoring malformed line `%s`\n", line.c_str());
      continue;
    }
    if (day >= lo && day <= hi) listed = true;
  }
  if (!listed)
    logf("[platd] %u is NOT a trading day per %s — refusing to start\n", day, path.c_str());
  return listed;
}

bool require_keys(const Config& c) {
  static constexpr const char* kRequired[] = {
      "td_front", "md_front", "broker_id", "user_id", "instruments"};
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
  // Credentials may come from the environment or a permissioned file; accept
  // any of `password`, `password_env`, `password_file`.
  if (resolve_secret(c, "password").empty()) {
    logf("config: missing `password` (or password_env/password_file)\n");
    ok = false;
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
