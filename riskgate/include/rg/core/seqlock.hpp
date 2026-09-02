#pragma once
// Single-record seqlock: one control-plane writer publishes a trivially
// copyable record; hot-path readers get a consistent snapshot without ever
// writing shared memory (no RMW, no reader registration, scales to any
// number of readers).
//
// This is the *write-rare* flavor of the seqlock family — seqbus uses per-slot
// stamps to move a firehose of data; here the record changes a few times a
// second at most, so readers essentially never retry and the read collapses
// to: load version, copy 56 bytes, load version, compare.
//
// The payload is relaxed-atomic words, not raw bytes: that makes the torn
// read that seqlocks intentionally allow *defined behavior* under the C++
// memory model (Boehm, MSPC'12) while compiling to plain MOVs on x86-64.

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <type_traits>

#include "hft/common/platform.hpp"

namespace rg {

template <typename T>
  requires std::is_trivially_copyable_v<T> && (sizeof(T) % 8 == 0)
class Seqlock {
 public:
  static constexpr std::size_t kWords = sizeof(T) / 8;
  using Words = std::array<std::uint64_t, kWords>;

  // Control-plane thread only (single writer).
  void publish(const T& v) noexcept {
    const std::uint32_t v0 = ver_.load(std::memory_order_relaxed);
    ver_.store(v0 + 1, std::memory_order_relaxed);  // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    const Words w = std::bit_cast<Words>(v);
    for (std::size_t i = 0; i < kWords; ++i)
      words_[i].store(w[i], std::memory_order_relaxed);
    ver_.store(v0 + 2, std::memory_order_release);  // even: published
  }

  // Any thread. Lock-free for readers; retries only while a write is in
  // flight, which for a write-rare record is (measurably) almost never.
  T read(std::uint64_t* retries = nullptr) const noexcept {
    for (;;) {
      const std::uint32_t v1 = ver_.load(std::memory_order_acquire);
      if (!(v1 & 1u)) [[likely]] {
        Words w;
        for (std::size_t i = 0; i < kWords; ++i)
          w[i] = words_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (ver_.load(std::memory_order_relaxed) == v1) [[likely]]
          return std::bit_cast<T>(w);
      }
      if (retries) ++*retries;
      hft::cpu_relax();
    }
  }

 private:
  std::atomic<std::uint32_t> ver_{0};
  std::array<std::atomic<std::uint64_t>, kWords> words_{};
};

}  // namespace rg
