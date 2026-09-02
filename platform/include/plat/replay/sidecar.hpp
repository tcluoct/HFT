#pragma once
// plat/replay/sidecar.hpp — the instrument-table sidecar (`<journal>.ref`).
// Written at session start, read back by replay/resume/dump: InstrumentIds
// inside a journal are indexes into the exact per-day table the session
// traded with (§9.6), so any journal consumer must reconstruct THAT table,
// never re-query a counter whose answers may have changed.

#include <cstdio>
#include <string>
#include <vector>

#include "plat/core/log.hpp"
#include "plat/ref/instrument.hpp"

namespace plat {

inline bool save_sidecar(const std::string& jnl_path, const InstrumentTable& table) {
  std::vector<std::uint8_t> blob;
  table.save(blob);
  std::FILE* f = std::fopen((jnl_path + ".ref").c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(blob.data(), 1, blob.size(), f) == blob.size();
  std::fclose(f);
  return ok;
}

inline bool load_sidecar(const std::string& jnl_path, InstrumentTable& table,
                         bool quiet = false) {
  std::FILE* f = std::fopen((jnl_path + ".ref").c_str(), "rb");
  if (!f) {
    if (!quiet) logf("cannot open %s.ref (instrument sidecar)\n", jnl_path.c_str());
    return false;
  }
  std::vector<std::uint8_t> buf(1 << 20);
  const std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  if (!table.load(buf.data(), n)) {
    if (!quiet) logf("corrupt instrument sidecar %s.ref\n", jnl_path.c_str());
    return false;
  }
  return true;
}

}  // namespace plat
