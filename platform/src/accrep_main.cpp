// accrep — fills the CSRC/exchange program-trading "external system access
// report" (附件95-3：外部系统接入报告（程序化）.docx) from a config file:
// software name/version, test account, APPID, local IP/MAC (auto-detected
// unless overridden), the CTP counter, the feature-scope checkboxes, and a
// computed MD5 line for the deployed binary.
//
// What this tool does NOT do: connect to CTP, or produce any of the ~20
// screenshots the report also requires (穿透式登录, order/cancel/close,
// threshold-alert triggers, error handling, connection-state transitions,
// logs, file-directory, MD5 tool, ...). Those are evidence of one specific
// live test session and have to be captured by whoever runs it — every
// screenshot placeholder in the template is left exactly as shipped for
// that. See the reminder accrep prints at the end of every run.
//
//   accrep <config.cfg>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

#include "plat/core/config.hpp"
#include "plat/core/crash.hpp"
#include "plat/core/log.hpp"
#include "plat/core/thread.hpp"
#include "plat/rep/docx_fill.hpp"
#include "plat/rep/md5.hpp"
#include "plat/rep/sysinfo.hpp"
#include "plat/rep/win_paths.hpp"

using namespace plat;
using namespace plat::rep;

namespace {

std::string now_string() {
  const std::time_t tt = std::time(nullptr);
  std::tm lt{};
#ifdef _WIN32
  localtime_s(&lt, &tt);
#else
  localtime_r(&tt, &lt);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", lt.tm_year + 1900,
                lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
  return buf;
}

int run(const Config& cfg) {
#ifndef _WIN32
  (void)cfg;
  logf("[accrep] Windows-only (fills the .docx via Expand-Archive/Compress-Archive)\n");
  return 1;
#else
  AccessReportFields f;
  f.software_name = cfg.str("software_name");
  f.software_version = cfg.str("software_version");
  f.test_account = cfg.str("test_account", cfg.str("user_id"));
  f.test_time = cfg.str("test_time", now_string());
  f.app_id = cfg.str("app_id");
  f.trading_system = cfg.str("trading_system", "CTP");
  f.relay_app_id = cfg.str("relay_app_id");
  f.relay_proxy_ip = cfg.str("relay_proxy_ip");

  f.local_ip = cfg.str("local_ip");
  f.local_mac = cfg.str("local_mac");
  if (f.local_ip.empty() || f.local_mac.empty()) {
    const auto nets = local_net_info();
    if (nets.empty()) {
      logf("[accrep] could not auto-detect a local network adapter — set local_ip / "
           "local_mac in the config\n");
    } else {
      if (f.local_ip.empty()) f.local_ip = nets.front().ip;
      if (f.local_mac.empty()) f.local_mac = nets.front().mac;
      logf("[accrep] auto-detected local_ip=%s local_mac=%s (adapter \"%s\") — override in "
           "the config if this is the wrong NIC or the terminal isn't this machine\n",
           f.local_ip.c_str(), f.local_mac.c_str(), nets.front().name.c_str());
    }
  }

  // Every scope checkbox defaults to OFF: this tool has no way to confirm
  // the running software actually implements a scope item, and ticking a
  // box that doesn't hold true misstates the filing. Set `scope.<name> = 1`
  // in the config for each item you've actually verified in a real test
  // session.
  f.scope_penetration = cfg.flag("scope.penetration");
  f.scope_interface = cfg.flag("scope.interface");
  f.scope_program_trading = cfg.flag("scope.program_trading");
  f.scope_access_security = cfg.flag("scope.access_security");
  f.scope_sub_account = cfg.flag("scope.sub_account");

  const std::string target_binary = cfg.str("target_binary");
  if (!target_binary.empty()) {
    const std::filesystem::path bin_path = path_from_utf8(target_binary);
    std::string hex;
    std::uint64_t size = 0;
    if (md5_file_hex(bin_path, hex, size)) {
      char note[512];
      std::snprintf(note, sizeof(note), "MD5\xEF\xBC\x88%s\xEF\xBC\x8C%llu \xE5\xAD\x97\xE8\x8A"
                                        "\x82\xEF\xBC\x8C%s \xE8\xAE\xA1\xE7\xAE\x97\xEF\xBC\x89"
                                        "\xEF\xBC\x9A%s",  // MD5（<file>，<n> 字节，<time> 计算）：<hex>
                    target_binary.c_str(), static_cast<unsigned long long>(size),
                    now_string().c_str(), hex.c_str());
      f.md5_note = note;
      logf("[accrep] %s\n", note);
    } else {
      logf("[accrep] could not read target_binary '%s' for the MD5 line\n",
           target_binary.c_str());
    }
  }

  const std::string template_path = cfg.str("template");
  const std::string output_path =
      cfg.str("output", "\xE9\x99\x84\xE4\xBB\xB6"  // \x escapes are greedy — "95-3" must start
                        "95-3\xEF\xBC\x9A\xE5\xA4\x96\xE9\x83\xA8\xE7\xB3"  // a fresh literal
                        "\xBB\xE7\xBB\x9F\xE6\x8E\xA5\xE5\x85\xA5\xE6\x8A\xA5\xE5\x91\x8A\xEF\xBC"
                        "\x88\xE7\xA8\x8B\xE5\xBA\x8F\xE5\x8C\x96\xEF\xBC\x89_filled.docx");
  if (template_path.empty()) {
    logf("[accrep] config: missing required key `template` (path to the .docx form)\n");
    return 1;
  }

  const bool ok =
      fill_access_report(path_from_utf8(template_path), path_from_utf8(output_path), f);

  logf("[accrep] ---- still needs a person before this can be filed ----\n");
  logf("[accrep]  * every screenshot placeholder (login, connectivity, orders/cancels,\n"
       "[accrep]    threshold alerts, error handling, emergency controls, logs,\n"
       "[accrep]    file-directory, MD5 tool, ...)\n");
  logf("[accrep]  * the scope checkboxes, unless set in the config (scope.* = 1)\n");
  logf("[accrep]  * 盖章/签字 and the date at the bottom of the report\n");
  return ok ? 0 : 1;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  init_console_utf8();
  install_crash_handlers("accrep");
  if (argc < 2) {
    logf("usage: accrep <config.cfg>\n");
    return 1;
  }
  Config cfg;
  if (!cfg.load(argv[1])) {
    logf("cannot open config %s\n", argv[1]);
    return 1;
  }
  return run(cfg);
}
