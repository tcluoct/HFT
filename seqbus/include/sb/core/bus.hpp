#pragma once
// seqbus core: a single-writer broadcast bus over shared memory.
//
// One writer publishes fixed-size records into a power-of-two ring of
// versioned slots (record.hpp); any number of reader processes follow along
// by sequence number. The defining property, and the reason this shape is
// the backbone transport inside trading plants (LMAX Disruptor, Aeron IPC,
// every exchange's shared-memory market-data bus):
//
//   THE WRITER IS WAIT-FREE AND NEVER AWARE OF READERS.
//
// publish() is a fixed, bounded sequence of stores — no locks, no RMW
// atomics, no reader bookkeeping, no backpressure. A reader that stalls,
// deschedules, or dies cannot slow the market-data path by one cycle. The
// cost is borne where it belongs: a reader that falls more than one ring
// behind is *lapped*, detects it from the stamp arithmetic, counts the loss,
// and resyncs to the live edge (in production it would then re-prime from a
// snapshot service — freshness beats completeness for market data).
//
// Memory-ordering contract (the whole protocol, four lines):
//
//   writer: stamp <- 2s+1 relaxed        reader: v1 <- stamp acquire
//           fence(release)                       words <- relaxed loads
//           words <- relaxed stores              fence(acquire)
//           stamp <- 2s+2 release                v2 <- stamp relaxed; ok iff v1==v2==2s+2
//
// The release fence keeps the "busy" stamp ahead of the payload stores; the
// release store keeps the payload ahead of the "done" stamp; the reader's
// acquire pair mirrors it. Stamps strictly increase per slot, so v1==v2
// proves the payload was not touched in between (no ABA). On x86-64 every
// one of these compiles to a plain MOV — the fences and orderings constrain
// only the compiler; the hardware's TSO memory model provides the rest for
// free. The same source is correct (with real fence instructions) on ARM.
//
// Cross-process note: this file never mentions processes. It operates on a
// span of bytes; whether two Writers/Readers share an address space or only
// a mapping (shm.hpp) is invisible to the protocol, because cache coherence
// works on physical addresses. The only requirement is that the atomics be
// address-free (lock-free), which record.hpp static_asserts.

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hft/common/platform.hpp"
#include "sb/core/record.hpp"

namespace sb {

inline constexpr std::uint64_t kBusMagic = 0x0100'5355'4251'4553ull;  // "SEQBUS" + layout v1

// One page. `magic` is atomic and stored *last* by format(): a process that
// races attach() against a concurrent format() either sees the magic (and
// therefore, by release/acquire, a fully formatted header) or fails cleanly.
struct alignas(4096) BusHeader {
  std::atomic<std::uint64_t> magic{0};
  std::uint32_t slot_bytes = 0;
  std::uint32_t reserved = 0;
  std::uint64_t capacity = 0;  // slots, power of two
  std::uint64_t mask = 0;
  // Writer cursor: sequences [0, pub_seq) are published. Own line — it is
  // written every publish and read by readers only on join and resync.
  alignas(hft::kCacheLine) std::atomic<std::uint64_t> pub_seq{0};
};
static_assert(sizeof(BusHeader) == 4096);

[[nodiscard]] constexpr std::size_t bus_bytes(std::uint64_t capacity) noexcept {
  return sizeof(BusHeader) + capacity * sizeof(Slot);
}

// Non-owning view of a formatted bus: header page + slot array.
class Bus {
 public:
  Bus() = default;

  // Lay a fresh bus over zero-initialized memory (the creator side).
  static Bus format(void* mem, std::size_t bytes, std::uint64_t capacity) noexcept {
    if (!mem || !std::has_single_bit(capacity) || bytes < bus_bytes(capacity)) return {};
    auto* hdr = new (mem) BusHeader{};
    // Slots are placement-constructed to their zero state. The memory is
    // already zero-filled (fresh shm), so this compiles to nothing useful —
    // it exists to start object lifetimes, not to write bytes.
    auto* slots = reinterpret_cast<Slot*>(static_cast<std::byte*>(mem) + sizeof(BusHeader));
    for (std::uint64_t i = 0; i < capacity; ++i) new (slots + i) Slot{};
    hdr->slot_bytes = static_cast<std::uint32_t>(sizeof(Slot));
    hdr->capacity = capacity;
    hdr->mask = capacity - 1;
    hdr->magic.store(kBusMagic, std::memory_order_release);  // publish the header itself
    return Bus{hdr, slots};
  }

  // Attach to a bus another process formatted. Validates before trusting.
  static Bus attach(void* mem, std::size_t bytes) noexcept {
    if (!mem || bytes < sizeof(BusHeader)) return {};
    auto* hdr = static_cast<BusHeader*>(mem);
    if (hdr->magic.load(std::memory_order_acquire) != kBusMagic) return {};
    if (hdr->slot_bytes != sizeof(Slot) || !std::has_single_bit(hdr->capacity) ||
        bytes < bus_bytes(hdr->capacity))
      return {};
    auto* slots = reinterpret_cast<Slot*>(static_cast<std::byte*>(mem) + sizeof(BusHeader));
    return Bus{hdr, slots};
  }

  [[nodiscard]] bool valid() const noexcept { return hdr_ != nullptr; }
  [[nodiscard]] BusHeader& header() const noexcept { return *hdr_; }
  [[nodiscard]] Slot& slot(std::uint64_t i) const noexcept { return slots_[i]; }
  [[nodiscard]] std::uint64_t capacity() const noexcept { return hdr_->capacity; }
  [[nodiscard]] std::uint64_t mask() const noexcept { return hdr_->mask; }

 private:
  Bus(BusHeader* h, Slot* s) : hdr_(h), slots_(s) {}
  BusHeader* hdr_ = nullptr;
  Slot* slots_ = nullptr;
};

// The single writer. Exactly one per bus — nothing checks or enforces this;
// like "one matching-engine thread per symbol" in matchd, single-writer is
// the architecture, not a runtime negotiation.
class Writer {
 public:
  explicit Writer(Bus bus) noexcept
      : bus_(bus), mask_(bus.mask()),
        seq_(bus.header().pub_seq.load(std::memory_order_acquire)) {}

  template <BusPayload T>
  HFT_FORCE_INLINE std::uint64_t publish(const T& msg) noexcept {
    const std::uint64_t s = seq_++;
    Slot& sl = bus_.slot(s & mask_);

    sl.stamp.store(stamp_busy(s), std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);  // busy stamp before payload

    std::uint64_t w[kPayloadWords] = {};
    std::memcpy(w, &msg, sizeof(T));  // ≤56 bytes: compiles to register moves
    for (std::size_t i = 0; i < kPayloadWords; ++i)
      sl.word[i].store(w[i], std::memory_order_relaxed);

    sl.stamp.store(stamp_done(s), std::memory_order_release);  // payload before done stamp
    bus_.header().pub_seq.store(s + 1, std::memory_order_release);
    return s;
  }

  [[nodiscard]] std::uint64_t next_seq() const noexcept { return seq_; }

 private:
  Bus bus_;
  std::uint64_t mask_;
  std::uint64_t seq_;
};

enum class PollKind : std::uint8_t {
  kEmpty,  // nothing new — spin, or go do something else
  kMsg,    // `out` holds the next message in sequence
  kGap,    // lapped: `gap` messages were lost; reader has resynced to the live edge
};

struct Poll {
  PollKind kind;
  std::uint64_t gap;
};

// A reader: private cursor, zero shared writes. Readers cost the writer
// nothing and each other nothing (they only *read* shared lines).
class Reader {
 public:
  enum class Start : std::uint8_t {
    kLiveEdge,  // begin at pub_seq — see only messages published after joining
    kOrigin,    // begin at sequence 0 — replay whatever history the ring still holds
  };

  explicit Reader(Bus bus, Start st = Start::kLiveEdge) noexcept
      : bus_(bus), mask_(bus.mask()),
        next_(st == Start::kLiveEdge ? bus.header().pub_seq.load(std::memory_order_acquire)
                                     : 0) {}

  template <BusPayload T>
  HFT_FORCE_INLINE Poll poll(T& out) noexcept {
    Slot& sl = bus_.slot(next_ & mask_);
    const std::uint64_t done = stamp_done(next_);

    const std::uint64_t v1 = sl.stamp.load(std::memory_order_acquire);
    if (v1 < done) return {PollKind::kEmpty, 0};  // not published yet (or mid-write)

    if (v1 == done) [[likely]] {
      std::uint64_t w[kPayloadWords];
      for (std::size_t i = 0; i < kPayloadWords; ++i)
        w[i] = sl.word[i].load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);  // payload before re-check
      if (sl.stamp.load(std::memory_order_relaxed) == done) [[likely]] {
        std::memcpy(&out, w, sizeof(T));
        ++next_;
        return {PollKind::kMsg, 0};
      }
      // Stamp moved while we copied: the writer re-entered this slot, i.e.
      // it is a full ring ahead of us. Fall through to the lap path.
    }

    // Lapped (v1 > done, or the re-check failed). Everything from next_ to
    // the live edge is unrecoverable ring-wise; jump there and report the
    // hole so the caller can trigger recovery. Seeing a stamp from a later
    // lap acquire-synchronizes with that publish, which guarantees pub_seq
    // is already visibly > next_ — `lost` is always at least 1.
    const std::uint64_t pub = bus_.header().pub_seq.load(std::memory_order_acquire);
    const std::uint64_t lost = pub - next_;
    next_ = pub;
    dropped_ += lost;
    ++gaps_;
    return {PollKind::kGap, lost};
  }

  [[nodiscard]] std::uint64_t next_seq() const noexcept { return next_; }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
  [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

 private:
  Bus bus_;
  std::uint64_t mask_;
  std::uint64_t next_;
  std::uint64_t dropped_ = 0;
  std::uint64_t gaps_ = 0;
};

}  // namespace sb
