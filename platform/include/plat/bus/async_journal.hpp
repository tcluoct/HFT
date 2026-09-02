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
#include <optional>
#include <string>
#include <thread>

#include "plat/bus/envelope.hpp"
#include "plat/bus/journal.hpp"
#include "plat/bus/ring.hpp"
#include "plat/core/thread.hpp"

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
  HFT_FORCE_INLINE void append(const Envelope& e) noexcept { writer_->publish(e); }

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
    for (;;) {
      const auto p = reader.poll(e);
      switch (p.kind) {
        case Ring::PollKind::Msg:
          jw_.append(e);
          dirty = true;
          continue;  // drain the batch before sleeping
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
        jw_.flush();
        dirty = false;
      }
      if (stop_.load(std::memory_order_acquire)) return;  // drained + stopping
      // Journal latency is not tick latency: sleep, don't burn a core.
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }

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
