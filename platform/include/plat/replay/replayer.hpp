#pragma once
// plat/replay/replayer.hpp — journal-driven rebuild (§11.1) and the recovery
// procedure (§11.2), as functions over Site's snapshot/inject API.
//
//   full_replay      — fresh deterministic components consume the whole
//                      Source prefix; every Derived envelope is regenerated.
//                      The returned stream hash must equal the live run's.
//   recover          — load a snapshot taken at journal position K, replay
//                      the Source suffix; end state must hash-equal the
//                      uninterrupted run. This *is* the warm-standby takeover
//                      path: the standby is a Site holding a fresher snapshot.

#include <cstdint>
#include <vector>

#include "plat/bus/journal.hpp"
#include "plat/engine/site.hpp"

namespace plat {

struct ReplayResult {
  std::uint64_t state_hash = 0;
  std::uint64_t stream_hash = 0;
  std::uint64_t injected = 0;
};

template <typename SiteT>
ReplayResult full_replay(SiteT& replay_site, const std::vector<Envelope>& journal) {
  ReplayResult r;
  for (const Envelope& e : journal) {
    if (e.origin == Origin::Source) ++r.injected;
    replay_site.inject(e);
  }
  r.state_hash = replay_site.state_hash();
  r.stream_hash = replay_site.stream().stream_hash();
  return r;
}

template <typename SiteT>
ReplayResult recover(SiteT& standby, const std::vector<std::uint8_t>& snapshot,
                            const std::vector<Envelope>& journal, std::size_t from_index) {
  standby.load_state(snapshot);
  ReplayResult r;
  for (std::size_t i = from_index; i < journal.size(); ++i) {
    if (journal[i].origin == Origin::Source) ++r.injected;
    standby.inject(journal[i]);
  }
  r.state_hash = standby.state_hash();
  return r;
}

}  // namespace plat
