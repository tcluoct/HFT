#pragma once
// fleet_cfg.hpp — fleet-section config machinery (`shardN.*` overlays and
// derived per-shard defaults), shared by platd (supervisor + children) and
// platops (the web ops console mirrors every shard from the same file), so
// the two can never disagree about which journal/cmd file a shard uses.

#include <string>
#include <vector>

#include "plat/core/config.hpp"

namespace platcfg {

// One shard's effective config: shardN.* keys override the shared ones, and
// the artifacts that MUST differ per shard (journal, flow dirs, cmd file)
// get derived defaults unless explicitly set.
inline plat::Config shard_config(const plat::Config& fleet, int n) {
  const std::string tag = "shard" + std::to_string(n);
  plat::Config c = fleet.overlay(tag);
  c.set_default("site_id", std::to_string(n));
  c.set_default("site_name", tag);
  c.set_default("journal", tag + ".jnl");
  c.set_default("td_flow_path", "ctp_td_" + tag + "/");
  c.set_default("md_flow_path", "ctp_md_" + tag + "/");
  c.set_default("cmd_file", tag + ".cmd");
  return c;
}

inline std::vector<int> fleet_shard_ids(const plat::Config& fleet) {
  std::vector<int> ids;
  for (int n = 1; n <= 32; ++n)
    if (fleet.has("shard" + std::to_string(n) + ".instruments")) ids.push_back(n);
  return ids;
}

}  // namespace platcfg
