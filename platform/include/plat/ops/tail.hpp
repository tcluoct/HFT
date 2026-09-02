#pragma once
// plat/ops/tail.hpp — incremental journal tailing for the ops monitor
// (platops): read complete new envelopes from a LIVE journal as platd's
// async writer flushes them, a few milliseconds behind the engine, with
// ZERO contact with the trading process.
//
// The file is opened and closed on every poll — the monitor never holds a
// handle that would block platd's startup archive rename — so the only
// state here is the consumed-envelope offset. A fresh session journaling to
// the same path (rotation) shows up as a day change or a size regression:
// the tailer resets to the new file's beginning and bumps generation(); the
// caller rebuilds its mirror site from scratch.
//
// Cadence is operator-scale (a few polls per second): two fopen()s per poll
// is nothing, and a torn trailing record (the writer mid-append) is simply
// left for the next poll — only whole 256-byte envelopes are consumed.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "plat/bus/envelope.hpp"
#include "plat/bus/journal.hpp"

namespace plat::ops {

class JournalTailer {
 public:
  explicit JournalTailer(std::string path) : path_(std::move(path)) {}

  // Header probe (4 KiB read, no consumption): refreshes header/rotation
  // state. False until the path holds a valid journal.
  bool probe() {
    std::FILE* f = std::fopen(path_.c_str(), "rb");
    if (!f) return seen_;
    JournalHeader h{};
    const bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && h.magic == kJournalMagic;
    const std::uint64_t avail = ok ? avail_envelopes(f) : 0;
    std::fclose(f);
    if (!ok) return seen_;
    if (seen_ && (h.day != hdr_.day || h.site != hdr_.site || avail < read_)) {
      ++gen_;      // a different session took this path over
      read_ = 0;   // consume the new file from its beginning
    }
    hdr_ = h;
    seen_ = true;
    return true;
  }

  // Appends up to `max_batch` complete new envelopes to `out`.
  std::size_t read(std::vector<Envelope>& out, std::size_t max_batch = 16'384) {
    if (!seen_) return 0;
    std::FILE* f = std::fopen(path_.c_str(), "rb");
    if (!f) return 0;
    const std::uint64_t avail = avail_envelopes(f);
    std::size_t n = 0;
    if (avail > read_) {
      n = static_cast<std::size_t>(avail - read_);
      if (n > max_batch) n = max_batch;
      seek_to(f, static_cast<std::int64_t>(sizeof(JournalHeader) + read_ * sizeof(Envelope)));
      const std::size_t old = out.size();
      out.resize(old + n);
      n = std::fread(out.data() + old, sizeof(Envelope), n, f);
      out.resize(old + n);
      read_ += n;
    }
    std::fclose(f);
    return n;
  }

  [[nodiscard]] bool exists() const noexcept { return seen_; }
  [[nodiscard]] const JournalHeader& header() const noexcept { return hdr_; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return gen_; }
  [[nodiscard]] std::uint64_t consumed() const noexcept { return read_; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  // 64-bit size math: a heavy day can cross 2 GiB.
  static void seek_to(std::FILE* f, std::int64_t off) {
#ifdef _WIN32
    _fseeki64(f, off, SEEK_SET);
#else
    fseeko(f, off, SEEK_SET);
#endif
  }
  static std::uint64_t avail_envelopes(std::FILE* f) {
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    const std::int64_t size = _ftelli64(f);
#else
    fseeko(f, 0, SEEK_END);
    const std::int64_t size = ftello(f);
#endif
    return size > static_cast<std::int64_t>(sizeof(JournalHeader))
               ? (static_cast<std::uint64_t>(size) - sizeof(JournalHeader)) / sizeof(Envelope)
               : 0;  // whole records only — a torn tail waits for the next poll
  }

  std::string path_;
  JournalHeader hdr_{};
  std::uint64_t read_ = 0;
  std::uint64_t gen_ = 0;
  bool seen_ = false;
};

}  // namespace plat::ops
