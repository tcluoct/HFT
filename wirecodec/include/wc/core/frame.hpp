#pragma once
// Framing: [ u16 msg_type | u16 body_len | body | u32 crc32c ], header and
// trailer big-endian like the body. The checksum is hardware CRC32-C
// (SSE4.2 _mm_crc32: 8 bytes per instruction, ~3 cycles throughput-free on
// the frame sizes here) over type+len+body — a receiver rejects a damaged
// frame before a single field is read.

#include <cstddef>
#include <cstdint>
#include <span>

#include "hft/common/platform.hpp"
#include "wc/core/codec.hpp"

namespace wc {

inline constexpr std::size_t kHeaderSize = 4;
inline constexpr std::size_t kTrailerSize = 4;

template <typename Msg>
inline constexpr std::size_t kFrameSize = kHeaderSize + Msg::body_size + kTrailerSize;

HFT_FORCE_INLINE std::uint32_t crc32c(std::span<const std::byte> d) noexcept {
  std::uint64_t c = 0xFFFFFFFFull;
  const std::byte* p = d.data();
  std::size_t n = d.size();
  while (n >= 8) {
    c = _mm_crc32_u64(c, load_raw<std::uint64_t>(p));
    p += 8;
    n -= 8;
  }
  std::uint32_t c32 = static_cast<std::uint32_t>(c);
  while (n) {
    c32 = _mm_crc32_u8(c32, static_cast<unsigned char>(*p));
    ++p;
    --n;
  }
  return ~c32;
}

// Write the header, hand back a typed pen over the body.
template <typename Msg>
[[nodiscard]] HFT_FORCE_INLINE Encoder<Msg> begin_frame(std::span<std::byte> buf) noexcept {
  store_raw(buf.data(), wire_swap<ByteOrder::kBig>(Msg::msg_type));
  store_raw(buf.data() + 2,
            wire_swap<ByteOrder::kBig>(static_cast<std::uint16_t>(Msg::body_size)));
  return Encoder<Msg>(buf.subspan(kHeaderSize, Msg::body_size));
}

// Compute and append the checksum once every field is set.
template <typename Msg>
HFT_FORCE_INLINE void seal_frame(std::span<std::byte> buf) noexcept {
  constexpr std::size_t n = kHeaderSize + Msg::body_size;
  store_raw(buf.data() + n, wire_swap<ByteOrder::kBig>(crc32c(buf.first(n))));
}

enum class FrameError : std::uint8_t { kOk, kTruncated, kBadCrc };

struct FrameInfo {
  std::uint16_t msg_type;
  std::span<const std::byte> body;
  std::size_t frame_len;  // consume this many bytes and parse again
};

// Parse the next frame off an rx byte stream. kTruncated = wait for more
// bytes; kBadCrc = frame (and connection, in production) is toast.
HFT_FORCE_INLINE FrameError try_parse(std::span<const std::byte> buf, FrameInfo& out) noexcept {
  if (buf.size() < kHeaderSize + kTrailerSize) return FrameError::kTruncated;
  const auto len = wire_swap<ByteOrder::kBig>(load_raw<std::uint16_t>(buf.data() + 2));
  const std::size_t frame_len = kHeaderSize + len + kTrailerSize;
  if (buf.size() < frame_len) return FrameError::kTruncated;
  const auto stored =
      wire_swap<ByteOrder::kBig>(load_raw<std::uint32_t>(buf.data() + kHeaderSize + len));
  if (stored != crc32c(buf.first(kHeaderSize + len))) return FrameError::kBadCrc;
  out.msg_type = wire_swap<ByteOrder::kBig>(load_raw<std::uint16_t>(buf.data()));
  out.body = buf.subspan(kHeaderSize, len);
  out.frame_len = frame_len;
  return FrameError::kOk;
}

}  // namespace wc
