#pragma once
// Two O(1) exactly-once building blocks, both fixed-footprint:
//
// SeenRing — the venue's duplicate-order filter. A resent token is only ever
// near the stream head (resends happen straight after a reconnect, and the
// open-order window is tiny next to the ring), so a direct-mapped ring that
// stores the FULL token answers "seen before?" in one probe — feedarb's
// ABA-proof dedup ring, reused for idempotent order entry. The window is the
// contract: dedup is guaranteed for any token within `Cap` newer tokens of
// its first arrival, which bounds client in-flight windows — real venues
// document exactly such bounds.
//
// FlatHash — the firm's open-order map (token -> order awaiting ack).
// Open addressing, linear probing, and backshift deletion (no tombstones, so
// probe chains never rot no matter how many million orders churn through).
// Key 0 is reserved as "empty"; tokens start at 1.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace sl {

template <std::size_t Cap>
  requires(std::has_single_bit(Cap))
class SeenRing {
 public:
  // True if `key` is new (and remember it); false if it is a duplicate.
  bool insert(std::uint64_t key) noexcept {
    std::uint64_t& slot = ring_[key & (Cap - 1)];
    if (slot == key) return false;
    slot = key;
    return true;
  }

 private:
  std::unique_ptr<std::uint64_t[]> mem_ = std::make_unique<std::uint64_t[]>(Cap);
  std::uint64_t* ring_ = mem_.get();  // zero-initialized by make_unique
};

template <typename V, std::size_t Cap>
  requires(std::has_single_bit(Cap))
class FlatHash {
 public:
  FlatHash() : slots_(new Slot[Cap]{}) {}

  bool insert(std::uint64_t key, const V& v) noexcept {
    if (size_ >= Cap - Cap / 8) return false;  // keep probes short
    std::size_t i = home(key);
    while (slots_[i].key != 0) {
      if (slots_[i].key == key) return false;
      i = (i + 1) & kMask;
    }
    slots_[i].key = key;
    slots_[i].val = v;
    ++size_;
    return true;
  }

  V* find(std::uint64_t key) noexcept {
    std::size_t i = home(key);
    while (slots_[i].key != 0) {
      if (slots_[i].key == key) return &slots_[i].val;
      i = (i + 1) & kMask;
    }
    return nullptr;
  }

  // Backshift deletion: refill the hole by sliding back every entry whose
  // probe chain passes through it, so lookups never scan dead slots.
  bool erase(std::uint64_t key) noexcept {
    std::size_t i = home(key);
    while (slots_[i].key != key) {
      if (slots_[i].key == 0) return false;
      i = (i + 1) & kMask;
    }
    std::size_t hole = i;
    std::size_t j = i;
    for (;;) {
      j = (j + 1) & kMask;
      if (slots_[j].key == 0) break;
      // Occupant of j may move into the hole iff its home position does not
      // lie strictly between hole and j (cyclically) — i.e. it probed past
      // the hole to get where it is.
      const std::size_t h = home(slots_[j].key);
      if (((j - h) & kMask) >= ((j - hole) & kMask)) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].key = 0;
    --size_;
    return true;
  }

  template <typename F>
  void for_each(F&& f) {
    for (std::size_t i = 0; i < Cap; ++i)
      if (slots_[i].key != 0) f(slots_[i].key, slots_[i].val);
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

 private:
  struct Slot {
    std::uint64_t key;
    V val;
  };
  static constexpr std::size_t kMask = Cap - 1;

  static std::size_t home(std::uint64_t key) noexcept {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> 32) & kMask;
  }

  std::unique_ptr<Slot[]> slots_;
  std::size_t size_ = 0;
};

}  // namespace sl
