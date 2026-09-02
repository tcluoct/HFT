// platbt — the strategy backtester. Replays recorded .mdr market data
// through the EXACT SAME engine and strategies platd runs live (§13.2):
// Site<BacktestGateway, SnapMomentum, MeanRevert, Breakout>, the 12-check
// riskgate, the same books, the same PnL/fee library. Only the venue is
// modelled (backtest_gateway.hpp) instead of connected — so a backtest can
// never diverge from live behaviour, because it is the same code.
//
//   platbt <config.cfg> [--from YYYYMMDD --to YYYYMMDD] [--dir DIR] [--out FILE]
//   platbt <config.cfg> <day1.mdr> [day2.mdr ...]
//
// With --from/--to (or a bt.from/bt.to config), platbt scans the data
// directory and selects one .mdr per trading day in the range (preferring a
// --compare golden file, else the most complete recording). Each day is run
// independently with a fresh Site (as a fresh platd process would each
// morning): the intraday strategies flatten by close, so per-day PnL sums to
// the total. It writes a self-contained HTML performance report (equity
// curve, drawdown, Sharpe, drawdown, trade count, fees, per-strategy split).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "plat/core/config.hpp"
#include "plat/core/log.hpp"
#include "plat/core/thread.hpp"
#include "plat/core/time.hpp"
#include "plat/ctp/conv.hpp"  // ctp::venue_from_exchange_id (typedefs only, no lib)
#include "plat/rec/backtest_gateway.hpp"
#include "plat/rec/backtest_report.hpp"
#include "plat/rec/tick_file.hpp"
#include "strat_params.hpp"  // the shared strategy registry: Pack, PackSite, param maps

using namespace plat;
using namespace plat::rec;
using namespace platcfg;

namespace {

// The backtest runs the SAME strategy pack as platd's LiveSite (defined once
// in strat_params.hpp) — only the gateway is a fill simulator, not a counter.
using BtSite = PackSite<BacktestGateway>;

// Build the per-day instrument table from the .mdr directory. Reference data
// (tick / multiplier / margin) is recorded in the file (§9); prev_close and
// the price-limit band are seeded from each instrument's first tick, so no
// live counter is needed.
InstrumentTable table_from_dir(const std::vector<MdrInstrument>& dir) {
  InstrumentTable t;
  for (const auto& d : dir) {
    const Venue venue = ctp::venue_from_exchange_id(d.exchange);
    t.add(d.symbol, venue, Board::Future, d.price_tick > 0 ? d.price_tick : px_from_cny(1.0),
          1, d.multiplier > 0 ? d.multiplier : 1, /*prev_close=*/0,
          d.margin_bps ? d.margin_bps : 1'200);
  }
  return t;
}

MdSnapshotEvt to_snapshot(const MdRecord& r, std::uint64_t feed_seq) {
  MdSnapshotEvt s{};
  s.feed_seq = feed_seq;
  s.ts = r.exch_ts;
  for (int i = 0; i < kMaxLevels; ++i) {
    s.bid_px[i] = r.bid_px[i];
    s.bid_qty[i] = r.bid_qty[i];
    s.ask_px[i] = r.ask_px[i];
    s.ask_qty[i] = r.ask_qty[i];
  }
  s.last_px = r.last_px;
  s.day_volume = r.day_volume;
  s.open_interest = r.open_interest;
  s.inst = r.inst;
  s.venue = Venue::SHFE;  // cosmetic; the table's venue drives risk
  return s;
}

// Replay one recorded day through a fresh Site; fill `out` with the realised
// per-strategy PnL, fees and fill count.
bool run_day(const Config& cfg, const std::vector<StratSpec>& specs, const std::string& path,
             DayResult& out) {
  TickReader r;
  if (!r.open(path)) {
    logf("[platbt] cannot open %s\n", path.c_str());
    return false;
  }

  InstrumentTable table = table_from_dir(r.dir());
  SiteConfig sc;
  sc.name = "backtest";
  sc.day = r.header().day;
  sc.hosted.clear();
  for (InstrumentId i = 0; i < table.size(); ++i) sc.hosted.push_back(i);
  if (table.size()) sc.venue = table.at(0).venue;
  sc.limits = limits_from(cfg);
  sc.fees = fees_from(cfg);

  BtSite site(table, sc, BtSite::Mode::Live, Pack::banks(cfg, table, specs));
  BacktestGateway gw(table.size());
  site.set_gateway(&gw);

  // Session up + continuous so strategies go live.
  SessionStateEvt up{};
  up.state = SessState::Up;
  up.ts = at_time(8, 55);
  site.source(up, up.ts);
  site.arm_session(at_time(8, 55));
  PhaseChangeEvt ph{};
  ph.venue = sc.venue;
  ph.phase = Phase::Continuous;
  ph.ts = at_time(9, 0);
  site.source(ph, ph.ts);

  std::vector<std::uint64_t> feed_seq(table.size(), 0);
  std::vector<std::uint8_t> primed(table.size(), 0);

  MdRecord rec;
  while (r.next(rec)) {
    if (rec.inst >= table.size()) continue;
    // Seed prev_close + a generous price-limit band from the first tick, so
    // the collar/limit checks pass for at-market orders (the backtest tests
    // strategy behaviour, not exchange limits).
    if (!primed[rec.inst] && rec.last_px > 0) {
      const Price band = rec.last_px / 5;  // ±20%
      table.set_prev_close(rec.inst, rec.last_px);
      table.set_limit_band(rec.inst, rec.last_px + band, rec.last_px - band);
      primed[rec.inst] = 1;
    }
    gw.set_book(rec.inst, rec.bid_px[0], rec.bid_qty[0], rec.ask_px[0], rec.ask_qty[0]);
    site.source(to_snapshot(rec, ++feed_seq[rec.inst]), rec.exch_ts);
    // Drain modelled fills back through the same Source path the live driver
    // uses, so OMS/PnL/strategies see them identically.
    if (!gw.drain().empty()) {
      auto reports = gw.drain();
      gw.clear();
      for (const auto& e : reports) site.source(e, e.ts ? e.ts : rec.exch_ts);
    }
  }

  // Integrity verdict — meaningful only now that every record was read.
  if (r.sealed() && !r.verified())
    logf("[platbt] WARNING: %s failed its checksum — results may be unreliable\n", path.c_str());
  else if (!r.sealed())
    logf("[platbt] note: %s is unsealed (crashed recording) — used %llu recovered ticks\n",
         path.c_str(), static_cast<unsigned long long>(r.decoded()));

  out.day = r.header().day;
  out.strat_pnl.clear();
  out.fees = 0;
  for (const auto& s : specs) {
    out.strat_pnl.push_back(px_to_cny(site.pnl().total_realized_e4(s.id)));
    out.fees += px_to_cny(site.pnl().total_fees_e4(s.id));
  }
  out.fills = site.tel().get("fills");
  logf("[platbt] %s (day %u): %llu ticks, %llu fills | net %.2f CNY\n", path.c_str(),
       out.day, static_cast<unsigned long long>(r.decoded()),
       static_cast<unsigned long long>(out.fills), out.net());
  return true;
}

// One .mdr file's identity, read cheaply from its header (no streaming).
struct Candidate {
  std::string path, source;
  std::uint32_t day = 0;
  std::uint64_t records = 0;
  bool sealed = false;
};

bool probe(const std::string& path, Candidate& c) {
  TickReader r;
  if (!r.open(path)) return false;
  c.path = path;
  c.day = r.header().day;
  c.source = r.header().source;
  c.records = r.header().record_count;
  c.sealed = r.sealed();
  return true;
}

// A golden merge (platrec --compare) is the canonical, gap-filled day; else
// prefer the sealed file with the most records.
bool better(const Candidate& a, const Candidate& b) {
  const bool ag = a.source.find("golden") != std::string::npos;
  const bool bg = b.source.find("golden") != std::string::npos;
  if (ag != bg) return ag;
  if (a.sealed != b.sealed) return a.sealed;
  return a.records > b.records;
}

// Select one file per trading day in [from,to], sorted ascending. `paths` may
// be an explicit file list or a directory scan.
std::vector<std::string> select_days(const std::vector<std::string>& paths, std::uint32_t from,
                                     std::uint32_t to) {
  std::map<std::uint32_t, Candidate> best;
  for (const auto& p : paths) {
    Candidate c;
    if (!probe(p, c)) {
      logf("[platbt] skipping unreadable %s\n", p.c_str());
      continue;
    }
    if (c.day < from || c.day > to) continue;
    auto it = best.find(c.day);
    if (it == best.end() || better(c, it->second)) best[c.day] = c;
  }
  std::vector<std::string> out;
  for (const auto& [day, c] : best) out.push_back(c.path);
  return out;  // std::map already orders by day
}

std::vector<std::string> scan_dir(const std::string& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (!e.is_regular_file()) continue;
    if (e.path().extension() == ".mdr") out.push_back(e.path().string());
  }
  if (ec) logf("[platbt] cannot scan directory %s\n", dir.c_str());
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  init_console_utf8();
  if (argc < 2) {
    logf("usage: platbt <config.cfg> [--from YYYYMMDD --to YYYYMMDD] [--dir DIR] [--out FILE]\n"
         "       platbt <config.cfg> <day1.mdr> [day2.mdr ...]\n"
         "  Backtests the platd strategies over recorded market data and writes an\n"
         "  HTML performance report. Config uses the same `strategies = ...` instance\n"
         "  blocks and risk.*/fees.* keys as platd, plus bt.capital_cny/bt.from/bt.to/\n"
         "  bt.dir/bt.out.\n");
    return 1;
  }
  Config cfg;
  if (!cfg.load(argv[1])) {
    logf("[platbt] cannot open config %s\n", argv[1]);
    return 1;
  }

  // Strategy lineup: the SAME instance-list parsing platd uses, so a backtest
  // exercises exactly the composition (types, ids, params, universes) that
  // would run live under this config.
  const auto account = static_cast<AccountId>(cfg.i64("account_id", 1));
  std::vector<StratSpec> specs;
  if (!parse_strategies(cfg, account, specs)) {
    logf("[platbt] strategy configuration is invalid\n");
    return 1;
  }
  if (specs.empty()) {
    logf("[platbt] no enabled strategies — nothing to backtest\n");
    return 1;
  }
  for (const auto& s : specs)
    logf("[platbt] strategy `%s`: %s id %u\n", s.name.c_str(), kind_name(s.kind), s.id);

  // ---- argument / config parsing ----
  std::uint32_t from = static_cast<std::uint32_t>(cfg.i64("bt.from", 0));
  std::uint32_t to = static_cast<std::uint32_t>(cfg.i64("bt.to", 99999999));
  std::string dir = cfg.str("bt.dir", "");
  std::string out = cfg.str("bt.out", "backtest_report.html");
  std::vector<std::string> files;  // explicit .mdr args, if any
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--from" && i + 1 < argc) from = static_cast<std::uint32_t>(std::atoll(argv[++i]));
    else if (a == "--to" && i + 1 < argc) to = static_cast<std::uint32_t>(std::atoll(argv[++i]));
    else if (a == "--dir" && i + 1 < argc) dir = argv[++i];
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a.rfind("--", 0) == 0) logf("[platbt] ignoring unknown option %s\n", a.c_str());
    else files.push_back(a);
  }

  // Candidate pool: explicit files if given, else scan the directory.
  std::vector<std::string> pool = files;
  if (pool.empty()) {
    if (dir.empty()) dir = ".";
    pool = scan_dir(dir);
  }
  const std::vector<std::string> selected = select_days(pool, from, to);
  if (selected.empty()) {
    logf("[platbt] no .mdr files in range %u..%u (looked in %s)\n", from, to,
         files.empty() ? dir.c_str() : "the given file list");
    return 1;
  }
  logf("[platbt] %zu day(s) selected, %u..%u\n", selected.size(), from, to);

  // ---- run + report ----
  std::vector<std::string> names;
  for (const auto& s : specs) names.push_back(s.name);
  BacktestReport report(cfg.f64("bt.capital_cny", 1'000'000.0), std::move(names));
  for (const auto& p : selected) {
    DayResult d;
    if (run_day(cfg, specs, p, d)) report.add(d);
  }
  if (report.empty()) {
    logf("[platbt] no usable market-data files\n");
    return 1;
  }

  report.print_summary();
  if (report.write_html(out, cfg.str("bt.report_title", "platbt backtest")))
    logf("\n[platbt] HTML report written to %s\n", out.c_str());
  else
    logf("\n[platbt] could not write report to %s\n", out.c_str());
  return 0;
}
