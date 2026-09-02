#pragma once
// Replayer: memory-maps a capture file and iterates ticks in storage order —
// which IS session order, so replay is deterministic by construction.
// Chunks decode on demand into a reused SoA buffer (one decode per 65536
// ticks); per-tick cost is seven array reads. seek_ts() binary-searches the
// chunk index, decodes one chunk, and scans within it — O(log chunks + chunk).

#include <cstdint>
#include <cstring>
#include <vector>

#include "tl/data/codec.hpp"
#include "tl/data/format.hpp"
#include "tl/data/mapped_file.hpp"
#include "tl/data/tick.hpp"

namespace tl {

class Replayer {
 public:
  bool open(const char* path) {
    if (!map_.open(path)) return false;
    if (map_.size() < sizeof(FileHeader) + sizeof(Footer)) return false;
    FileHeader h;
    std::memcpy(&h, map_.data(), sizeof h);
    if (h.magic != kFileMagic || h.version != kFormatVersion) return false;
    nsym_ = h.nsym;
    Footer ft;
    std::memcpy(&ft, map_.data() + map_.size() - sizeof ft, sizeof ft);
    if (ft.magic != kFileMagic) return false;
    total_ = ft.total_ticks;
    index_.resize(ft.chunk_count);
    std::memcpy(index_.data(), map_.data() + ft.index_offset,
                ft.chunk_count * sizeof(ChunkDesc));
    rewind();
    return true;
  }

  void rewind() noexcept {
    chunk_ = ~std::size_t{0};
    pos_ = 0;
    count_ = 0;
  }

  HFT_FORCE_INLINE bool next(Tick& t) noexcept {
    if (pos_ == count_) [[unlikely]] {
      if (!load_chunk(chunk_ + 1)) return false;
    }
    t.ts_ns = soa_.col[0][pos_];
    t.symbol = static_cast<SymbolId>(soa_.col[1][pos_]);
    t.kind = static_cast<std::uint8_t>(soa_.col[2][pos_]);
    t.px1 = static_cast<Price>(soa_.col[3][pos_]);
    t.px2 = static_cast<Price>(soa_.col[4][pos_]);
    t.q1 = static_cast<Qty>(soa_.col[5][pos_]);
    t.q2 = static_cast<Qty>(soa_.col[6][pos_]);
    ++pos_;
    return true;
  }

  // Position the cursor at the first tick with ts_ns >= ts.
  bool seek_ts(std::uint64_t ts) noexcept {
    // Binary search: first chunk whose ts_last >= ts.
    std::size_t lo = 0, hi = index_.size();
    while (lo < hi) {
      const std::size_t mid = (lo + hi) / 2;
      if (index_[mid].ts_last < ts)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo == index_.size()) return false;  // past end of session
    if (!load_chunk(lo)) return false;
    while (pos_ < count_ && soa_.col[0][pos_] < ts) ++pos_;
    return pos_ < count_;
  }

  [[nodiscard]] std::uint64_t total_ticks() const noexcept { return total_; }
  [[nodiscard]] std::uint16_t nsym() const noexcept { return nsym_; }
  [[nodiscard]] std::size_t chunk_count() const noexcept { return index_.size(); }
  [[nodiscard]] std::size_t file_bytes() const noexcept { return map_.size(); }

 private:
  bool load_chunk(std::size_t i) noexcept {
    if (i >= index_.size()) return false;
    chunk_ = i;
    count_ = decode_chunk(map_.data() + index_[i].offset, soa_);
    pos_ = 0;
    return true;
  }

  MappedFile map_;
  std::vector<ChunkDesc> index_;
  ChunkSoA soa_;
  std::size_t chunk_ = ~std::size_t{0};
  std::uint32_t pos_ = 0, count_ = 0;
  std::uint64_t total_ = 0;
  std::uint16_t nsym_ = 0;
};

}  // namespace tl
