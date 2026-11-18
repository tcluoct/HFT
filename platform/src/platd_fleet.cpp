// platd_fleet.cpp — the multi-shard supervisor (`platd --fleet fleet.cfg`):
// one command spawns one platd process per `shardN.*` section — each a full
// site with its own core-pinned engine, journal, and CTP session — routes
// operator commands (`2 halt`, `quit`), and supervises the children.
//
// Production supervision, not just reaping:
//   · unexpected exits are restarted with exponential backoff, bounded by
//     `fleet.max_restarts`; a clean EOD exit (code 0) is not restarted;
//   · each child is bound to a JobObject (Windows) / gets a parent-death
//     signal + own process group (Linux), so a supervisor crash cannot leave
//     orphaned trading processes behind;
//   · `quit` asks every shard to shut down gracefully, then after
//     `fleet.quit_grace_ms` terminates whatever is left.

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "plat/core/crash.hpp"  // exception_code_name: decode a dead shard's exit code
#include "plat/core/log.hpp"
#include "plat/engine/live.hpp"  // OpsConsole
#include "platd_app.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace plat;
using Clock = std::chrono::steady_clock;
using Millis = std::chrono::milliseconds;

namespace platd {
namespace {

struct Shard {
  int n = 0;
#ifdef _WIN32
  HANDLE proc = nullptr;
  unsigned long pid = 0;
#else
  pid_t pid = -1;
#endif
  bool up = false;
  int restarts = 0;
  Clock::time_point next_start{};  // zero = not scheduled
};

struct FleetOptions {
  bool restart = true;
  int max_restarts = 5;
  int backoff_ms = 1'000;
  int max_backoff_ms = 30'000;
  int quit_grace_ms = 10'000;
};

#ifdef _WIN32
HANDLE g_job = nullptr;

bool create_job() {
  g_job = CreateJobObjectA(nullptr, nullptr);
  if (!g_job) return false;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
  // The children die when the supervisor's last handle closes, including an
  // abnormal supervisor death — a crashed supervisor can never leave trading
  // processes running unsupervised.
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  return SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &info,
                                 sizeof(info)) != 0;
}

bool spawn(Shard& s, const char* exe, const std::string& cfg_path,
           const std::string& log_path) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  // APPEND, not truncate: a restarted shard must not erase the previous
  // run's console capture — that capture is the crash evidence.
  HANDLE hlog = CreateFileA(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &sa,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE hnul = CreateFileA("NUL", GENERIC_READ, 0, &sa, OPEN_EXISTING, 0, nullptr);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = hlog;
  si.hStdError = hlog;
  si.hStdInput = hnul;  // shards take commands via shardN.cmd, not stdin
  std::string cmd = std::string("\"") + exe + "\" --shard " + std::to_string(s.n) + " \"" +
                    cfg_path + "\"";
  PROCESS_INFORMATION pi{};
  const bool ok =
      CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si,
                     &pi) != 0;
  if (hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
  if (hnul != INVALID_HANDLE_VALUE) CloseHandle(hnul);
  if (!ok) return false;
  CloseHandle(pi.hThread);
  if (g_job && !AssignProcessToJobObject(g_job, pi.hProcess))
    logf("[fleet] WARNING: shard %d could not join the supervisor JobObject\n", s.n);
  s.proc = pi.hProcess;
  s.pid = static_cast<unsigned long>(pi.dwProcessId);
  s.up = true;
  logf("[fleet] shard %d started (pid %lu) -> %s\n", s.n, s.pid, log_path.c_str());
  return true;
}

bool reap(Shard& s, int& code) {
  if (!s.up) return false;
  if (WaitForSingleObject(s.proc, 0) != WAIT_OBJECT_0) return false;
  DWORD c = 0;
  GetExitCodeProcess(s.proc, &c);
  code = static_cast<int>(c);
  CloseHandle(s.proc);
  s.proc = nullptr;
  s.up = false;
  return true;
}

void kill_all(const std::vector<Shard>&) {
  if (g_job) TerminateJobObject(g_job, 1);
}

void close_job() {
  if (g_job) {
    CloseHandle(g_job);  // KILL_ON_JOB_CLOSE: any straggler dies here
    g_job = nullptr;
  }
}
#else
bool spawn(Shard& s, const char* exe, const std::string& cfg_path,
           const std::string& log_path) {
  const pid_t pid = fork();
  if (pid == 0) {
    // Own process group + parent-death signal: a supervisor crash tears the
    // shard down instead of orphaning a live trading process.
    setpgid(0, 0);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    (void)!freopen(log_path.c_str(), "a", stdout);  // append: keep crash evidence
    (void)!freopen(log_path.c_str(), "a", stderr);
    (void)!freopen("/dev/null", "r", stdin);
    const std::string ns = std::to_string(s.n);
    execl(exe, "platd", "--shard", ns.c_str(), cfg_path.c_str(),
          static_cast<char*>(nullptr));
    _exit(127);
  }
  if (pid < 0) return false;
  setpgid(pid, pid);
  s.pid = pid;
  s.up = true;
  logf("[fleet] shard %d started (pid %d) -> %s\n", s.n, pid, log_path.c_str());
  return true;
}

bool reap(Shard& s, int& code) {
  if (!s.up) return false;
  int status = 0;
  if (waitpid(s.pid, &status, WNOHANG) != s.pid) return false;
  if (WIFSIGNALED(status)) {
    logf("[fleet] shard %d CRASHED: signal %d (%s) — post-mortem in shard%d.log\n", s.n,
         WTERMSIG(status), strsignal(WTERMSIG(status)), s.n);
    code = 1;
  } else {
    code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  }
  s.up = false;
  return true;
}

void kill_all(const std::vector<Shard>& shards) {
  for (const Shard& s : shards)
    if (s.up && s.pid > 0) ::kill(-s.pid, SIGKILL);  // the shard's process group
}

void close_job() {}
#endif

std::string shard_log_path(int n) { return "shard" + std::to_string(n) + ".log"; }

}  // namespace

int run_fleet(const std::string& cfg_path) {
  Config fleet;
  if (!fleet.load(cfg_path)) {
    logf("cannot open fleet config %s\n", cfg_path.c_str());
    return 1;
  }
  const std::vector<int> ids = fleet_shard_ids(fleet);
  if (!unique_fleet_instruments(fleet)) {
    logf("[fleet] duplicate broker account/instrument ownership\n");
    return 1;
  }
  if (ids.empty()) {
    logf("fleet config has no `shardN.instruments` sections\n");
    return 1;
  }

  // Static compliance slice check: each shard has its OWN riskgate, so the
  // fleet total is the sum of per-shard limits. If that sum crosses the
  // exchange HFT lines (300 orders/s, 20k/day per account), the config is
  // sliced wrong and the fleet refuses to start — a runtime aggregate needs
  // the Phase-2 risk-budget leases, not a guess.
  {
    std::uint64_t sum_sec = 0, sum_day = 0;
    std::int64_t sum_margin = 0, sum_net = 0;
    for (const int n : ids) {
      const auto l = limits_from(shard_config(fleet, n));
      sum_sec += l.max_actions_per_sec;
      sum_day += l.max_actions_per_day;
      sum_margin += l.max_margin_e4;
      sum_net += l.max_net_notional_e4;
    }
    logf("[fleet] risk slice totals: %llu actions/s, %llu actions/day, margin %.0f CNY, "
         "net notional %.0f CNY across %zu shards\n",
         static_cast<unsigned long long>(sum_sec), static_cast<unsigned long long>(sum_day),
         px_to_cny(sum_margin), px_to_cny(sum_net), ids.size());
    // The 300/s and 20k/day figures are the CSRC program-trading HFT lines;
    // leave a 10% buffer under them for manual/operator flow.
    if (sum_sec > 270 || sum_day > 18'000) {
      logf("[fleet] REFUSING to start: sliced risk limits exceed the compliance envelope "
           "(300 actions/s, 20k/day per account). Lower risk.max_actions_per_sec/day per "
           "shard.\n");
      return 1;
    }
  }
  // The supervisor's own record — shard starts, restarts, exits, and DECODED
  // crash codes — persists even after the console scrolls away.
  if (!log_open_file(fleet.str("log_file", "platd_fleet.log").c_str()))
    logf("[fleet] WARNING: cannot open fleet log file — console only\n");
  log_banner("platd --fleet");
  // The supervisor must outlive Ctrl+C: the console delivers it to every
  // attached process, the children shut down gracefully, the parent reaps.
  std::signal(SIGINT, SIG_IGN);
  std::signal(SIGTERM, SIG_IGN);

  FleetOptions opt;
  opt.restart = fleet.flag("fleet.restart", true);
  opt.max_restarts = static_cast<int>(fleet.i64("fleet.max_restarts", 5));
  opt.backoff_ms = static_cast<int>(fleet.i64("fleet.restart_backoff_ms", 1'000));
  opt.max_backoff_ms = static_cast<int>(fleet.i64("fleet.restart_max_backoff_ms", 30'000));
  opt.quit_grace_ms = static_cast<int>(fleet.i64("fleet.quit_grace_ms", 10'000));

#ifdef _WIN32
  if (!create_job())
    logf("[fleet] WARNING: cannot create JobObject — children will not be bound to "
         "the supervisor\n");
  char exe[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
#else
  char exe[4096] = {};
  const ssize_t exe_n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (exe_n > 0) exe[exe_n] = '\0';
  else std::snprintf(exe, sizeof(exe), "platd");
#endif

  std::vector<Shard> shards;
  shards.reserve(ids.size());
  for (const int n : ids) {
    Shard s;
    s.n = n;
    if (!spawn(s, exe, cfg_path, shard_log_path(n))) {
      logf("[fleet] FAILED to start shard %d\n", n);
      s.next_start = Clock::now() + Millis(opt.backoff_ms);
    }
    shards.push_back(std::move(s));
  }

  logf("[fleet] %zu shards configured (restart=%s, max_restarts=%d). Commands: "
       "`<shard> <cmd> [arg]` (e.g. `2 halt`, `3 halt 4` = halt strategy 4 in shard 3), "
       "`quit` for all; Ctrl+C = graceful fleet stop\n",
       shards.size(), opt.restart ? "on" : "off", opt.max_restarts);

  OpsConsole::instance().start();
  bool quitting = false;
  Clock::time_point quit_deadline{};
  int failures = 0;

  for (;;) {
    std::string line;
    while (OpsConsole::instance().pop(line)) {
      if (line == "quit") {
        if (!quitting) {
          quitting = true;
          quit_deadline = Clock::now() + Millis(opt.quit_grace_ms);
          logf("[fleet] quit: asking every shard to shut down (grace %d ms)\n",
               opt.quit_grace_ms);
        }
        for (const Shard& s : shards)
          if (s.up) write_cmd_file(s.n, "quit");
      } else {
        // Forward the WHOLE rest of the line: shard commands take arguments
        // (`3 halt 4` = halt strategy 4 in shard 3).
        int n = 0, consumed = 0;
        if (std::sscanf(line.c_str(), "%d %n", &n, &consumed) == 1 &&
            consumed < static_cast<int>(line.size()))
          write_cmd_file(n, line.substr(static_cast<std::size_t>(consumed)));
        else
          logf("[fleet] usage: `<shard> <cmd> [arg]` or `quit`\n");
      }
    }

    const auto now = Clock::now();
    bool any_up = false;
    bool any_scheduled = false;
    for (Shard& s : shards) {
      int code = 0;
      if (reap(s, code)) {
        if (code != 0) {
          // A crash exit code carries the exception that killed the shard
          // (the shard's crash handler exits with it) — name it here so the
          // fleet log alone answers "how did it die".
          if (const char* why =
                  exception_code_name(static_cast<std::uint32_t>(code)))
            logf("[fleet] shard %d CRASHED: 0x%08x %s — post-mortem in shard%d.log / "
                 "shard's .jnl.log, minidump platd_crash_*.dmp\n",
                 s.n, static_cast<unsigned>(code), why, s.n);
          else
            logf("[fleet] shard %d exited, code %d\n", s.n, code);
          ++failures;
        } else {
          logf("[fleet] shard %d exited cleanly (code 0)\n", s.n);
        }
        // A clean exit is the normal EOD end of a shard: never restart it.
        if (!quitting && code != 0 && opt.restart && s.restarts < opt.max_restarts) {
          const long long backoff =
              std::min<long long>(static_cast<long long>(opt.backoff_ms) << s.restarts,
                                  opt.max_backoff_ms);
          s.next_start = now + Millis(backoff);
          ++s.restarts;
          logf("[fleet] shard %d unexpected exit — restart %d/%d in %lld ms\n", s.n,
               s.restarts, opt.max_restarts, backoff);
        } else if (!quitting && code != 0) {
          logf("[fleet] shard %d is DOWN (restart disabled or attempts exhausted)\n", s.n);
        }
      }
      if (s.up) {
        any_up = true;
        continue;
      }
      if (!quitting && s.next_start != Clock::time_point{} && now >= s.next_start) {
        s.next_start = Clock::time_point{};
        if (!spawn(s, exe, cfg_path, shard_log_path(s.n))) {
          logf("[fleet] restart of shard %d FAILED\n", s.n);
          if (s.restarts < opt.max_restarts) {
            const long long backoff =
                std::min<long long>(static_cast<long long>(opt.backoff_ms) << s.restarts,
                                    opt.max_backoff_ms);
            s.next_start = now + Millis(backoff);
            ++s.restarts;
          }
        } else {
          any_up = true;
        }
      }
      if (s.next_start != Clock::time_point{}) any_scheduled = true;
    }

    if (quitting) {
      if (!any_up) break;
      if (now >= quit_deadline) {
        logf("[fleet] quit grace elapsed — terminating remaining shards\n");
        kill_all(shards);
        for (Shard& s : shards) s.up = false;  // reaped below / process gone
        break;
      }
    } else if (!any_up && !any_scheduled) {
      break;
    }
    std::this_thread::sleep_for(Millis(200));
  }

  close_job();
  logf("[fleet] all shards down (%d failed)\n", failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace platd
