#pragma once
// Deterministic dual-line impairment simulator. Takes the venue's perfect
// sequence stream and produces what actually hits the handler's two sockets:
// per-line loss, duplication, and delay jitter (jitter is what creates
// reordering). Same seed, same arrivals — every bench and test is replayable.
//
// Timeline unit is nanoseconds. Generation is windowed: because delay is
// bounded by (delay_min + jitter), an arrival can only interleave with
// near-by sends, so the harness can simulate tens of millions of packets
// with a small carry buffer instead of materializing the whole stream.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "fa/core/packet.hpp"

namespace fa {

struct Impairment {
  std::uint32_t drop_pm = 0;       // per-mille of packets lost
  std::uint32_t dup_pm = 0;        // per-mille of packets duplicated in flight
  std::uint64_t delay_min_ns = 0;  // propagation floor
  std::uint64_t jitter_ns = 0;     // uniform extra delay [0, jitter_ns]
};

struct Arrival {
  std::uint64_t when;  // arrival timestamp (ns, sim timeline)
  MdPacket pkt;
};

class LineSim {
 public:
  // send_gap_ns: venue inter-packet spacing (send_ts of seq s = s * gap).
  LineSim(std::uint64_t seed, Impairment line_a, Impairment line_b,
          std::uint64_t send_gap_ns)
      : seed_(seed), imp_{line_a, line_b}, gap_(send_gap_ns) {}

  [[nodiscard]] std::uint64_t max_delay_ns() const noexcept {
    return std::max(imp_[0].delay_min_ns + imp_[0].jitter_ns,
                    imp_[1].delay_min_ns + imp_[1].jitter_ns);
  }

  // Append every arrival for seqs [first, last) to `out`, then sort `out` by
  // arrival time. Deterministic: the RNG is re-seeded per (seq, line), so a
  // batch's contents don't depend on batch boundaries.
  void generate(std::uint64_t first, std::uint64_t last, std::vector<Arrival>& out) const {
    for (std::uint64_t s = first; s < last; ++s) {
      const std::uint64_t send_ts = s * gap_;
      for (std::uint32_t ln = 0; ln < 2; ++ln) {
        std::uint64_t r = mix64(seed_ ^ (s * 2 + ln));
        const Impairment& im = imp_[ln];
        if (r % 1000 < im.drop_pm) continue;
        r = mix64(r);
        const std::uint64_t delay = im.delay_min_ns + (im.jitter_ns ? r % (im.jitter_ns + 1) : 0);
        const MdPacket p{s, send_ts, mix64(s), ln, 0};
        out.push_back(Arrival{send_ts + delay, p});
        r = mix64(r);
        if (r % 1000 < im.dup_pm) {  // network-level duplicate, jittered again
          const std::uint64_t d2 = im.delay_min_ns + (im.jitter_ns ? mix64(r) % (im.jitter_ns + 1) : 0);
          out.push_back(Arrival{send_ts + d2, p});
        }
      }
    }
    std::sort(out.begin(), out.end(),
              [](const Arrival& a, const Arrival& b) { return a.when < b.when; });
  }

 private:
  std::uint64_t seed_;
  Impairment imp_[2];
  std::uint64_t gap_;
};

}  // namespace fa
