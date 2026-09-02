#pragma once
// The unit of arbitration: one market-data datagram as it arrives off a
// redundant multicast line. Venues broadcast the identical sequence-numbered
// stream on two lines (A/B) over disjoint network paths; the arbitrator's
// job is to reconstruct one gapless in-order stream from whichever copies
// survive, first copy wins.

#include <cstdint>

namespace fa {

struct MdPacket {
  std::uint64_t seq;      // venue sequence number, gapless at the source
  std::uint64_t send_ts;  // stamped by the venue at send (sim timeline: ns)
  std::uint64_t payload;  // stands in for the body; harness checksums it
  std::uint32_t line;     // 0 = A, 1 = B
  std::uint32_t pad_;
};
static_assert(sizeof(MdPacket) == 32);

// splitmix64: the harness derives payload = mix(seq) so every delivered
// packet is verifiable in isolation (same trick as seqbus's messages).
constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

}  // namespace fa
