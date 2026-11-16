#pragma once
// plat/engine/snapshot.hpp — periodic checkpoint + suffix replay (§11.1/§11.2).
//
// The journal remains the complete audit record; a snapshot is a recovery
// accelerator. Every `journal.snapshot_sec` the driver writes the site's
// deterministic state (Site::save_state) plus the canonical-stream index it
// was taken at. Resume can then:
//
//   1. copy the journal's envelope prefix (no dispatch) to re-create the
//      complete new journal and its stream hash,
//   2. load the snapshot state instead of re-dispatching the prefix,
//   3. dispatch only the Source suffix (usually seconds/minutes of events).
//
// The snapshot is an optimization, never a substitute: a missing, torn,
// wrong-day or wrong-composition snapshot falls back to a full replay. It is
// written to a temp file + renamed, checksummed, and bound to the session
// fingerprint so it cannot be applied to another build/desk/config.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "plat/core/hash.hpp"
#include "plat/engine/fingerprint.hpp"

namespace plat {

inline constexpr std::uint64_t kSnapshotMagic = 0x504C4154534E4150ull;  // "PLATSNAP"
inline constexpr std::uint32_t kSnapshotVersion = 1;

// Header written field-by-field (no struct padding / ABI dependence).
struct SnapshotInfo {
  std::uint64_t index = 0;  // canonical-stream envelope index of the state
};

template <typename SiteT>
bool save_snapshot(SiteT& site, const std::string& path, const SessionFingerprint& fp) {
  std::vector<std::uint8_t> state;
  site.save_state(state);
  const std::uint64_t index = site.stream().next_seq();
  const std::uint64_t state_size = state.size();
  StateHash h;
  h.mix(index);
  h.mix(state_size);
  h.mix(fp.build);
  h.mix(fp.pack);
  h.mix(fp.config);
  h.mix(fp.day);
  h.mix(fp.site);
  h.mix_bytes(state.data(), state.size());
  const std::uint64_t checksum = h.value();

  const std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const auto put = [f](const void* p, std::size_t n) { return std::fwrite(p, 1, n, f) == n; };
  bool ok = put(&kSnapshotMagic, sizeof(kSnapshotMagic)) &&
            put(&kSnapshotVersion, sizeof(kSnapshotVersion)) && put(&index, sizeof(index)) &&
            put(&state_size, sizeof(state_size)) && put(&checksum, sizeof(checksum)) &&
            put(&fp.build, sizeof(fp.build)) && put(&fp.pack, sizeof(fp.pack)) &&
            put(&fp.config, sizeof(fp.config)) && put(&fp.day, sizeof(fp.day)) &&
            put(&fp.site, sizeof(fp.site)) &&
            (state.empty() || std::fwrite(state.data(), 1, state.size(), f) == state.size());
  ok = (std::fflush(f) == 0) && ok;
  ok = (std::fclose(f) == 0) && ok;
  if (!ok) {
    std::remove(tmp.c_str());
    return false;
  }
  std::remove(path.c_str());
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// Loads the snapshot into `site` and returns its journal index. False means
// "no usable snapshot" — the caller must fall back to a full replay. The
// fingerprint, checksum and `index <= max_index` are checked before any state
// is touched, so a rejected snapshot leaves the site untouched.
template <typename SiteT>
bool load_snapshot(SiteT& site, const std::string& path, const SessionFingerprint& fp,
                   SnapshotInfo& info, std::uint64_t max_index = ~0ull) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  const auto get = [f](void* p, std::size_t n) { return std::fread(p, 1, n, f) == n; };
  std::uint64_t magic = 0, index = 0, state_size = 0, checksum = 0;
  std::uint32_t version = 0;
  SessionFingerprint recorded;
  bool ok = get(&magic, sizeof(magic)) && get(&version, sizeof(version)) &&
            get(&index, sizeof(index)) && get(&state_size, sizeof(state_size)) &&
            get(&checksum, sizeof(checksum)) && get(&recorded.build, sizeof(recorded.build)) &&
            get(&recorded.pack, sizeof(recorded.pack)) &&
            get(&recorded.config, sizeof(recorded.config)) &&
            get(&recorded.day, sizeof(recorded.day)) &&
            get(&recorded.site, sizeof(recorded.site)) && magic == kSnapshotMagic &&
            version == kSnapshotVersion && same_fingerprint(recorded, fp) &&
            index <= max_index && state_size <= (1ull << 30);  // corrupt/hostile guard
  std::vector<std::uint8_t> state;
  if (ok) {
    state.resize(static_cast<std::size_t>(state_size));
    ok = state.empty() || std::fread(state.data(), 1, state.size(), f) == state.size();
  }
  std::fclose(f);
  if (!ok) return false;
  StateHash h;
  h.mix(index);
  h.mix(state_size);
  h.mix(fp.build);
  h.mix(fp.pack);
  h.mix(fp.config);
  h.mix(fp.day);
  h.mix(fp.site);
  h.mix_bytes(state.data(), state.size());
  if (h.value() != checksum) return false;
  // Site::save_state starts with the canonical-stream next_seq: validate it
  // BEFORE loading, so a reject leaves the perfectly replayable site untouched.
  if (state.size() < sizeof(std::uint64_t)) return false;
  std::uint64_t stored_next_seq = 0;
  std::memcpy(&stored_next_seq, state.data(), sizeof(stored_next_seq));
  if (stored_next_seq != index) return false;
  site.load_state(state);
  info.index = index;
  return true;
}

}  // namespace plat
