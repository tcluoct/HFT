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
#include "plat/engine/site.hpp"
#include "plat/ref/instrument.hpp"
#include "plat/ref/positions_file.hpp"
#include "fleet_cfg.hpp"     // shardN.* overlay machinery (also used by platops)
#include "strat_params.hpp"  // shared strategy registry + Config→params mapping (also used by platbt)

namespace platd {

// Shared strategy registry (Pack) and risk/fee param mapping (limits_from,
// fees_from) — the same definitions the backtester uses, so live and
// backtest can never diverge. Pulled into this namespace so platd code calls
// them unqualified.
using namespace platcfg;

// The composition this daemon ships (§9.1 inline mode): the shared strategy
// type pack (strat_params.hpp) over the CTP counter gateway. Which instances
// of those types actually run — how many, with what ids/params/universes —
// is config (`strategies = ...`), parsed by platcfg::parse_strategies so
// live and backtest are built from the identical definitions. (StratPack
// itself asserts every pack member is CRTP — no vtable on the hot path.)
using LiveSite = PackSite<plat::ctp::TraderClient>;

// ---- platd-specific config (platd_config.cpp; the shared params come from
// strat_params.hpp / platcfg above; shard_config / fleet_shard_ids from
// fleet_cfg.hpp, shared with platops)
plat::PhaseSchedule schedule_from(const plat::Config& c);
bool require_keys(const plat::Config& c);
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
                        const std::vector<StratSpec>& specs);

// ---- entry points
int run_live(const plat::Config& cfg, bool force_resume = false);  // platd_live.cpp
int run_fleet(const std::string& cfg_path);                        // platd_fleet.cpp
int run_check(const plat::Config& cfg);                            // platd_tools.cpp
int run_replay(const plat::Config& cfg, const std::string& jnl_path);

}  // namespace platd
