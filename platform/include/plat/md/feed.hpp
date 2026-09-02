#pragma once
// plat/md/feed.hpp — A/B feed arbitration and gap quarantine (feedarb
// graduation, §8.1). Venues transmit every event on two feeds; the arbiter
// accepts the first copy by feed sequence, drops the duplicate, and on a gap
// flips the venue's books into quarantine — the riskgate stale-data interlock
// (§10.1 #11) then refuses new orders on affected instruments until recovery
// clears it. Freshness beats completeness for trading; completeness is the
// capture/journal layer's job.

#include <cstdint>
#include <functional>

#include "plat/codec/msgs.hpp"

namespace plat {

class FeedArbiter {
 public:
  using GapFn = std::function<void(std::uint64_t lost)>;

  void set_gap_sink(GapFn g) { on_gap_ = std::move(g); }

  // Returns true when the event is new (first copy, in order); false for
  // duplicates. Gaps fire the sink, then the late event is still delivered —
  // the book layer decides what remains trustworthy.
  bool accept(std::uint64_t feed_seq) {
    if (feed_seq <= last_seq_) {
      ++duplicates_;
      return false;
    }
    if (feed_seq != last_seq_ + 1 && last_seq_ != 0) {
      const std::uint64_t lost = feed_seq - last_seq_ - 1;
      lost_ += lost;
      ++gaps_;
      if (on_gap_) on_gap_(lost);
    }
    last_seq_ = feed_seq;
    return true;
  }

  [[nodiscard]] std::uint64_t duplicates() const noexcept { return duplicates_; }
  [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }
  [[nodiscard]] std::uint64_t lost() const noexcept { return lost_; }
  [[nodiscard]] std::uint64_t last_seq() const noexcept { return last_seq_; }

 private:
  std::uint64_t last_seq_ = 0;
  std::uint64_t duplicates_ = 0;
  std::uint64_t gaps_ = 0;
  std::uint64_t lost_ = 0;
  GapFn on_gap_;
};

}  // namespace plat
