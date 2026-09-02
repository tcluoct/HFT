#pragma once
// plat/rep/md5.hpp — RFC 1321 MD5: the checksum line futures brokers ask for
// in a program-trading access report ("软件压缩包（或安装包）的MD5码", 95-3
// §4). This is a file-integrity checksum shown to a compliance reviewer, not
// a security primitive — MD5 is what the regulatory template names, not a
// choice made for collision resistance.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace plat::rep {

namespace md5_detail {

inline std::uint32_t left_rotate(std::uint32_t x, std::uint32_t c) noexcept {
  return (x << c) | (x >> (32 - c));
}

// Per-round shift amounts and the sine-derived additive constants (RFC 1321 §3.4).
inline constexpr std::uint32_t kShift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

inline constexpr std::uint32_t kSine[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
    0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
    0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
    0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
    0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
    0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
    0xeb86d391};

}  // namespace md5_detail

class Md5 {
 public:
  Md5() = default;

  void update(const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    total_len_ += len;
    while (len > 0) {
      const std::size_t take = (len < 64 - buf_len_) ? len : (64 - buf_len_);
      std::memcpy(buf_ + buf_len_, p, take);
      buf_len_ += take;
      p += take;
      len -= take;
      if (buf_len_ == 64) {
        process(buf_);
        buf_len_ = 0;
      }
    }
  }

  // RFC 1321 padding: one 0x80 byte, zeros out to 56 mod 64, then the
  // original bit-length as a little-endian 64-bit word.
  std::string hex_digest() {
    const std::uint64_t bit_len = total_len_ * 8;
    const unsigned char pad = 0x80;
    update(&pad, 1);
    const unsigned char zero = 0;
    while (buf_len_ != 56) update(&zero, 1);
    unsigned char len_bytes[8];
    for (int i = 0; i < 8; ++i)
      len_bytes[i] = static_cast<unsigned char>(bit_len >> (8 * i));
    std::memcpy(buf_ + buf_len_, len_bytes, 8);
    process(buf_);
    buf_len_ = 0;

    char out[33];
    const auto* h = reinterpret_cast<const unsigned char*>(state_);
    for (int i = 0; i < 16; ++i) std::snprintf(out + i * 2, 3, "%02x", h[i]);
    return std::string(out, 32);
  }

 private:
  void process(const unsigned char block[64]) noexcept {
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i)
      m[i] = static_cast<std::uint32_t>(block[i * 4]) |
             (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
             (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
             (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    for (int i = 0; i < 64; ++i) {
      std::uint32_t f;
      int g;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
      }
      const std::uint32_t temp = d;
      d = c;
      c = b;
      b = b + md5_detail::left_rotate(a + f + md5_detail::kSine[i] + m[g], md5_detail::kShift[i]);
      a = temp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
  }

  std::uint32_t state_[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  unsigned char buf_[64];
  std::size_t buf_len_ = 0;
  std::uint64_t total_len_ = 0;
};

inline std::string md5_hex(const void* data, std::size_t len) {
  Md5 m;
  m.update(data, len);
  return m.hex_digest();
}

// Streams the file through MD5 in fixed-size chunks — no whole-file read, so
// this is fine on a multi-hundred-MB installer package. Returns false if the
// file can't be opened or a read fails partway through.
inline bool md5_file_hex(const std::filesystem::path& path, std::string& out_hex,
                          std::uint64_t& out_size) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  Md5 m;
  std::vector<char> buf(1 << 16);
  std::uint64_t total = 0;
  while (f.read(buf.data(), static_cast<std::streamsize>(buf.size())) || f.gcount() > 0) {
    const auto n = static_cast<std::size_t>(f.gcount());
    m.update(buf.data(), n);
    total += n;
  }
  if (f.bad()) return false;
  out_hex = m.hex_digest();
  out_size = total;
  return true;
}

}  // namespace plat::rep
