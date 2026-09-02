#pragma once
// OaMap — the family's usual open-addressing map (matchd, omsd), here as the
// baseline the perfect hash has to beat. Linear probing, power-of-2
// capacity, 50% max load, key 0 = empty. Same per-probe cost model as one
// Mph slot read — but hits average >1 probe under load, misses must walk to
// an empty slot, and capacity is 2x the key count instead of 1.0x.

#include <cstdint>
#include <vector>

#include "st/sym.hpp"

namespace st {

class OaMap {
 public:
  static constexpr u32 kMiss = 0xFFFFFFFFu;

  void build(const std::vector<Sym>& keys, const std::vector<u32>& ids) {
    u64 cap = 16;
    while (cap < keys.size() * 2) cap <<= 1;
    mask_ = cap - 1;
    slots_.assign(cap, Slot{0, kMiss});
    for (std::size_t i = 0; i < keys.size(); ++i) {
      u64 pos = mix(keys[i].v, 0) & mask_;
      while (slots_[pos].key != 0) {
        if (slots_[pos].key == keys[i].v) break;  // duplicate: keep first
        pos = (pos + 1) & mask_;
      }
      slots_[pos] = Slot{keys[i].v, ids[i]};
    }
  }

  HFT_FORCE_INLINE u32 lookup(Sym k) const noexcept {
    u64 pos = mix(k.v, 0) & mask_;
    for (;;) {
      const Slot& s = slots_[pos];
      if (s.key == k.v) return s.id;
      if (s.key == 0) return kMiss;
      pos = (pos + 1) & mask_;
    }
  }

  [[nodiscard]] u64 capacity() const noexcept { return mask_ + 1; }

 private:
  struct Slot {
    u64 key;
    u32 id;
  };
  std::vector<Slot> slots_;
  u64 mask_ = 15;
};

}  // namespace st
