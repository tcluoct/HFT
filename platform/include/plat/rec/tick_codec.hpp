#pragma once
// plat/rec/tick_codec.hpp — the market-data recording codec (ticklab
// graduation): depth snapshots delta-encoded per instrument with zigzag
// varints. A book level that didn't move costs ONE byte, so a tick lands in
// the smallest self-contained representation that still replays exactly
// (integer fields, no floats, no loss).
//
// L1 or L2 in one format: a record holds up to kMaxLevels book levels, but
// the file records how many levels it actually carries (MdrHeader.depth, 1
// for L1 / 5 for L2) and the codec encodes only those. An L1 file never pays
// for the four empty deep levels an L2 layout would carry. The depth lives
// in the header, so a decoder reads it once and knows the record shape.
//
// Everything here is a pure function of its arguments — the whole codec is
// unit-tested without a market or a file.

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "plat/core/types.hpp"

namespace plat::rec {

inline constexpr int kMaxLevels = 5;   // storage capacity; a file uses depth ≤ this

// The decoded record — what replay/backtest consume. For L1 only level 0 of
// each side is populated; levels 1..4 stay zero and are not encoded. recv_ts
// is the recorder's local clock (per datacenter); exch_ts and the cumulative
// day_volume form the venue's own sequence, which the after-close compare
// aligns on.
struct MdRecord {
  std::uint32_t inst = 0;        // dense index into the file's directory
  Nanos recv_ts = 0;             // local receive time (ns since midnight)
  Nanos exch_ts = 0;             // exchange update time (ms precision)
  Price last_px = 0;
  Qty day_volume = 0;            // cumulative — monotone, the sequence proof
  std::int64_t turnover_e1 = 0;  // cumulative turnover, 0.1 CNY units
  Qty open_interest = 0;
  Price bid_px[kMaxLevels]{};
  Qty bid_qty[kMaxLevels]{};
  Price ask_px[kMaxLevels]{};
  Qty ask_qty[kMaxLevels]{};
};
static_assert(std::is_trivially_copyable_v<MdRecord>);

// Worst case: 1 index + 6 scalar + 5 levels × 4 = 27 fields × 10-byte
// varints — the reader's refill threshold (safe upper bound for any depth).
inline constexpr std::size_t kMaxEncoded = 288;

// --------------------------------------------------------------- varints

inline std::size_t put_varint(std::uint8_t* out, std::uint64_t v) noexcept {
  std::size_t n = 0;
  while (v >= 0x80) {
    out[n++] = static_cast<std::uint8_t>(v) | 0x80;
    v >>= 7;
  }
  out[n++] = static_cast<std::uint8_t>(v);
  return n;
}

// Returns bytes consumed; 0 = truncated input.
inline std::size_t get_varint(const std::uint8_t* in, std::size_t avail,
                              std::uint64_t& v) noexcept {
  v = 0;
  for (std::size_t n = 0; n < avail && n < 10; ++n) {
    v |= static_cast<std::uint64_t>(in[n] & 0x7F) << (7 * n);
    if (!(in[n] & 0x80)) return n + 1;
  }
  return 0;
}

inline constexpr std::uint64_t zigzag(std::int64_t v) noexcept {
  return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
}
inline constexpr std::int64_t unzigzag(std::uint64_t v) noexcept {
  return static_cast<std::int64_t>(v >> 1) ^ -static_cast<std::int64_t>(v & 1);
}

// ------------------------------------------------------------ record codec

// Encode `cur` as deltas against `prev` (the previous record of the SAME
// instrument; a zero-initialized prev encodes absolutes for the first tick).
// `levels` book levels per side are written (1 = L1, 5 = L2). `out` must hold
// kMaxEncoded bytes. Returns bytes written.
inline std::size_t encode_md(const MdRecord& prev, const MdRecord& cur, int levels,
                             std::uint8_t* out) noexcept {
  std::size_t n = put_varint(out, cur.inst);
  const auto d = [&](std::int64_t c, std::int64_t p) { n += put_varint(out + n, zigzag(c - p)); };
  d(cur.recv_ts, prev.recv_ts);
  d(cur.exch_ts, prev.exch_ts);
  d(cur.last_px, prev.last_px);
  d(cur.day_volume, prev.day_volume);
  d(cur.turnover_e1, prev.turnover_e1);
  d(cur.open_interest, prev.open_interest);
  for (int i = 0; i < levels; ++i) {
    d(cur.bid_px[i], prev.bid_px[i]);
    d(cur.bid_qty[i], prev.bid_qty[i]);
    d(cur.ask_px[i], prev.ask_px[i]);
    d(cur.ask_qty[i], prev.ask_qty[i]);
  }
  return n;
}

// Reads the leading instrument index without consuming the record — the
// reader uses it to pick the right per-instrument prev state.
inline std::size_t peek_inst(const std::uint8_t* in, std::size_t avail,
                             std::uint32_t& inst) noexcept {
  std::uint64_t v = 0;
  const std::size_t n = get_varint(in, avail, v);
  inst = static_cast<std::uint32_t>(v);
  return n;
}

// Decode one record against `prev` (same instrument), reading `levels` book
// levels. Returns bytes consumed; 0 = truncated input (refill and retry).
inline std::size_t decode_md(const MdRecord& prev, const std::uint8_t* in, std::size_t avail,
                             int levels, MdRecord& out) noexcept {
  std::uint64_t v = 0;
  std::size_t n = get_varint(in, avail, v);
  if (!n) return 0;
  out.inst = static_cast<std::uint32_t>(v);
  const auto d = [&](std::int64_t p, std::int64_t& c) -> bool {
    const std::size_t k = get_varint(in + n, avail - n, v);
    if (!k) return false;
    n += k;
    c = p + unzigzag(v);
    return true;
  };
  if (!d(prev.recv_ts, out.recv_ts)) return 0;
  if (!d(prev.exch_ts, out.exch_ts)) return 0;
  if (!d(prev.last_px, out.last_px)) return 0;
  if (!d(prev.day_volume, out.day_volume)) return 0;
  if (!d(prev.turnover_e1, out.turnover_e1)) return 0;
  if (!d(prev.open_interest, out.open_interest)) return 0;
  for (int i = 0; i < levels; ++i) {
    if (!d(prev.bid_px[i], out.bid_px[i])) return 0;
    if (!d(prev.bid_qty[i], out.bid_qty[i])) return 0;
    if (!d(prev.ask_px[i], out.ask_px[i])) return 0;
    if (!d(prev.ask_qty[i], out.ask_qty[i])) return 0;
  }
  return n;
}

// The venue's own ordering: exchange time, then cumulative volume, then
// open interest as the tiebreak. UDP reordering and cross-datacenter merge
// both sort on this — cumulative volume is monotone by construction, so a
// record violating it is provably out of sequence.
inline bool canon_less(const MdRecord& a, const MdRecord& b) noexcept {
  if (a.exch_ts != b.exch_ts) return a.exch_ts < b.exch_ts;
  if (a.day_volume != b.day_volume) return a.day_volume < b.day_volume;
  return a.open_interest < b.open_interest;
}

// Venue-content equality (recv_ts excluded: every datacenter has its own).
// Compares all storage levels; unused L1 levels are zero on both sides.
inline bool same_market_content(const MdRecord& a, const MdRecord& b) noexcept {
  return a.exch_ts == b.exch_ts && a.last_px == b.last_px && a.day_volume == b.day_volume &&
         a.turnover_e1 == b.turnover_e1 && a.open_interest == b.open_interest &&
         std::memcmp(a.bid_px, b.bid_px, sizeof(a.bid_px)) == 0 &&
         std::memcmp(a.bid_qty, b.bid_qty, sizeof(a.bid_qty)) == 0 &&
         std::memcmp(a.ask_px, b.ask_px, sizeof(a.ask_px)) == 0 &&
         std::memcmp(a.ask_qty, b.ask_qty, sizeof(a.ask_qty)) == 0;
}

}  // namespace plat::rec
