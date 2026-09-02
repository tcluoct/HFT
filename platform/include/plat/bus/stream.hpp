#pragma once
// plat/bus/stream.hpp — the canonical-stream core (§7.1): sequence
// assignment, the running stream hash, and the journal tap. Dispatch is NOT
// here — the site engine owns a fixed, fully-inlined consumer chain (books →
// OMS → riskgate → PnL → strategies), so there is no std::function, no
// subscriber vector, and no indirect call between an envelope being sealed
// and every component seeing it. This core keeps the two properties replay
// depends on:
//
//   · total order: every envelope gets the next sequence number, whether
//     produced live (seal) or re-injected from a journal (absorb);
//   · verifiability: the hash accumulates (seq, schema, payload) for every
//     envelope, so a replay that regenerates the Derived stream
//     byte-identically ends with the same hash as the live run (§11.1).

#include <cstdint>

#include "hft/common/platform.hpp"
#include "plat/bus/async_journal.hpp"
#include "plat/bus/envelope.hpp"
#include "plat/core/hash.hpp"

namespace plat {

class StreamCore {
 public:
  explicit StreamCore(SiteId site = 0) noexcept : site_(site) {}

  // The journal tap sees every envelope (audit record) before any consumer.
  // The tap is one wait-free ring publish; file I/O runs on the journal's
  // own thread (async_journal.hpp).
  void set_journal(AsyncJournal* j) noexcept { journal_ = j; }

  // Live path: stamp the next sequence number, hash, journal, return the
  // sealed envelope for the engine's dispatch chain.
  template <PlatMsg T>
  HFT_FORCE_INLINE Envelope seal(Nanos ts, const T& msg, Origin origin) {
    Envelope e = make_envelope(next_seq_++, ts, msg, origin, site_);
    mix(e);
    if (journal_) journal_->append(e);
    return e;
  }

  // Replay path: the envelope keeps its recorded identity; the counter
  // follows along so Derived publishes interleave with original sequence
  // numbers, and the hash accumulates identically — replay's stream hash
  // equals live's iff regeneration was byte-exact. When a journal is
  // attached (--resume: replaying the crashed prefix into a LIVE site), the
  // injected envelope is re-journaled, so the new journal is the complete
  // day again — prefix plus live continuation, self-contained for the next
  // recovery. Pure verification replay (--replay) never attaches a journal.
  void absorb(const Envelope& e) {
    next_seq_ = e.seq + 1;
    mix(e);
    if (journal_) journal_->append(e);
  }

  [[nodiscard]] std::uint64_t next_seq() const noexcept { return next_seq_; }
  void set_next_seq(std::uint64_t s) noexcept { next_seq_ = s; }
  [[nodiscard]] std::uint64_t stream_hash() const noexcept { return stream_hash_.value(); }
  [[nodiscard]] SiteId site() const noexcept { return site_; }

 private:
  HFT_FORCE_INLINE void mix(const Envelope& e) noexcept {
    stream_hash_.mix(e.seq);
    stream_hash_.mix(e.schema);
    stream_hash_.mix_bytes(e.payload, e.size);
  }

  StateHash stream_hash_;
  std::uint64_t next_seq_ = 0;
  AsyncJournal* journal_ = nullptr;
  SiteId site_;
};

}  // namespace plat
