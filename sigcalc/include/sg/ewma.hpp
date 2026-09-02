#pragma once
// EwmaLattice — eight exponential moving averages of the same input at
// horizons alpha = 2^-k, updated with shifts only:
//
//     m += (x - m) >> k        // k in {2,3,4,5,6,8,10,12}
//
// No multiplies, no floats, and the state is exactly reproducible. The
// AVX2 path updates all eight lanes at once. AVX2 has no per-lane
// *arithmetic* 64-bit shift (_mm256_srav_epi64 is AVX-512), so it uses the
// bias trick: for B = 2^45 (divisible by every 2^k used, and larger than
// any |x - m| here),
//
//     (x - m) >> k  ==  ((x - m + B) >> k) - (B >> k)     [logical shifts]
//
// because adding B makes the operand non-negative (logical == arithmetic)
// and B contributes exactly B >> k, which is subtracted back out. The
// equality is exact, and the tests assert scalar == AVX2 bit-for-bit on
// every step.

#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif

#include "hft/common/platform.hpp"
#include "sg/types.hpp"

namespace sg {

struct alignas(32) EwmaLattice {
  static constexpr unsigned kLanes = 8;
  static constexpr i64 kBias = i64{1} << 45;
  static constexpr i64 kShift[kLanes] = {2, 3, 4, 5, 6, 8, 10, 12};

  i64 m[kLanes] = {};
  bool seeded = false;

  HFT_FORCE_INLINE void update_scalar(i64 x) noexcept {
    if (!seeded) return seed(x);
    for (unsigned i = 0; i < kLanes; ++i) m[i] += (x - m[i]) >> kShift[i];
  }

  HFT_FORCE_INLINE void update_avx2(i64 x) noexcept {
    if (!seeded) return seed(x);
    // Constant tables are static so the compiler keeps them in .rdata and
    // hoists the loads out of hot loops instead of re-materializing them.
    alignas(32) static constexpr i64 kShLo[4] = {kShift[0], kShift[1], kShift[2], kShift[3]};
    alignas(32) static constexpr i64 kShHi[4] = {kShift[4], kShift[5], kShift[6], kShift[7]};
    alignas(32) static constexpr i64 kBshLo[4] = {kBias >> kShift[0], kBias >> kShift[1],
                                                  kBias >> kShift[2], kBias >> kShift[3]};
    alignas(32) static constexpr i64 kBshHi[4] = {kBias >> kShift[4], kBias >> kShift[5],
                                                  kBias >> kShift[6], kBias >> kShift[7]};
    const __m256i xv = _mm256_set1_epi64x(x);
    const __m256i bias = _mm256_set1_epi64x(kBias);
    const __m256i sh_lo = _mm256_load_si256(reinterpret_cast<const __m256i*>(kShLo));
    const __m256i sh_hi = _mm256_load_si256(reinterpret_cast<const __m256i*>(kShHi));
    const __m256i bsh_lo = _mm256_load_si256(reinterpret_cast<const __m256i*>(kBshLo));
    const __m256i bsh_hi = _mm256_load_si256(reinterpret_cast<const __m256i*>(kBshHi));
    __m256i lo = _mm256_load_si256(reinterpret_cast<const __m256i*>(&m[0]));
    __m256i hi = _mm256_load_si256(reinterpret_cast<const __m256i*>(&m[4]));
    __m256i d = _mm256_add_epi64(_mm256_sub_epi64(xv, lo), bias);
    lo = _mm256_add_epi64(lo, _mm256_sub_epi64(_mm256_srlv_epi64(d, sh_lo), bsh_lo));
    d = _mm256_add_epi64(_mm256_sub_epi64(xv, hi), bias);
    hi = _mm256_add_epi64(hi, _mm256_sub_epi64(_mm256_srlv_epi64(d, sh_hi), bsh_hi));
    _mm256_store_si256(reinterpret_cast<__m256i*>(&m[0]), lo);
    _mm256_store_si256(reinterpret_cast<__m256i*>(&m[4]), hi);
  }

  [[nodiscard]] i64 fastest() const noexcept { return m[0]; }
  [[nodiscard]] i64 slowest() const noexcept { return m[kLanes - 1]; }

 private:
  HFT_FORCE_INLINE void seed(i64 x) noexcept {
    for (unsigned i = 0; i < kLanes; ++i) m[i] = x;
    seeded = true;
  }
};

}  // namespace sg
