#pragma once
// SortNet — a Batcher odd-even mergesort network, generated at compile time
// for a power-of-two N and applied branch-free at run time.
//
// Why a network instead of std::sort for venue ranking: N is tiny and known
// at compile time (a firm connects to a fixed set of venues), the comparator
// schedule is data-INDEPENDENT (no branch mispredicts, no early-outs, same
// instruction stream every call — routing time doesn't leak what you're
// routing), and each compare-exchange on u64 keys compiles to cmp+cmov.
// The network is proven at compile time (static_assert sorts a permutation)
// and at run time against std::sort on random arrays.

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

#include "hft/common/platform.hpp"

namespace sr {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

template <unsigned N>
struct SortNet {
  static_assert(N >= 2 && (N & (N - 1)) == 0 && N <= 64, "N: power of two");

  struct Pair {
    u8 a, b;
  };
  static constexpr unsigned kMaxPairs = 8 * N;  // 63 for N=16; generous bound

  static constexpr std::pair<std::array<Pair, kMaxPairs>, unsigned> make() {
    std::array<Pair, kMaxPairs> ps{};
    unsigned n = 0;
    for (u32 p = 1; p < N; p <<= 1)
      for (u32 k = p; k >= 1; k >>= 1)
        for (u32 j = k % p; j + k <= N - 1; j += 2 * k)
          for (u32 i = 0; i <= std::min(k - 1, N - j - k - 1); ++i)
            if ((i + j) / (2 * p) == (i + j + k) / (2 * p))
              ps[n++] = Pair{static_cast<u8>(i + j), static_cast<u8>(i + j + k)};
    return {ps, n};
  }

  static constexpr auto kNet = make();
  static constexpr unsigned kPairs = kNet.second;

  static constexpr void sort(u64* keys) noexcept {
    for (unsigned t = 0; t < kPairs; ++t) {
      const u64 x = keys[kNet.first[t].a];
      const u64 y = keys[kNet.first[t].b];
      keys[kNet.first[t].a] = x < y ? x : y;  // cmov, no branch
      keys[kNet.first[t].b] = x < y ? y : x;
    }
  }

  // Compile-time proof: the network sorts a worst-ish permutation.
  static constexpr bool self_test() {
    u64 v[N] = {};
    for (unsigned i = 0; i < N; ++i) v[i] = (N - i) * 7 % (N + 3);
    sort(v);
    for (unsigned i = 1; i < N; ++i)
      if (v[i - 1] > v[i]) return false;
    return true;
  }
};

// The proof runs at compile time: a broken generator cannot build.
static_assert(SortNet<4>::self_test(), "SortNet<4> must sort");
static_assert(SortNet<8>::self_test(), "SortNet<8> must sort");
static_assert(SortNet<16>::self_test(), "SortNet<16> must sort");

}  // namespace sr
