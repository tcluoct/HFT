#pragma once
// plat/md/fut_book.hpp — the futures/options book is a *snapshot-delta*
// engine (§8.2): Chinese futures venues disclose 250–500 ms snapshots, not
// order flow, so the book holds the latest snapshot plus features derived
// from consecutive snapshot deltas (imbalance shift, implied traded volume).
// Staleness is first-class: the riskgate stale-data interlock reads it.

#include <cstdint>
#include <cstring>
#include <vector>

#include "plat/codec/msgs.hpp"
#include "plat/core/hash.hpp"
#include "plat/core/types.hpp"

namespace plat {

class FutBook {
 public:
  struct Features {
    std::int64_t imbalance_e4 = 0;   // (bidQ-askQ)/(bidQ+askQ) at L1, 1e-4 fixed point
    std::int64_t d_imbalance_e4 = 0; // change vs previous snapshot
    Price d_mid = 0;                 // mid move vs previous snapshot
    Qty traded = 0;                  // volume between snapshots
  };

  // Double-buffer flip: the new snapshot lands in the spare buffer and an
  // index swap promotes it — one 208-byte copy per tick instead of the
  // naive two (prev_ = snap_; snap_ = s).
  bool on_snapshot(const MdSnapshotEvt& s) {
    if (have_ && s.feed_seq <= buf_[cur_].feed_seq) return false;  // stale/duplicate
    const int nxt = cur_ ^ 1;
    buf_[nxt] = s;
    had_prev_ = have_;
    cur_ = nxt;
    have_ = true;
    return true;
  }

  [[nodiscard]] bool valid() const noexcept { return have_; }
  [[nodiscard]] const MdSnapshotEvt& snap() const noexcept { return buf_[cur_]; }
  [[nodiscard]] Price bid() const noexcept { return snap().bid_px[0]; }
  [[nodiscard]] Price ask() const noexcept { return snap().ask_px[0]; }
  [[nodiscard]] Qty bid_qty() const noexcept { return snap().bid_qty[0]; }
  [[nodiscard]] Qty ask_qty() const noexcept { return snap().ask_qty[0]; }
  [[nodiscard]] Price mid() const noexcept { return (bid() + ask()) / 2; }
  [[nodiscard]] Nanos last_event_ts() const noexcept { return snap().ts; }

  [[nodiscard]] Features features() const noexcept {
    const MdSnapshotEvt& cur = buf_[cur_];
    const MdSnapshotEvt& prev = buf_[cur_ ^ 1];
    Features f;
    f.imbalance_e4 = imb(cur);
    if (had_prev_) {
      f.d_imbalance_e4 = f.imbalance_e4 - imb(prev);
      f.d_mid = (cur.bid_px[0] + cur.ask_px[0]) / 2 - (prev.bid_px[0] + prev.ask_px[0]) / 2;
      f.traded = cur.day_volume - prev.day_volume;
    }
    return f;
  }

  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    h.mix(snap());
    return h.value();
  }

  void save(std::vector<std::uint8_t>& out) const {
    const auto* b = reinterpret_cast<const std::uint8_t*>(this);
    out.insert(out.end(), b, b + sizeof(*this));
  }
  std::size_t load(const std::uint8_t* p, std::size_t avail) {
    if (sizeof(*this) <= avail) std::memcpy(this, p, sizeof(*this));
    return sizeof(*this);
  }

 private:
  static std::int64_t imb(const MdSnapshotEvt& s) noexcept {
    const Qty b = s.bid_qty[0], a = s.ask_qty[0];
    return (b + a) ? (b - a) * 10'000 / (b + a) : 0;
  }

  MdSnapshotEvt buf_[2]{};
  int cur_ = 0;
  bool have_ = false;
  bool had_prev_ = false;
};

}  // namespace plat
