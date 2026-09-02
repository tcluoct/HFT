#pragma once
// plat/core/thread.hpp — core pinning and scheduling class for the few
// threads the platform runs (§6.3): the engine thread and the journal
// writer get dedicated cores in colo; the CTP api threads and the ops
// console stay on the shared pool. Pinning kills cross-core migration
// jitter; time-critical class keeps the hypervisor/OS from parking the
// engine mid-tick.

#ifdef _WIN32
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
#endif

namespace plat {

// Switch the console to UTF-8 so log messages (em-dashes, and Chinese in CTP
// error strings) render instead of mojibake on a GBK-code-page terminal.
// One call at startup; harmless on Linux.
inline void init_console_utf8() {
#ifdef _WIN32
  SetConsoleOutputCP(65001u);
#endif
}

// Pin the calling thread to one core. core < 0 = leave unpinned.
inline bool pin_thread_to_core(int core) {
  if (core < 0) return false;
#ifdef _WIN32
  return SetThreadAffinityMask(GetCurrentThread(), 1ull << core) != 0;
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#endif
}

// Raise the calling thread to the highest scheduling class the OS grants.
inline bool set_thread_time_critical() {
#ifdef _WIN32
  return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
#else
  sched_param p{};
  p.sched_priority = sched_get_priority_max(SCHED_FIFO);
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0;
#endif
}

}  // namespace plat
