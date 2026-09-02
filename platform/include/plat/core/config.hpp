#pragma once
// plat/core/config.hpp — the platd config file: flat `key = value` lines,
// `#` comments, no sections. Everything the daemon needs to trade one venue
// lives in one reviewable file (§15: config is release-managed, credentials
// come from the deployment environment, not the repo).

#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace plat {

class Config {
 public:
  bool load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
      const auto hash = line.find('#');
      if (hash != std::string::npos) line.erase(hash);
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = trim(line.substr(0, eq));
      const std::string v = trim(line.substr(eq + 1));
      if (!k.empty()) kv_[k] = v;
    }
    return true;
  }

  [[nodiscard]] bool has(const std::string& k) const { return kv_.count(k) != 0; }

  // A present-but-empty value (`key =` with nothing after it) means "use the
  // default", not "the empty string" — consistent with i64/f64 below, so a
  // blank line in a config never shadows a sensible default.
  [[nodiscard]] std::string str(const std::string& k, const std::string& dflt = "") const {
    const auto it = kv_.find(k);
    return it == kv_.end() || it->second.empty() ? dflt : it->second;
  }
  [[nodiscard]] std::int64_t i64(const std::string& k, std::int64_t dflt = 0) const {
    const auto it = kv_.find(k);
    if (it == kv_.end() || it->second.empty()) return dflt;
    return std::stoll(it->second);
  }
  [[nodiscard]] double f64(const std::string& k, double dflt = 0.0) const {
    const auto it = kv_.find(k);
    if (it == kv_.end() || it->second.empty()) return dflt;
    return std::stod(it->second);
  }
  [[nodiscard]] bool flag(const std::string& k, bool dflt = false) const {
    const auto it = kv_.find(k);
    if (it == kv_.end()) return dflt;
    return it->second == "1" || it->second == "true" || it->second == "yes";
  }
  // Comma-separated list.
  [[nodiscard]] std::vector<std::string> list(const std::string& k) const {
    std::vector<std::string> out;
    std::stringstream ss(str(k));
    std::string item;
    while (std::getline(ss, item, ',')) {
      const std::string t = trim(item);
      if (!t.empty()) out.push_back(t);
    }
    return out;
  }

  // Shard view of a fleet config: every `prefix.key` overrides the plain
  // `key`, so shared keys (fronts, credentials, fees) are written once and
  // each shard section carries only its differences.
  [[nodiscard]] Config overlay(const std::string& prefix) const {
    Config out;
    out.kv_ = kv_;
    const std::string p = prefix + ".";
    for (const auto& [k, v] : kv_)
      if (k.rfind(p, 0) == 0) out.kv_[k.substr(p.size())] = v;
    return out;
  }

  // Fill a key only if absent — derived per-shard defaults (journal name,
  // flow dirs) that must differ across shards unless explicitly set.
  void set_default(const std::string& k, const std::string& v) { kv_.emplace(k, v); }

 private:
  static std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
  }

  std::map<std::string, std::string> kv_;
};

}  // namespace plat
