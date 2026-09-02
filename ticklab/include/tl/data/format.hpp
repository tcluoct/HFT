#pragma once
// Capture-file layout:
//
//   [FileHeader][Chunk 0][Chunk 1]...[Chunk N-1][ChunkDesc x N][Footer]
//
// Chunks are appended sequentially while recording (the write path is a plain
// buffered append — sequential I/O is what disks and SSDs are best at). The
// chunk index and footer are written once at close; readers find the footer
// at (file_size - sizeof(Footer)), then jump to the index. Per-chunk ts
// ranges make time-seek a binary search over the index — no scan of the data.

#include <cstdint>

namespace tl {

inline constexpr std::uint32_t kFileMagic = 0x424C4B54;  // "TKLB"
inline constexpr std::uint16_t kFormatVersion = 1;

#pragma pack(push, 1)
struct FileHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t nsym;
};
static_assert(sizeof(FileHeader) == 8);

struct ChunkDesc {
  std::uint64_t offset;    // absolute file offset of the encoded chunk
  std::uint32_t bytes;     // encoded size
  std::uint32_t count;     // ticks in the chunk
  std::uint64_t ts_first;  // for time-seek binary search
  std::uint64_t ts_last;
};
static_assert(sizeof(ChunkDesc) == 32);

struct Footer {
  std::uint64_t index_offset;
  std::uint32_t chunk_count;
  std::uint32_t magic;
  std::uint64_t total_ticks;
};
static_assert(sizeof(Footer) == 24);
#pragma pack(pop)

}  // namespace tl
