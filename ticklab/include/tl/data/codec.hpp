#pragma once
// Columnar frame-of-reference (FOR) codec.
//
// Within a chunk, each field is stored as its own column (SoA on disk). Per
// column the codec stores {base = min, width} and then (value - min) packed
// at 0/1/2/4/8 bytes per element:
//
//   * width 0: the column is CONSTANT across the chunk -> zero bytes of data
//     (kind columns, symbol columns in single-symbol files, unchanged sizes)
//   * timestamps: a chunk spans ~tens of ms, so ts-min fits 4 bytes
//   * prices: cluster around the session mids -> 2 bytes typical
//   * sizes: small integers -> 1-2 bytes
//
// This is the core idea behind real tick stores (kdb+, columnar Parquet
// encodings): columnar layout makes values self-similar, self-similar values
// compress with trivial, branch-predictable, SIMD-friendly transforms.
// Decode is a tight base+widen loop — no entropy coding on the read path.

#include <cstdint>
#include <cstring>
#include <vector>

#include "hft/common/platform.hpp"

namespace tl {

inline constexpr std::size_t kChunkTicks = 1u << 16;  // 65536 ticks per chunk

namespace codec {

inline void append_raw(std::vector<std::byte>& out, const void* p, std::size_t n) {
  const std::size_t off = out.size();
  out.resize(off + n);
  std::memcpy(out.data() + off, p, n);
}

// Encode one column of n values. Layout: [u64 base][u8 width][n * width bytes].
inline void encode_column(const std::uint64_t* v, std::size_t n, std::vector<std::byte>& out) {
  std::uint64_t mn = v[0], mx = v[0];
  for (std::size_t i = 1; i < n; ++i) {
    if (v[i] < mn) mn = v[i];
    if (v[i] > mx) mx = v[i];
  }
  const std::uint64_t range = mx - mn;
  const std::uint8_t width = range == 0            ? 0
                             : range <= 0xFFull    ? 1
                             : range <= 0xFFFFull  ? 2
                             : range <= 0xFFFFFFFFull ? 4
                                                       : 8;
  append_raw(out, &mn, sizeof mn);
  append_raw(out, &width, sizeof width);
  if (width == 0) return;

  const std::size_t off = out.size();
  out.resize(off + n * width);
  std::byte* p = out.data() + off;
  switch (width) {
    case 1:
      for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::byte>(v[i] - mn);
      break;
    case 2:
      for (std::size_t i = 0; i < n; ++i) {
        const std::uint16_t d = static_cast<std::uint16_t>(v[i] - mn);
        std::memcpy(p + i * 2, &d, 2);
      }
      break;
    case 4:
      for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t d = static_cast<std::uint32_t>(v[i] - mn);
        std::memcpy(p + i * 4, &d, 4);
      }
      break;
    default:
      for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t d = v[i] - mn;
        std::memcpy(p + i * 8, &d, 8);
      }
      break;
  }
}

// Decode one column; returns the pointer past its encoded bytes.
inline const std::byte* decode_column(const std::byte* p, std::uint64_t* v, std::size_t n) {
  std::uint64_t base;
  std::memcpy(&base, p, sizeof base);
  p += sizeof base;
  const std::uint8_t width = static_cast<std::uint8_t>(*p);
  p += 1;
  switch (width) {
    case 0:
      for (std::size_t i = 0; i < n; ++i) v[i] = base;
      return p;
    case 1:
      for (std::size_t i = 0; i < n; ++i) v[i] = base + static_cast<std::uint8_t>(p[i]);
      return p + n;
    case 2:
      for (std::size_t i = 0; i < n; ++i) {
        std::uint16_t d;
        std::memcpy(&d, p + i * 2, 2);
        v[i] = base + d;
      }
      return p + n * 2;
    case 4:
      for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t d;
        std::memcpy(&d, p + i * 4, 4);
        v[i] = base + d;
      }
      return p + n * 4;
    default:
      for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t d;
        std::memcpy(&d, p + i * 8, 8);
        v[i] = base + d;
      }
      return p + n * 8;
  }
}

}  // namespace codec

// Chunk in SoA form: seven u64 columns (ts, symbol, kind, px1, px2, q1, q2).
struct ChunkSoA {
  static constexpr unsigned kCols = 7;
  std::vector<std::uint64_t> col[kCols];

  ChunkSoA() {
    for (auto& c : col) c.resize(kChunkTicks);
  }
};

inline void encode_chunk(const ChunkSoA& soa, std::uint32_t n, std::vector<std::byte>& out) {
  out.clear();
  codec::append_raw(out, &n, sizeof n);
  for (const auto& c : soa.col) codec::encode_column(c.data(), n, out);
}

// Returns the tick count decoded.
inline std::uint32_t decode_chunk(const std::byte* p, ChunkSoA& soa) {
  std::uint32_t n;
  std::memcpy(&n, p, sizeof n);
  p += sizeof n;
  for (auto& c : soa.col) p = codec::decode_column(p, c.data(), n);
  return n;
}

}  // namespace tl
