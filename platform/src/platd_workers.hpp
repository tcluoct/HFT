#pragma once

#include <memory>

#include "plat/bus/ring.hpp"
#include "platd_app.hpp"

namespace platd {

class StrategyProcesses {
 public:
  StrategyProcesses();
  ~StrategyProcesses();
  bool launch(LiveSite& site, const plat::Config& cfg, const std::vector<StratSpec>& specs,
              const std::string& config_path);
  plat::Ring& market();
  static bool poll(void*, plat::Nanos);
  static void publish(void*, const plat::Envelope&);
  static int check(const plat::Config&, const std::string& config_path);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace platd
