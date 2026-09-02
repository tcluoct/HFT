// platd — the production site daemon (ARCHITECTURE.md §17, Phase 1):
// one CTP counter, one venue's instruments, the full deterministic stack
// (books → strategies → riskgate → gateway; OMS/PnL/compliance on the same
// canonical stream), journaled for replay/recovery.
//
// This file is command-line dispatch only; the daemon lives in its sibling
// translation units (see platd_app.hpp for the map).
//
// Credentials note (§15): the config file carries the account/front data;
// keep it out of version control and readable only by the trading user.

#include <cstdio>
#include <cstring>

#include "plat/core/config.hpp"
#include "plat/core/crash.hpp"
#include "plat/core/log.hpp"
#include "plat/core/thread.hpp"
#include "plat/replay/journal_dump.hpp"
#include "platd_app.hpp"

using namespace platd;

namespace {

bool load_or_die(plat::Config& cfg, const char* path) {
  if (cfg.load(path)) return true;
  plat::logf("cannot open config %s\n", path);
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  plat::init_console_utf8();  // logs contain em-dashes / CTP error strings may be 中文
  plat::install_crash_handlers("platd");  // abnormal death → named reason + stack + minidump
  if (argc >= 3 && std::strcmp(argv[1], "--fleet") == 0) {
    return run_fleet(argv[2]);
  }
  if (argc >= 4 && std::strcmp(argv[1], "--shard") == 0) {
    plat::Config fleet;
    if (!load_or_die(fleet, argv[3])) return 1;
    const int n = std::atoi(argv[2]);
    const bool resume = argc >= 5 && std::strcmp(argv[4], "--resume") == 0;
    plat::logf("[platd] running as shard %d of %s%s\n", n, argv[3],
               resume ? " (FORCED RESUME)" : "");
    return run_live(shard_config(fleet, n), resume);
  }
  if (argc >= 3 && std::strcmp(argv[1], "--dump") == 0) {
    return plat::dump_journal(argv[2], argc >= 4 ? std::strtoull(argv[3], nullptr, 10) : 0);
  }
  if (argc >= 3 && std::strcmp(argv[1], "--check") == 0) {
    plat::Config cfg;
    if (!load_or_die(cfg, argv[2])) return 1;
    return run_check(cfg);
  }
  if (argc >= 4 && std::strcmp(argv[1], "--replay") == 0) {
    plat::Config cfg;
    if (!load_or_die(cfg, argv[2])) return 1;
    return run_replay(cfg, argv[3]);
  }
  if (argc >= 2 && argv[1][0] != '-') {
    plat::Config cfg;
    if (!load_or_die(cfg, argv[1])) return 1;
    const bool resume = argc >= 3 && std::strcmp(argv[2], "--resume") == 0;
    return run_live(cfg, resume);
  }
  plat::logf(
      "usage:\n"
      "  platd <config.cfg>                 live session (single site). Start mode is\n"
      "                                     AUTOMATIC: an unsealed same-day journal\n"
      "                                     auto-resumes (replay + reconcile + halted\n"
      "                                     for review); sealed/old journals are\n"
      "                                     archived and the session starts fresh\n"
      "  platd --fleet <fleet.cfg>          spawn + supervise one process per shard\n"
      "                                     (each shard auto-decides fresh/resume)\n"
      "  platd --shard N <fleet.cfg> [--resume]  run as one shard of the fleet\n"
      "  platd <config.cfg> --resume        force resume (fails if nothing to resume)\n"
      "  platd --check <config.cfg>         validate config + CTP linkage\n"
      "  platd --replay <config.cfg> <jnl>  deterministic replay/recovery drill\n"
      "  platd --dump <jnl> [count]         human-readable journal listing\n");
  return 1;
}
