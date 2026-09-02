#pragma once
// A/B line arbitration with a sequence-window reorder ring.
//
// The hot path is the in-sync case — the next expected seq arrives, its
// twin arrives a moment later — and costs one ring probe each: first copy
// is delivered and remembered, second copy identifies itself by matching
// the ring entry (full seq stored, so no ABA) and is dropped after donating
// a skew sample.
//
// Everything else is the gap machinery:
//   * ahead-of-sequence packets are parked in the ring (presence bitmap +
//     slot array, one & and one bit test per probe) — natural reordering
//     and single-line drops resolve silently when the hole fills from the
//     other line;
//   * a hole that persists past `gap_timeout` (both copies lost) is declared
//     dead via on_gap(lo, hi) and the stream resumes at the next survivor —
//     freshness beats completeness, same contract as seqbus's lapped reader;
//   * a seq beyond the window (recovery after an outage) fast-forwards,
//     delivering what's parked and gap-reporting the rest.
//
// Single-threaded by design, like matchd's core: arbitration is a serial
// decision (what is *the* next packet?), so the fast path has no atomics.

#include <array>
#include <bit>
#include <cstdint>

#include "hft/common/histogram.hpp"
#include "fa/core/packet.hpp"

namespace fa {

template <typename S>
concept ArbSink = requires(S s, const MdPacket& p, std::uint64_t lo, std::uint64_t hi) {
  { s.on_packet(p) } noexcept;
  { s.on_gap(lo, hi) } noexcept;  // seqs [lo, hi) declared lost
};

struct LineStats {
  std::uint64_t received = 0;  // everything that hit this line's socket
  std::uint64_t firsts = 0;    // races won (this line's copy arrived first)
  std::uint64_t dups = 0;      // races lost (twin of an in-window seq)
  std::uint64_t stales = 0;    // copy of a seq already out of the window
};

struct ArbStats {
  LineStats line[2];
  std::uint64_t delivered = 0;
  std::uint64_t gap_events = 0;
  std::uint64_t gap_seqs = 0;      // total seqs declared lost
  std::uint64_t window_jumps = 0;  // fast-forwards past the whole window
};

enum class ArbState : std::uint8_t { kSynced, kGapPending };

template <ArbSink Sink, std::size_t Window = 1024>
  requires(std::has_single_bit(Window))
class Arbiter {
 public:
  static constexpr std::size_t kWindow = Window;

  // `gap_timeout`: how long a hole may stay open (same time unit as `now`)
  // before the missing seqs are declared lost. In production this is tuned
  // to a few times the inter-line skew: long enough for the B copy, too
  // short to stall the book.
  Arbiter(Sink& sink, std::uint64_t gap_timeout, std::uint64_t first_seq = 1)
      : sink_(&sink), timeout_(gap_timeout), next_(first_seq) {}

  void on_packet(const MdPacket& p, std::uint64_t now) noexcept {
    LineStats& ls = stats_.line[p.line & 1];
    ++ls.received;

    SeqInfo& info = seen_[p.seq & kMask];
    if (info.seq == p.seq) {  // the twin: drop, harvest race statistics
      ++ls.dups;
      skew_.record(now - info.first_ts);
      return;
    }
    if (p.seq < next_) {  // copy of something long delivered; window recycled
      ++ls.stales;
      return;
    }
    if (p.seq >= next_ + kWindow) fast_forward(p.seq - kWindow + 1, now);

    info = SeqInfo{p.seq, now};
    ++ls.firsts;
    if (p.seq == next_) {
      sink_->on_packet(p);
      ++stats_.delivered;
      ++next_;
      drain(now);
    } else {  // ahead: park it, the hole below it is now on the clock
      slots_[p.seq & kMask] = p;
      bitmap_set(p.seq & kMask);
      ++parked_;
      if (state_ == ArbState::kSynced) {
        state_ = ArbState::kGapPending;
        deadline_ = now + timeout_;
      }
    }
  }

  // Drive the gap clock. Call when idle or after a batch; costs one compare
  // unless a hole has actually expired.
  void poll(std::uint64_t now) noexcept {
    if (state_ == ArbState::kGapPending && now >= deadline_) [[unlikely]] {
      const std::uint64_t lo = lowest_parked();
      sink_->on_gap(next_, lo);
      ++stats_.gap_events;
      stats_.gap_seqs += lo - next_;
      next_ = lo;
      drain(now);
    }
  }

  // Test/debug aid: full internal-consistency sweep. Returns nullptr when
  // sound, else a description of the first violated invariant.
  [[nodiscard]] const char* audit() const noexcept {
    std::uint64_t bits = 0;
    for (const std::uint64_t w : bits_) bits += static_cast<std::uint64_t>(std::popcount(w));
    if (bits != parked_) return "bitmap population != parked_";
    for (std::uint64_t i = 0; i < Window; ++i)
      if (bitmap_test(i)) {
        const std::uint64_t s = slots_[i].seq;
        if ((s & kMask) != i) return "parked slot stored at wrong ring position";
        if (s <= next_ || s >= next_ + Window) return "parked seq outside (next_, next_+W)";
      }
    if ((state_ == ArbState::kGapPending) != (parked_ > 0)) return "state / parked_ mismatch";
    return nullptr;
  }

  [[nodiscard]] const ArbStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const hft::LatencyHistogram& skew() const noexcept { return skew_; }
  [[nodiscard]] ArbState state() const noexcept { return state_; }
  [[nodiscard]] std::uint64_t next_expected() const noexcept { return next_; }

 private:
  static constexpr std::uint64_t kMask = Window - 1;
  static constexpr std::size_t kWords = Window / 64;

  struct SeqInfo {
    std::uint64_t seq = ~std::uint64_t{0};
    std::uint64_t first_ts = 0;
  };

  void bitmap_set(std::uint64_t i) noexcept { bits_[i >> 6] |= std::uint64_t{1} << (i & 63); }
  void bitmap_clear(std::uint64_t i) noexcept { bits_[i >> 6] &= ~(std::uint64_t{1} << (i & 63)); }
  [[nodiscard]] bool bitmap_test(std::uint64_t i) const noexcept {
    return (bits_[i >> 6] >> (i & 63)) & 1u;
  }

  // Deliver every consecutively-parked packet starting at next_.
  void drain(std::uint64_t now) noexcept {
    while (bitmap_test(next_ & kMask) && slots_[next_ & kMask].seq == next_) {
      bitmap_clear(next_ & kMask);
      --parked_;
      sink_->on_packet(slots_[next_ & kMask]);
      ++stats_.delivered;
      ++next_;
    }
    if (parked_ == 0) {
      state_ = ArbState::kSynced;
    } else {  // progress resets the clock for the next hole
      state_ = ArbState::kGapPending;
      deadline_ = now + timeout_;
    }
  }

  // Lowest parked seq > next_: scan the bitmap in ring order starting at
  // next_'s word (<= Window/64 words, countr_zero within a word). Ring
  // position order from next_ upward IS ascending seq order, so the first
  // set bit found names the lowest survivor. Only called when parked_ > 0.
  [[nodiscard]] std::uint64_t lowest_parked() const noexcept {
    const std::uint64_t n0 = next_ & kMask;
    for (std::uint64_t off = 0; off < kWords; ++off) {
      const std::uint64_t wi = ((n0 >> 6) + off) % kWords;
      std::uint64_t word = bits_[wi];
      if (off == 0) word &= ~std::uint64_t{0} << (n0 & 63);  // bits at/above next_ only
      if (word) return slots_[(wi << 6) + std::countr_zero(word)].seq;
    }
    // Not found: the survivor sits in the low bits of next_'s own word —
    // ring positions below next_, i.e. the highest seqs in the window.
    const std::uint64_t wi = n0 >> 6;
    const std::uint64_t word = bits_[wi] & ~(~std::uint64_t{0} << (n0 & 63));
    return slots_[(wi << 6) + std::countr_zero(word)].seq;
  }

  // A seq landed beyond the window: advance next_ to target, delivering the
  // parked survivors and gap-reporting the holes. Rare recovery path.
  void fast_forward(std::uint64_t target, std::uint64_t now) noexcept {
    ++stats_.window_jumps;
    std::uint64_t gap_lo = ~std::uint64_t{0};
    while (next_ < target) {
      if (bitmap_test(next_ & kMask) && slots_[next_ & kMask].seq == next_) {
        if (gap_lo != ~std::uint64_t{0}) {
          sink_->on_gap(gap_lo, next_);
          ++stats_.gap_events;
          stats_.gap_seqs += next_ - gap_lo;
          gap_lo = ~std::uint64_t{0};
        }
        bitmap_clear(next_ & kMask);
        --parked_;
        sink_->on_packet(slots_[next_ & kMask]);
        ++stats_.delivered;
      } else if (gap_lo == ~std::uint64_t{0}) {
        gap_lo = next_;
      }
      ++next_;
    }
    if (gap_lo != ~std::uint64_t{0}) {
      sink_->on_gap(gap_lo, next_);
      ++stats_.gap_events;
      stats_.gap_seqs += next_ - gap_lo;
    }
    // The seq at `target` may itself be parked — deliver it (and any run
    // behind it) now, or next_ would sit *on* a parked seq until a timeout
    // "resolved" it as an empty gap.
    drain(now);
  }

  Sink* sink_;
  std::uint64_t timeout_;
  std::uint64_t next_;
  std::uint64_t parked_ = 0;
  std::uint64_t deadline_ = 0;
  ArbState state_ = ArbState::kSynced;

  std::array<std::uint64_t, kWords> bits_{};
  std::array<SeqInfo, Window> seen_{};
  std::array<MdPacket, Window> slots_{};

  ArbStats stats_{};
  hft::LatencyHistogram skew_;
};

}  // namespace fa
