#pragma once
// plat/bus/journal.hpp — the total-order journal (§7.2): a flat file of
// 256-byte envelopes behind a 4 KiB header. It is simultaneously the legal
// audit record, the recovery substrate, and the replay/backtest input; one
// format serves all three so they can never disagree.
//
// Durability model for this implementation: buffered writes + explicit
// flush at seal; production would double-buffer through io_uring/O_DIRECT on
// a dedicated core. The reader validates magic, count, and the running
// FNV-1a stream checksum written at seal time.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "plat/bus/envelope.hpp"
#include "plat/core/hash.hpp"

namespace plat {

inline constexpr std::uint64_t kJournalMagic = 0x0100'4C4E'524A'4C50ull;  // "PLJRNL" v1

struct JournalHeader {
  std::uint64_t magic = 0;
  std::uint64_t count = 0;       // envelopes; written at seal
  std::uint64_t stream_hash = 0; // FNV over (seq, schema, payload); at seal
  std::uint32_t day = 0;         // yyyymmdd tag
  SiteId site = 0;
  std::uint8_t sealed = 0;
  std::uint8_t pad_[4066]{};
};
static_assert(sizeof(JournalHeader) == 4096);

class JournalWriter {
 public:
  JournalWriter() = default;
  JournalWriter(const JournalWriter&) = delete;
  JournalWriter& operator=(const JournalWriter&) = delete;
  ~JournalWriter() { close(); }

  bool open(const std::string& path, std::uint32_t day, SiteId site) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);  // 1 MiB: amortize syscalls
    hdr_ = JournalHeader{};
    hdr_.magic = kJournalMagic;
    hdr_.day = day;
    hdr_.site = site;
    return std::fwrite(&hdr_, sizeof(hdr_), 1, f_) == 1;
  }

  void append(const Envelope& e) {
    if (!f_) return;
    hash_.mix(e.seq);
    hash_.mix(e.schema);
    hash_.mix_bytes(e.payload, e.size);
    ++hdr_.count;
    std::fwrite(&e, sizeof(e), 1, f_);
  }

  // Crash durability: push buffered envelopes to the OS so a process crash
  // loses nothing already appended (the async journal thread calls this
  // after each drain burst — never the engine).
  void flush() {
    if (f_) std::fflush(f_);
  }

  // Seal: finalize count + checksum in the header. Idempotent.
  bool seal() {
    if (!f_) return false;
    hdr_.stream_hash = hash_.value();
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

  [[nodiscard]] std::uint64_t count() const noexcept { return hdr_.count; }
  [[nodiscard]] std::uint64_t stream_hash() const noexcept { return hash_.value(); }

 private:
  std::FILE* f_ = nullptr;
  JournalHeader hdr_{};
  StateHash hash_;
};

class JournalReader {
 public:
  // Header-only peek (4 KiB read): enough to decide what the file IS —
  // which trading day, sealed (clean EOD) or unsealed (crash) — without
  // loading a day's worth of envelopes. The startup auto-resume decision
  // runs on this.
  static bool peek(const std::string& path, JournalHeader& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const bool ok = std::fread(&out, sizeof(out), 1, f) == 1;
    std::fclose(f);
    return ok;
  }

  // Loads the whole journal; a session day at platform scale fits comfortably
  // (production would mmap and stream).
  //
  // A journal from a CRASHED session is unsealed: its header still says
  // count 0 because count/checksum are only finalized at seal. The record
  // itself is intact up to the crash (the writer thread flushes after every
  // drain burst), so recovery derives the envelope count from the file size
  // and drops any torn trailing record. `sealed()` distinguishes the cases;
  // an unsealed journal is `verified() == false` by construction (there is
  // no checksum to verify against) but is exactly what --resume replays.
  bool open(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    JournalHeader hdr{};
    bool ok = std::fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == kJournalMagic;
    std::uint64_t count = hdr.count;
    if (ok && !hdr.sealed) {
      std::fseek(f, 0, SEEK_END);
      const long size = std::ftell(f);
      std::fseek(f, sizeof(JournalHeader), SEEK_SET);
      count = size > static_cast<long>(sizeof(JournalHeader))
                  ? (static_cast<std::uint64_t>(size) - sizeof(JournalHeader)) / sizeof(Envelope)
                  : 0;  // whole records only: a torn tail is dropped
    }
    if (ok) {
      envelopes_.resize(count);
      ok = count == 0 ||
           std::fread(envelopes_.data(), sizeof(Envelope), count, f) == count;
    }
    std::fclose(f);
    if (!ok) {
      envelopes_.clear();
      return false;
    }
    hdr_ = hdr;
    hdr_.count = count;
    // Verify the sealed checksum — a journal that fails this is evidence of
    // corruption and must not drive recovery.
    StateHash h;
    for (const auto& e : envelopes_) {
      h.mix(e.seq);
      h.mix(e.schema);
      h.mix_bytes(e.payload, e.size);
    }
    verified_ = hdr_.sealed && h.value() == hdr_.stream_hash;
    return true;
  }

  [[nodiscard]] bool verified() const noexcept { return verified_; }
  [[nodiscard]] bool sealed() const noexcept { return hdr_.sealed != 0; }
  [[nodiscard]] const JournalHeader& header() const noexcept { return hdr_; }
  [[nodiscard]] const std::vector<Envelope>& envelopes() const noexcept { return envelopes_; }

 private:
  JournalHeader hdr_{};
  std::vector<Envelope> envelopes_;
  bool verified_ = false;
};

}  // namespace plat
