#pragma once
// The harness payload: a full 56-byte message whose body is derived from its
// sequence number and whose last word is a mix of everything before it.
// Every delivered message is verifiable in isolation — if the seqlock ever
// let a torn read through (half old lap, half new), the checksum catches it.
// The stress tests hammer a tiny ring precisely to give torn reads the best
// possible odds, then assert the count of checksum failures is exactly zero.

#include <cstdint>

#include "sb/core/record.hpp"

namespace sb {

enum class MsgKind : std::uint32_t { kWarmup = 0, kMeasured = 1, kStop = 2 };

struct BusMsg {
  std::uint64_t seq;     // writer's global sequence (readers cross-check their cursor)
  std::uint64_t t0_tsc;  // hft::rdtscp at publish — hop latency = hft::rdtscp at delivery − t0
  std::uint32_t kind;    // MsgKind
  std::uint32_t sym;
  std::uint64_t a, b, c;  // pseudo-random body, a function of seq
  std::uint64_t csum;
};
static_assert(sizeof(BusMsg) == kPayloadBytes);

// splitmix64 finalizer — the family's stock bit mixer.
HFT_FORCE_INLINE constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

HFT_FORCE_INLINE constexpr std::uint64_t csum_of(const BusMsg& m) noexcept {
  std::uint64_t h = mix64(m.seq);
  h = mix64(h ^ m.t0_tsc);
  h = mix64(h ^ (static_cast<std::uint64_t>(m.kind) << 32 | m.sym));
  h = mix64(h ^ m.a);
  h = mix64(h ^ m.b);
  h = mix64(h ^ m.c);
  return h;
}

HFT_FORCE_INLINE BusMsg make_msg(std::uint64_t seq, MsgKind kind, std::uint64_t t0) noexcept {
  BusMsg m{};
  m.seq = seq;
  m.t0_tsc = t0;
  m.kind = static_cast<std::uint32_t>(kind);
  m.sym = static_cast<std::uint32_t>(seq & 3);
  m.a = mix64(seq);
  m.b = mix64(seq ^ 0xA5A5A5A5A5A5A5A5ull);
  m.c = mix64(~seq);
  m.csum = csum_of(m);
  return m;
}

HFT_FORCE_INLINE bool verify_msg(const BusMsg& m) noexcept { return csum_of(m) == m.csum; }

}  // namespace sb
