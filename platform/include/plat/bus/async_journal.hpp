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
#include "plat/core/time.hpp"
#include "plat/tel/telemetry.hpp"

namespace plat {

class AsyncJournal {
 public:
  AsyncJournal() = default;
  AsyncJournal(const AsyncJournal&) = delete;
  AsyncJournal& operator=(const AsyncJournal&) = delete;
  ~AsyncJournal() { finish(); }

  // Opens the journal file and starts the writer thread (pinned to
  // `writer_core` when ≥ 0). `sync_interval` is the stable-storage cadence:
  // every drain burst reaches the OS (process-crash durable); `sync()` pushes
  // it to disk at most that often and at seal (machine-crash durable).
  bool open(const std::string& path, std::uint32_t day, SiteId site, int writer_core = -1,
            Nanos sync_interval = 100 * kNsPerMs) {
    if (!jw_.open(path, day, site)) return false;
    sync_interval_ = sync_interval;
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

  // False once the writer thread was lapped or hit a write error (audit hole)
  // — fail closed.
  [[nodiscard]] bool healthy() const noexcept {
    return !fault_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool is_open() const noexcept { return open_; }
  // Test-only: force the fault path so a suite can prove a faulted journal is
  // left UNSEALED (an incomplete record must never look complete).
  void simulate_fault_for_test() noexcept { fault_.store(true, std::memory_order_release); }

  // Drain everything still in flight, then seal. A journal that faulted is
  // abandoned unsealed: recovery/replay must see it as incomplete, never as a
  // certified day. Idempotent.
  void finish() {
    if (thread_.joinable()) {
      stop_.store(true, std::memory_order_release);
      thread_.join();
      {
        std::lock_guard lock(timing_mutex_);
        published_timing_ = worker_timing_; // final snapshot after join
      }
      if (fault_.load(std::memory_order_acquire)) {
        jw_.abandon();  // leave the header unsealed; keep the bytes for post-mortem
      } else {
        if (!jw_.seal()) fault_.store(true, std::memory_order_release);
        jw_.abandon(); // seal was checked above; close without another attempt
      }
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
    bool unsynced = false;  // appended since the last fsync
    Nanos batch_begin = 0, batch_end = 0;
    std::uint64_t messages = 0;
    Nanos last_sync = perf_now_ns();
    for (;;) {
      // Read the shutdown request before polling. Once it is visible, all
      // final producer publishes precede the poll that can end the drain.
      const bool stopping = stop_.load(std::memory_order_acquire);
      Nanos enqueued = 0;
      const auto p = reader.poll(e, &enqueued);
      switch (p.kind) {
        case Ring::PollKind::Msg: {
          const auto begin = enqueued ? perf_now_ns() : 0;
          if (enqueued && begin >= enqueued) worker_timing_.queue.record(begin - enqueued);
          if (enqueued && !batch_begin) batch_begin = begin;
          if (!jw_.append(e)) {  // write error: the audit record is broken
            fault_.store(true, std::memory_order_release);
            return;
          }
          if (enqueued) batch_end = perf_now_ns();
          dirty = true;
          unsynced = true;
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
        if (!jw_.flush()) {
          fault_.store(true, std::memory_order_release);
          return;
        }
        if (batch_begin) {
          worker_timing_.write_batch.record(batch_end - batch_begin);
          worker_timing_.flush.record(perf_now_ns() - flush_begin);
          batch_begin = 0;
        }
        publish_timing();
        dirty = false;
      }
      // Machine-crash durability on a cadence (and at seal): the engine's
      // per-envelope cost stays one ring publish; this thread pays the fsync.
      // Only when something was appended — an idle journal does not fsync.
      const Nanos now = perf_now_ns();
      if (sync_interval_ > 0 && unsynced && now - last_sync >= sync_interval_) {
        if (!jw_.sync()) {
          fault_.store(true, std::memory_order_release);
          return;
        }
        last_sync = now;
        unsynced = false;
      }
      if (stopping) return;  // drained after observing the final publish
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
  Nanos sync_interval_ = 100 * kNsPerMs;  // engine-visible; set before start
  bool open_ = false;
};

}  // namespace plat
