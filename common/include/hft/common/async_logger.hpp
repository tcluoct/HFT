#pragma once
// Deferred-formatting async logger (NanoLog-style, simplified).
//
// Hot threads enqueue a 40-byte binary record that stores a *pointer to a
// string-literal format* plus raw integer args -- no formatting, no I/O, no
// allocation, no locks on the hot path. A background thread does the
// formatting and file writes. When a queue is full the record is dropped and
// counted: a latency-critical path must never block on its logger.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "hft/common/spsc_ring.hpp"
#include "hft/common/tsc_clock.hpp"

namespace hft {

struct LogRecord {
  std::uint64_t tsc;
  const char* fmt;  // MUST be a string literal with up to 3 %llu / %lld
  std::uint64_t a, b, c;
};

class LogQueue {
 public:
  HFT_FORCE_INLINE void log(const char* fmt, std::uint64_t a = 0, std::uint64_t b = 0,
                            std::uint64_t c = 0) noexcept {
    if (!ring_.try_push(LogRecord{rdtsc(), fmt, a, b, c})) [[unlikely]] ++drops_;
  }

  [[nodiscard]] std::uint64_t drops() const noexcept { return drops_; }

 private:
  friend class AsyncLogger;
  SpscRing<LogRecord, 4096> ring_;
  std::uint64_t drops_ = 0;  // producer-owned; read after threads join
};

class AsyncLogger {
 public:
  static constexpr unsigned kQueues = 4;

  explicit AsyncLogger(const char* path) : tsc0_(rdtsc()) {
    file_ = std::fopen(path, "w");
    worker_ = std::jthread([this](std::stop_token st) { run(st); });
  }

  ~AsyncLogger() {
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();  // run() drains queues before exiting
    if (file_) std::fclose(file_);
  }

  [[nodiscard]] LogQueue& queue(unsigned i) noexcept { return queues_[i]; }

  [[nodiscard]] std::uint64_t total_drops() const noexcept {
    std::uint64_t n = 0;
    for (const auto& q : queues_) n += q.drops();
    return n;
  }

 private:
  void run(std::stop_token st) {
    LogRecord r;
    while (!st.stop_requested()) {
      bool any = false;
      for (auto& q : queues_)
        while (q.ring_.try_pop(r)) {
          write(r);
          any = true;
        }
      if (!any) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    for (auto& q : queues_)  // final drain
      while (q.ring_.try_pop(r)) write(r);
    if (file_) std::fflush(file_);
  }

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4774)  // fmt is not a literal *here* -- it was at the call site
#endif
  void write(const LogRecord& r) {
    if (!file_) return;
    const unsigned long long ns = TscClock::to_ns(r.tsc - tsc0_);
    std::fprintf(file_, "[%12llu ns] ", ns);
    std::fprintf(file_, r.fmt, static_cast<unsigned long long>(r.a),
                 static_cast<unsigned long long>(r.b), static_cast<unsigned long long>(r.c));
    std::fputc('\n', file_);
  }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

  std::FILE* file_ = nullptr;
  std::uint64_t tsc0_;
  std::array<LogQueue, kQueues> queues_{};
  std::jthread worker_;
};

}  // namespace hft
