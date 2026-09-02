#pragma once
// plat/ref/positions_file.hpp — the EOD position-attribution file
// (`<journal>.positions`). The counter's position query is ACCOUNT-level;
// which strategy owns which lots is knowledge only this platform has, so it
// is written down at end of day and read back at the next fresh start,
// where it distributes the counter's carried totals per strategy (§9.5).
// The counter stays the authority on HOW MUCH is carried; this file only
// says WHOSE it is — any disagreement is reconciled loudly in favor of the
// counter.
//
// Format: human-readable text, one row per (strategy, instrument, side):
//   day 20260714
//   pos <strategy_id> <symbol> <long_lots> <short_lots> <long_cost_e4> <short_cost_e4>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "plat/core/types.hpp"

namespace plat {

struct PositionsRow {
  StrategyId strategy = 0;
  char symbol[32]{};
  Qty long_lots = 0;
  Qty short_lots = 0;
  Price long_cost_e4 = 0;   // avg open px, 1e-4 CNY
  Price short_cost_e4 = 0;
};

inline bool save_positions(const std::string& path, std::uint32_t day,
                           const std::vector<PositionsRow>& rows) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "day %u\n", day);
  for (const auto& r : rows) {
    std::fprintf(f, "pos %u %s %lld %lld %lld %lld\n", r.strategy, r.symbol,
                 static_cast<long long>(r.long_lots), static_cast<long long>(r.short_lots),
                 static_cast<long long>(r.long_cost_e4),
                 static_cast<long long>(r.short_cost_e4));
  }
  std::fclose(f);
  return true;
}

inline bool load_positions(const std::string& path, std::uint32_t& day,
                           std::vector<PositionsRow>& rows) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  day = 0;
  rows.clear();
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    unsigned u = 0;
    if (std::sscanf(line, "day %u", &u) == 1) {
      day = u;
      continue;
    }
    PositionsRow r{};
    long long lg = 0, sh = 0, lc = 0, sc = 0;
    unsigned sid = 0;
    if (std::sscanf(line, "pos %u %31s %lld %lld %lld %lld", &sid, r.symbol, &lg, &sh, &lc,
                    &sc) == 6) {
      r.strategy = static_cast<StrategyId>(sid);
      r.long_lots = lg;
      r.short_lots = sh;
      r.long_cost_e4 = lc;
      r.short_cost_e4 = sc;
      if (r.long_lots > 0 || r.short_lots > 0) rows.push_back(r);
    }
  }
  std::fclose(f);
  return true;
}

}  // namespace plat
