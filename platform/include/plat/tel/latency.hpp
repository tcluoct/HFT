#pragma once
// Explicit performance measurements. No destructor records timing, and no
// performance value enters the canonical stream or strategy decisions.
#include <array>
#include <format>
#include <vector>
#include "plat/core/types.hpp"
#include "plat/tel/telemetry.hpp"

namespace plat {
enum class LatencyStage : std::size_t {
  MdQueue, BookUpdate, OrderPrepare, CancelPrepare, RiskNew, RiskCancel,
  GatewayNew, GatewayCancel, ResponseQueue, ResponseUpdate, EngineEvent,
  TickToTrade, OrderRoundtrip, TickToAck,
  JournalEnqueue, JournalQueue, JournalWriteBatch, JournalFlush, Count
};
struct LatencyStageInfo { const char* name; const char* group; };
inline constexpr std::array<LatencyStageInfo, static_cast<std::size_t>(LatencyStage::Count)> kLatencyStages{{
  {"md.queue_ns", "Market data"}, {"book.update_ns", "Market data"},
  {"order.prepare_ns", "Order processing"}, {"cancel.prepare_ns", "Order processing"},
  {"risk.new_order_ns", "Risk"}, {"risk.cancel_ns", "Risk"},
  {"gateway.place_ns", "Gateway"}, {"gateway.cancel_ns", "Gateway"},
  {"response.queue_ns", "Responses"}, {"response.update_ns", "Responses"},
  {"engine.event_ns", "End-to-end (overlapping)"},
  {"tick_to_trade_ns", "End-to-end (overlapping)"},
  {"order_roundtrip_ns", "End-to-end (overlapping)"},
  {"tick_to_ack_ns", "End-to-end (overlapping)"},
  {"journal.enqueue_ns", "Journal (asynchronous branch)"},
  {"journal.queue_ns", "Journal (asynchronous branch)"},
  {"journal.write_batch_ns", "Journal (asynchronous branch)"},
  {"journal.flush_ns", "Journal (asynchronous branch)"}
}};

// One timer per active strategy callback. Strategies do not re-enter.
// pause/resume explicitly bracket synchronous context actions.
class StrategyCalcTimer {
 public:
  void begin(Nanos now) { active_ = true; paused_ = false; segment_ = now; calculation_ = 0; }
  bool pause(Nanos now) {
    if (!active_ || paused_) return false;
    calculation_ += now >= segment_ ? now - segment_ : 0;
    paused_ = true;
    return true;
  }
  void resume(Nanos now, bool was_paused) {
    if (was_paused) { segment_ = now; paused_ = false; }
  }
  std::uint64_t finish(Nanos now) {
    pause(now);
    active_ = false;
    return calculation_;
  }
 private:
  bool active_ = false, paused_ = false;
  Nanos segment_ = 0;
  std::uint64_t calculation_ = 0;
};

class LatencyMetrics {
 public:
  void wire(Telemetry& tel, bool enabled) {
    enabled_ = enabled;
    for (std::size_t i = 0; i < stages_.size(); ++i) stages_[i] = &tel.histo(kLatencyStages[i].name);
    collisions_ = &tel.counter("latency.correlation_overwrite");
  }
  void add_strategy(Telemetry& tel, StrategyId id) {
    if (strategies_.size() <= id) strategies_.resize(static_cast<std::size_t>(id) + 1);
    const auto prefix = "strategy." + std::to_string(id);
    strategies_[id] = {&tel.histo(prefix + ".callback_ns"), &tel.histo(prefix + ".calc_ns")};
  }
  void set_enabled(bool enabled) { enabled_ = enabled; }
  void set_session_clock(PerfClock clock) { clock_ = clock; session_clock_ = true; }
  Nanos now() const { return enabled_ ? clock_() : 0; }
  void record(LatencyStage stage, Nanos begin, Nanos end) {
    if (enabled_ && end >= begin) stages_[static_cast<std::size_t>(stage)]->record(end - begin);
  }
  void record_queue(LatencyStage stage, Nanos received, Nanos consumed) {
    if (session_clock_ && received) record(stage, received, consumed);
  }
  void record_strategy(StrategyId id, Nanos begin, Nanos end, std::uint64_t calculation) {
    if (!enabled_ || end < begin) return;
    strategies_[id].callback->record(end - begin);
    strategies_[id].calculation->record(calculation);
  }
  void order_sent(OrderId id, Nanos sent) {
    if (!enabled_ || !session_clock_) return;
    auto& slot = sent_[id % sent_.size()];
    if (slot.id) ++*collisions_;
    slot = {id, sent};
  }
  void order_submitted(Nanos trigger, Nanos end) {
    if (session_clock_ && trigger) record(LatencyStage::TickToTrade, trigger, end);
  }
  void acknowledgment(OrderId id, Nanos trigger, Nanos received) {
    auto& slot = sent_[id % sent_.size()];
    if (!enabled_ || !session_clock_ || !id || slot.id != id) return;
    record(LatencyStage::OrderRoundtrip, slot.ts, received);
    if (trigger) record(LatencyStage::TickToAck, trigger, received);
    slot = {};
  }
  void terminal(OrderId id) {
    auto& slot = sent_[id % sent_.size()];
    if (slot.id == id) slot = {};
  }
  void replace(LatencyStage stage, const LatencyHisto& snapshot) {
    *stages_[static_cast<std::size_t>(stage)] = snapshot;
  }
 private:
  struct StrategyHistograms { LatencyHisto* callback = nullptr; LatencyHisto* calculation = nullptr; };
  struct Sent { OrderId id = 0; Nanos ts = 0; };
  std::array<LatencyHisto*, static_cast<std::size_t>(LatencyStage::Count)> stages_{};
  std::vector<StrategyHistograms> strategies_;
  std::vector<Sent> sent_ = std::vector<Sent>(65536);
  std::uint64_t* collisions_ = nullptr;
  PerfClock clock_ = &perf_now_ns;
  bool enabled_ = false, session_clock_ = false;
};

inline std::string latency_report(const Telemetry& tel) {
  std::string out = "Latency map (ns; cumulative; stage percentiles must not be added):\n";
  const auto row = [&](const std::string& name, const LatencyHisto& h) {
    if (!h.count()) return std::format("  {}: n=0 (no samples)\n", name);
    return std::format("  {}: n={} p50={} p99={} p99.9={} max={}\n", name, h.count(),
                       h.percentile(.50), h.percentile(.99), h.percentile(.999), h.max());
  };
  std::string group;
  for (std::size_t i = 0; i < kLatencyStages.size(); ++i) {
    if (i == 2) {
      out += "Strategies (callback includes engine actions; calc pauses during actions):\n";
      for (const auto& [name, h] : tel.histos())
        if (name.starts_with("strategy.")) out += row(name, h);
    }
    const auto& info = kLatencyStages[i];
    if (group != info.group) { group = info.group; out += group + ":\n"; }
    const auto it = tel.histos().find(info.name);
    if (it != tel.histos().end()) out += row(it->first, it->second);
  }
  out += std::format("RTT correlation overwrites: {}\n", tel.get("latency.correlation_overwrite"));
  return out;
}
}  // namespace plat
