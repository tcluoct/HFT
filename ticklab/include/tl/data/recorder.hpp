#pragma once
// Recorder: accumulates ticks into an in-memory SoA chunk; when the chunk
// fills, encodes it (FOR codec) and appends with buffered sequential writes.
// The index and footer land at close(). No allocation per tick.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "tl/data/codec.hpp"
#include "tl/data/format.hpp"
#include "tl/data/tick.hpp"

namespace tl {

class Recorder {
 public:
  Recorder(const char* path, std::uint16_t nsym) {
    f_ = std::fopen(path, "wb");
    if (!f_) return;
    const FileHeader h{kFileMagic, kFormatVersion, nsym};
    std::fwrite(&h, sizeof h, 1, f_);
    offset_ = sizeof h;
    encoded_.reserve(kChunkTicks * 24);
  }

  ~Recorder() { close(); }

  [[nodiscard]] bool ok() const noexcept { return f_ != nullptr; }

  void add(const Tick& t) noexcept {
    soa_.col[0][n_] = t.ts_ns;
    soa_.col[1][n_] = t.symbol;
    soa_.col[2][n_] = t.kind;
    soa_.col[3][n_] = static_cast<std::uint64_t>(t.px1);
    soa_.col[4][n_] = static_cast<std::uint64_t>(t.px2);
    soa_.col[5][n_] = t.q1;
    soa_.col[6][n_] = t.q2;
    if (++n_ == kChunkTicks) flush_chunk();
  }

  void close() {
    if (!f_) return;
    if (n_ != 0) flush_chunk();
    const std::uint64_t index_offset = offset_;
    if (!index_.empty())
      std::fwrite(index_.data(), sizeof(ChunkDesc), index_.size(), f_);
    const Footer ft{index_offset, static_cast<std::uint32_t>(index_.size()), kFileMagic,
                    total_ticks_};
    std::fwrite(&ft, sizeof ft, 1, f_);
    std::fclose(f_);
    f_ = nullptr;
  }

  [[nodiscard]] std::uint64_t total_ticks() const noexcept { return total_ticks_; }
  [[nodiscard]] std::uint64_t file_bytes() const noexcept {
    return offset_ + index_.size() * sizeof(ChunkDesc) + sizeof(Footer);
  }

 private:
  void flush_chunk() {
    encode_chunk(soa_, n_, encoded_);
    std::fwrite(encoded_.data(), 1, encoded_.size(), f_);
    index_.push_back(ChunkDesc{offset_, static_cast<std::uint32_t>(encoded_.size()), n_,
                               soa_.col[0][0], soa_.col[0][n_ - 1]});
    offset_ += encoded_.size();
    total_ticks_ += n_;
    n_ = 0;
  }

  std::FILE* f_ = nullptr;
  ChunkSoA soa_;
  std::uint32_t n_ = 0;
  std::uint64_t offset_ = 0;
  std::uint64_t total_ticks_ = 0;
  std::vector<std::byte> encoded_;
  std::vector<ChunkDesc> index_;
};

}  // namespace tl
