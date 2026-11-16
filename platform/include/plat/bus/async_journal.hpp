#pragma once
// plat/bus/async_journal.hpp — the journal writer on its own thread (§7.2
// production form). The engine's per-envelope journal cost drops to one
// 256-byte publish into the broadcast ring; the file I/O — CRT buffering,
// flush syscalls, disk stalls — runs on a dedicated consumer thread that
// drains the ring in batches. This is the multi-reader ring earning its
// keep: the writer (engine) is wait-free and never learns the journal
// thread exists.
//
// Loss policy is Intolerant and fail-closed: the journal is the legal audit
// record and the replay substrate — if the consumer is ever lapped (disk
// stalled long enough to overrun a 64 Ki-slot buffer), the record has a
// hole, healthy() flips false, and the driver halts trading. We do not
// trade without an audit trail.
//
// Determinism: envelopes enter the ring in canonical-stream order from the
// single engine thread and leave in order on the journal thread, so the
// file is byte-identical to what the synchronous writer produced — the
// JournalReader/replay contract is unchanged.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "plat/bus/envelope.hpp"
#include "plat/bus/journal.hpp"
#include "plat/bus/ring.hpp"
#include "plat/core/thread.hpp"
#include "plat/tel/telemetry.hpp"

namespace plat {

class AsyncJournal {
 public:
  AsyncJournal() = default;
  AsyncJournal(const AsyncJournal&) = delete;
  AsyncJournal& operator=(const AsyncJournal&) = delete;
  ~AsyncJournal() { finish(); }

  // Opens the journal file and starts the writer thread (pinned to
  // `writer_core` when ≥ 0).
  bool open(const std::string& path, std::uint32_t day, SiteId site, int writer_core = -1) {
    if (!jw_.open(path, day, site)) return false;
    ring_ = std::make_unique<Ring>(kSlots);
    writer_.emplace(*ring_);
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this, writer_core] { run(writer_core); });
    open_ = true;
    return true;
  }

  // Engine thread, hot path: one wait-free 256-byte publish, no syscalls.
  HFT_FORCE_INLINE void append(const Envelope& e) noexcept {
    const auto begin = latency_enabled_ ? perf_now_ns() : 0;
    writer_->publish(e, begin);
    if (latency_enabled_) enqueue_.record(perf_now_ns() - begin);
  }

  // Called by the producer only; worker uses each record's metadata instead.
  void set_latency_enabled(bool enabled) { latency_enabled_ = enabled; }
  struct TimingSnapshot { LatencyHisto enqueue, queue, write_batch, flush; };
  // Cold path, called by the engine/report owner. Worker publishes copies;
  // it never writes the engine's Telemetry object.
  TimingSnapshot timing_snapshot() {
    std::lock_guard lock(timing_mutex_);
    auto result = published_timing_;
    result.enqueue = enqueue_;
    return result;
  }

  // False once the writer thread was lapped (audit hole) — fail closed.
  [[nodiscard]] bool healthy() const noexcept {
    return !fault_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool is_open() const noexcept { return open_; }

  // Drain everything still in flight, seal the header, join. Idempotent.
  void finish() {
    if (thread_.joinable()) {
      stop_.store(true, std::memory_order_release);
      thread_.join();
      {
        std::lock_guard lock(timing_mutex_);
        published_timing_ = worker_timing_; // final snapshot after join
      }
      jw_.close();  // seals (count + running hash) and closes
    }
    open_ = false;
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return jw_.count(); }
  [[nodiscard]] std::uint64_t stream_hash() const noexcept { return jw_.stream_hash(); }

 private:
  void run(int core) {
    pin_thread_to_core(core);
    Ring::Reader reader(*ring_, /*from_origin=*/true, Ring::LossPolicy::Intolerant);
    Envelope e;
    bool dirty = false;  // appended since the last flush
    Nanos batch_begin = 0, batch_end = 0;
    std::uint64_t messages = 0;
    for (;;) {
      Nanos enqueued = 0;
      const auto p = reader.poll(e, &enqueued);
      switch (p.kind) {
        case Ring::PollKind::Msg: {
          const auto begin = enqueued ? perf_now_ns() : 0;
          if (enqueued && begin >= enqueued) worker_timing_.queue.record(begin - enqueued);
          if (enqueued && !batch_begin) batch_begin = begin;
          jw_.append(e);
          if (enqueued) batch_end = perf_now_ns();
          dirty = true;
          if (++messages % 256 == 0) publish_timing();
          continue;
        }  // drain the batch before sleeping
        case Ring::PollKind::Fault:
          fault_.store(true, std::memory_order_release);
          return;  // the record is already broken; stop appending
        case Ring::PollKind::Gap:
        case Ring::PollKind::Empty:
          break;
      }
      if (dirty) {
        // Crash durability: hand the burst to the OS before sleeping — a
        // process crash then loses nothing that was ever appended (§11.2,
        // what --resume replays). Costs a syscall per burst, on this thread.
        const auto flush_begin = batch_begin ? perf_now_ns() : 0;
        jw_.flush();
        if (batch_begin) {
          worker_timing_.write_batch.record(batch_end - batch_begin);
          worker_timing_.flush.record(perf_now_ns() - flush_begin);
          batch_begin = 0;
        }
        publish_timing();
        dirty = false;
      }
      if (stop_.load(std::memory_order_acquire)) return;  // drained + stopping
      // Journal latency is not tick latency: sleep, don't burn a core.
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }

  void publish_timing() {
    // A report must never stall the journal worker.
    std::unique_lock lock(timing_mutex_, std::try_to_lock);
    if (lock.owns_lock()) published_timing_ = worker_timing_;
  }

  LatencyHisto enqueue_; // producer only
  TimingSnapshot worker_timing_; // worker only until joined
  TimingSnapshot published_timing_; // guarded by timing_mutex_
  std::mutex timing_mutex_;
  bool latency_enabled_ = true; // producer only

  // 64 Ki slots × 320 B ≈ 20 MiB ≈ many minutes of futures snapshots, or
  // ~0.6 s of a full 逐笔 stock feed — the disk must stall that long before
  // the fail-closed path triggers.
  static constexpr std::uint64_t kSlots = 1ull << 16;

  JournalWriter jw_;
  std::unique_ptr<Ring> ring_;
  std::optional<Ring::Writer> writer_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> fault_{false};
  bool open_ = false;
};

}  // namespace plat
