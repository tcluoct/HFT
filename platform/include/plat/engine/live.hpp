#pragma once
// plat/engine/live.hpp — the production driver: the single engine thread
// that owns the wall clock and the deterministic boundary (§5.3, §6.4).
//
// Everything that crosses into the site does so here, as a Source envelope:
//   · market data drained from the md adapter's broadcast ring;
//   · exec reports / session events drained from the trader adapter's ring;
//   · phase transitions from the venue timetable (wall-clock driven);
//   · strategy timer firings (TimerEvt) from the driver's timer heap;
//   · operator kill-switch commands from the console.
//
// The site itself never reads a clock — now() is the timestamp of the
// envelope being dispatched — so a day journaled here replays byte-identically
// through the same Site in Replay mode (§11.1). That is the production
// recovery drill: platd --replay <journal> <ref>.
//
// Loss policy (§7.1, review R3): the market-data ring is loss-Tolerant (a
// lap resyncs to the live edge; staleness interlock covers the hole); the
// trade ring is loss-Intolerant — losing an exec report means our order
// state is no longer trustworthy, so the driver halts the site.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "hft/common/platform.hpp"
#include "plat/bus/ring.hpp"
#include "plat/core/log.hpp"
#include "plat/core/phase.hpp"
#include "plat/core/thread.hpp"
#include "plat/core/types.hpp"
#include "plat/engine/ops_cmds.hpp"
#include "plat/engine/site.hpp"

namespace plat {

// Session clock: nanoseconds since the local midnight the process started
// under, monotonic (steady-clock anchored, immune to NTP steps intraday).
// Night sessions that cross midnight express late times as 24h+ (01:00 next
// day = at_time(25, 0)) and keep a single monotonic session axis.
inline Nanos live_now_ns() noexcept {
  using namespace std::chrono;
  struct Anchor {
    steady_clock::time_point steady;
    Nanos day_ns;
  };
  static const Anchor anchor = [] {
    const auto sys_now = system_clock::now();
    const std::time_t tt = system_clock::to_time_t(sys_now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &tt);
#else
    localtime_r(&tt, &local);
#endif
    const Nanos subsec =
        duration_cast<nanoseconds>(sys_now.time_since_epoch()).count() % kNsPerSec;
    return Anchor{steady_clock::now(),
                  at_time(local.tm_hour, local.tm_min, local.tm_sec, subsec)};
  }();
  return anchor.day_ns +
         duration_cast<nanoseconds>(steady_clock::now() - anchor.steady).count();
}

// Operator console: a detached thread blocks on stdin and enqueues lines;
// the engine thread drains them. Static lifetime — the reader thread may
// outlive the driver during shutdown.
class OpsConsole {
 public:
  static OpsConsole& instance() {
    static OpsConsole c;
    return c;
  }
  void start() {
    if (started_.exchange(true)) return;
    std::thread([this] {
      std::string line;
      while (std::getline(std::cin, line)) push(line);
    }).detach();
  }
  void push(const std::string& s) {
    std::lock_guard lk(mtx_);
    q_.push_back(s);
  }
  bool pop(std::string& out) {
    std::lock_guard lk(mtx_);
    if (q_.empty()) return false;
    out = q_.front();
    q_.pop_front();
    return true;
  }

 private:
  std::mutex mtx_;
  std::deque<std::string> q_;
  std::atomic<bool> started_{false};
};

// Orderly-shutdown request flag, set from signal handlers (template-free so
// a handler needs no LiveDriver instantiation).
inline std::atomic<bool>& live_stop_flag() {
  static std::atomic<bool> f{false};
  return f;
}

struct LiveOptions {
  bool spin = false;          // true: busy-poll (colo); false: 200 µs idle sleep (dev/VM)
  int engine_core = -1;       // pin the engine thread to this core (colo)
  bool rt = false;            // time-critical scheduling class for the engine
  Nanos eod_grace = 2 * kNsPerSec;  // run EOD this long after the Closed transition
  int session_up_timeout_ms = 60'000;
  std::string cmd_file;       // operator command file (fleet children); empty = stdin only
  AccountId account = 1;      // account manual orders/transfers book under
};

template <typename SiteT>
class LiveDriver {
 public:
  LiveDriver(SiteT& site, Ring& md_ring, Ring& trade_ring,
             PhaseSchedule schedule, LiveOptions opt)
      : site_(&site),
        md_reader_(md_ring, /*from_origin=*/true, Ring::LossPolicy::Tolerant),
        trade_reader_(trade_ring, /*from_origin=*/true, Ring::LossPolicy::Intolerant),
        schedule_(schedule), opt_(opt) {
    site.set_timer_sink(&LiveDriver::timer_thunk, this);
  }

  // Strategy timers land on the driver's heap through the site's timer
  // sink; firings enter the site as journaled Source TimerEvt envelopes.
  static void timer_thunk(void* self, Nanos when, std::uint64_t token, StrategyId strategy) {
    static_cast<LiveDriver*>(self)->timers_.push(Timer{when, token, strategy});
  }

  // Runs the session to EOD (or operator quit). Returns false if the session
  // never armed.
  bool run() {
    OpsConsole::instance().start();
    if (opt_.engine_core >= 0 && pin_thread_to_core(opt_.engine_core))
      logf("[drv] engine pinned to core %d\n", opt_.engine_core);
    if (opt_.rt && set_thread_time_critical())
      logf("[drv] engine at time-critical priority\n");

    // Catch-up: publish the phase in effect *now* so a mid-session start has
    // a correct risk phase gate, then continue from the next transition.
    const Nanos start_ts = live_now_ns();
    publish_phase(schedule_.phase_at(start_ts), start_ts);
    while (phase_idx_ < schedule_.count && schedule_.entries[phase_idx_].when <= start_ts)
      ++phase_idx_;

    // Drain any session events already queued (login happened before run()),
    // then pre-flight.
    drain_trade_ring();
    if (!site_->arm_session(live_now_ns())) {
      logf("[drv] pre-flight FAILED — site halted; fix, then `disarm` + `arm`\n");
    }

    const Nanos close_ts =
        schedule_.count ? schedule_.entries[schedule_.count - 1].when : at_time(15, 0);
    bool eod_done = false;

    while (!eod_done) {
      bool did_work = false;
      did_work |= drain_md_ring();
      did_work |= drain_trade_ring();

      // One clock read per pass: everything time-driven shares it.
      const Nanos now = live_now_ns();
      did_work |= fire_phases(now);
      did_work |= fire_timers(now);
      did_work |= process_console(now);

      if (!site_->journal_healthy() && !journal_faulted_) {
        // Audit hole (journal thread lapped): we do not trade without a
        // complete record — fail closed (§7.2).
        journal_faulted_ = true;
        logf("[drv] JOURNAL FAULT — halting site\n");
        site_->command_kill(KillScope::Site, KillAction::Halt, 0, now);
      }

      if (live_stop_flag().load(std::memory_order_relaxed)) {
        logf("[drv] stop requested — cancelling resting orders\n");
        site_->command_kill(KillScope::Site, KillAction::Halt, 0, now);
        settle_and_eod();
        eod_done = true;
      } else if (now >= close_ts + opt_.eod_grace) {
        const auto tp = time_parts(close_ts);
        logf("[drv] session close (%02d:%02d) reached — running EOD\n", tp.hh, tp.mm);
        settle_and_eod();
        eod_done = true;
      } else if (!did_work) {
        idle();
      }
    }
    return site_->armed();
  }

  // EOD reconciliation inputs are queried by main (it owns the concrete
  // TraderClient); the driver signals readiness through this hook.
  using EodQueryFn = void (*)(void* ctx, std::vector<VenueOpenOrder>& open, Qty& buy, Qty& sell);
  void set_eod_query(EodQueryFn fn, void* ctx) {
    eod_fn_ = fn;
    eod_ctx_ = ctx;
  }

 private:
  struct Timer {
    Nanos when;
    std::uint64_t token;
    StrategyId strategy;
    bool operator>(const Timer& o) const noexcept { return when > o.when; }
  };

  bool drain_md_ring() {
    bool any = false;
    for (int i = 0; i < 512; ++i) {  // bounded batch: order flow stays responsive
      Envelope e;
      const auto p = md_reader_.poll(e);
      if (p.kind == Ring::PollKind::Empty || p.kind == Ring::PollKind::Fault) break;
      if (p.kind == Ring::PollKind::Gap) {
        logf("[drv] md ring lapped, lost %llu (books re-baseline)\n",
                     static_cast<unsigned long long>(p.gap));
        continue;
      }
      if (e.is<MdSnapshotEvt>()) site_->source(e.as<MdSnapshotEvt>(), e.ts);
      else if (e.is<MdOrderEvt>()) site_->source(e.as<MdOrderEvt>(), e.ts);
      else if (e.is<MdTradeEvt>()) site_->source(e.as<MdTradeEvt>(), e.ts);
      else if (e.is<FeedGapEvt>()) site_->source(e.as<FeedGapEvt>(), e.ts);
      any = true;
    }
    return any;
  }

  bool drain_trade_ring() {
    bool any = false;
    for (;;) {
      Envelope e;
      const auto p = trade_reader_.poll(e);
      if (p.kind == Ring::PollKind::Empty) break;
      if (p.kind == Ring::PollKind::Fault) {
        // Exec-report loss: order state is no longer trustworthy (§7.1).
        if (!trade_faulted_) {
          trade_faulted_ = true;
          logf("[drv] TRADE RING FAULT — halting site\n");
          site_->command_kill(KillScope::Site, KillAction::Halt, 0, live_now_ns());
        }
        break;
      }
      if (e.is<ExecReport>()) site_->source(e.as<ExecReport>(), e.ts);
      else if (e.is<SessionStateEvt>()) site_->source(e.as<SessionStateEvt>(), e.ts);
      any = true;
    }
    return any;
  }

  bool fire_phases(Nanos now) {
    bool any = false;
    while (phase_idx_ < schedule_.count && schedule_.entries[phase_idx_].when <= now) {
      publish_phase(schedule_.entries[phase_idx_].phase, schedule_.entries[phase_idx_].when);
      ++phase_idx_;
      any = true;
    }
    return any;
  }

  void publish_phase(Phase p, Nanos ts) {
    PhaseChangeEvt e{};
    e.ts = ts;
    e.venue = site_->config().venue;
    e.phase = p;
    site_->source(e, ts);
    logf("[drv] phase → %s\n", std::string(phase_name(p)).c_str());
  }

  bool fire_timers(Nanos now) {
    bool any = false;
    while (!timers_.empty() && timers_.top().when <= now) {
      const Timer t = timers_.top();
      timers_.pop();
      TimerEvt e{};
      e.ts = now;
      e.token = t.token;
      e.strategy = t.strategy;
      site_->source(e, now);
      any = true;
    }
    return any;
  }

  // Operator commands arrive on stdin (standalone run) or through the
  // command file (fleet children don't own the console): a line written to
  // `<cmd_file>` — e.g. `echo halt > shard1.cmd` — is executed and the file
  // removed. Polled at 100 ms; operator cadence, not tick cadence.
  bool process_console(Nanos now) {
    std::string line;
    if (!OpsConsole::instance().pop(line)) {
      if (opt_.cmd_file.empty() || now - last_cmd_poll_ < 100 * kNsPerMs) return false;
      last_cmd_poll_ = now;
      std::FILE* f = std::fopen(opt_.cmd_file.c_str(), "rb");
      if (!f) return false;
      char buf[256] = {};
      const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
      std::fclose(f);
      std::remove(opt_.cmd_file.c_str());
      line.assign(buf, n);
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
        line.pop_back();
      if (line.empty()) return false;
      logf("[ops] command file: %s\n", line.c_str());
    }
    // `halt` / `flatten` / `disarm` act on the whole site, or on ONE
    // strategy with a numeric argument (`halt 3`) — the intraday way to
    // stop a strategy for a while and start it again (`disarm 3`) without
    // touching the rest of the lineup (§10.3 strategy scope).
    std::string verb = line, arg;
    if (const auto sp = line.find(' '); sp != std::string::npos) {
      verb = line.substr(0, sp);
      const auto a = line.find_first_not_of(' ', sp);
      if (a != std::string::npos) arg = line.substr(a);
    }
    const bool scoped = !arg.empty();
    const KillScope scope = scoped ? KillScope::Strategy : KillScope::Site;
    const auto target = static_cast<std::uint16_t>(scoped ? std::atoi(arg.c_str()) : 0);
    if (verb == "halt") {
      site_->command_kill(scope, KillAction::Halt, target, now);
      if (scoped) logf("[ops] strategy %u HALTED (its resting orders cancelled)\n", target);
      else logf("[ops] site HALTED (resting orders cancelled)\n");
    } else if (verb == "flatten") {
      site_->command_kill(scope, KillAction::Flatten, target, now);
      if (scoped) logf("[ops] strategy %u FLATTEN commanded\n", target);
      else logf("[ops] site FLATTEN commanded\n");
    } else if (verb == "disarm") {
      site_->command_kill(scope, KillAction::Disarm, target, now);
      if (scoped) logf("[ops] strategy %u disarmed — trading again\n", target);
      else logf("[ops] kill switch disarmed\n");
    } else if (line == "arm") {
      const bool ok = site_->arm_session(now);
      logf("[ops] arm: pre-flight %s\n", ok ? "PASS" : "FAIL");
    } else if (line == "quit") {
      live_stop_flag().store(true);
    } else if (line == "status") {
      auto& tel = site_->tel();
      logf("[ops] envelopes %llu fills %llu risk pass/deny %llu/%llu sess %d\n",
                   static_cast<unsigned long long>(tel.get("stream.envelopes")),
                   static_cast<unsigned long long>(tel.get("fills")),
                   static_cast<unsigned long long>(tel.get("risk.pass")),
                   static_cast<unsigned long long>(tel.get("risk.deny")),
                   static_cast<int>(site_->session_state()));
    } else if (apply_ops_command(*site_, opt_.account, line, now)) {
      // Extended verb (param/order/cancel/transfer): handled + logged there;
      // each is a journaled Source envelope, so it replays and audits.
    } else if (!line.empty()) {
      logf("[ops] commands: halt [strat_id] | flatten [strat_id] | disarm [strat_id] | "
           "arm | status | quit | param <id> <key> <val> | order <id> <sym> buy|sell "
           "open|close|closetoday <px> <lots> | cancel <cl_id> | transfer <sym> <from> "
           "<to> long|short <lots>\n");
    }
    return true;
  }

  // Give in-flight cancels a moment to confirm, then reconcile and seal.
  void settle_and_eod() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < until) {
      if (!drain_trade_ring()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      drain_md_ring();
    }
    std::vector<VenueOpenOrder> open;
    Qty buy = 0, sell = 0;
    if (eod_fn_) eod_fn_(eod_ctx_, open, buy, sell);
    site_->run_eod(open, buy, sell);
    logf("[drv] EOD done: recon vs counter (%zu open, buy/sell %lld/%lld) — journal "
         "sealed\n",
         open.size(), static_cast<long long>(buy), static_cast<long long>(sell));
  }

  void idle() {
    if (opt_.spin) {
      hft::cpu_relax();  // busy-poll on a dedicated core (colo mode)
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }

  SiteT* site_;
  Ring::Reader md_reader_;
  Ring::Reader trade_reader_;
  PhaseSchedule schedule_;
  LiveOptions opt_;
  std::size_t phase_idx_ = 0;
  std::priority_queue<Timer, std::vector<Timer>, std::greater<>> timers_;
  EodQueryFn eod_fn_ = nullptr;
  void* eod_ctx_ = nullptr;
  Nanos last_cmd_poll_ = 0;
  bool trade_faulted_ = false;
  bool journal_faulted_ = false;
};

}  // namespace plat
