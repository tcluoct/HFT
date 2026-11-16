#pragma once
// desk/pack.hpp — the house desk's strategy lineup: the ONE place that names
// which strategy-team libraries this desk deploys, and in what pack order.
//
// A hedge fund runs many strategy teams; each team owns a static-library
// project under strategies/ (trend, fade, basis, cash) and only knows the
// strategy SDK. The desk is the composition root: it includes the team
// headers, appends their Defs to the StratPack, and exports the derived
// machinery every platform binary uses:
//
//   desk::Pack            the type list (one BANK per entry in a Site)
//   desk::PackSite<Gw>    the engine composed over that pack + a gateway
//   desk::parse_strategies / desk::kind_name   config-facing helpers
//
// Building a different desk (futures-only, a single team's paper book, a
// research build) is a new header in this project, not a platform change: a
// desk is a static library that links the team libs it fields and exports
// its own Pack. The platform engine and its apps never include a strategy
// header; they link a desk.
//
// APPEND ONLY, NEVER REORDER: banks are dispatched, saved and hashed in pack
// order (engine/site_state.hpp), so reordering breaks snapshot/replay
// compatibility. The id is the durable identity (risk scopes, kill targets,
// PnL, carry) — never reuse one for a different purpose.
//
// Strategies are configured as named INSTANCES:
//
//   strategies   = fast_cu, slow_al, brk        # instance names
//   fast_cu.type = momentum                     # any Pack type name
//   fast_cu.id   = 1                            # required, unique, nonzero
//   fast_cu.instruments = cu2609                # defaults to global `instruments`
//   fast_cu.lots = 2                            # type params under the instance name
//
// so one site can run any subset of the desk's strategy types, several
// instances of the same type with different params/universes, and different
// sites (or fleet shards via shardN.*) freely differ. `<name>.enabled = 0`
// stops an instance without deleting its block: it is simply not constructed
// next session, while its StrategyId stays reserved so PnL attribution and
// the overnight .positions carry chain survive until it is switched back on.
//
// Without a `strategies` key the classic trio (momentum=1, meanrev=2,
// breakout=3) is assumed, so existing configs keep working unchanged.
//
// ADDING A STRATEGY TYPE to this desk is a two-line change here:
//   1. #include the team header that carries the strategy + its Def, and
//   2. append the Def to the Pack list below (append only).
// Parsing, bank building, Site composition, platd, platbt and --replay are
// all derived from this list — nothing else changes.

#include <cstdint>
#include <vector>

#include "plat/core/config.hpp"
#include "plat/core/types.hpp"
#include "plat/engine/pack.hpp"

#include "strat/fade/meanrev.hpp"
#include "strat/trend/breakout.hpp"
#include "strat/trend/momentum.hpp"

namespace desk {

using namespace plat;

// The strategy types this desk deploys — THE one list (append only, never
// reorder; see the header comment). Each type is a BANK in the Site: zero or
// more configured instances.
using Pack = StratPack<MomentumDef, MeanRevDef, BreakoutDef>;

// The pack composed over any gateway (CTP counter, backtest simulator, …).
template <typename Gateway>
using PackSite = Pack::SiteFor<Gateway>;

inline const char* kind_name(std::uint8_t kind) noexcept { return Pack::kind_name(kind); }

// Parse + validate the instance list (fail closed — see StratPack::parse).
inline bool parse_strategies(const Config& c, AccountId default_account,
                             std::vector<StratSpec>& out) {
  return Pack::parse(c, default_account, out);
}

}  // namespace desk
