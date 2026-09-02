#pragma once
// Mph — a minimal perfect hash over the instrument universe, built at
// startup, immutable afterwards.
//
// Hash-and-displace (CHD-style): keys are split into small buckets by one
// hash; buckets are placed largest-first, each searching for a per-bucket
// seed under which ALL of its keys land in distinct free slots of an
// exactly-n-slot table. Lookup is then branch-free and collision-free by
// construction:
//
//     bucket = fastrange(mix(key, kBucketSeed), nbuckets)   // 1 mul-chain
//     slot   = fastrange(mix(key, seeds[bucket]), n)        // 1 mul-chain
//     hit    = slots[slot].key == key                       // 1 compare
//
// Two data reads per lookup — a 4-byte seed and a 16-byte slot — and the
// slot read verifies membership exactly (unknown keys cannot false-hit).
// Load factor is 1.0: n keys in n slots, no probing, no tombstones. The
// price is that inserts require a rebuild — which is what the snapshot-swap
// layer (table.hpp) is for.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "st/sym.hpp"

namespace st {

// key + stable instrument id in 16 bytes: the verify compare and the id it
// yields are always on the same cache line.
struct MphSlot {
  u64 key;
  u32 id;
  u32 pad_;
};

class Mph {
 public:
  static constexpr u64 kBucketSeedBase = 0xA5A5F00D5EEDBA5Eull;
  static constexpr u32 kMiss = 0xFFFFFFFFu;
  static constexpr u32 kMaxTries = 1u << 20;  // per-bucket displacement cap

  struct BuildStats {
    u32 max_seed = 0;      // worst displacement search over all buckets
    u64 total_tries = 0;   // summed seed attempts (avg = /nonempty buckets)
    u32 max_bucket = 0;    // largest bucket size
    u32 attempts = 0;      // global rebuild attempts used (1 = first try)
  };

  // keys[i] carries stable id ids[i]. Returns false on duplicate keys or
  // (astronomically unlikely at lambda<=5) displacement failure after
  // exhausting global attempts.
  bool build(const std::vector<Sym>& keys, const std::vector<u32>& ids, u32 lambda = 4) {
    const u64 n = keys.size();
    if (n == 0 || keys.size() != ids.size()) return false;

    {  // duplicate detection up front: sort a copy, look for equal neighbors
      std::vector<u64> sorted;
      sorted.reserve(n);
      for (const Sym& k : keys) sorted.push_back(k.v);
      std::sort(sorted.begin(), sorted.end());
      for (u64 i = 1; i < n; ++i)
        if (sorted[i] == sorted[i - 1]) return false;
    }

    const u64 m = (n + lambda - 1) / lambda;
    for (u32 attempt = 0; attempt < 8; ++attempt) {
      stats_ = BuildStats{};
      stats_.attempts = attempt + 1;
      if (try_build(keys, ids, n, m, kBucketSeedBase + attempt)) {
        bucket_seed_ = kBucketSeedBase + attempt;
        return true;
      }
    }
    return false;
  }

  HFT_FORCE_INLINE u32 lookup(Sym k) const noexcept {
    const u64 b = fastrange(mix(k.v, bucket_seed_), nbuckets_);
    const u64 pos = fastrange(mix(k.v, seeds_[b]), nslots_);
    const MphSlot& sl = slots_[pos];
    return sl.key == k.v ? sl.id : kMiss;
  }

  // Slot index only (no verify) — for the two-phase batched lookup below.
  HFT_FORCE_INLINE u64 slot_of(Sym k) const noexcept {
    const u64 b = fastrange(mix(k.v, bucket_seed_), nbuckets_);
    return fastrange(mix(k.v, seeds_[b]), nslots_);
  }

  HFT_FORCE_INLINE const MphSlot& slot(u64 pos) const noexcept { return slots_[pos]; }

  // Resolve W symbols at once: compute all slots first (seeds array is tiny
  // and cache-resident), prefetch every slot line, then verify. The verify
  // loads that would each stall ~a-memory-latency instead overlap.
  template <unsigned W>
  HFT_FORCE_INLINE void lookup_batch(const Sym* keys, u32* out) const noexcept {
    u64 pos[W];
    for (unsigned i = 0; i < W; ++i) {
      pos[i] = slot_of(keys[i]);
      hft::prefetch_l1(&slots_[pos[i]]);
    }
    for (unsigned i = 0; i < W; ++i) {
      const MphSlot& sl = slots_[pos[i]];
      out[i] = sl.key == keys[i].v ? sl.id : kMiss;
    }
  }

  [[nodiscard]] u64 size() const noexcept { return nslots_; }
  [[nodiscard]] u64 buckets() const noexcept { return nbuckets_; }
  [[nodiscard]] const BuildStats& stats() const noexcept { return stats_; }

  // FNV-1a over seeds and slots: the determinism proof (same universe, same
  // lambda -> same table, bit for bit).
  [[nodiscard]] u64 digest() const noexcept {
    u64 h = 0xCBF29CE484222325ull;
    auto fold = [&h](u64 x) {
      for (int i = 0; i < 8; ++i) {
        h ^= (x >> (i * 8)) & 0xFF;
        h *= 0x100000001B3ull;
      }
    };
    for (u32 s : seeds_) fold(s);
    for (const MphSlot& sl : slots_) {
      fold(sl.key);
      fold(sl.id);
    }
    return h;
  }

 private:
  bool try_build(const std::vector<Sym>& keys, const std::vector<u32>& ids, u64 n, u64 m,
                 u64 bucket_seed) {
    // Bucket the keys.
    std::vector<std::vector<u32>> bucket(m);  // key indices per bucket
    for (u64 i = 0; i < n; ++i)
      bucket[fastrange(mix(keys[i].v, bucket_seed), m)].push_back(static_cast<u32>(i));

    // Place largest buckets first (they are the hardest to displace), bucket
    // id as tie-break so the build is deterministic.
    std::vector<u32> order(m);
    for (u64 b = 0; b < m; ++b) order[b] = static_cast<u32>(b);
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
      if (bucket[a].size() != bucket[b].size()) return bucket[a].size() > bucket[b].size();
      return a < b;
    });

    seeds_.assign(m, 0);
    slots_.assign(n, MphSlot{0, kMiss, 0});
    std::vector<u8> used(n, 0);
    u64 tmp[64];  // scratch slot positions for one bucket

    for (u32 b : order) {
      const auto& kv = bucket[b];
      if (kv.empty()) break;  // sorted by size: the rest are empty too
      if (kv.size() > 64) return false;
      if (kv.size() > stats_.max_bucket) stats_.max_bucket = static_cast<u32>(kv.size());
      bool placed = false;
      for (u32 s = 1; s < kMaxTries; ++s) {
        ++stats_.total_tries;
        bool ok = true;
        for (std::size_t i = 0; i < kv.size() && ok; ++i) {
          const u64 pos = fastrange(mix(keys[kv[i]].v, s), n);
          if (used[pos]) {
            ok = false;
            break;
          }
          for (std::size_t j = 0; j < i; ++j)
            if (tmp[j] == pos) {
              ok = false;
              break;
            }
          tmp[i] = pos;
        }
        if (ok) {
          for (std::size_t i = 0; i < kv.size(); ++i) {
            used[tmp[i]] = 1;
            slots_[tmp[i]] = MphSlot{keys[kv[i]].v, ids[kv[i]], 0};
          }
          seeds_[b] = s;
          if (s > stats_.max_seed) stats_.max_seed = s;
          placed = true;
          break;
        }
      }
      if (!placed) return false;
    }

    nslots_ = n;
    nbuckets_ = m;
    return true;
  }

  std::vector<u32> seeds_;
  std::vector<MphSlot> slots_;
  u64 nslots_ = 0;
  u64 nbuckets_ = 1;
  u64 bucket_seed_ = kBucketSeedBase;
  BuildStats stats_{};
};

}  // namespace st
