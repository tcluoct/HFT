#pragma once
// plat/bus/envelope.hpp — the canonical-stream record (§7.1): fixed 256-byte
// envelope carrying any codec message. Fixed size keeps the journal a flat
// array of records (seekable, mmap-able) and the ring slot layout trivial.
//
// `origin` partitions the stream for replay (§11.1): Source envelopes enter
// the site from outside the deterministic component set (venue feeds, gateway
// exec reports, scheduler-driven phase/timer events, operator commands,
// remote-site arrivals). Everything else is Derived — regenerated identically
// when the Source prefix is replayed, which is exactly what the golden-replay
// tests assert.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "plat/codec/msgs.hpp"
#include "plat/core/types.hpp"

namespace plat {

inline constexpr std::size_t kMaxPayload = 224;

enum class Origin : std::uint8_t { Source, Derived };

struct Envelope {
  std::uint64_t seq;
  Nanos ts;
  Schema schema;
  Topic topic;
  std::uint32_t size;
  Origin origin;
  SiteId site;
  std::uint8_t pad_[6]{};
  alignas(8) std::uint8_t payload[kMaxPayload];

  template <PlatMsg T>
  [[nodiscard]] const T& as() const noexcept {
    static_assert(sizeof(T) <= kMaxPayload);
    return *reinterpret_cast<const T*>(payload);
  }
  template <PlatMsg T>
  [[nodiscard]] bool is() const noexcept {
    return schema == MsgTraits<T>::schema;
  }
};
static_assert(std::is_trivially_copyable_v<Envelope> && sizeof(Envelope) == 256);

template <PlatMsg T>
Envelope make_envelope(std::uint64_t seq, Nanos ts, const T& msg, Origin origin,
                       SiteId site) noexcept {
  static_assert(sizeof(T) <= kMaxPayload);
  Envelope e{};
  e.seq = seq;
  e.ts = ts;
  e.schema = MsgTraits<T>::schema;
  e.topic = MsgTraits<T>::topic;
  e.size = static_cast<std::uint32_t>(sizeof(T));
  e.origin = origin;
  e.site = site;
  std::memcpy(e.payload, &msg, sizeof(T));
  return e;
}

}  // namespace plat
