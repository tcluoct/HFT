#pragma once
// SymTable — the live instrument master: an immutable Snapshot (perfect
// hash + static data) behind one atomic pointer, with epoch-based
// reclamation so readers never lock, never retry, never fence a write.
//
// The read side is deliberately dumber than a seqlock (riskgate's choice
// for hot-reloaded limits): a reader announces the epoch it entered, loads
// the snapshot pointer, and reads plain immutable memory. No version
// re-check, no retry loop — an in-flight reader can never observe a torn
// snapshot because snapshots are never mutated, only replaced. The writer
// pays everything: it clones, rebuilds the perfect hash, publishes with one
// release exchange, and frees an old snapshot only once every announced
// reader epoch has moved past the retire point (or gone quiescent).
//
// Single writer assumed (symbol adds come from one control-plane thread).

#include <atomic>
#include <cstdint>
#include <vector>

#include "hft/common/platform.hpp"
#include "st/mph.hpp"
#include "st/sym.hpp"

namespace st {

struct Instrument {
  Sym sym{};
  u32 id = 0;     // stable dense id: assigned once, survives rebuilds
  u32 lot = 100;  // round-lot size
  i32 tick = 100;    // price tick, 1e-4 currency units
  i32 mult = 1;      // contract multiplier
  u16 venue = 0;
  u8 asset = 0;   // 0 equity, 1 future, ...
  u8 status = 0;  // 0 active, 1 halted
};

// Immutable after build. Lookup by wire symbol or by stable id.
struct Snapshot {
  u64 version = 0;
  Mph mph;
  std::vector<Instrument> byid;  // stable id -> static data

  HFT_FORCE_INLINE const Instrument* find(Sym k) const noexcept {
    const u32 id = mph.lookup(k);
    return id == Mph::kMiss ? nullptr : &byid[id];
  }

  bool build(const std::vector<Instrument>& universe) {
    byid = universe;
    std::vector<Sym> keys;
    std::vector<u32> ids;
    keys.reserve(byid.size());
    ids.reserve(byid.size());
    for (const Instrument& ins : byid) {
      keys.push_back(ins.sym);
      ids.push_back(ins.id);
    }
    return mph.build(keys, ids);
  }
};

class SymTable {
 public:
  static constexpr unsigned kMaxReaders = 16;
  static constexpr u64 kQuiescent = ~u64{0};

  struct alignas(hft::kCacheLine) Reader {
    std::atomic<u64> epoch{kQuiescent};
  };

  SymTable() = default;
  ~SymTable() {
    delete live_.load(std::memory_order_relaxed);
    for (const Retired& r : retired_) delete r.snap;
  }
  SymTable(const SymTable&) = delete;
  SymTable& operator=(const SymTable&) = delete;

  // ---- reader side ---------------------------------------------------------
  Reader& register_reader() noexcept {
    return readers_[nreaders_.fetch_add(1, std::memory_order_relaxed) % kMaxReaders];
  }

  // Announce presence, then take the pointer. The seq_cst fence orders the
  // announcement before the pointer load against the writer's fence in
  // reclaim(): a writer that saw no reader in epoch E cannot have its
  // published-later snapshot missed by a reader announcing E.
  HFT_FORCE_INLINE const Snapshot* pin(Reader& r) const noexcept {
    r.epoch.store(epoch_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return live_.load(std::memory_order_acquire);
  }

  HFT_FORCE_INLINE void unpin(Reader& r) const noexcept {
    r.epoch.store(kQuiescent, std::memory_order_release);
  }

  // ---- writer side (single thread) -----------------------------------------
  // Replace the universe wholesale (initial load) or add one instrument
  // (intraday listing): clone + rebuild + publish + reclaim.
  bool load(const std::vector<Instrument>& universe) {
    auto* snap = new Snapshot;
    if (!snap->build(universe)) {
      delete snap;
      return false;
    }
    publish(snap);
    return true;
  }

  bool add(Instrument ins) {
    const Snapshot* cur = live_.load(std::memory_order_relaxed);
    std::vector<Instrument> next = cur ? cur->byid : std::vector<Instrument>{};
    ins.id = static_cast<u32>(next.size());  // stable ids are assign-once
    next.push_back(ins);
    return load(next);
  }

  [[nodiscard]] u64 version() const noexcept { return version_; }
  [[nodiscard]] u64 retired_pending() const noexcept { return retired_.size(); }
  [[nodiscard]] u64 freed() const noexcept { return freed_; }

  // Free every retired snapshot no announced reader can still hold.
  void reclaim() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    u64 min_epoch = kQuiescent;
    const unsigned n =
        nreaders_.load(std::memory_order_relaxed) < kMaxReaders
            ? nreaders_.load(std::memory_order_relaxed)
            : kMaxReaders;
    for (unsigned i = 0; i < n; ++i) {
      const u64 e = readers_[i].epoch.load(std::memory_order_acquire);
      if (e < min_epoch) min_epoch = e;
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < retired_.size(); ++i) {
      if (retired_[i].epoch < min_epoch) {
        delete retired_[i].snap;
        ++freed_;
      } else {
        retired_[kept++] = retired_[i];
      }
    }
    retired_.resize(kept);
  }

 private:
  void publish(Snapshot* snap) noexcept {
    snap->version = ++version_;
    const Snapshot* old = live_.exchange(snap, std::memory_order_acq_rel);
    if (old != nullptr)
      retired_.push_back(Retired{old, epoch_.load(std::memory_order_relaxed)});
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    reclaim();
  }

  struct Retired {
    const Snapshot* snap;
    u64 epoch;  // epoch at retire time: free once min announced epoch > this
  };

  std::atomic<const Snapshot*> live_{nullptr};
  std::atomic<u64> epoch_{1};
  mutable Reader readers_[kMaxReaders];
  std::atomic<unsigned> nreaders_{0};
  std::vector<Retired> retired_;
  u64 version_ = 0;
  u64 freed_ = 0;
};

}  // namespace st
