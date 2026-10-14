// platrec — the market-data recorder. A STANDALONE tool (no strategies, no
// order path, no risk gate): it enumerates every instrument the counter
// serves, subscribes to full-depth L2 market data, and streams it to a
// compact .mdr file for replay and backtest.
//
//   platrec <config.cfg>              record until close / Ctrl+C
//   platrec --compare A.mdr B.mdr [golden.mdr]
//                                     after-close cross-datacenter check;
//                                     writes the merged golden file if named
//   platrec --dump <file.mdr> [count] human-readable record listing
//
// Professional-recorder discipline (how an HFT desk keeps market data):
//   · deploy N recorders in different datacenters, each with a distinct
//     `source` tag; a crash in one loses nothing — the others kept recording;
//   · files seal with a payload checksum; a crashed file stays readable to
//     its last flush (per-burst fflush), so even a killed recorder yields a
//     usable partial;
//   · UDP can reorder — the file stores the venue's own sequence key
//     (exch_ts, cumulative volume), and the after-close --compare re-sorts
//     on it, reconciles the datacenters, and emits one golden file.
//
// Threading: the CTP md thread only converts depth records and pushes them
// to an SPSC ring; a dedicated writer thread drains and encodes to disk.
// The network thread never blocks on I/O.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "ThostFtdcMdApi.h"
#include "hft/common/spsc_ring.hpp"
#include "plat/core/config.hpp"
#include "plat/core/crash.hpp"
#include "plat/core/log.hpp"
#include "plat/core/thread.hpp"
#include "plat/core/time.hpp"
#include "plat/ctp/conv.hpp"
#include "plat/ctp/gbk.hpp"
#include "plat/rec/inst_enum.hpp"
#include "plat/rec/tick_file.hpp"
#include "plat/rec/tick_merge.hpp"

using namespace plat;
using namespace plat::rec;

namespace {

std::atomic<bool> g_stop{false};

// Standalone session clock (platrec doesn't link the engine driver): ns
// since local midnight, steady-anchored so it doesn't jump on an NTP step.
Nanos live_clock_ns() {
  using namespace std::chrono;
  struct Anchor { steady_clock::time_point s; Nanos d; };
  static const Anchor anchor = [] {
    const auto sys = system_clock::now();
    const std::time_t tt = system_clock::to_time_t(sys);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    const Nanos sub = duration_cast<nanoseconds>(sys.time_since_epoch()).count() % kNsPerSec;
    return Anchor{steady_clock::now(), at_time(lt.tm_hour, lt.tm_min, lt.tm_sec, sub)};
  }();
  return anchor.d +
         duration_cast<nanoseconds>(steady_clock::now() - anchor.s).count();
}

// ---- the SPSC handoff: md network thread → writer thread.
using RecRing = hft::SpscRing<MdRecord, 1 << 16>;

// ---- md SPI: convert depth → MdRecord → ring. No file I/O on this thread.
class RecMdSpi final : public CThostFtdcMdSpi {
 public:
  RecMdSpi(Config& cfg, RecRing& ring, int depth, const std::vector<MdrInstrument>& dir)
      : cfg_(&cfg), ring_(&ring), depth_(depth) {
    for (std::uint32_t i = 0; i < dir.size(); ++i) {
      Sub s{};
      std::memcpy(s.w, dir[i].symbol, sizeof(s.w));
      std::snprintf(s.symbol, sizeof(s.symbol), "%s", dir[i].symbol);
      s.inst = i;
      subs_.push_back(s);
    }
  }

  void start() {
    api_ = CThostFtdcMdApi::CreateFtdcMdApi(cfg_->str("md_flow_path", "rec_md_flow/").c_str());
    api_->RegisterSpi(this);
    api_->RegisterFront(const_cast<char*>(cfg_->str("md_front").c_str()));
    api_->Init();
  }
  void stop() {
    if (api_) {
      api_->RegisterSpi(nullptr);
      api_->Release();
      api_ = nullptr;
    }
  }
  [[nodiscard]] std::uint64_t received() const noexcept {
    return received_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t subscribed() const noexcept {
    return sub_ok_.load(std::memory_order_relaxed);
  }

  void OnFrontConnected() override {
    logf("[md] front connected %s\n", cfg_->str("md_front").c_str());
    CThostFtdcReqUserLoginField req{};
    std::snprintf(req.BrokerID, sizeof(req.BrokerID), "%s", cfg_->str("broker_id").c_str());
    std::snprintf(req.UserID, sizeof(req.UserID), "%s", cfg_->str("user_id").c_str());
    std::snprintf(req.Password, sizeof(req.Password), "%s", cfg_->str("password").c_str());
    api_->ReqUserLogin(&req, 1);
  }
  void OnFrontDisconnected(int reason) override {
    logf("[md] front disconnected 0x%x (api auto-reconnects; recording continues)\n", reason);
  }
  void OnRspUserLogin(CThostFtdcRspUserLoginField* l, CThostFtdcRspInfoField* info, int,
                      bool) override {
    if (info && info->ErrorID != 0) {
      logf("[md] login failed: %d %s\n", info->ErrorID, ctp::gbk_to_utf8(info->ErrorMsg).c_str());
      return;
    }
    logf("[md] logged in, trading day %s — subscribing %zu instruments\n",
         l ? l->TradingDay : "?", subs_.size());
    subscribe_all();
  }
  void OnRspError(CThostFtdcRspInfoField* info, int, bool) override {
    if (info) logf("[md] rsp error: %d %s\n", info->ErrorID, ctp::gbk_to_utf8(info->ErrorMsg).c_str());
  }
  // Subscription acks: failures were previously SILENT — an instrument that
  // never subscribed just recorded nothing. Log every rejection; count acks
  // so the per-minute status line can show subscribed/requested.
  void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* in, CThostFtdcRspInfoField* info,
                          int, bool) override {
    if (info && info->ErrorID != 0)
      logf("[md] SUBSCRIBE FAILED %s: %d %s\n", in ? in->InstrumentID : "?", info->ErrorID,
           ctp::gbk_to_utf8(info->ErrorMsg).c_str());
    else
      sub_ok_.fetch_add(1, std::memory_order_relaxed);
  }
  void OnHeartBeatWarning(int lag) override {
    logf("[md] heartbeat warning: no traffic for %d s\n", lag);
  }

  void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* d) override {
    if (!d) return;
    const Sub* sub = find(d->InstrumentID);
    if (!sub) return;
    MdRecord r{};
    r.inst = sub->inst;
    r.recv_ts = live_clock_ns();
    r.exch_ts = ctp::ns_from_ctp_time(d->UpdateTime, d->UpdateMillisec);
    r.last_px = ctp::px_from_ctp(d->LastPrice);
    r.day_volume = d->Volume;
    r.turnover_e1 = static_cast<std::int64_t>(d->Turnover * 10.0);
    r.open_interest = static_cast<Qty>(d->OpenInterest);
    // Level 0 always (L1). Deeper levels only for L2 — leaving them zero for
    // L1 means the codec encodes just the one level (depth in the header).
    r.bid_px[0] = ctp::px_from_ctp(d->BidPrice1); r.bid_qty[0] = d->BidVolume1;
    r.ask_px[0] = ctp::px_from_ctp(d->AskPrice1); r.ask_qty[0] = d->AskVolume1;
    if (depth_ >= 5) {
      r.bid_px[1] = ctp::px_from_ctp(d->BidPrice2); r.bid_qty[1] = d->BidVolume2;
      r.bid_px[2] = ctp::px_from_ctp(d->BidPrice3); r.bid_qty[2] = d->BidVolume3;
      r.bid_px[3] = ctp::px_from_ctp(d->BidPrice4); r.bid_qty[3] = d->BidVolume4;
      r.bid_px[4] = ctp::px_from_ctp(d->BidPrice5); r.bid_qty[4] = d->BidVolume5;
      r.ask_px[1] = ctp::px_from_ctp(d->AskPrice2); r.ask_qty[1] = d->AskVolume2;
      r.ask_px[2] = ctp::px_from_ctp(d->AskPrice3); r.ask_qty[2] = d->AskVolume3;
      r.ask_px[3] = ctp::px_from_ctp(d->AskPrice4); r.ask_qty[3] = d->AskVolume4;
      r.ask_px[4] = ctp::px_from_ctp(d->AskPrice5); r.ask_qty[4] = d->AskVolume5;
    }
    received_.fetch_add(1, std::memory_order_relaxed);
    if (!ring_->try_push(r)) dropped_.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  struct Sub {
    std::uint64_t w[4];
    char symbol[32];
    std::uint32_t inst;
  };
  void subscribe_all() {
    std::vector<char*> names;
    names.reserve(subs_.size());
    for (auto& s : subs_) names.push_back(s.symbol);
    // Subscribe in chunks — some fronts cap a single call.
    constexpr int kChunk = 500;
    for (std::size_t i = 0; i < names.size(); i += kChunk) {
      const int n = static_cast<int>(std::min<std::size_t>(kChunk, names.size() - i));
      api_->SubscribeMarketData(names.data() + i, n);
    }
  }
  [[nodiscard]] const Sub* find(const char* symbol) const noexcept {
    std::uint64_t w[4] = {};
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%s", symbol);
    std::memcpy(w, buf, sizeof(w));
    for (const auto& s : subs_)
      if ((s.w[0] == w[0]) & (s.w[1] == w[1]) & (s.w[2] == w[2]) & (s.w[3] == w[3]))
        return &s;
    return nullptr;
  }

  Config* cfg_;
  RecRing* ring_;
  int depth_;
  CThostFtdcMdApi* api_ = nullptr;
  std::vector<Sub> subs_;
  std::atomic<std::uint64_t> received_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> sub_ok_{0};
};

int run_record(const Config& cfg) {
  // ---- log file FIRST: everything after this line — connection trouble,
  // login/subscription rejects, and the crash handler's post-mortem — must
  // land on disk, not just on a console nobody was watching. Named after the
  // wall date + source tag; append mode, so a same-day restart adds to the
  // story instead of erasing it.
  const std::string source = cfg.str("source", "dc1");
  const std::time_t tt = std::time(nullptr);
  std::tm lt{};
#ifdef _WIN32
  localtime_s(&lt, &tt);
#else
  localtime_r(&tt, &lt);
#endif
  char def_log[80];
  std::snprintf(def_log, sizeof(def_log), "platrec_%04d%02d%02d_%s.log", lt.tm_year + 1900,
                lt.tm_mon + 1, lt.tm_mday, source.c_str());
  const std::string log_path = cfg.str("log_file", def_log);
  if (!log_open_file(log_path.c_str()))
    logf("[platrec] WARNING: cannot open log file '%s' — console only\n", log_path.c_str());
  log_banner("platrec");

  // ---- config sanity BEFORE the CTP api sees it: the SDK does not survive
  // an empty front address (null write inside thosttraderapi_se.dll — found
  // by the crash handler, kept out by this check).
  bool cfg_ok = true;
  for (const char* k : {"td_front", "md_front", "broker_id", "user_id", "password"})
    if (cfg.str(k).empty()) {
      logf("[platrec] config: missing required key `%s`\n", k);
      cfg_ok = false;
    }
  if (!cfg_ok) return 1;

  std::signal(SIGINT, [](int) { g_stop.store(true); });
  std::signal(SIGTERM, [](int) { g_stop.store(true); });

  // ---- enumerate the whole market: a minimal trader session (no order path
  // — see plat/rec/inst_enum.hpp) logs in once and dumps the instrument list.
  EnumConfig ec_cfg;
  ec_cfg.front = cfg.str("td_front");
  ec_cfg.broker_id = cfg.str("broker_id");
  ec_cfg.user_id = cfg.str("user_id");
  ec_cfg.password = cfg.str("password");
  ec_cfg.investor_id = cfg.str("investor_id", cfg.str("user_id"));
  ec_cfg.app_id = cfg.str("app_id");
  ec_cfg.auth_code = cfg.str("auth_code");
  ec_cfg.flow_path = cfg.str("td_flow_path", "rec_td_flow/");
  std::error_code ec;
  std::filesystem::create_directories(ec_cfg.flow_path, ec);
  std::filesystem::create_directories(cfg.str("md_flow_path", "rec_md_flow/"), ec);
  InstEnumerator enumr(ec_cfg);
  enumr.start();
  logf("[platrec] connecting to counter %s to enumerate instruments ...\n",
       ec_cfg.front.c_str());
  if (!enumr.wait_until_up(static_cast<int>(cfg.i64("session_up_timeout_ms", 60'000)))) {
    logf("[platrec] counter did not come up — aborting\n");
    return 1;
  }
  const std::uint32_t day = enumr.trading_day();
  const std::vector<EnumInstrument> all = enumr.query_all();
  enumr.stop();
  if (all.empty()) {
    logf("[platrec] could not enumerate instruments — aborting\n");
    return 1;
  }

  // Optional allow-list: `record = *` (default) or a comma list of prefixes.
  const auto prefixes = cfg.list("record");
  std::vector<MdrInstrument> dir;
  for (const auto& in : all) {
    bool keep = prefixes.empty();
    for (const auto& p : prefixes)
      if (p == "*" || std::strncmp(in.symbol, p.c_str(), p.size()) == 0) keep = true;
    if (keep) {
      MdrInstrument m{};
      std::snprintf(m.symbol, sizeof(m.symbol), "%s", in.symbol);
      std::snprintf(m.exchange, sizeof(m.exchange), "%s", in.exchange);
      // Reference data for backtest (§9): tick / multiplier / margin from the
      // counter's instrument record. Margin ratio → basis points ×1e-4.
      m.price_tick = ctp::px_from_ctp(in.price_tick);
      m.multiplier = in.volume_multiple > 0 ? in.volume_multiple : 1;
      m.margin_bps = ctp::margin_bps_from_ratio(in.long_margin_ratio);
      dir.push_back(m);
    }
  }
  // ---- market depth: 1 = L1 (level-1 top of book), 5 = L2 (five levels).
  // Accepts `depth = 1|5` or `depth = L1|L2`. Stored in the file header.
  const std::string depth_s = cfg.str("depth", "1");
  const int depth = (depth_s == "5" || depth_s == "L2" || depth_s == "l2") ? 5 : 1;
  logf("[platrec] recording %zu of %zu instruments, day %u, depth %s\n", dir.size(),
       all.size(), day, depth == 5 ? "L2 (5 levels)" : "L1 (1 level)");

  // ---- open the output file (source tag identifies this datacenter). An
  // empty `outfile =` in the config falls back to the default name.
  const std::string path =
      cfg.str("outfile", "md_" + std::to_string(day) + "_" + source + ".mdr");
  TickWriter writer;
  if (!writer.open(path, day, source.c_str(), depth, dir)) {
    logf("[platrec] cannot open output '%s' -- aborting\n", path.c_str());
    return 1;
  }
  logf("[platrec] writing %s (source '%s')\n", path.c_str(), source.c_str());

  // ---- ring + md subscription + writer thread.
  static RecRing ring;  // 64Ki × sizeof(MdRecord) — seconds of the whole market
  RecMdSpi md(const_cast<Config&>(cfg), ring, depth, dir);
  md.start();

  const int writer_core = static_cast<int>(cfg.i64("writer_core", -1));
  std::atomic<bool> writer_stop{false};
  std::thread writer_thread([&] {
    pin_thread_to_core(writer_core);
    MdRecord r;
    bool dirty = false;
    while (!writer_stop.load(std::memory_order_acquire)) {
      bool any = false;
      for (int i = 0; i < 4096 && ring.try_pop(r); ++i) {
        writer.append(r);
        any = dirty = true;
      }
      if (!any) {
        if (dirty) {
          writer.flush();  // crash durability: hand each burst to the OS
          dirty = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    while (ring.try_pop(r)) writer.append(r);  // final drain
  });

  // ---- record until close + grace, or Ctrl+C. Status once a minute.
  const Nanos close_ts = at_time(static_cast<int>(cfg.i64("close_hh", 15)),
                                 static_cast<int>(cfg.i64("close_mm", 30)));
  Nanos next_status = 0;
  while (!g_stop.load(std::memory_order_relaxed)) {
    const Nanos now = live_clock_ns();
    if (now >= close_ts) {
      logf("[platrec] session close reached — stopping\n");
      break;
    }
    if (now >= next_status) {
      logf("[platrec] recorded %llu ticks (%.1f MB), subscribed %llu/%zu, dropped %llu\n",
           static_cast<unsigned long long>(writer.count()),
           static_cast<double>(writer.bytes()) / (1024 * 1024),
           static_cast<unsigned long long>(md.subscribed()), dir.size(),
           static_cast<unsigned long long>(md.dropped()));
      if (writer.io_error())
        logf("[platrec] ERROR: disk write FAILING (disk full / volume lost) — ticks are "
             "being LOST\n");
      next_status = now + 60 * kNsPerSec;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  md.stop();  // no more producer
  writer_stop.store(true, std::memory_order_release);
  writer_thread.join();
  writer.close();  // seals

  logf("[platrec] DONE: %llu ticks, %.1f MB, %llu received, %llu dropped -> %s\n",
       static_cast<unsigned long long>(writer.count()),
       static_cast<double>(writer.bytes()) / (1024 * 1024),
       static_cast<unsigned long long>(md.received()),
       static_cast<unsigned long long>(md.dropped()), path.c_str());
  if (md.dropped() > 0)
    logf("[platrec] WARNING: %llu ticks dropped (ring full — raise writer priority or "
         "writer_core)\n",
         static_cast<unsigned long long>(md.dropped()));
  if (writer.io_error()) {
    logf("[platrec] ERROR: disk writes failed during the session — the file is NOT "
         "complete\n");
    return 1;
  }
  return 0;
}

int run_compare(const std::string& a, const std::string& b, const std::string& golden) {
  CompareStats s{};
  if (!compare_recordings(a, b, golden, s)) {
    logf("[compare] could not read the input files\n");
    return 1;
  }
  std::printf("compare %s  vs  %s\n", a.c_str(), b.c_str());
  std::printf("  matched (both agree):     %llu\n", (unsigned long long)s.matched);
  std::printf("  A-only (gaps B missed):   %llu\n", (unsigned long long)s.a_only);
  std::printf("  B-only (gaps A missed):   %llu\n", (unsigned long long)s.b_only);
  std::printf("  CONFLICTS (same key,\n"
              "    different content):     %llu\n", (unsigned long long)s.conflicts);
  std::printf("  out-of-order repaired:    A %llu, B %llu\n",
              (unsigned long long)s.a_resorted, (unsigned long long)s.b_resorted);
  std::printf("  duplicates dropped:       %llu\n", (unsigned long long)s.duplicates);
  if (!golden.empty())
    std::printf("  golden written:           %llu records -> %s\n",
                (unsigned long long)s.golden, golden.c_str());
  const bool clean = s.conflicts == 0;
  std::printf("result: %s%s\n", clean ? "CLEAN" : "CONFLICTS PRESENT — investigate",
              (s.a_only || s.b_only) ? " (datacenters filled each other's gaps)" : "");
  return clean ? 0 : 2;
}

int run_dump(const std::string& path, std::uint64_t limit) {
  TickReader r;
  if (!r.open(path)) {
    logf("[dump] cannot open %s\n", path.c_str());
    return 1;
  }
  std::printf("mdr %s: day %u source '%s' %s %u instruments, %llu records, %s\n", path.c_str(),
              r.header().day, r.header().source, r.depth() == 5 ? "L2" : "L1",
              r.header().instrument_count, (unsigned long long)r.header().record_count,
              r.sealed() ? "sealed" : "UNSEALED (crashed)");
  MdRecord rec;
  std::uint64_t shown = 0;
  bool truncated_by_limit = false;
  while (r.next(rec)) {
    if (limit && shown++ >= limit) {
      std::printf("... (more; pass a larger count)\n");
      truncated_by_limit = true;
      break;
    }
    const char* sym = rec.inst < r.dir().size() ? r.dir()[rec.inst].symbol : "?";
    const auto tp = time_parts(rec.recv_ts);
    std::printf("%02d:%02d:%02d.%03d %-10s last %.1f vol %lld open-interest %lld  b %.1fx%lld  a %.1fx%lld",
                tp.hh, tp.mm, tp.ss, tp.ms, sym, px_to_cny(rec.last_px),
                (long long)rec.day_volume, (long long)rec.open_interest, px_to_cny(rec.bid_px[0]), (long long)rec.bid_qty[0],
                px_to_cny(rec.ask_px[0]), (long long)rec.ask_qty[0]);
    for (int lvl = 1; lvl < r.depth(); ++lvl)  // L2: show the deeper levels too
      std::printf("  | b%d %.1fx%lld a%d %.1fx%lld", lvl, px_to_cny(rec.bid_px[lvl]),
                  (long long)rec.bid_qty[lvl], lvl, px_to_cny(rec.ask_px[lvl]),
                  (long long)rec.ask_qty[lvl]);
    std::printf("\n");
  }
  // Integrity verdict only makes sense over the WHOLE file.
  if (!truncated_by_limit) {
    if (!r.sealed())
      std::printf("(unsealed — recovered %llu records to the torn tail)\n",
                  (unsigned long long)r.decoded());
    else if (!r.verified())
      std::printf("WARNING: checksum/count mismatch — file is damaged\n");
    else
      std::printf("(sealed, checksum OK — %llu records)\n", (unsigned long long)r.decoded());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  init_console_utf8();  // logs contain em-dashes / CTP error strings may be 中文
  install_crash_handlers("platrec");  // abnormal death → named reason + stack + minidump
  if (argc >= 4 && std::strcmp(argv[1], "--compare") == 0)
    return run_compare(argv[2], argv[3], argc >= 5 ? argv[4] : "");
  if (argc >= 3 && std::strcmp(argv[1], "--dump") == 0)
    return run_dump(argv[2], argc >= 4 ? std::strtoull(argv[3], nullptr, 10) : 0);
  if (argc >= 2 && argv[1][0] != '-') {
    Config cfg;
    if (!cfg.load(argv[1])) {
      logf("cannot open config %s\n", argv[1]);
      return 1;
    }
    return run_record(cfg);
  }
  logf("usage:\n"
       "  platrec <config.cfg>                    record the whole market to a .mdr file\n"
       "  platrec --compare A.mdr B.mdr [gold.mdr] cross-datacenter verify (+ merge golden)\n"
       "  platrec --dump <file.mdr> [count]        human-readable record listing\n");
  return 1;
}
