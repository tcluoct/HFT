#pragma once
// Inbound-event journal + outbound-stream digest.
//
// Because the core is single-threaded and deterministic, the 32-byte inbound
// records ARE the full state of the world: replaying them through a fresh
// engine reproduces every fill byte-for-byte. Real exchanges build recovery,
// hot-standby failover, and dispute resolution on exactly this property.
// The digest (FNV-1a folding of every outbound event) is how the harness
// PROVES determinism: run, replay, compare hashes.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "hft/common/platform.hpp"
#include "me/proto/ouch.hpp"

namespace me {

class Journal {
 public:
  explicit Journal(std::size_t reserve_n) { log_.reserve(reserve_n); }

  void append(const InMsg& m) noexcept { log_.push_back(m); }

  [[nodiscard]] const std::vector<InMsg>& entries() const noexcept { return log_; }

  bool save(const char* path) const {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const std::size_t n = std::fwrite(log_.data(), sizeof(InMsg), log_.size(), f);
    std::fclose(f);
    return n == log_.size();
  }

  static std::vector<InMsg> load(const char* path) {
    std::vector<InMsg> v;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    const long bytes = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (bytes > 0 && bytes % static_cast<long>(sizeof(InMsg)) == 0) {
      v.resize(static_cast<std::size_t>(bytes) / sizeof(InMsg));
      if (std::fread(v.data(), sizeof(InMsg), v.size(), f) != v.size()) v.clear();
    }
    std::fclose(f);
    return v;
  }

 private:
  std::vector<InMsg> log_;
};

// Order-sensitive hash of the outbound event stream.
struct Digest {
  std::uint64_t h = 0xcbf29ce484222325ull;  // FNV offset basis

  HFT_FORCE_INLINE void mix(std::uint64_t v) noexcept {
    h = (h ^ v) * 0x100000001b3ull;  // FNV prime
  }
};

}  // namespace me
