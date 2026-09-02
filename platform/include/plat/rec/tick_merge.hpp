#pragma once
// plat/rec/tick_merge.hpp — the after-close cross-datacenter verification
// and merge. Two recorders watched the same market from different places;
// this proves they agree and produces the golden file backtests replay.
//
// Sequence discipline: within an instrument the venue's own order is
// (exch_ts, cumulative day_volume, open_interest) — cumulative volume is
// monotone by construction, so sorting on this key REPAIRS transport
// reordering (UDP arrival order is not data order) and any record that
// still violates monotonicity is provably damaged, not just late.
//
// Alignment: per instrument, two-pointer walk over the canonically sorted
// sequences; equal keys compare full venue content (recv_ts excluded —
// each datacenter has its own clock). One side missing = the other side's
// gap filled by the merge; same key with different content = a conflict
// (kept from A, counted loudly).

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "plat/core/log.hpp"
#include "plat/rec/tick_file.hpp"

namespace plat::rec {

struct CompareStats {
  std::uint64_t matched = 0;
  std::uint64_t a_only = 0;       // gaps in B that A covers
  std::uint64_t b_only = 0;       // gaps in A that B covers
  std::uint64_t conflicts = 0;    // same venue key, different content
  std::uint64_t a_resorted = 0;   // records arriving out of venue order at A
  std::uint64_t b_resorted = 0;
  std::uint64_t duplicates = 0;   // identical consecutive records dropped
  std::uint64_t golden = 0;       // records written to the merged output
};

namespace detail {

struct InstSeq {
  MdrInstrument meta{};
  std::vector<MdRecord> recs;
};

// Reads a whole file into per-(symbol,exchange) sequences, canonically
// sorted; counts transport reordering and drops exact duplicates.
inline bool load_by_instrument(const std::string& path,
                               std::map<std::string, InstSeq>& out,
                               std::uint64_t& resorted, std::uint64_t& duplicates) {
  TickReader r;
  if (!r.open(path)) return false;
  std::vector<std::string> keys;
  keys.reserve(r.dir().size());
  for (const auto& d : r.dir()) {
    std::string k = std::string(d.exchange) + ":" + d.symbol;
    out[k].meta = d;
    keys.push_back(std::move(k));
  }
  MdRecord rec;
  while (r.next(rec)) {
    if (rec.inst < keys.size()) out[keys[rec.inst]].recs.push_back(rec);
  }
  for (auto& [k, seq] : out) {
    auto& v = seq.recs;
    if (!std::is_sorted(v.begin(), v.end(), canon_less)) {
      for (std::size_t i = 1; i < v.size(); ++i)
        if (canon_less(v[i], v[i - 1])) ++resorted;
      std::stable_sort(v.begin(), v.end(), canon_less);
    }
    const auto last = std::unique(v.begin(), v.end(), [](const auto& a, const auto& b) {
      return same_market_content(a, b);
    });
    duplicates += static_cast<std::uint64_t>(v.end() - last);
    v.erase(last, v.end());
  }
  return true;
}

}  // namespace detail

// Compares two recordings; when `golden_path` is non-empty, writes the
// merged (gap-filled, canonically ordered) golden file. Returns false only
// when a file cannot be read.
inline bool compare_recordings(const std::string& a_path, const std::string& b_path,
                               const std::string& golden_path, CompareStats& s) {
  std::map<std::string, detail::InstSeq> a, b;
  if (!detail::load_by_instrument(a_path, a, s.a_resorted, s.duplicates)) return false;
  if (!detail::load_by_instrument(b_path, b, s.b_resorted, s.duplicates)) return false;

  // Union directory in deterministic (map) order.
  std::map<std::string, MdrInstrument> union_dir;
  for (const auto& [k, v] : a) union_dir[k] = v.meta;
  for (const auto& [k, v] : b) union_dir.emplace(k, v.meta);

  std::vector<MdrInstrument> dir;
  std::map<std::string, std::uint32_t> idx;
  for (const auto& [k, m] : union_dir) {
    idx[k] = static_cast<std::uint32_t>(dir.size());
    dir.push_back(m);
  }

  TickWriter golden;
  const bool write_golden = !golden_path.empty();
  if (write_golden) {
    TickReader ra;
    ra.open(a_path);  // day + depth come from A (both inputs share them)
    if (!golden.open(golden_path, ra.header().day, "golden", ra.depth(), dir)) return false;
  }

  for (const auto& [k, gi] : idx) {
    const auto ai = a.find(k);
    const auto bi = b.find(k);
    const std::vector<MdRecord> empty;
    const auto& va = ai != a.end() ? ai->second.recs : empty;
    const auto& vb = bi != b.end() ? bi->second.recs : empty;
    std::size_t i = 0, j = 0;
    while (i < va.size() || j < vb.size()) {
      const MdRecord* take = nullptr;
      if (i < va.size() && j < vb.size()) {
        if (canon_less(va[i], vb[j])) {
          take = &va[i++];
          ++s.a_only;
        } else if (canon_less(vb[j], va[i])) {
          take = &vb[j++];
          ++s.b_only;
        } else {
          if (same_market_content(va[i], vb[j])) ++s.matched;
          else ++s.conflicts;  // keep A's view; the count is the alarm
          take = &va[i];
          ++i;
          ++j;
        }
      } else if (i < va.size()) {
        take = &va[i++];
        ++s.a_only;
      } else {
        take = &vb[j++];
        ++s.b_only;
      }
      if (write_golden) {
        MdRecord g = *take;
        g.inst = gi;
        golden.append(g);
        ++s.golden;
      }
    }
  }
  if (write_golden) golden.close();  // seals
  return true;
}

}  // namespace plat::rec
