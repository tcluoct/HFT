// platops — the web ops console (§14): monitor strategy PnL / positions /
// trades / risk across every platd shard, and drive operator actions (halt,
// flatten, arm, cancel, manual order, parameter tune, position transfer)
// from a browser.
//
//   platops <config.cfg> [--port N] [--bind ADDR]
//
// Same config file the fleet runs from. Architecture (see platops_app.hpp):
// read side tails each shard's live journal into a Replay-mode mirror Site
// (identical state reconstruction to `platd --replay`, a few ms behind
// live); write side puts operator lines into the existing per-shard cmd
// files, which platd polls at 100 ms and injects as journaled Source
// envelopes. The trading processes are completely untouched: no server, no
// thread, no lock anywhere near the tick path. Run it from platd's working
// directory (journal/cmd paths are relative). Binds loopback by default —
// reach it remotely over an SSH tunnel.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "plat/core/crash.hpp"
#include "plat/core/log.hpp"
#include "plat/core/thread.hpp"
#include "plat/engine/live.hpp"  // live_now_ns: same session clock as platd
#include "plat/ops/http.hpp"
#include "fleet_cfg.hpp"
#include "platops_app.hpp"
#include "platops_json.hpp"
#include "platops_web.hpp"

using namespace plat;
using namespace platops;

namespace {

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true); }

// One printable line, bounded — the only thing we ever put in a cmd file.
bool sanitize_cmd(std::string& cmd) {
  if (cmd.empty() || cmd.size() > 200) return false;
  for (const char c : cmd)
    if (static_cast<unsigned char>(c) < 0x20) return false;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  init_console_utf8();
  install_crash_handlers("platops");
  if (argc < 2) {
    std::printf("usage: platops <config.cfg> [--port N] [--bind ADDR]\n"
                "  Web ops console over a platd (or fleet) config: mirrors every shard's\n"
                "  journal for PnL/positions/orders/risk, and drives the shard cmd files\n"
                "  for operator actions. Run from platd's working directory.\n");
    return 1;
  }
  Config cfg;
  if (!cfg.load(argv[1])) {
    std::printf("platops: cannot open config %s\n", argv[1]);
    return 1;
  }
  std::string bind = cfg.str("ops.bind", "127.0.0.1");
  auto port = static_cast<std::uint16_t>(cfg.i64("ops.port", 8848));
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    else if (a == "--bind" && i + 1 < argc) bind = argv[++i];
  }
  log_open_file(cfg.str("ops.log", "platops.log").c_str());
  log_banner("platops");

  // ---- mirrors: one per fleet shard, or the single standalone site.
  std::vector<std::unique_ptr<MirrorShard>> shards;
  const std::vector<int> ids = fleet_shard_ids(cfg);
  if (ids.empty()) {
    Config c = cfg;
    c.set_default("site_name", "site");
    shards.push_back(std::make_unique<MirrorShard>(c.str("site_name"), c));
  } else {
    for (const int n : ids) {
      Config c = shard_config(cfg, n);
      shards.push_back(std::make_unique<MirrorShard>(c.str("site_name"), c));
    }
  }
  std::map<std::string, std::string> cmd_paths;  // immutable after this point
  for (const auto& sh : shards) cmd_paths[sh->name()] = sh->cmd_path();

  // ---- http: GET / (page), GET /api/state (snapshot), POST /api/cmd.
  std::mutex json_mtx;
  std::string json_state = "{\"now\":\"\",\"shards\":[]}";
  ops::HttpServer server;
  const bool up = server.start(bind, port, [&](const ops::HttpRequest& req) {
    ops::HttpResponse res;
    if (req.method == "GET" && req.path == "/") {
      res.content_type = "text/html; charset=utf-8";
      res.body = kWebPage;
    } else if (req.method == "GET" && req.path == "/api/state") {
      res.content_type = "application/json";
      std::lock_guard lk(json_mtx);
      res.body = json_state;
    } else if (req.method == "POST" && req.path == "/api/cmd") {
      // body = "<shard>\n<command line>"
      const std::size_t nl = req.body.find('\n');
      std::string shard = nl == std::string::npos ? "" : req.body.substr(0, nl);
      std::string cmd = nl == std::string::npos ? "" : req.body.substr(nl + 1);
      const auto it = cmd_paths.find(shard);
      if (it == cmd_paths.end() || !sanitize_cmd(cmd)) {
        res.status = 400;
        res.body = "bad shard or command";
      } else {
        std::FILE* f = std::fopen(it->second.c_str(), "wb");
        if (!f) {
          res.status = 500;
          res.body = "cannot write cmd file";
        } else {
          std::fwrite(cmd.data(), 1, cmd.size(), f);
          std::fclose(f);
          res.body = "ok";
        }
        // platops' own audit line; platd journals the action itself.
        logf("[platops] cmd -> %s: %s\n", shard.c_str(), cmd.c_str());
      }
    } else {
      res.status = 404;
      res.body = "not found";
    }
    return res;
  });
  if (!up) {
    logf("[platops] cannot listen on %s:%u\n", bind.c_str(), port);
    return 1;
  }
  logf("[platops] serving http://%s:%u — %zu shard mirror(s)\n", bind.c_str(), port,
       shards.size());
  for (const auto& sh : shards)
    logf("[platops]   %s: journal %s, cmd %s\n", sh->name().c_str(), sh->jnl_path().c_str(),
         sh->cmd_path().c_str());
  std::signal(SIGINT, &on_sigint);

  // ---- monitor loop: tail journals, refresh the JSON snapshot.
  Nanos last_json = 0;
  while (!g_stop.load(std::memory_order_relaxed)) {
    bool any = false;
    for (auto& sh : shards) any |= sh->poll();
    const Nanos now = live_now_ns();
    if (any || now - last_json > 250 * kNsPerMs) {
      last_json = now;
      std::string j = build_state_json(shards, now);
      std::lock_guard lk(json_mtx);
      json_state = std::move(j);
    }
    if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  logf("[platops] stop requested — shutting down\n");
  server.stop();
  return 0;
}
