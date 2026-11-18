// platd_live.cpp — the live/resume session (`platd <cfg>` and the fleet
// children): counter login → instrument master → three-strategy site on the
// asynchronously journaled canonical stream → LiveDriver to EOD. Start mode
// is decided automatically from the journal found at the configured path;
// a same-day crashed journal replays (gateway muted) and reconciles against
// the counter before starting HALTED (§11.2).

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "plat/bus/journal.hpp"
#include "plat/bus/ring.hpp"
#include "plat/core/log.hpp"
#include "plat/ctp/md_client.hpp"
#include "plat/engine/fingerprint.hpp"
#include "plat/engine/live.hpp"
#include "plat/engine/reconcile.hpp"
#include "plat/engine/session_marker.hpp"
#include "plat/engine/report.hpp"
#include "plat/engine/snapshot.hpp"
#include "plat/replay/sidecar.hpp"
#include "platd_app.hpp"
#include "platd_workers.hpp"

using namespace plat;

namespace platd {
namespace {

struct EodBridge {
  ctp::TraderClient* trader;
  static bool query(void* ctx, std::vector<VenueOpenOrder>& open, Qty& buy, Qty& sell) {
    auto* self = static_cast<EodBridge*>(ctx);
    const auto orders = self->trader->query_open_orders();
    const auto t = self->trader->query_trade_totals();
    if (!orders || !t) return false;
    open = *orders;
    buy = t->buy_qty;
    sell = t->sell_qty;
    return true;
  }
};

// Move a leftover journal (and its sidecar) out of the way instead of ever
// truncating it — the journal is the audit record, whatever state it's in.
void archive_journal(const std::string& path, std::uint32_t day, const char* tag) {
  std::error_code ec;
  std::string base = path + "." + std::to_string(day) + "." + tag;
  std::string target = base;
  for (int i = 1; std::filesystem::exists(target, ec); ++i)
    target = base + "." + std::to_string(i);
  std::filesystem::rename(path, target, ec);
  if (std::filesystem::exists(path + ".ref", ec))
    std::filesystem::rename(path + ".ref", target + ".ref", ec);
  if (std::filesystem::exists(path + ".positions", ec))
    std::filesystem::rename(path + ".positions", target + ".positions", ec);
  if (std::filesystem::exists(path + ".session", ec))
    std::filesystem::rename(path + ".session", target + ".session", ec);
  if (std::filesystem::exists(path + ".snap", ec))
    std::filesystem::rename(path + ".snap", target + ".snap", ec);
  logf("[platd] archived previous journal -> %s\n", target.c_str());
}

// Live risk-limit reload: re-read the same file the session started from (and
// the same shard overlay) and hand the limits to the driver, which journals a
// Source RiskLimitsEvt. Credentials are irrelevant here; only risk.* is used.
struct ReloadContext {
  std::string path;
  int shard = 0;
};

bool reload_limits(void* ctx, RiskLimits& out) {
  auto* rc = static_cast<ReloadContext*>(ctx);
  Config fleet;
  if (!fleet.load(rc->path)) return false;
  Config c = rc->shard > 0 ? shard_config(fleet, rc->shard) : fleet;
  out = limits_from(c);
  return true;
}

}  // namespace

int run_live(const Config& cfg, bool force_resume, const std::string& cfg_path, int shard) {
  // ---- persistent log FIRST: the session story (start-mode decision,
  // resume/reconcile, incidents, and the crash handler's post-mortem) must
  // survive the process — a console line is gone the moment the window is.
  // Default: beside the journal, so it is per-shard unique under --fleet
  // (shardN.jnl.log). Append + unbuffered (plat/core/log.hpp).
  const std::string log_path =
      cfg.str("log_file", cfg.str("journal", "platd.jnl") + ".log");
  if (!log_open_file(log_path.c_str()))
    logf("[platd] WARNING: cannot open log file '%s' — console only\n", log_path.c_str());
  log_banner("platd");

  if (!require_keys(cfg)) return 1;
  const bool isolated = cfg.flag("strategy_processes");
  if (isolated && (shard || cfg.i64("strategy_stall_ms", 5000) <= 0 ||
                   cfg.i64("strategy_trigger_age_us", 5000) <= 0)) {
    logf("[platd] strategy_processes needs a standalone site and positive worker timeouts\n");
    return 1;
  }

  std::signal(SIGINT, [](int) { live_stop_flag().store(true); });
  std::signal(SIGTERM, [](int) { live_stop_flag().store(true); });

  Ring md_ring(1 << 14);
  TradeRing trade_ring;

  // ---- counter session first: reference data comes from the counter.
  ctp::TraderConfig tc;
  tc.front = cfg.str("td_front");
  tc.broker_id = cfg.str("broker_id");
  tc.user_id = cfg.str("user_id");
  tc.password = resolve_secret(cfg, "password");
  tc.investor_id = cfg.str("investor_id", cfg.str("user_id"));
  tc.app_id = cfg.str("app_id");
  tc.auth_code = resolve_secret(cfg, "auth_code");
  tc.flow_path = cfg.str("td_flow_path", "ctp_td_flow/");
  tc.epoch = static_cast<std::uint16_t>(cfg.i64("epoch", 1));
  std::error_code fs_ec;
  std::filesystem::create_directories(tc.flow_path, fs_ec);  // CTP needs the dir
  ctp::TraderClient trader(tc, trade_ring, &live_now_ns);
  trader.start();
  logf("[platd] connecting to counter %s ...\n", tc.front.c_str());
  if (!trader.wait_until_up(static_cast<int>(cfg.i64("session_up_timeout_ms", 60'000)))) {
    logf("[platd] counter session did not come up — aborting\n");
    return 1;
  }

  // ---- trading calendar (optional but recommended): a configured calendar
  // that does not list today means a holiday/half-day misconfiguration —
  // refuse to trade rather than trust the phase timetable.
  if (const std::string cal = cfg.str("trade_calendar"); !cal.empty()) {
    if (!is_trading_day(cal, trader.trading_day())) return 1;
  } else {
    logf("[platd] note: no `trade_calendar` configured — the phase timetable is trusted\n");
  }

  // ---- instrument master. Fresh start: counter queries (§9.6). Resume: the
  // crashed session's sidecar is authoritative — InstrumentIds inside the
  // journal are indexes into THAT table, and the trading day hasn't changed.
  const auto symbols = cfg.list("instruments");
  const std::string jnl_path = cfg.str("journal", "platd.jnl");
  // EOD break gate: a previous session whose reconciliation against the
  // counter found open-order or fill breaks leaves this marker. The next
  // session starts HALTED until an operator reviews the report and removes
  // it — a break must never be silently traded through.
  const std::string eod_breaks_path = jnl_path + ".eod-breaks";
  SessionMarker marker(eod_breaks_path);
  if (!marker.begin()) {
    logf("[platd] cannot durably establish reconciliation gate %s — refusing startup\n",
         eod_breaks_path.c_str());
    return 1;
  }
  const bool eod_breaks_pending = marker.preexisting();
  if (eod_breaks_pending)
    logf("[platd] pending reconciliation marker: trading locked. Review counter/orders, "
         "stop this process, remove %s, then restart.\n", eod_breaks_path.c_str());
  InstrumentTable table;
  std::vector<InstrumentId> hosted;
  Venue site_venue = Venue::SHFE;
  JournalReader crashed;

  // ---- start-mode decision. The journal at the configured path tells the
  // previous run's story, so the operator never has to: sealed = clean EOD
  // (archive it, start fresh); unsealed + today's date = a crash THIS
  // session (auto-resume); unsealed + older date = a crash on a previous day
  // (can't replay across days — archive loudly, start fresh with carry-in).
  // `--resume` forces the resume path and fails rather than guessing.
  bool resume = force_resume;
  std::vector<PositionsRow> prev_positions;
  JournalHeader prev_hdr{};
  const bool have_prev = JournalReader::peek(jnl_path, prev_hdr);
  std::error_code fs_probe;
  if (force_resume && !have_prev) {
    logf("[resume] no journal at %s to resume from\n", jnl_path.c_str());
    return 1;
  }
  if (!force_resume && have_prev) {
    if (prev_hdr.magic != kJournalMagic) {
      logf("[platd] %s is not a platform journal — archiving, starting fresh\n",
           jnl_path.c_str());
      archive_journal(jnl_path, 0, "corrupt");
    } else if (prev_hdr.sealed) {
      logf("[platd] previous session (day %u) ended cleanly — archiving its journal\n",
           prev_hdr.day);
      prev_positions = read_prev_positions(jnl_path, prev_hdr.day);
      archive_journal(jnl_path, prev_hdr.day, "sealed");
    } else if (prev_hdr.day != trader.trading_day()) {
      logf("[platd] WARNING: found a CRASHED journal from day %u (today is %u) — resume "
           "is same-day only; archiving for manual review, starting fresh\n",
           prev_hdr.day, trader.trading_day());
      archive_journal(jnl_path, prev_hdr.day, "crashed");
    } else if (!std::filesystem::exists(jnl_path + ".ref", fs_probe)) {
      logf("[platd] WARNING: crashed journal from today has no .ref sidecar — cannot "
           "resume; archiving for manual review, starting fresh\n");
      archive_journal(jnl_path, prev_hdr.day, "crashed-noref");
    } else {
      logf("[platd] UNSEALED journal from TODAY (day %u) found — a crashed session: "
           "AUTO-RESUME\n",
           prev_hdr.day);
      resume = true;
    }
  }

  if (resume) {
    if (!crashed.open(jnl_path)) {
      logf("[resume] cannot open journal %s\n", jnl_path.c_str());
      return 1;
    }
    if (crashed.sealed() && !crashed.verified()) {
      logf("[resume] journal checksum FAILED — refusing to resume from it\n");
      return 1;
    }
    if (crashed.header().day != trader.trading_day()) {
      logf("[resume] journal is from day %u but the counter says %u — resume is "
                   "same-day only (a new day starts fresh with carry-in)\n",
                   crashed.header().day, trader.trading_day());
      return 1;
    }
    if (!load_sidecar(jnl_path, table)) return 1;
    for (InstrumentId i = 0; i < table.size(); ++i) {
      hosted.push_back(i);
      site_venue = table.at(i).venue;
      trader.add_instrument(i, table.at(i).symbol,
                            std::string(venue_name(table.at(i).venue)).c_str());
    }
    // Preserve the crashed record; the resumed site rewrites the canonical
    // path with the full day (replayed prefix + live continuation).
    std::error_code ec;
    std::filesystem::remove(jnl_path + ".crashed", ec);
    std::filesystem::rename(jnl_path, jnl_path + ".crashed", ec);
    logf("[resume] crashed journal: %llu envelopes (%s), day %u — backed up to "
                 "%s.crashed\n",
                 static_cast<unsigned long long>(crashed.header().count),
                 crashed.sealed() ? "sealed" : "UNSEALED", crashed.header().day,
                 jnl_path.c_str());
  } else {
  // A wrong symbol must not kill the shard: skip it (riskgate would refuse
  // it anyway — no book, no reference prices) and trade what resolves. But
  // "skip" requires an ANSWER: right after login the counter's query service
  // may still be warming up (empty/errored responses), and a transient
  // non-answer must never be read as "instrument does not exist" — retry.
  constexpr int kQueryAttempts = 6;
  const auto backoff = [](int attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1'500 * attempt));
  };
  for (const auto& sym : symbols) {
    ctp::InstrumentMeta meta{};
    for (int attempt = 1; attempt <= kQueryAttempts && !meta.answered; ++attempt) {
      meta = trader.query_instrument(sym.c_str());
      if (!meta.answered) {
        logf("[platd] instrument query %s unanswered (attempt %d/%d) — counter warming "
             "up, retrying\n",
             sym.c_str(), attempt, kQueryAttempts);
        backoff(attempt);
      }
    }
    if (!meta.answered) {
      logf("[platd] instrument query %s never answered — skipped this start\n", sym.c_str());
      continue;
    }
    if (!meta.found) {
      logf("[platd] instrument %s not found at counter — skipped\n", sym.c_str());
      continue;
    }
    ctp::DepthMeta depth{};
    for (int attempt = 1; attempt <= kQueryAttempts && !depth.answered; ++attempt) {
      depth = trader.query_depth(sym.c_str());
      if (!depth.answered) {
        logf("[platd] depth query %s unanswered (attempt %d/%d) — retrying\n", sym.c_str(),
             attempt, kQueryAttempts);
        backoff(attempt);
      }
    }
    const Venue venue = ctp::venue_from_exchange_id(meta.exchange_id);
    const Price prev = ctp::px_from_ctp(depth.pre_settlement ? depth.pre_settlement
                                                             : depth.pre_close);
    if (prev <= 0 && !(depth.found && depth.upper_limit > 0)) {
      logf("[platd] instrument %s has no reference prices — skipped\n",
                   sym.c_str());
      continue;  // the collar would deny every order on it anyway
    }
    std::uint32_t margin_bps = ctp::margin_bps_from_ratio(meta.long_margin_ratio);
    if (margin_bps == 0)
      margin_bps = static_cast<std::uint32_t>(cfg.i64("default_margin_bps", 1'200));
    const InstrumentId id =
        table.add(sym, venue, Board::Future, ctp::px_from_ctp(meta.price_tick), 1,
                  meta.volume_multiple, prev, margin_bps);
    if (depth.found && depth.upper_limit > 0)
      table.set_limit_band(id, ctp::px_from_ctp(depth.upper_limit),
                           ctp::px_from_ctp(depth.lower_limit));
    hosted.push_back(id);
    site_venue = venue;  // one venue per site by construction (§5.3)
    trader.add_instrument(id, meta.symbol, meta.exchange_id);
    logf("[platd] %s @%s tick %.4f mult %d margin %ubps limits [%.1f, %.1f]\n",
                 meta.symbol, meta.exchange_id, ctp::px_to_ctp(table.at(id).tick),
                 meta.volume_multiple, margin_bps, ctp::px_to_ctp(table.at(id).lower_limit),
                 ctp::px_to_ctp(table.at(id).upper_limit));
  }
  }  // fresh-start instrument resolution
  if (hosted.empty()) {
    logf("[platd] no configured instrument resolved at the counter (after retries) — "
         "aborting\n");
    return 1;
  }

  // ---- strategy lineup: named instances from config (`strategies = ...`,
  // legacy trio when the key is absent). A config that doesn't parse must
  // not trade (fail closed).
  const AccountId account = static_cast<AccountId>(cfg.i64("account_id", 1));
  std::vector<StratSpec> specs;
  if (!parse_strategies(cfg, account, specs)) {
    logf("[platd] strategy configuration is invalid — aborting\n");
    return 1;
  }
  for (const auto& s : specs)
    logf("[platd] strategy `%s`: %s id %u account %u\n", s.name.c_str(), kind_name(s.kind),
         s.id, s.account);
  if (isolated) {
    for (const auto& s : specs) if (s.account != account) {
      logf("[platd] strategy %u must use the configured account in process mode\n", s.id);
      return 1;
    }
    for (const auto id : hosted) if (table.at(id).is_stock()) {
      logf("[platd] strategy process mode currently supports futures snapshots\n");
      return 1;
    }
  }
  if (specs.empty())
    logf("[platd] NOTE: no enabled strategies — running as a position/journal holder only\n");

  // ---- site on the canonical stream, journaled asynchronously.
  SiteConfig sc;
  sc.id = static_cast<SiteId>(cfg.i64("site_id", 1));
  sc.name = cfg.str("site_name", "site");
  sc.venue = site_venue;
  sc.hosted = hosted;
  sc.day = trader.trading_day();
  sc.journal_path = jnl_path;
  sc.journal_core = static_cast<int>(cfg.i64("journal_core", -1));
  // Stable-storage cadence: a machine crash loses at most this window; a
  // process crash loses nothing (every burst is flushed to the OS).
  sc.journal_sync = cfg.i64("journal.sync_ms", 100) * kNsPerMs;
  sc.limits = limits_from(cfg);
  sc.fees = fees_from(cfg);
  sc.epoch = tc.epoch;
  sc.shared_account_risk = isolated;

  // ---- session fingerprint: the composition that produced this journal.
  // The crashed journal is unsealed (no stream hash), so this sidecar is the
  // only way resume can prove the binary/pack/config match the dead session.
  SessionFingerprint session_fp;
  session_fp.build = build_fingerprint();
  session_fp.pack = Pack::fingerprint();
  session_fp.config = config_fingerprint(specs, sc.fees, table, hosted,
                                         sc.limits.max_actions_per_sec, isolated);
  session_fp.day = sc.day;
  session_fp.site = sc.id;
  const std::string session_path = sc.journal_path + ".session";
  const std::string snap_path = sc.journal_path + ".snap";
  if (!resume) {
    // A snapshot without the crashed journal it belongs to is stale state; a
    // fresh session must never adopt it.
    std::error_code snap_ec;
    std::filesystem::remove(snap_path, snap_ec);
  }
  if (resume) {
    SessionFingerprint recorded;
    if (!load_session_fingerprint(session_path, recorded)) {
      if (!force_resume) {
        logf("[resume] no `.session` fingerprint beside the crashed journal — cannot prove the "
             "binary/pack/config match; refusing AUTO-RESUME (use --resume for a drill, or "
             "archive the journal and start fresh)\n");
        return 1;
      }
      logf("[resume] WARNING: no session fingerprint (forced resume) — composition cannot be "
           "verified\n");
    } else if (!same_fingerprint(recorded, session_fp)) {
      logf("[resume] COMPOSITION MISMATCH — refusing to replay the crashed journal:\n"
           "  recorded build %016llx pack %016llx config %016llx day %u site %u\n"
           "  current  build %016llx pack %016llx config %016llx day %u site %u\n",
           static_cast<unsigned long long>(recorded.build),
           static_cast<unsigned long long>(recorded.pack),
           static_cast<unsigned long long>(recorded.config), recorded.day, recorded.site,
           static_cast<unsigned long long>(session_fp.build),
           static_cast<unsigned long long>(session_fp.pack),
           static_cast<unsigned long long>(session_fp.config), session_fp.day,
           session_fp.site);
      return 1;
    }
    std::error_code sec;
    std::filesystem::remove(jnl_path + ".crashed.session", sec);
    std::filesystem::rename(session_path, jnl_path + ".crashed.session", sec);
  }
  if (!save_session_fingerprint(session_path, session_fp)) {
    logf("[platd] cannot write session fingerprint %s — refusing to trade\n",
         session_path.c_str());
    return 1;
  }

  LiveSite site(table, sc, LiveSite::Mode::Live,
                isolated ? Pack::Banks{} : Pack::banks(cfg, table, specs), account,
                cfg.i64("strategy_trigger_age_us", 5000) * 1000);
  site.set_gateway(&trader);
  if (!resume) {
    // Pin the startup limits into the journal. Replay/recovery therefore uses
    // the limits the session actually started with, not whatever the config
    // file says at resume time; later `reload`s are journaled events too.
    RiskLimitsEvt initial{};
    initial.ts = live_now_ns();
    initial.version = 0;
    initial.limits = sc.limits;
    site.source(initial, initial.ts);
    logf("[platd] journaled initial risk limits: max_order_qty %lld, actions/s %u, "
         "day %u\n",
         static_cast<long long>(sc.limits.max_order_qty), sc.limits.max_actions_per_sec,
         sc.limits.max_actions_per_day);
  }
  if (eod_breaks_pending) site.block_trading();

  // Operator-configured order/cancel-count alerts (§10.2) — off by default;
  // set comp.order_count_alert / comp.cancel_count_alert to exercise them.
  if (const auto oca = cfg.i64("comp.order_count_alert", 0);
      oca || cfg.i64("comp.cancel_count_alert", 0)) {
    ComplianceThresholds ct;
    ct.order_count_alert = static_cast<std::uint32_t>(oca);
    ct.cancel_count_alert = static_cast<std::uint32_t>(cfg.i64("comp.cancel_count_alert", 0));
    site.compliance().set_thresholds(ct);
    logf("[platd] compliance alert thresholds: order_count=%u cancel_count=%u\n",
         ct.order_count_alert, ct.cancel_count_alert);
  }

  // Persist the instrument snapshot next to the journal: replay reconstructs
  // the exact table this session traded with.
  if (!save_sidecar(sc.journal_path, table) && isolated) return 1;

  if (resume) {
    // ---- crash recovery (§11.2): restore the prefix as fast as possible.
    // A periodic snapshot (journal.snapshot_sec) lets us load the state at
    // envelope K, re-create the journal prefix (hash + bytes, no dispatch)
    // and dispatch only the Source suffix. No snapshot → full replay.
    site.set_recovering(true);
    std::uint64_t injected = 0;
    const std::size_t total = crashed.envelopes().size();
    SnapshotInfo snap_info;
    bool used_snapshot = false;
    if (load_snapshot(site, snap_path, session_fp, snap_info, total)) {
      for (std::size_t i = 0; i < snap_info.index; ++i)
        site.stream().absorb(crashed.envelopes()[i]);
      for (std::size_t i = static_cast<std::size_t>(snap_info.index); i < total; ++i) {
        const Envelope& e = crashed.envelopes()[i];
        if (e.origin == Origin::Source) ++injected;
        site.inject(e);
      }
      used_snapshot = true;
      logf("[resume] snapshot at envelope %llu loaded — replayed %zu suffix envelope(s) "
           "(%llu source) instead of the full %zu\n",
           static_cast<unsigned long long>(snap_info.index), total - snap_info.index,
           static_cast<unsigned long long>(injected), total);
    } else {
      for (const Envelope& e : crashed.envelopes()) {
        if (e.origin == Origin::Source) ++injected;
        site.inject(e);
      }
      logf("[resume] no usable snapshot — full replay of %zu envelope(s)\n", total);
    }
    {
      std::error_code snap_ec;
      if (used_snapshot) {
        std::filesystem::remove(jnl_path + ".crashed.snap", snap_ec);
        std::filesystem::rename(snap_path, jnl_path + ".crashed.snap", snap_ec);
      } else {
        std::filesystem::remove(snap_path, snap_ec);  // stale for the new journal
      }
    }
    logf("[resume] replayed %llu source envelopes — state hash %016llx at crash "
                 "point\n",
         static_cast<unsigned long long>(injected),
         static_cast<unsigned long long>(site.state_hash()));

    // Gate by the phase in effect NOW (the replayed phase is crash-time's).
    PhaseChangeEvt pe{};
    pe.ts = live_now_ns();
    pe.venue = site_venue;
    pe.phase = schedule_from(cfg).phase_at(pe.ts);
    site.source(pe, pe.ts);

    // Keep the gateway muted through phase changes and synthesized fills.
    reconcile_resume(site, trader, table, live_now_ns());
    site.set_recovering(false);
    site.command_kill(KillScope::Site, KillAction::Halt, 0, live_now_ns());
    if (site.safety_blocked())
      logf("[resume] safety interlock LOCKED: resolve broker discrepancies, stop, "
           "review/remove the pending marker, then restart. Disarm cannot bypass this.\n");
    else
      logf("[resume] site is HALTED. Review reconciliation, then `disarm` and `arm`.\n");
  } else {
    // ---- carry-in (§9.5): the counter says how much is carried; yesterday's
    // .positions file says whose — per-strategy attribution restored here,
    // including books whose strategy is stopped this session.
    if (!carry_in_positions(site, trader, table, account, prev_positions, specs, live_now_ns()))
      site.block_trading();
  }

  // ---- market data adapter.
  ctp::MdConfig mc;
  mc.front = cfg.str("md_front");
  mc.broker_id = cfg.str("broker_id");
  mc.user_id = cfg.str("user_id");
  mc.password = resolve_secret(cfg, "password");
  mc.flow_path = cfg.str("md_flow_path", "ctp_md_flow/");
  std::filesystem::create_directories(mc.flow_path, fs_ec);
  StrategyProcesses workers;
  if (isolated && !workers.launch(site, cfg, specs, cfg_path)) {
    logf("[platd] could not launch strategy processes\n");
    return 1;
  }
  Ring& market = isolated ? workers.market() : md_ring;
  ctp::MdClient md(mc, market, &live_now_ns);
  for (const InstrumentId id : hosted)
    md.add_instrument(table.at(id).symbol, id, table.at(id).venue);
  md.start();

  // ---- drive the session.
  LiveOptions opt;
  opt.spin = cfg.flag("spin", false);
  opt.engine_core = static_cast<int>(cfg.i64("engine_core", -1));
  opt.rt = cfg.flag("rt", false);
  opt.cmd_file = cfg.str("cmd_file", sc.name + ".cmd");
  opt.account = account;  // manual orders/transfers book under the site account
  LiveDriver<LiveSite> driver(site, market, trade_ring, schedule_from(cfg), opt);
  if (isolated) {
    site.set_event_sink(&StrategyProcesses::publish, &workers);
    driver.set_worker_poll(&StrategyProcesses::poll, &workers);
  }
  EodBridge eod{&trader};
  driver.set_eod_query(&EodBridge::query, &eod);
  ReloadContext reload_ctx{cfg_path, shard};
  driver.set_limits_loader(&reload_limits, &reload_ctx);
  // Checkpoint cadence: a restart replays at most this window (0 disables).
  driver.set_snapshot(snap_path, cfg.i64("journal.snapshot_sec", 300) * kNsPerSec,
                      session_fp);
  driver.run();

  // The durable marker predates all trading. Only a fully clean session may
  // remove it; disk errors, query failures and recovery breaks leave it intact.
  const bool clean = site.eod().ran && site.eod().order_breaks == 0 &&
                     site.eod().fills_ok && site.journal_healthy() && !site.safety_blocked();
  const bool gate_cleared = marker.complete(clean);
  if (!gate_cleared)
    logf("[platd] reconciliation gate retained: review %s before restart\n",
         eod_breaks_path.c_str());

  // ---- per-strategy attribution for tomorrow's carry-in.
  write_eod_positions(site, table, jnl_path);

  // ---- end-of-session report (§11.3, §14): one block per configured
  // instance, plus any id the ledger carries for a stopped strategy.
  std::string report = session_report(site);
  for (const auto& s : specs) report += pnl_report(site, s.id, s.name.c_str());
  std::vector<StrategyId> carried_only;
  site.pnl().for_each([&](const PosKey& k, const Position&) {
    for (const auto& s : specs) if (s.id == k.strategy) return;
    for (const StrategyId id : carried_only) if (id == k.strategy) return;
    carried_only.push_back(k.strategy);
  });
  for (const StrategyId id : carried_only) report += pnl_report(site, id, "carried, not running");
  const auto funds = trader.query_funds();
  if (funds && funds->found) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "account %s funds: balance %.2f available %.2f margin %.2f CNY\n",
                  funds->account_id, funds->balance, funds->available, funds->margin);
    report += buf;
  }
  std::printf("%s", report.c_str());
  const std::string report_path = cfg.str("report", sc.journal_path + ".report.txt");
  if (std::FILE* f = std::fopen(report_path.c_str(), "wb")) {
    std::fwrite(report.data(), 1, report.size(), f);
    std::fclose(f);
  }

  md.stop();
  trader.stop();
  return gate_cleared ? 0 : 1;
}

}  // namespace platd
