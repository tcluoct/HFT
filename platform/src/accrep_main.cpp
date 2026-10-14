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
#include <vector>

#include "plat/core/config.hpp"
#include "plat/core/crash.hpp"
#include "plat/core/log.hpp"
#include "plat/core/thread.hpp"
#include "plat/rep/docx_fill.hpp"
#include "plat/rep/md5.hpp"
#include "plat/rep/shot_render.hpp"
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

// Every screenshot placeholder accrep can fill from a `shot.<key> = <path>`
// config line: the file at that path is a plain UTF-8 text excerpt of REAL
// captured evidence (platd/CTP log lines, `certutil` output, a directory
// listing, ...) — accrep only re-renders that text as an image, it never
// invents content. Missing `shot.<key>` just leaves that placeholder as
// shipped, same as every other optional field in this tool.
struct ShotSpec {
  const char* cfg_key;
  const char* title;    // header line drawn inside the rendered image
  std::string label;     // exact docx placeholder text (verified byte-exact
                          // against the template — see docx_fill.hpp's
                          // comment on the label-matching convention)
};

const std::vector<ShotSpec>& shot_specs() {
  static const std::vector<ShotSpec> specs = {
      {"login", "\xE7\xA9\xBF\xE9\x80\x8F\xE5\xBC\x8F\xE7\x99\xBB\xE5\xBD\x95 — platd \xE5\xAE\x9E"
                "\xE6\x97\xB6\xE6\x97\xA5\xE5\xBF\x97",
       "\xE7\xA9\xBF\xE9\x80\x8F\xE5\xBC\x8F\xE7\x99\xBB\xE5\xBD\x95\xE6\x88\xAA\xE5\x9B\xBE"},
      {"funds_verify", "\xE8\xB5\x84\xE9\x87\x91\xE8\xB4\xA6\xE5\x8F\xB7\xE9\xAA\x8C\xE8\xAF\x81\xE9\x80\x9A\xE8\xBF\x87",
       "\x33\x2E\x32\x2E\x31\x2E\x31\x20\xE8\xB5\x84\xE9\x87\x91\xE8\xB4\xA6\xE5\x8F\xB7\xE9"
       "\xAA\x8C\xE8\xAF\x81\xE9\x80\x9A\xE8\xBF\x87\xE6\x88\xAA\xE5\x9B\xBE"},
      {"query", "\xE6\x9F\xA5\xE8\xAF\xA2 — \xE5\x90\x88\xE7\xBA\xA6\xE5\x8F\x82\xE6\x95\xB0\xE6\x9F\xA5\xE8\xAF\xA2",
       "\x33\x2E\x32\x2E\x31\x2E\x32\x20\xE6\x9F\xA5\xE8\xAF\xA2\xE6\x88\xAA\xE5\x9B\xBE"},
      {"order_fill", "\xE6\x8A\xA5\xE5\x8D\x95\xE6\x88\x90\xE4\xBA\xA4",
       "\xE6\x8A\xA5\xE5\x8D\x95\xE6\x88\x90\xE4\xBA\xA4\xE6\x88\xAA\xE5\x9B\xBE"},
      {"cancel", "\xE6\x92\xA4\xE5\x8D\x95", "\xE6\x92\xA4\xE5\x8D\x95\xE6\x88\xAA\xE5\x9B\xBE"},
      {"close", "\xE5\xB9\xB3\xE4\xBB\x93", "\xE5\xB9\xB3\xE4\xBB\x93\xE6\x88\xAA\xE5\x9B\xBE"},
      {"conn_status", "\xE8\xBF\x9E\xE6\x8E\xA5\xE7\x8A\xB6\xE6\x80\x81",
       "\xE8\xBF\x9E\xE6\x8E\xA5\xE7\x8A\xB6\xE6\x80\x81\xE6\x88\xAA\xE5\x9B\xBE\xEF\xBC\x88\xE8"
       "\xBF\x9E\xE6\x8E\xA5\xE6\xAD\xA3\xE5\xB8\xB8\xE3\x80\x81\xE8\xBF\x9E\xE6\x8E\xA5\xE5\xBC"
       "\x82\xE5\xB8\xB8\xE3\x80\x81\xE9\x87\x8D\xE6\x96\xB0\xE8\xBF\x9E\xE6\x8E\xA5\xEF\xBC\x89"},
      {"order_cancel_count", "\xE6\x8A\xA5\xE5\x8D\x95\xE7\xAC\x94\xE6\x95\xB0\xE5\x92\x8C\xE6\x92"
                             "\xA4\xE5\x8D\x95\xE7\xAC\x94\xE6\x95\xB0",
       "\xE6\x8A\xA5\xE5\x8D\x95\xE7\xAC\x94\xE6\x95\xB0\xE5\x92\x8C\xE6\x92\xA4\xE5\x8D\x95\xE7"
       "\xAC\x94\xE6\x95\xB0\xE6\x88\xAA\xE5\x9B\xBE"},
      {"order_threshold_alert", "\xE6\x8A\xA5\xE5\x8D\x95\xE9\x98\x88\xE5\x80\xBC\xE9\xA2\x84\xE8\xAD\xA6",
       "\xE6\x8A\xA5\xE5\x8D\x95\xE7\xAC\x94\xE6\x95\xB0\xE8\xAE\xBE\xE7\xBD\xAE\xE6\x88\xAA\xE5"
       "\x9B\xBE\xE5\x92\x8C\xE8\xA7\xA6\xE5\x8F\x91\xE9\xA2\x84\xE8\xAD\xA6\xE6\x88\xAA\xE5\x9B\xBE"},
      {"cancel_threshold_alert", "\xE6\x92\xA4\xE5\x8D\x95\xE9\x98\x88\xE5\x80\xBC\xE9\xA2\x84\xE8\xAD\xA6",
       "\xE6\x92\xA4\xE5\x8D\x95\xE7\xAC\x94\xE6\x95\xB0\xE8\xAE\xBE\xE7\xBD\xAE\xE6\x88\xAA\xE5"
       "\x9B\xBE\xE5\x92\x8C\xE8\xA7\xA6\xE5\x8F\x91\xE9\xA2\x84\xE8\xAD\xA6\xE6\x88\xAA\xE5\x9B\xBE"},
      {"instruction_check", "\xE4\xBA\xA4\xE6\x98\x93\xE6\x8C\x87\xE4\xBB\xA4\xE6\xA3\x80\xE6\x9F\xA5",
       "\xE4\xBA\xA4\xE6\x98\x93\xE6\x8C\x87\xE4\xBB\xA4\xE6\xA3\x80\xE6\x9F\xA5\xE6\x88\xAA\xE5\x9B\xBE"},
      {"error_msg", "\xE9\x94\x99\xE8\xAF\xAF\xE6\x8F\x90\xE7\xA4\xBA", "\xE9\x94\x99\xE8\xAF\xAF"
       "\xE6\x8F\x90\xE7\xA4\xBA\xE6\x88\xAA\xE5\x9B\xBE"},
      {"pause_strategy", "\xE6\x9A\x82\xE5\x81\x9C\xE4\xBA\xA4\xE6\x98\x93\xE7\xAD\x96\xE7\x95\xA5\xE6\x89\xA7\xE8\xA1\x8C",
       "\xE6\x9A\x82\xE5\x81\x9C\xE4\xBA\xA4\xE6\x98\x93\xE7\xAD\x96\xE7\x95\xA5\xE6\x89\xA7\xE8"
       "\xA1\x8C\xE6\x88\xAA\xE5\x9B\xBE"},
      {"restrict_permission", "\xE9\x99\x90\xE5\x88\xB6\xE8\xB4\xA6\xE5\x8F\xB7\xE4\xBA\xA4\xE6\x98\x93\xE6\x9D\x83\xE9\x99\x90",
       "\xE9\x99\x90\xE5\x88\xB6\xE8\xB4\xA6\xE5\x8F\xB7\xE4\xBA\xA4\xE6\x98\x93\xE6\x9D\x83\xE9"
       "\x99\x90\xE6\x88\xAA\xE5\x9B\xBE"},
      {"force_logout", "\xE5\xBC\xBA\xE5\x88\xB6\xE8\xB4\xA6\xE5\x8F\xB7\xE7\x99\xBB\xE5\x87\xBA",
       "\xE5\xBC\xBA\xE5\x88\xB6\xE8\xB4\xA6\xE5\x8F\xB7\xE7\x99\xBB\xE5\x87\xBA\xE6\x88\xAA\xE5\x9B\xBE"},
      {"trade_log", "\xE4\xBA\xA4\xE6\x98\x93\xE6\x97\xA5\xE5\xBF\x97",
       "\xE4\xBA\xA4\xE6\x98\x93\xE6\x97\xA5\xE5\xBF\x97\xE6\x88\xAA\xE5\x9B\xBE"},
      {"system_log", "\xE7\xB3\xBB\xE7\xBB\x9F\xE8\xBF\x90\xE8\xA1\x8C\xE6\x97\xA5\xE5\xBF\x97",
       "\xE7\xB3\xBB\xE7\xBB\x9F\xE8\xBF\x90\xE8\xA1\x8C\xE6\x97\xA5\xE5\xBF\x97\xE6\x88\xAA\xE5\x9B\xBE"},
      {"monitor_log", "\xE7\x9B\x91\xE6\xB5\x8B\xE8\xAE\xB0\xE5\xBD\x95\xE6\x97\xA5\xE5\xBF\x97",
       "\xE7\x9B\x91\xE6\xB5\x8B\xE8\xAE\xB0\xE5\xBD\x95\xE6\x97\xA5\xE5\xBF\x97\xE6\x88\xAA\xE5\x9B\xBE"},
      {"error_log", "\xE9\x94\x99\xE8\xAF\xAF\xE6\x8F\x90\xE7\xA4\xBA\xE8\xAE\xB0\xE5\xBD\x95",
       "\xE9\x94\x99\xE8\xAF\xAF\xE6\x8F\x90\xE7\xA4\xBA\xE8\xAE\xB0\xE5\xBD\x95\xE6\x88\xAA\xE5\x9B\xBE"},
      {"access_security", "\xE6\x8E\xA5\xE5\x85\xA5\xE5\xAE\x89\xE5\x85\xA8\xE6\x80\xA7",
       "\xE6\x8E\xA5\xE5\x85\xA5\xE5\xAE\x89\xE5\x85\xA8\xE6\x80\xA7\xE6\x88\xAA\xE5\x9B\xBE\x20"},
      {"dir_listing", "\xE8\xBD\xAF\xE4\xBB\xB6\xE6\x96\x87\xE4\xBB\xB6\xE7\x9B\xAE\xE5\xBD\x95",
       "\xE8\xBD\xAF\xE4\xBB\xB6\xE6\x96\x87\xE4\xBB\xB6\xE7\x9B\xAE\xE5\xBD\x95"},
      {"md5_tool", "MD5", "\xE8\xBD\xAF\xE4\xBB\xB6\xE5\x8E\x8B\xE7\xBC\xA9\xE5\x8C\x85\xEF\xBC\x88"
       "\xE6\x88\x96\xE5\xAE\x89\xE8\xA3\x85\xE5\x8C\x85\xEF\xBC\x89\xE7\x9A\x84\x4D\x44\x35\xE7"
       "\xA0\x81\xE6\x88\xAA\xE5\x9B\xBE"},
  };
  return specs;
}

// Reads `shot.<key>` from cfg (a path to a UTF-8 text file of real captured
// evidence), renders it, and appends a Screenshot to `f.screenshots`. Missing
// or unreadable inputs are skipped, not errors — same optionality as every
// other field this tool fills only when told to.
void collect_screenshots(const Config& cfg, const std::filesystem::path& shots_dir,
                          AccessReportFields& f) {
  for (const ShotSpec& spec : shot_specs()) {
    const std::string src = cfg.str(std::string("shot.") + spec.cfg_key);
    if (src.empty()) continue;
    const std::filesystem::path src_path = path_from_utf8(src);
    const std::string body = docx_detail::read_file(src_path);
    // A .png is a real window capture — embedded as-is, pixel size from IHDR.
    if (src_path.extension() == ".png") {
      if (body.size() < 24 || body.compare(1, 3, "PNG") != 0) {
        logf("[accrep] shot.%s: \"%s\" is not a PNG — skipped\n", spec.cfg_key, src.c_str());
        continue;
      }
      const auto be32 = [&](std::size_t o) {
        return (static_cast<unsigned char>(body[o]) << 24) |
               (static_cast<unsigned char>(body[o + 1]) << 16) |
               (static_cast<unsigned char>(body[o + 2]) << 8) |
               static_cast<unsigned char>(body[o + 3]);
      };
      const int w = be32(16), h = be32(20);
      f.screenshots.push_back({spec.label, src_path, w, h});
      logf("[accrep] shot.%s: window capture %dx%d \"%s\"\n", spec.cfg_key, w, h, src.c_str());
      continue;
    }
    if (body.empty()) {
      logf("[accrep] shot.%s: could not read \"%s\" — skipped\n", spec.cfg_key, src.c_str());
      continue;
    }
    const std::filesystem::path out_png =
        shots_dir / (std::string(spec.cfg_key) + ".png");
    const auto dims = render_text_shot(spec.title, body, out_png);
    if (!dims) continue;  // render_text_shot already logged why
    f.screenshots.push_back({spec.label, out_png, dims->width_px, dims->height_px});
    logf("[accrep] shot.%s: rendered %dx%d from \"%s\"\n", spec.cfg_key, dims->width_px,
         dims->height_px, src.c_str());
  }
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

  const std::filesystem::path shots_dir =
      path_from_utf8(output_path).parent_path() / "accrep_shots";
  collect_screenshots(cfg, shots_dir, f);

  const bool ok =
      fill_access_report(path_from_utf8(template_path), path_from_utf8(output_path), f);

  logf("[accrep] ---- still needs a person before this can be filed ----\n");
  logf("[accrep]  * any screenshot placeholder you didn't set `shot.<key>` for — see\n"
       "[accrep]    accrep.example.cfg for the full list of keys\n");
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
