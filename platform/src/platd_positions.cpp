// platd_positions.cpp — per-strategy overnight position carry (§9.5).
//
// The counter's position query is account-level; strategy attribution is
// OURS to keep. At EOD every strategy's surviving lots are written to
// `<journal>.positions`; the next fresh start reads yesterday's file and
// distributes the counter's carried totals per strategy. The counter is
// the authority on quantity — any attribution mismatch is logged loudly and
// the unattributed remainder falls back to the momentum book, exactly the
// pre-file behavior.

#include <cstring>
#include <string>
#include <vector>

#include "plat/core/log.hpp"
#include "plat/ref/positions_file.hpp"
#include "platd_app.hpp"

using namespace plat;

namespace platd {

// Snapshot every strategy's carried lots at EOD. Today's lots become
// yesterday's at the venue roll, so total (yd + td) at close is tomorrow's
// carry-in per strategy. This walks the LEDGER, not the configured strategy
// list: a stopped strategy's carried lots must keep flowing through the
// .positions chain until it is re-enabled or flattened.
void write_eod_positions(LiveSite& site, const InstrumentTable& table,
                         const std::string& jnl_path) {
  std::vector<PositionsRow> rows;
  site.pnl().for_each([&](const PosKey& k, const Position& p) {
    PositionsRow r{};
    r.strategy = k.strategy;
    std::snprintf(r.symbol, sizeof(r.symbol), "%s", table.at(k.inst).symbol);
    r.long_lots = p.long_yd + p.long_td;
    r.short_lots = p.short_yd + p.short_td;
    r.long_cost_e4 = p.long_cost_e4;
    r.short_cost_e4 = p.short_cost_e4;
    if (r.long_lots > 0 || r.short_lots > 0) rows.push_back(r);
  });
  if (save_positions(jnl_path + ".positions", site.config().day, rows))
    logf("[platd] EOD positions written: %zu attributed rows -> %s.positions\n", rows.size(),
         jnl_path.c_str());
}

// Read yesterday's attribution (called BEFORE the sealed journal is
// archived). `expect_day` is the sealed journal's day — a file from any
// other day is stale and ignored.
std::vector<PositionsRow> read_prev_positions(const std::string& jnl_path,
                                              std::uint32_t expect_day) {
  std::vector<PositionsRow> rows;
  std::uint32_t day = 0;
  if (!load_positions(jnl_path + ".positions", day, rows)) return {};
  if (day != expect_day) {
    logf("[platd] WARNING: %s.positions is from day %u (journal day %u) — stale, ignored\n",
         jnl_path.c_str(), day, expect_day);
    return {};
  }
  return rows;
}

// Fresh-start carry-in: the counter says how much is carried per
// (instrument, direction); yesterday's file says whose it is. Attribution
// rows are honored only while they fit inside the counter totals — including
// rows for strategies that are stopped this session (their book just sits,
// marked and margined, until re-enabled). Any remainder — manual trades, a
// lost file, another terminal on the account — lands on the first configured
// instance's book with a loud warning (id 0 = unattributed if none run).
void carry_in_positions(LiveSite& site, ctp::TraderClient& trader,
                        const InstrumentTable& table, AccountId account,
                        const std::vector<PositionsRow>& prev,
                        const std::vector<StratSpec>& specs) {
  const StrategyId fallback = specs.empty() ? StrategyId{0} : specs.front().id;
  const auto account_for = [&](StrategyId sid) {
    for (const auto& s : specs)
      if (s.id == sid) return s.account;
    return account;
  };
  for (const auto& p : trader.query_positions()) {
    const InstrumentId id = table.find(p.symbol);
    if (id == kNoInstrument) continue;  // position in an unhosted instrument
    const Instrument& in = table.at(id);
    const Price counter_cost =
        p.total > 0 && in.multiplier > 0
            ? ctp::px_from_ctp(p.open_cost / (static_cast<double>(p.total) * in.multiplier))
            : 0;

    // Hand out this (instrument, direction)'s carried lots per attribution.
    Qty remaining = p.yd;
    for (const auto& r : prev) {
      if (in.sym() != r.symbol) continue;
      const Qty lots = p.is_long ? r.long_lots : r.short_lots;
      if (lots <= 0) continue;
      const Qty take = lots < remaining ? lots : remaining;
      if (take <= 0) break;
      const Price cost = p.is_long ? r.long_cost_e4 : r.short_cost_e4;
      site.pnl().carry_in(account_for(r.strategy), r.strategy, id, 0, p.is_long ? take : 0,
                          p.is_long ? 0 : take, cost ? cost : counter_cost);
      logf("[platd] carry-in %s %s yd %lld -> strategy %u (cost %.1f)\n", p.symbol,
           p.is_long ? "long" : "short", static_cast<long long>(take), r.strategy,
           px_to_cny(cost ? cost : counter_cost));
      remaining -= take;
      if (lots > take)
        logf("[platd] WARNING: %s attribution for strategy %u exceeds counter carry "
             "(%lld > available) — trimmed to the counter's truth\n",
             p.symbol, r.strategy, static_cast<long long>(lots));
    }

    if (remaining > 0) {
      // Unattributed carry: manual trades or a missing/stale file.
      site.pnl().carry_in(account_for(fallback), fallback, id, 0, p.is_long ? remaining : 0,
                          p.is_long ? 0 : remaining, counter_cost);
      if (!prev.empty())
        logf("[platd] WARNING: %s %s has %lld unattributed carried lots — assigned to "
             "strategy %u; review yesterday's .positions file\n",
             p.symbol, p.is_long ? "long" : "short", static_cast<long long>(remaining),
             fallback);
      else
        logf("[platd] carry-in %s %s yd %lld -> strategy %u (no attribution file)\n",
             p.symbol, p.is_long ? "long" : "short", static_cast<long long>(remaining),
             fallback);
    }

    if (p.total != p.yd)
      logf("[platd] WARNING: %s has today-opened lots at the counter (%lld td); start "
           "platd before the session opens\n",
           p.symbol, static_cast<long long>(p.total - p.yd));
  }
}

}  // namespace platd
