#pragma once
// plat/oms/oms.hpp — the order-state keeper (omsd graduation, §9.5). A pure
// consumer of the canonical stream: order requests and execution reports in,
// authoritative order table out. Never in the order hot path; its mirrors
// (resting orders per instrument for self-trade prevention, open-order
// exposure sums) are read by riskgate at memory cost.

#include <cstdint>
#include <map>
#include <vector>

#include "plat/codec/msgs.hpp"
#include "plat/core/hash.hpp"
#include "plat/core/types.hpp"

namespace plat {

// The counter's view of one of our open orders — the reconciliation input
// (§11.2/§11.3). Produced by the production gateway's order query (CTP
// ReqQryOrder); the venue is the source of truth.
struct VenueOpenOrder {
  OrderId cl_id = kNoOrder;
  std::uint64_t order_no = 0;
  Qty leaves = 0;
};

enum class OrderState : std::uint8_t {
  PendingNew,   // sent, no ack yet
  Live,         // acked, resting or partially filled
  Filled,
  Canceled,
  Rejected,
};

struct OmsOrder {
  OrderId cl_id = kNoOrder;
  std::uint64_t venue_no = 0;
  Nanos create_ts = 0;
  Nanos last_ts = 0;
  Price px = 0;
  Qty qty = 0;
  Qty cum = 0;
  Qty leaves = 0;
  InstrumentId inst = kNoInstrument;
  StrategyId strategy = 0;
  AccountId account = 0;
  Side side = Side::Buy;
  Offset offset = Offset::Open;
  Tif tif = Tif::Day;
  OrderState state = OrderState::PendingNew;

  [[nodiscard]] bool open() const noexcept {
    return state == OrderState::PendingNew || state == OrderState::Live;
  }
};

class Oms {
 public:
  void on_new(const NewOrderReq& r) {
    OmsOrder o{};
    o.cl_id = r.cl_id;
    o.create_ts = r.ts;
    o.last_ts = r.ts;
    o.px = r.px;
    o.qty = r.qty;
    o.leaves = r.qty;
    o.inst = r.inst;
    o.strategy = r.strategy;
    o.account = r.account;
    o.side = r.side;
    o.offset = r.offset;
    o.tif = r.tif;
    orders_[r.cl_id] = o;
    index_add(o);
  }

  void on_exec(const ExecReport& e) {
    auto it = orders_.find(e.cl_id);
    if (it == orders_.end()) return;  // agent/foreign report
    OmsOrder& o = it->second;
    const bool was_open = o.open();
    o.last_ts = e.ts;
    switch (e.type) {
      case ExecType::Ack:
        o.venue_no = e.venue_no;
        if (o.state == OrderState::PendingNew) o.state = OrderState::Live;
        break;
      case ExecType::Reject:
        o.state = OrderState::Rejected;
        o.leaves = 0;
        break;
      case ExecType::Fill:
        o.cum = e.cum;
        o.leaves = e.leaves;
        if (o.leaves == 0) o.state = OrderState::Filled;
        break;
      case ExecType::Canceled:
        o.state = OrderState::Canceled;
        o.leaves = 0;
        break;
      case ExecType::CancelReject:
        break;  // order state unchanged; the cancel failed, not the order
    }
    if (was_open && !o.open()) index_remove(o);
  }

  [[nodiscard]] const OmsOrder* find(OrderId id) const {
    const auto it = orders_.find(id);
    return it == orders_.end() ? nullptr : &it->second;
  }

  // Visit open (live or in-flight) orders on an instrument through the
  // per-instrument index — the self-trade-prevention and exposure inputs
  // (§10.1 #4/#9). No allocation: riskgate runs this inside the battery.
  template <typename Fn>
  void for_each_open(InstrumentId inst, Fn&& fn) const {
    const auto it = open_idx_.find(inst);
    if (it == open_idx_.end()) return;
    for (const OrderId id : it->second) {
      const auto oit = orders_.find(id);
      if (oit != orders_.end() && oit->second.open()) fn(oit->second);
    }
  }

  // Allocating convenience for cold callers (kill-switch sweep, tests).
  [[nodiscard]] std::vector<const OmsOrder*> open_orders(InstrumentId inst) const {
    std::vector<const OmsOrder*> out;
    for_each_open(inst, [&](const OmsOrder& o) { out.push_back(&o); });
    return out;
  }

  [[nodiscard]] Qty open_qty(InstrumentId inst, Side side) const {
    Qty q = 0;
    for_each_open(inst, [&](const OmsOrder& o) {
      if (o.side == side) q += o.leaves;
    });
    return q;
  }

  [[nodiscard]] std::size_t total_orders() const noexcept { return orders_.size(); }

  struct Break {
    OrderId cl_id;
    std::uint64_t venue_no;
    Qty oms_leaves;
    Qty venue_leaves;
  };

  // End-of-day / post-recovery reconciliation against the counter's
  // open-order query (§11.2, §11.3): the venue is the source of truth; any
  // break blocks re-enable.
  [[nodiscard]] std::vector<Break> reconcile(
      const std::vector<VenueOpenOrder>& venue_open) const {
    std::vector<Break> breaks;
    // Venue-open orders must exist here with matching leaves.
    for (const auto& v : venue_open) {
      const auto it = orders_.find(v.cl_id);
      if (it == orders_.end() || !it->second.open() || it->second.leaves != v.leaves)
        breaks.push_back({v.cl_id, v.order_no,
                          it == orders_.end() ? -1 : it->second.leaves, v.leaves});
    }
    // Orders we think are open must be venue-open.
    for (const auto& [id, o] : orders_) {
      if (!o.open() || o.state == OrderState::PendingNew) continue;
      bool found = false;
      for (const auto& v : venue_open)
        if (v.cl_id == id) { found = true; break; }
      if (!found) breaks.push_back({id, o.venue_no, o.leaves, -1});
    }
    return breaks;
  }

  [[nodiscard]] std::uint64_t state_hash() const {
    StateHash h;
    for (const auto& [id, o] : orders_) {  // std::map: deterministic order
      h.mix(id);
      h.mix(o.state);
      h.mix(o.cum);
      h.mix(o.leaves);
      h.mix(o.venue_no);
    }
    return h.value();
  }

  // Snapshot support (§11.1): the table is flat POD rows.
  void save(std::vector<std::uint8_t>& out) const {
    const std::uint64_t n = orders_.size();
    append(out, n);
    for (const auto& [id, o] : orders_) append(out, o);
  }
  std::size_t load(const std::uint8_t* p, std::size_t avail) {
    orders_.clear();
    open_idx_.clear();
    std::uint64_t n = 0;
    std::size_t off = read(p, 0, avail, n);
    for (std::uint64_t i = 0; i < n; ++i) {
      OmsOrder o{};
      off = read(p, off, avail, o);
      orders_[o.cl_id] = o;
      if (o.open()) index_add(o);  // rebuilt in cl_id order: deterministic
    }
    return off;
  }

 private:
  void index_add(const OmsOrder& o) { open_idx_[o.inst].push_back(o.cl_id); }
  void index_remove(const OmsOrder& o) {
    const auto it = open_idx_.find(o.inst);
    if (it == open_idx_.end()) return;
    auto& v = it->second;
    for (auto vi = v.begin(); vi != v.end(); ++vi) {
      if (*vi == o.cl_id) {
        v.erase(vi);  // ordered erase keeps visit order deterministic
        break;
      }
    }
    if (v.empty()) open_idx_.erase(it);
  }

  template <typename T>
  static void append(std::vector<std::uint8_t>& out, const T& v) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), b, b + sizeof(T));
  }
  template <typename T>
  static std::size_t read(const std::uint8_t* p, std::size_t off, std::size_t avail, T& v) {
    if (off + sizeof(T) <= avail) std::memcpy(&v, p + off, sizeof(T));
    return off + sizeof(T);
  }

  std::map<OrderId, OmsOrder> orders_;
  std::map<InstrumentId, std::vector<OrderId>> open_idx_;  // open orders per instrument
};

}  // namespace plat
