#pragma once
// Deterministic instrument-universe generator shared by the tests and the
// bench harness: same seed -> same tickers, same static data, bit for bit.

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "st/table.hpp"

namespace st {

struct Rng {
  u64 s;
  explicit Rng(u64 seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  u64 next() noexcept {  // xorshift64*
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1Dull;
  }
  u64 below(u64 n) noexcept { return fastrange(next(), n); }
};

inline std::vector<Instrument> gen_universe(u64 n, u64 seed) {
  Rng rng(seed);
  std::unordered_set<u64> seen;
  std::vector<Instrument> out;
  out.reserve(n);
  while (out.size() < n) {
    const u64 len = 1 + rng.below(6);  // 1..6 letters, like real tickers
    char buf[8];
    for (u64 i = 0; i < len; ++i) buf[i] = static_cast<char>('A' + rng.below(26));
    const Sym sym = Sym::from({buf, static_cast<std::size_t>(len)});
    if (!seen.insert(sym.v).second) continue;
    Instrument ins;
    ins.sym = sym;
    ins.id = static_cast<u32>(out.size());
    ins.lot = rng.below(10) == 0 ? 10 : 100;
    ins.tick = rng.below(20) == 0 ? 500 : 100;  // most penny-tick, some 5c
    ins.mult = 1;
    ins.venue = static_cast<u16>(rng.below(4));
    ins.asset = 0;
    ins.status = 0;
    out.push_back(ins);
  }
  return out;
}

}  // namespace st
