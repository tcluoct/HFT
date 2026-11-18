// platd_positions.cpp — per-strategy overnight position carry (§9.5).
//
// The counter's position query is account-level; strategy attribution is
// OURS to keep. At EOD every strategy's surviving lots are written to
// `<journal>.positions`; the next fresh start reads yesterday's file and
// distributes the counter's carried totals per strategy. The counter is
// the authority on quantity — any attribution mismatch is logged loudly and
// unattributed carry locks trading pending reconciliation.

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
bool write_eod_positions(LiveSite& site, const InstrumentTable& table,
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
  if (!save_positions(jnl_path + ".positions", site.config().day, rows)) {
    logf("[platd] failed to persist EOD attribution: %s.positions\n", jnl_path.c_str());
    return false;
  }
  return true;
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

namespace {

// Carry-in is a journaled Source event, not a direct PnL poke: replay and the
// platops mirror rebuild the same carried book and cost basis from the log.
void source_carry_in(LiveSite& site, Nanos ts, AccountId account, StrategyId strategy,
                     InstrumentId inst, Qty stock_qty, Qty long_yd, Qty short_yd,
                     Price cost_px) {
  CarryInCmd c{};
  c.ts = ts;
  c.account = account;
  c.strategy = strategy;
  c.inst = inst;
  c.stock_qty = stock_qty;
  c.fut_long_yd = long_yd;
  c.fut_short_yd = short_yd;
  c.cost_px = cost_px;
  site.source(c, ts);
}

}  // namespace

// Fresh-start carry-in: the counter says how much is carried per
// (instrument, direction); yesterday's file says whose it is. Attribution
// rows are honored only while they fit inside the counter totals — including
// rows for strategies that are stopped this session (their book just sits,
// marked and margined, until re-enabled). Any remainder — manual trades, a
// lost file, another terminal on the account — blocks trading.
bool carry_in_positions(LiveSite& site, ctp::TraderClient& trader,
                        const InstrumentTable& table, AccountId account,
                        const std::vector<PositionsRow>& prev, Nanos ts) {
  const auto positions = trader.query_positions();
  if (!positions) return false;
  const auto open_orders = trader.query_account_open_orders();
  if (!open_orders) return false;
  for (const auto& order : *open_orders)
    if (table.find(order.symbol) != kNoInstrument) {
      logf("[platd] unmanaged open order on %s at fresh start; trading locked\n", order.symbol);
      return false;
    }
  return apply_carry(site, table, account, prev, *positions, ts);
}

bool apply_carry(LiveSite& site, const InstrumentTable& table, AccountId account,
                 std::vector<PositionsRow> unallocated,
                 const std::vector<ctp::PositionRow>& positions, Nanos ts) {
  bool complete = true;
  for (const auto& p : positions) {
    const InstrumentId id = table.find(p.symbol);
    if (id == kNoInstrument) continue;  // position in an unhosted instrument
    const Instrument& in = table.at(id);
    const Price counter_cost =
        p.total > 0 && in.multiplier > 0
            ? ctp::px_from_ctp(p.open_cost / (static_cast<double>(p.total) * in.multiplier))
            : 0;

    // Hand out this (instrument, direction)'s carried lots per attribution.
    Qty remaining = p.yd;
    for (auto& r : unallocated) {
      if (in.sym() != r.symbol) continue;
      Qty& lots = p.is_long ? r.long_lots : r.short_lots;
      if (lots <= 0) continue;
      const Qty take = lots < remaining ? lots : remaining;
      if (take <= 0) break;
      const Price cost = p.is_long ? r.long_cost_e4 : r.short_cost_e4;
      source_carry_in(site, ts, account, r.strategy, id, 0,
                      p.is_long ? take : 0, p.is_long ? 0 : take,
                      cost ? cost : counter_cost);
      logf("[platd] carry-in %s %s yd %lld -> strategy %u (cost %.1f)\n", p.symbol,
           p.is_long ? "long" : "short", static_cast<long long>(take), r.strategy,
           px_to_cny(cost ? cost : counter_cost));
      remaining -= take;
      lots -= take;
    }

    if (remaining > 0) {
      // Unattributed carry: manual trades or a missing/stale file.
      complete = false;
      source_carry_in(site, ts, account, 0, id, 0,
                      p.is_long ? remaining : 0, p.is_long ? 0 : remaining, counter_cost);
      logf("[platd] %s has %lld unattributed lots; trading locked\n",
           p.symbol, static_cast<long long>(remaining));
    }

    if (p.total != p.yd) {
      complete = false;
      logf("[platd] WARNING: %s has today-opened lots at the counter (%lld td); start "
           "platd before the session opens\n",
           p.symbol, static_cast<long long>(p.total - p.yd));
    }
  }
  for (const auto& row : unallocated)
    if (row.long_lots || row.short_lots) {
      logf("[platd] unmatched attribution for %s strategy %u; trading locked\n",
           row.symbol, row.strategy);
      complete = false;
    }
  return complete;
}

}  // namespace platd
