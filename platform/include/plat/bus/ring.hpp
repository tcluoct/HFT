#pragma once
// plat/bus/ring.hpp — the transport-grade broadcast ring (§7.1), the seqbus
// lab's versioned-stamp protocol generalized to platform-sized slots. The
// writer is wait-free and reader-oblivious; a lapped reader detects the loss
// from stamp arithmetic and resyncs to the live edge. The protocol operates
// on any span of memory — same address space or a shared mapping — because
// the atomics are address-free.
//
// In this repository the site engine composes components in one process on
// the synchronous SeqStream (stream.hpp) for byte-deterministic replay; this
// ring is the production inter-process transport, exercised by plat_tests
// (ordering, lap/gap) and plat_bench (throughput/latency).

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

#include "hft/common/platform.hpp"
#include "plat/bus/envelope.hpp"

namespace plat {

// Stamp encoding per slot generation: writer sets 2s+1 (busy) then 2s+2
// (done) for sequence s on its lap of the slot; stamps strictly increase, so
// a reader that sees the expected "done" value twice around a payload copy
// has read a torn-free record.
class Ring {
 public:
  // Payload lives as an array of relaxed atomics: the seqlock's concurrent
  // read-during-write is then defined behavior, not a benign-by-convention
  // race — same discipline as the seqbus lab. On x86-64 every access still
  // compiles to a plain MOV.
  static constexpr std::size_t kWords = sizeof(Envelope) / 8;

  struct Slot {
    std::atomic<std::uint64_t> stamp{0};
    std::uint8_t pad_[hft::kCacheLine - sizeof(std::atomic<std::uint64_t>)]{};
    std::atomic<std::uint64_t> word[kWords]{};
  };
  static_assert(sizeof(Slot) == hft::kCacheLine + sizeof(Envelope));
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

  // Power-of-two capacity keeps slot addressing a mask. Slots hold atomics
  // (immovable), so the vector is sized exactly once and never reallocated.
  explicit Ring(std::uint64_t capacity)
      : slots_(std::has_single_bit(capacity) ? capacity : std::bit_ceil(capacity)),
        mask_(slots_.size() - 1) {}

  class Writer {
   public:
    explicit Writer(Ring& r) noexcept : r_(&r) {}

    HFT_FORCE_INLINE std::uint64_t publish(const Envelope& e) noexcept {
      const std::uint64_t s = seq_++;
      Slot& sl = r_->slots_[s & r_->mask_];
      std::uint64_t w[kWords];
      std::memcpy(w, &e, sizeof(Envelope));
      w[0] = s;  // Envelope.seq is the leading u64 — stamped with the ring seq
      sl.stamp.store(2 * s + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);  // busy before payload
      for (std::size_t i = 0; i < kWords; ++i)
        sl.word[i].store(w[i], std::memory_order_relaxed);
      sl.stamp.store(2 * s + 2, std::memory_order_release);  // payload before done
      r_->pub_seq_.store(s + 1, std::memory_order_release);
      return s;
    }

   private:
    Ring* r_;
    std::uint64_t seq_ = 0;
  };

  enum class PollKind : std::uint8_t { Empty, Msg, Gap, Fault };

  // Overflow policy per topic class (§7.1, review R3): a lapped reader on a
  // loss-tolerant topic (raw depth, telemetry) resyncs to the live edge and
  // §8.1 gap handling owns recovery; on a loss-intolerant topic (order
  // intents, exec reports, risk decisions, control) a lap is a sticky
  // fail-closed fault — the consumer must halt trading, not paper over it.
  enum class LossPolicy : std::uint8_t { Tolerant, Intolerant };

  struct Poll {
    PollKind kind;
    std::uint64_t gap;
  };

  class Reader {
   public:
    explicit Reader(Ring& r, bool from_origin = true,
                    LossPolicy policy = LossPolicy::Tolerant) noexcept
        : r_(&r), next_(from_origin ? 0 : r.pub_seq_.load(std::memory_order_acquire)),
          policy_(policy) {}

    HFT_FORCE_INLINE Poll poll(Envelope& out) noexcept {
      if (faulted_) [[unlikely]] return {PollKind::Fault, 0};
      Slot& sl = r_->slots_[next_ & r_->mask_];
      const std::uint64_t done = 2 * next_ + 2;
      const std::uint64_t v1 = sl.stamp.load(std::memory_order_acquire);
      if (v1 < done) return {PollKind::Empty, 0};
      if (v1 == done) [[likely]] {
        std::uint64_t w[kWords];
        for (std::size_t i = 0; i < kWords; ++i)
          w[i] = sl.word[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);  // payload before re-check
        if (sl.stamp.load(std::memory_order_relaxed) == done) [[likely]] {
          std::memcpy(&out, w, sizeof(Envelope));
          ++next_;
          return {PollKind::Msg, 0};
        }
      }
      // Lapped: the hole is real data loss.
      const std::uint64_t pub = r_->pub_seq_.load(std::memory_order_acquire);
      const std::uint64_t lost = pub - next_;
      dropped_ += lost;
      if (policy_ == LossPolicy::Intolerant) {
        faulted_ = true;  // sticky: this stream is no longer trustworthy
        return {PollKind::Fault, lost};
      }
      next_ = pub;  // tolerant: resync to the live edge, report the hole
      return {PollKind::Gap, lost};
    }

    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

   private:
    Ring* r_;
    std::uint64_t next_;
    std::uint64_t dropped_ = 0;
    LossPolicy policy_;
    bool faulted_ = false;
  };

 private:
  std::vector<Slot> slots_;
  std::uint64_t mask_;
  alignas(hft::kCacheLine) std::atomic<std::uint64_t> pub_seq_{0};
};

}  // namespace plat
