#pragma once
// plat/engine/fingerprint.hpp — session identity for safe recovery/replay.
//
// A journal is only meaningful against the exact composition that produced
// it: the build, the desk's strategy pack (bank order is deterministic state)
// and the effective config (strategy instances, risk limits, fees, hosted
// symbols). `run_live` writes a `<journal>.session` sidecar before the session
// opens; resume and `--replay` refuse when the current process would replay
// the record against a different composition. The crashed journal is unsealed
// (no stream hash), so without this check a rebuilt binary or edited config
// would resume into a silently different state.
//
// Credentials are deliberately NOT part of the fingerprint: only behavior
// (composition + limits + universe) is hashed.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "plat/core/hash.hpp"
#include "plat/core/types.hpp"
#include "plat/engine/pack.hpp"
#include "plat/ref/fees.hpp"
#include "plat/ref/instrument.hpp"
#include "plat/risk/limits.hpp"

#ifndef HFT_BUILD_ID
#define HFT_BUILD_ID "dev"
#endif

namespace plat {

struct SessionFingerprint {
  std::uint64_t build = 0;
  std::uint64_t pack = 0;
  std::uint64_t config = 0;
  std::uint32_t day = 0;
  SiteId site = 0;
};

inline std::uint64_t build_fingerprint() noexcept {
  StateHash h;
  for (const char* p = HFT_BUILD_ID; *p; ++p) h.mix(static_cast<std::uint64_t>(*p));
  return h.value();
}

// Effective behavior of one site: the configured strategy instances (names,
// types, ids, accounts), fee schedule, hosted symbols and the action-rate cap.
// Other risk limits are deliberately not hashed: the session journals its
// initial limits as the first Source event and every `reload` after it, so the
// journal/snapshot — not the current config file — is the authority for
// replay. `max_actions_per_sec` is included because it sizes the rate ring at
// construction and is restart-only; a config change there must block recovery.
inline std::uint64_t config_fingerprint(const std::vector<StratSpec>& specs,
                                        const FeeSchedule& fees,
                                        const InstrumentTable& table,
                                        const std::vector<InstrumentId>& hosted,
                                        std::uint32_t max_actions_per_sec,
                                        bool shared_account_risk = false) {
  StateHash h;
  h.mix(specs.size());
  for (const auto& s : specs) {
    for (const char c : s.name) h.mix(static_cast<std::uint64_t>(c));
    h.mix(s.kind);
    h.mix(s.id);
    h.mix(s.account);
  }
  h.mix(max_actions_per_sec);
  if (shared_account_risk) h.mix(1u);
  h.mix(fees.stock_commission_bps);
  h.mix(fees.stamp_duty_sell_bps);
  h.mix(fees.transfer_fee_bps);
  h.mix(fees.fut_per_lot_e4);
  h.mix(fees.fut_per_lot_close_today_e4);
  h.mix(fees.fut_notional_bps);
  h.mix(hosted.size());
  for (const InstrumentId i : hosted) {
    const Instrument& in = table.at(i);
    for (const char c : in.sym()) h.mix(static_cast<std::uint64_t>(c));
    h.mix(static_cast<std::uint64_t>(in.venue));
    h.mix(in.tick);
    h.mix(in.multiplier);
  }
  return h.value();
}

inline constexpr std::uint64_t kSessionMagic = 0x504C4154534553ull;  // "PLATSES"
inline constexpr std::uint32_t kSessionVersion = 1;

// Binary sidecar, written with explicit fields (no struct padding).
inline bool save_session_fingerprint(const std::string& path, const SessionFingerprint& fp) {
  const std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const auto put = [f](const void* p, std::size_t n) { return std::fwrite(p, 1, n, f) == n; };
  bool ok = put(&kSessionMagic, sizeof(kSessionMagic)) &&
            put(&kSessionVersion, sizeof(kSessionVersion)) &&
            put(&fp.build, sizeof(fp.build)) && put(&fp.pack, sizeof(fp.pack)) &&
            put(&fp.config, sizeof(fp.config)) && put(&fp.day, sizeof(fp.day)) &&
            put(&fp.site, sizeof(fp.site));
  ok = (std::fflush(f) == 0) && ok;
  ok = (std::fclose(f) == 0) && ok;
  if (!ok) return false;
  std::remove(path.c_str());
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

inline bool load_session_fingerprint(const std::string& path, SessionFingerprint& fp) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  const auto get = [f](void* p, std::size_t n) { return std::fread(p, 1, n, f) == n; };
  std::uint64_t magic = 0;
  std::uint32_t version = 0;
  bool ok = get(&magic, sizeof(magic)) && get(&version, sizeof(version)) &&
            magic == kSessionMagic && version == kSessionVersion &&
            get(&fp.build, sizeof(fp.build)) && get(&fp.pack, sizeof(fp.pack)) &&
            get(&fp.config, sizeof(fp.config)) && get(&fp.day, sizeof(fp.day)) &&
            get(&fp.site, sizeof(fp.site));
  std::fclose(f);
  return ok;
}

inline bool same_fingerprint(const SessionFingerprint& a, const SessionFingerprint& b) noexcept {
  return a.build == b.build && a.pack == b.pack && a.config == b.config && a.day == b.day &&
         a.site == b.site;
}

}  // namespace plat
