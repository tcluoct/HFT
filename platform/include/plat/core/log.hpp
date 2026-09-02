#pragma once
// plat/core/log.hpp — timestamped operational logging. Every lifecycle /
// ops line carries wall-clock HH:MM:SS.mmm; the line is assembled into one
// buffer and written with a single fwrite so lines from different threads
// (engine, CTP api threads, journal writer, fleet supervisor) never
// interleave mid-line.
//
// Lines go to stderr always, and — once log_open_file() names a file — to
// that file as well. The file is opened APPEND (a restart never destroys
// the previous run's evidence) and UNBUFFERED (the last line before a crash
// is already on disk; the crash handler in plat/core/crash.hpp relies on
// this to make its post-mortem survive the process).
//
// This is COLD-path logging only (session lifecycle, operator actions,
// incidents). Hot-path observability is the telemetry module (§14):
// preresolved counters and histograms, no strings, no clock reads.

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace plat {

inline std::FILE*& log_file_slot() {
  static std::FILE* f = nullptr;
  return f;
}

// Tee every subsequent logf line into `path` (append + unbuffered). Call
// once at startup, before anything that can fail; returns false if the file
// cannot be opened (logging then continues on stderr alone).
inline bool log_open_file(const char* path) {
  std::FILE* f = std::fopen(path, "ab");
  if (!f) return false;
  std::setvbuf(f, nullptr, _IONBF, 0);
  log_file_slot() = f;
  return true;
}

inline void logf(const char* fmt, ...) {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t tt = system_clock::to_time_t(now);
  std::tm lt{};
#ifdef _WIN32
  localtime_s(&lt, &tt);
#else
  localtime_r(&tt, &lt);
#endif
  const int ms =
      static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  char line[1024];
  int n = std::snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d ", lt.tm_hour, lt.tm_min,
                        lt.tm_sec, ms);
  va_list ap;
  va_start(ap, fmt);
  const int m = std::vsnprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), fmt, ap);
  va_end(ap);
  if (m > 0) n += m;
  if (n > static_cast<int>(sizeof(line)) - 1) n = static_cast<int>(sizeof(line)) - 1;
  std::fwrite(line, 1, static_cast<std::size_t>(n), stderr);
  if (std::FILE* f = log_file_slot()) std::fwrite(line, 1, static_cast<std::size_t>(n), f);
}

// Session-start banner: full date, pid, binary tag. Per-line timestamps are
// HH:MM:SS.mmm only, so an APPENDED log file needs this to date its runs.
inline void log_banner(const char* app) {
  using namespace std::chrono;
  const std::time_t tt = system_clock::to_time_t(system_clock::now());
  std::tm lt{};
#ifdef _WIN32
  localtime_s(&lt, &tt);
  const unsigned long pid = static_cast<unsigned long>(_getpid());
#else
  localtime_r(&tt, &lt);
  const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
  logf("==== %s session start %04d-%02d-%02d (pid %lu) ====\n", app, lt.tm_year + 1900,
       lt.tm_mon + 1, lt.tm_mday, pid);
}

}  // namespace plat
