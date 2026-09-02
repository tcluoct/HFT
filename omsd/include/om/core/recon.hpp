#pragma once
// Drop-copy reconciliation: the regulator-grade answer to "does my OMS's
// picture of the world match the venue's?". The drop-copy port is a second,
// independent feed of every execution — delivered out-of-band, immune to
// whatever ate a report on the primary path. Folding it gives the venue's
// truth per order; comparing that against the OMS store yields findings in
// four categories:
//
//   phantom        — the venue executed an order this OMS never sent
//                    (port mixup, session bleed: the scary one)
//   state mismatch — venue says filled/canceled/rejected/open, OMS disagrees
//                    (a lost terminal report; risk is sized off stale state)
//   cum mismatch   — fill quantity differs (a lost fill: position is WRONG,
//                    and every downstream hedge is wrong with it)
//   qty mismatch   — order size differs (a lost replace-ack)
//
// plus `stuck` for OMS orders the venue never mentions at all. The fold is a
// mini state machine over reports only (no requests) — deliberately simpler
// than the OMS matrix, because drop-copy is complete and ordered per order,
// so none of the crossing cases exist here.

#include <cstddef>
#include <memory>

#include "om/core/types.hpp"

namespace om {

struct Findings {
  u64 checked = 0;         // orders seen in drop-copy
  u64 phantom = 0;
  u64 state_mismatch = 0;
  u64 cum_mismatch = 0;
  u64 qty_mismatch = 0;
  u64 flagged_orders = 0;  // orders with >= 1 finding
  u64 stuck = 0;           // in OMS, absent from drop-copy
};

enum : u32 {
  kFindPhantom = 1u << 0,
  kFindState = 1u << 1,
  kFindCum = 1u << 2,
  kFindQty = 1u << 3,
};

inline constexpr Cat category_of(St s) noexcept {
  switch (s) {
    case St::kFilled: return Cat::kFilled;
    case St::kCanceled: return Cat::kCanceled;
    case St::kRejected: return Cat::kRejected;
    default: return Cat::kOpen;  // all pending/live states are "open interest"
  }
}

template <std::size_t Cap>
  requires((Cap & (Cap - 1)) == 0)
class Recon {
 public:
  Recon() : slots_(new Slot[Cap]{}) {}

  // Fold one drop-copy report into the venue-truth view.
  void on_dropcopy(const ExecReport& e) {
    VOrd& v = upsert(e.cl_id);
    switch (e.ev) {
      case Ev::kAckNew:
        v.qty = e.qty;
        v.cat = Cat::kOpen;
        break;
      case Ev::kRejNew:
        v.cat = Cat::kRejected;
        break;
      case Ev::kFill:
        v.cum += e.qty;
        if (v.qty != 0 && v.cum >= v.qty) v.cat = Cat::kFilled;
        break;
      case Ev::kAckCancel:
      case Ev::kUnsolCancel:
        v.cat = Cat::kCanceled;
        break;
      case Ev::kAckReplace:
        v.qty = e.qty;
        break;
      case Ev::kRejCancel:
      case Ev::kRejReplace:
        break;  // answers, not state
    }
  }

  // Compare venue truth against the OMS store. `per_finding(cl_id, mask)` is
  // invoked once per flagged order. The OMS must have been run with
  // auto_prune off — reconciliation needs the corpses.
  template <typename OmsT, typename F>
  Findings compare(const OmsT& oms, F&& per_finding) const {
    Findings out;
    for (std::size_t i = 0; i < Cap; ++i) {
      const Slot& s = slots_[i];
      if (s.key == 0) continue;
      ++out.checked;
      u32 mask = 0;
      const Order* o = oms.find(s.key);
      if (o == nullptr) {
        ++out.phantom;
        mask |= kFindPhantom;
      } else {
        if (category_of(o->st) != s.v.cat) {
          ++out.state_mismatch;
          mask |= kFindState;
        }
        if (o->cum != s.v.cum) {
          ++out.cum_mismatch;
          mask |= kFindCum;
        }
        // Size only matters while it can still trade (and a rejected order
        // never had a venue-side size to disagree about).
        if (s.v.cat != Cat::kRejected && o->qty != s.v.qty && s.v.qty != 0) {
          ++out.qty_mismatch;
          mask |= kFindQty;
        }
      }
      if (mask != 0) {
        ++out.flagged_orders;
        per_finding(s.key, mask);
      }
    }
    oms.for_each_order([&](const Order& o) {
      if (find(o.cl_id) == nullptr) ++out.stuck;
    });
    return out;
  }

 private:
  struct VOrd {
    u32 cum = 0;
    u32 qty = 0;
    Cat cat = Cat::kOpen;
  };
  struct Slot {
    u64 key;
    VOrd v;
  };
  static constexpr std::size_t kMask = Cap - 1;

  static std::size_t home(u64 key) noexcept {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> 32) & kMask;
  }

  VOrd& upsert(u64 key) noexcept {
    std::size_t i = home(key);
    while (slots_[i].key != 0 && slots_[i].key != key) i = (i + 1) & kMask;
    slots_[i].key = key;
    return slots_[i].v;
  }

  [[nodiscard]] const VOrd* find(u64 key) const noexcept {
    std::size_t i = home(key);
    while (slots_[i].key != 0) {
      if (slots_[i].key == key) return &slots_[i].v;
      i = (i + 1) & kMask;
    }
    return nullptr;
  }

  std::unique_ptr<Slot[]> slots_;
};

}  // namespace om
