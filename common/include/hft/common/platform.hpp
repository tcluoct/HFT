#pragma once
// Platform primitives: cache-line size, forced inlining, spin hints,
// thread pinning, prefetch. Everything the hot path needs from the OS/CPU.
// Shared toolbox for every project in the hft solution (namespace hft).

#include <cstddef>
#include <cstdint>
#include <new>

#if defined(_MSC_VER)
  #include <intrin.h>
#else
  #include <immintrin.h>
  #include <x86intrin.h>
#endif

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <pthread.h>
  #include <sched.h>
  #include <unistd.h>
#endif

#if defined(_MSC_VER)
  #define HFT_FORCE_INLINE __forceinline
#else
  #define HFT_FORCE_INLINE inline __attribute__((always_inline))
#endif

namespace hft {

// Destructive interference size: two objects this far apart never false-share.
#ifdef __cpp_lib_hardware_interference_size
inline constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

// PAUSE: tells the core we are spin-waiting (saves power, frees pipeline
// resources for the sibling hyperthread, avoids memory-order mis-speculation).
HFT_FORCE_INLINE void cpu_relax() noexcept { _mm_pause(); }

HFT_FORCE_INLINE void prefetch_l1(const void* p) noexcept {
#if defined(_MSC_VER)
  _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
#else
  __builtin_prefetch(p, 0, 3);
#endif
}

// Pin the calling thread to one core: keeps caches/TLB warm and gives the
// busy-spin loops a core of their own (prod: isolcpus/nohz_full on Linux).
inline bool pin_current_thread(unsigned core) noexcept {
#if defined(_WIN32)
  return ::SetThreadAffinityMask(::GetCurrentThread(), DWORD_PTR{1} << core) != 0;
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
#endif
}

inline bool boost_current_thread_priority() noexcept {
#if defined(_WIN32)
  return ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
#else
  sched_param sp{};
  sp.sched_priority = 80;
  return ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &sp) == 0;
#endif
}

inline std::uint32_t current_pid() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

inline unsigned logical_core_count() noexcept {
#if defined(_WIN32)
  SYSTEM_INFO si;
  ::GetSystemInfo(&si);
  return static_cast<unsigned>(si.dwNumberOfProcessors);
#else
  const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? static_cast<unsigned>(n) : 1u;
#endif
}

}  // namespace hft
