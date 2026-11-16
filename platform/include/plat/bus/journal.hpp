#pragma once
// plat/bus/journal.hpp — the total-order journal (§7.2): a flat file of
// 256-byte envelopes behind a 4 KiB header. It is simultaneously the legal
// audit record, the recovery substrate, and the replay/backtest input; one
// format serves all three so they can never disagree.
//
// Durability model: buffered writes, flushed to the OS after every drain
// burst (process crash loses nothing already appended), and pushed to stable
// storage on a cadence and at seal (`sync()` → fdatasync / _commit). A journal
// that hit a write error is NEVER sealed: it stays unsealed exactly like a
// crashed session, so recovery/replay treats it as incomplete instead of
// certifying a truncated audit record. The reader validates magic, count, and
// the running FNV-1a stream checksum written at seal time.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "plat/bus/envelope.hpp"
#include "plat/core/hash.hpp"

namespace plat {

inline constexpr std::uint64_t kJournalMagic = 0x0100'4C4E'524A'4C50ull;  // "PLJRNL" v1
inline constexpr std::uint16_t kJournalVersion = 1;

struct JournalHeader {
  std::uint64_t magic = 0;
  std::uint64_t count = 0;       // envelopes; written at seal
  std::uint64_t stream_hash = 0; // FNV over (seq, schema, payload); at seal
  std::uint32_t day = 0;         // yyyymmdd tag
  SiteId site = 0;
  std::uint8_t sealed = 0;
  std::uint16_t version = kJournalVersion;  // 0 = pre-versioning (legacy) record
  std::uint8_t pad_[4064]{};
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
    hdr_.version = kJournalVersion;
    hash_ = StateHash{};
    error_ = false;
    if (std::fwrite(&hdr_, sizeof(hdr_), 1, f_) != 1) {
      error_ = true;
      std::fclose(f_);
      f_ = nullptr;
      return false;
    }
    return true;
  }

  // Appends one envelope. False means the audit record is broken from here on
  // (write error / closed journal): the caller must stop trading, and this
  // writer will refuse to seal (an incomplete record must not look complete).
  bool append(const Envelope& e) {
    if (!f_ || error_) return false;
    if (std::fwrite(&e, sizeof(e), 1, f_) != 1) {
      error_ = true;
      return false;
    }
    hash_.mix(e.seq);
    hash_.mix(e.schema);
    hash_.mix_bytes(e.payload, e.size);
    ++hdr_.count;
    return true;
  }

  // Process-crash durability: push buffered envelopes to the OS so a process
  // crash loses nothing already appended (the async journal thread calls this
  // after each drain burst — never the engine).
  bool flush() {
    if (!f_ || error_) return false;
    if (std::fflush(f_) != 0) {
      error_ = true;
      return false;
    }
    return true;
  }

  // Machine-crash durability: push the OS cache to stable storage. Called on
  // the journal thread's cadence and at seal — never on the engine thread.
  bool sync() {
    if (!flush()) return false;
    if (!sync_fd()) {
      error_ = true;
      return false;
    }
    return true;
  }

  // Seal: finalize count + checksum in the header, sync it, and only then
  // report success. A faulted writer refuses: the file stays unsealed so
  // readers treat it as an incomplete (crashed) session.
  bool seal() {
    if (!f_ || error_) return false;
    hdr_.stream_hash = hash_.value();
    hdr_.sealed = 1;
    const JournalHeader out = hdr_;
    if (std::fseek(f_, 0, SEEK_SET) != 0 ||
        std::fwrite(&out, sizeof(out), 1, f_) != 1) {
      hdr_.sealed = 0;
      error_ = true;
      return false;
    }
    if (!sync()) {
      hdr_.sealed = 0;
      return false;
    }
    std::fseek(f_, 0, SEEK_END);
    return true;
  }

  // Close WITHOUT sealing: the faulted-journal path. The file is left
  // unsealed exactly like a crashed session, never certified as complete.
  void abandon() {
    if (!f_) return;
    std::fflush(f_);
    std::fclose(f_);
    f_ = nullptr;
  }

  void close() {
    if (f_) {
      seal();
      std::fclose(f_);
      f_ = nullptr;
    }
  }

  [[nodiscard]] bool healthy() const noexcept { return !error_; }
  [[nodiscard]] std::uint64_t count() const noexcept { return hdr_.count; }
  [[nodiscard]] std::uint64_t stream_hash() const noexcept { return hash_.value(); }

 private:
  bool sync_fd() {
#ifdef _WIN32
    return ::_commit(::_fileno(f_)) == 0;
#else
    return ::fdatasync(::fileno(f_)) == 0;
#endif
  }

  std::FILE* f_ = nullptr;
  JournalHeader hdr_{};
  StateHash hash_;
  bool error_ = false;
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
    // 0 is a pre-versioning record (written by an older build); a future
    // version is refused rather than misread with today's message layouts.
    if (ok && hdr.version > kJournalVersion) ok = false;
    // Count from the file itself, not from a possibly corrupt header: a
    // sealed file's count must match its size, an unsealed one IS its size
    // (whole records only; a torn tail is dropped).
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    const std::uint64_t max_records =
        size > static_cast<long>(sizeof(JournalHeader))
            ? (static_cast<std::uint64_t>(size) - sizeof(JournalHeader)) / sizeof(Envelope)
            : 0;
    std::uint64_t count = hdr.count;
    if (ok && hdr.sealed) {
      if (count > max_records) ok = false;  // corrupt/truncated sealed header
    } else if (ok) {
      count = max_records;
    }
    if (ok) {
      std::fseek(f, sizeof(JournalHeader), SEEK_SET);
      envelopes_.resize(count);
      ok = count == 0 ||
           std::fread(envelopes_.data(), sizeof(Envelope), count, f) == count;
      for (const Envelope& e : envelopes_) {
        if (!ok) break;
        if (e.size == 0 || e.size > kMaxPayload) ok = false;  // torn/garbage record
      }
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
