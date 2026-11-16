#pragma once
// platd_app.hpp — the daemon's app layer, shared by its translation units:
//
//   platd_main.cpp    command-line dispatch only
//   platd_config.cpp  config-file → platform-struct mapping, fleet sections
//   platd_live.cpp    the live/resume session (run_live)
//   platd_fleet.cpp   the multi-shard supervisor (run_fleet)
//   platd_tools.cpp   offline tools: --check, --replay
//
// Everything here is cold-path composition; the hot path lives entirely in
// the plat:: library headers.

#include <string>
#include <vector>

#include "plat/core/config.hpp"
#include "plat/core/phase.hpp"
#include "plat/core/types.hpp"
#include "plat/ctp/trader_client.hpp"
#include "plat/engine/params.hpp"
#include "plat/engine/site.hpp"
#include "plat/ref/instrument.hpp"
#include "plat/ref/positions_file.hpp"
#include "desk/pack.hpp"     // the deployed strategy lineup (composition root)
#include "fleet_cfg.hpp"     // shardN.* overlay machinery (also used by platops)

namespace platd {

// The desk's strategy lineup (Pack) and the platform risk/fee param mapping
// (limits_from, fees_from) — the same definitions the backtester and ops
// mirror use, so live/backtest/mirror can never diverge. The platform engine
// itself ships no strategies: this daemon is one composition of one desk.
using namespace platcfg;
using namespace desk;

// The composition this daemon ships (§9.1 inline mode): the desk's strategy
// type pack (desk/pack.hpp) over the CTP counter gateway. Which instances
// of those types actually run — how many, with what ids/params/universes —
// is config (`strategies = ...`), parsed by desk::parse_strategies so live
// and backtest are built from the identical definitions. (StratPack itself
// asserts every pack member is CRTP — no vtable on the hot path.)
using LiveSite = PackSite<plat::ctp::TraderClient>;

// ---- platd-specific config (platd_config.cpp; the shared params come from
// plat/engine/params.hpp + desk/pack.hpp above; shard_config /
// fleet_shard_ids from fleet_cfg.hpp, shared with platops)
plat::PhaseSchedule schedule_from(const plat::Config& c);
bool require_keys(const plat::Config& c);
// Exchange trading-day calendar (optional `trade_calendar` file: one
// `yyyymmdd` or `yyyymmdd-yyyymmdd` per line, `#` comments). A missing file
// is an error when the key is set; an empty key means "no calendar".
bool is_trading_day(const std::string& path, std::uint32_t day);
void write_cmd_file(int shard, const std::string& cmd);

// ---- per-strategy overnight carry (platd_positions.cpp): the counter
// knows how much is carried; yesterday's `.positions` file knows whose.
// The EOD write walks the PnL ledger itself, so a strategy that is stopped
// (`<name>.enabled = 0`) keeps its carried lots flowing through the chain
// until it is re-enabled or manually flattened.
void write_eod_positions(LiveSite& site, const plat::InstrumentTable& table,
                         const std::string& jnl_path);
std::vector<plat::PositionsRow> read_prev_positions(const std::string& jnl_path,
                                                    std::uint32_t expect_day);
void carry_in_positions(LiveSite& site, plat::ctp::TraderClient& trader,
                        const plat::InstrumentTable& table, plat::AccountId account,
                        const std::vector<plat::PositionsRow>& prev,
                        const std::vector<StratSpec>& specs, plat::Nanos now);

// ---- entry points
int run_live(const plat::Config& cfg, bool force_resume, const std::string& cfg_path,
             int shard);                                                    // platd_live.cpp
int run_fleet(const std::string& cfg_path);                        // platd_fleet.cpp
int run_check(const plat::Config& cfg);                            // platd_tools.cpp
int run_replay(const plat::Config& cfg, const std::string& jnl_path);

}  // namespace platd
