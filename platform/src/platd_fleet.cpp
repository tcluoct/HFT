// platd_fleet.cpp — the multi-shard supervisor (`platd --fleet fleet.cfg`):
// one command spawns one platd process per `shardN.*` section — each a full
// site with its own core-pinned engine, journal, and CTP session — then
// routes operator commands (`2 halt`, `quit`) and reaps exits. Process
// isolation is the point: one shard crashing cannot take the fleet down.

#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include "plat/core/crash.hpp"  // exception_code_name: decode a dead shard's exit code
#include "plat/core/log.hpp"
#include "plat/engine/live.hpp"  // OpsConsole
#include "platd_app.hpp"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace plat;

namespace platd {

int run_fleet(const std::string& cfg_path) {
  Config fleet;
  if (!fleet.load(cfg_path)) {
    logf("cannot open fleet config %s\n", cfg_path.c_str());
    return 1;
  }
  const std::vector<int> ids = fleet_shard_ids(fleet);
  if (ids.empty()) {
    logf("fleet config has no `shardN.instruments` sections\n");
    return 1;
  }
  // The supervisor's own record — shard starts, exits, and DECODED crash
  // codes — persists even after the console scrolls away.
  if (!log_open_file(fleet.str("log_file", "platd_fleet.log").c_str()))
    logf("[fleet] WARNING: cannot open fleet log file — console only\n");
  log_banner("platd --fleet");
  // The supervisor must outlive Ctrl+C: the console delivers it to every
  // attached process, the children shut down gracefully, the parent reaps.
  std::signal(SIGINT, SIG_IGN);
  std::signal(SIGTERM, SIG_IGN);

#ifdef _WIN32
  char exe[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  struct Kid {
    int n;
    HANDLE proc;
  };
  std::vector<Kid> kids;
  for (const int n : ids) {
    const std::string log_path = "shard" + std::to_string(n) + ".log";
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
    std::string cmd = std::string("\"") + exe + "\" --shard " + std::to_string(n) + " \"" +
                      cfg_path + "\"";
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si,
                        &pi)) {
      logf("[fleet] FAILED to start shard %d\n", n);
    } else {
      CloseHandle(pi.hThread);
      kids.push_back({n, pi.hProcess});
      logf("[fleet] shard %d started (pid %lu) -> %s\n", n,
                   static_cast<unsigned long>(pi.dwProcessId), log_path.c_str());
    }
    if (hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
    if (hnul != INVALID_HANDLE_VALUE) CloseHandle(hnul);
  }

  logf("[fleet] %zu shards up. Commands: `<shard> <cmd> [arg]` (e.g. `2 halt`, "
               "`3 halt 4` = halt strategy 4 in shard 3), `quit` for all; "
               "Ctrl+C = graceful fleet stop\n",
               kids.size());
  OpsConsole::instance().start();
  int failures = 0;
  while (!kids.empty()) {
    std::string line;
    while (OpsConsole::instance().pop(line)) {
      if (line == "quit") {
        for (const Kid& k : kids) write_cmd_file(k.n, "quit");
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
    for (auto it = kids.begin(); it != kids.end();) {
      if (WaitForSingleObject(it->proc, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(it->proc, &code);
        // A crash exit code carries the exception that killed the shard
        // (the shard's crash handler exits with it) — name it here so the
        // fleet log alone answers "how did it die".
        if (const char* why = plat::exception_code_name(code))
          logf("[fleet] shard %d CRASHED: 0x%08lx %s — post-mortem in shard%d.log / "
               "shard's .jnl.log, minidump platd_crash_*.dmp\n",
               it->n, static_cast<unsigned long>(code), why, it->n);
        else
          logf("[fleet] shard %d exited, code %lu\n", it->n,
               static_cast<unsigned long>(code));
        if (code != 0) ++failures;
        CloseHandle(it->proc);
        it = kids.erase(it);
      } else {
        ++it;
      }
    }
    Sleep(200);
  }
#else
  struct Kid {
    int n;
    pid_t pid;
  };
  std::vector<Kid> kids;
  for (const int n : ids) {
    const pid_t pid = fork();
    if (pid == 0) {
      const std::string log_path = "shard" + std::to_string(n) + ".log";
      (void)!freopen(log_path.c_str(), "a", stdout);  // append: keep crash evidence
      (void)!freopen(log_path.c_str(), "a", stderr);
      (void)!freopen("/dev/null", "r", stdin);
      const std::string ns = std::to_string(n);
      execlp("/proc/self/exe", "platd", "--shard", ns.c_str(), cfg_path.c_str(),
             static_cast<char*>(nullptr));
      _exit(127);
    }
    kids.push_back({n, pid});
    logf("[fleet] shard %d started (pid %d)\n", n, pid);
  }
  OpsConsole::instance().start();
  int failures = 0;
  while (!kids.empty()) {
    std::string line;
    while (OpsConsole::instance().pop(line)) {
      if (line == "quit") {
        for (const Kid& k : kids) write_cmd_file(k.n, "quit");
      } else {
        int n = 0, consumed = 0;
        if (std::sscanf(line.c_str(), "%d %n", &n, &consumed) == 1 &&
            consumed < static_cast<int>(line.size()))
          write_cmd_file(n, line.substr(static_cast<std::size_t>(consumed)));
      }
    }
    for (auto it = kids.begin(); it != kids.end();) {
      int status = 0;
      if (waitpid(it->pid, &status, WNOHANG) == it->pid) {
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (WIFSIGNALED(status))
          logf("[fleet] shard %d CRASHED: signal %d (%s) — post-mortem in shard%d.log\n",
               it->n, WTERMSIG(status), strsignal(WTERMSIG(status)), it->n);
        else
          logf("[fleet] shard %d exited, code %d\n", it->n, code);
        if (code != 0) ++failures;
        it = kids.erase(it);
      } else {
        ++it;
      }
    }
    usleep(200'000);
  }
#endif
  logf("[fleet] all shards down (%d failed)\n", failures);
  return failures == 0 ? 0 : 1;
}


}  // namespace platd
