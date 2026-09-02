#pragma once
// plat/rec/tick_file.hpp — the .mdr recording file: a 4 KiB header, a fixed
// instrument directory, then the delta-encoded record stream, sealed with a
// count and running checksum at close (the journal's discipline, applied to
// market data):
//
//   [MdrHeader 4096] [MdrInstrument × N] [encoded records ...]
//
// A crashed recorder leaves the file unsealed but intact to the last flush;
// the reader streams to the torn tail and stops — everything before it is
// usable. Writer buffering is 1 MiB with an explicit flush per drain burst,
// so the OS holds every appended record even if the process dies.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "plat/core/hash.hpp"
#include "plat/rec/tick_codec.hpp"

namespace plat::rec {

inline constexpr std::uint64_t kMdrMagic = 0x3252444D'4C50ull;  // "PLMDR2"

struct MdrHeader {
  std::uint64_t magic = 0;
  std::uint32_t day = 0;             // yyyymmdd trading day
  std::uint32_t instrument_count = 0;
  char source[32]{};                 // which recorder/datacenter wrote this
  std::uint64_t record_count = 0;    // finalized at seal
  std::uint64_t payload_hash = 0;    // FNV over the encoded record bytes
  std::uint8_t sealed = 0;
  std::uint8_t depth = 1;            // book levels per side: 1 = L1, 5 = L2
  std::uint8_t pad_[4030]{};
};
static_assert(sizeof(MdrHeader) == 4096);

// The instrument directory carries the reference data a backtest needs but
// the tick stream doesn't hold: tick size, contract multiplier, and initial
// margin. platrec captures these from the counter's instrument record at
// enumeration, so an .mdr is self-sufficient for replay/backtest — no live
// counter needed later.
struct MdrInstrument {
  char symbol[32]{};
  char exchange[8]{};
  std::int64_t price_tick = 0;   // fixed-point 1e-4 CNY (Price units)
  std::int32_t multiplier = 1;   // contract multiplier
  std::uint32_t margin_bps = 0;  // initial margin, 1e-4 of notional
  std::uint8_t pad_[8]{};
};
static_assert(sizeof(MdrInstrument) == 64);

class TickWriter {
 public:
  TickWriter() = default;
  TickWriter(const TickWriter&) = delete;
  TickWriter& operator=(const TickWriter&) = delete;
  ~TickWriter() { close(); }

  // `depth` = book levels per side to record (1 = L1, 5 = L2); stored in the
  // header so the reader knows the record shape.
  bool open(const std::string& path, std::uint32_t day, const char* source, int depth,
            const std::vector<MdrInstrument>& dir) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);
    hdr_ = MdrHeader{};
    hdr_.magic = kMdrMagic;
    hdr_.day = day;
    hdr_.instrument_count = static_cast<std::uint32_t>(dir.size());
    hdr_.depth = static_cast<std::uint8_t>(depth < 1 ? 1 : depth > kMaxLevels ? kMaxLevels
                                                                              : depth);
    std::snprintf(hdr_.source, sizeof(hdr_.source), "%s", source);
    if (std::fwrite(&hdr_, sizeof(hdr_), 1, f_) != 1) return false;
    if (!dir.empty() &&
        std::fwrite(dir.data(), sizeof(MdrInstrument), dir.size(), f_) != dir.size())
      return false;
    prev_.assign(dir.size(), MdRecord{});
    return true;
  }

  // Writer thread only: delta-encode against this instrument's previous
  // record, append, mix the checksum.
  void append(const MdRecord& r) {
    if (!f_ || r.inst >= prev_.size()) return;
    std::uint8_t buf[kMaxEncoded];
    const std::size_t n = encode_md(prev_[r.inst], r, hdr_.depth, buf);
    if (std::fwrite(buf, 1, n, f_) != n) io_error_.store(true, std::memory_order_relaxed);
    hash_.mix_bytes(buf, n);
    prev_[r.inst] = r;
    ++hdr_.record_count;
    bytes_ += n;
  }

  void flush() {
    if (f_ && std::fflush(f_) != 0) io_error_.store(true, std::memory_order_relaxed);
  }

  // Latched on any failed write/flush (disk full, volume gone). The status
  // loop polls this from another thread — a recorder that stopped persisting
  // must SAY so, not sit silently at 100% apparent health.
  [[nodiscard]] bool io_error() const noexcept {
    return io_error_.load(std::memory_order_relaxed);
  }

  bool seal() {
    if (!f_) return false;
    hdr_.payload_hash = hash_.value();
    hdr_.sealed = 1;
    if (std::fseek(f_, 0, SEEK_SET) != 0) return false;
    const bool ok = std::fwrite(&hdr_, sizeof(hdr_), 1, f_) == 1;
    std::fflush(f_);
    std::fseek(f_, 0, SEEK_END);
    return ok;
  }

  void close() {
    if (f_) {
      seal();
      std::fclose(f_);
      f_ = nullptr;
    }
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return hdr_.record_count; }
  [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

 private:
  std::FILE* f_ = nullptr;
  MdrHeader hdr_{};
  StateHash hash_;
  std::vector<MdRecord> prev_;
  std::uint64_t bytes_ = 0;
  std::atomic<bool> io_error_{false};
};

class TickReader {
 public:
  TickReader() = default;
  TickReader(const TickReader&) = delete;
  TickReader& operator=(const TickReader&) = delete;
  ~TickReader() { close(); }

  bool open(const std::string& path) {
    close();
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) return false;
    if (std::fread(&hdr_, sizeof(hdr_), 1, f_) != 1 || hdr_.magic != kMdrMagic) {
      close();
      return false;
    }
    dir_.resize(hdr_.instrument_count);
    if (!dir_.empty() &&
        std::fread(dir_.data(), sizeof(MdrInstrument), dir_.size(), f_) != dir_.size()) {
      close();
      return false;
    }
    depth_ = hdr_.depth < 1 ? 1 : hdr_.depth > kMaxLevels ? kMaxLevels : hdr_.depth;
    prev_.assign(dir_.size(), MdRecord{});
    buf_.resize(1 << 20);
    fill_ = pos_ = 0;
    decoded_ = 0;
    eof_ = false;
    return true;
  }

  // Streams the record sequence; false at end of data (sealed end or the
  // torn tail of a crashed recording).
  bool next(MdRecord& out) {
    for (;;) {
      if (fill_ - pos_ >= kMaxEncoded || eof_) {
        if (pos_ >= fill_ && eof_) return false;
        std::uint32_t inst = 0;
        if (!peek_inst(buf_.data() + pos_, fill_ - pos_, inst) || inst >= prev_.size())
          return false;  // torn tail or corrupt stream: stop here
        const std::size_t n =
            decode_md(prev_[inst], buf_.data() + pos_, fill_ - pos_, depth_, out);
        if (n == 0) return false;  // truncated mid-record (crash)
        hash_.mix_bytes(buf_.data() + pos_, n);
        pos_ += n;
        prev_[inst] = out;
        ++decoded_;
        return true;
      }
      // Refill: keep the unconsumed remainder, read the next chunk.
      std::memmove(buf_.data(), buf_.data() + pos_, fill_ - pos_);
      fill_ -= pos_;
      pos_ = 0;
      const std::size_t got = std::fread(buf_.data() + fill_, 1, buf_.size() - fill_, f_);
      fill_ += got;
      if (got == 0) eof_ = true;
    }
  }

  // After streaming to the end: sealed files must reproduce the header's
  // count and checksum; unsealed (crashed) files report what was recovered.
  [[nodiscard]] bool verified() const noexcept {
    return hdr_.sealed && decoded_ == hdr_.record_count && hash_.value() == hdr_.payload_hash;
  }
  [[nodiscard]] bool sealed() const noexcept { return hdr_.sealed != 0; }
  [[nodiscard]] int depth() const noexcept { return depth_; }
  [[nodiscard]] std::uint64_t decoded() const noexcept { return decoded_; }
  [[nodiscard]] const MdrHeader& header() const noexcept { return hdr_; }
  [[nodiscard]] const std::vector<MdrInstrument>& dir() const noexcept { return dir_; }

  void close() {
    if (f_) {
      std::fclose(f_);
      f_ = nullptr;
    }
  }

 private:
  std::FILE* f_ = nullptr;
  MdrHeader hdr_{};
  std::vector<MdrInstrument> dir_;
  std::vector<MdRecord> prev_;
  std::vector<std::uint8_t> buf_;
  std::size_t fill_ = 0, pos_ = 0;
  std::uint64_t decoded_ = 0;
  int depth_ = 1;
  StateHash hash_;
  bool eof_ = false;
};

}  // namespace plat::rec
