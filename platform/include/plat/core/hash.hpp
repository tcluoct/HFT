#pragma once
// plat/core/hash.hpp — incremental FNV-1a used for component state hashes.
// Determinism is a tested property, not an aspiration (§16 golden replay):
// every stateful component exposes state_hash(), and replay/recovery tests
// assert bit-equality of these hashes against the uninterrupted run.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace plat {

struct StateHash {
  std::uint64_t h = 0xCBF29CE484222325ull;

  // Word-at-a-time FNV-1a (byte loop only for the tail). The stream hash
  // runs over every envelope's payload, and FNV's dependent-multiply chain
  // priced the whole tick: byte-wise it measured ~234 ns/tick on the bench
  // (208 serial multiplies for a snapshot payload); word-wise it is 26.
  void mix_bytes(const void* p, std::size_t n) noexcept {
    const auto* b = static_cast<const unsigned char*>(p);
    while (n >= 8) {
      std::uint64_t w;
      std::memcpy(&w, b, 8);
      h ^= w;
      h *= 0x100000001B3ull;
      b += 8;
      n -= 8;
    }
    for (std::size_t i = 0; i < n; ++i) {
      h ^= b[i];
      h *= 0x100000001B3ull;
    }
  }

  template <typename T>
    requires std::is_trivially_copyable_v<T>
  void mix(const T& v) noexcept {
    mix_bytes(&v, sizeof(T));
  }

  [[nodiscard]] std::uint64_t value() const noexcept { return h; }
};

}  // namespace plat
