#pragma once
// The order-state engine. Single-threaded and deterministic like matchd's
// core — an OMS is a serial fold over two merged streams (requests down,
// reports up), and designing the concurrency away is what makes its
// correctness provable: same tape in, same digest out, every time.
//
// Structure per event:
//   1 id-map probe (open addressing, backshift erase)  -> the order slot
//   1 dedup-ring probe (full exec_id stored, ABA-proof) -> drop duplicates
//   1 verdict-matrix lookup                             -> classify the pair
//   a few branches of quantity arithmetic               -> apply
//   1 reserve retarget                                  -> exposure accounting
//
// The reserve retarget is the accounting trick worth stealing: every order
// carries `reserved`, the leaves it currently contributes to per-symbol open
// exposure. After EVERY transition, one choke point recomputes the target
// reserve from (state, qty, pending_qty, cum) and applies the delta to the
// symbol book. No transition-specific exposure code exists, so no transition
// can leak exposure — the invariant "book == sum over orders" is testable by
// brute-force recompute, and the tests do exactly that.

#include <cstddef>
#include <memory>

#include "om/core/types.hpp"

namespace om {

struct Counters {
  u64 requests = 0, reports = 0;
  u64 verdicts[kVerdictCount] = {};  // indexed by Verdict
  u64 dup_execs = 0;                 // dropped by the exec-id ring
  u64 unknown_order = 0;             // report for a cl_id we never sent
  u64 local_unknown = 0;             // cancel/replace request for unknown cl_id
  u64 local_dup_new = 0;             // second kNew with a used cl_id
  u64 late_intent = 0;               // cancel/replace request on a terminal order
  u64 overfill_qty = 0;              // qty of fills refused as venue errors
  u64 healed_qty_bumps = 0;          // fills beyond known qty absorbed by heal
};

template <std::size_t MaxOrders, std::size_t MaxSyms = 1024>
  requires(MaxOrders >= 2 && (MaxOrders & (MaxOrders - 1)) == 0)
class Oms {
 public:
  Oms()
      : slots_(new Order[MaxOrders]{}),
        map_(new MapSlot[kMapCap]{}),
        dedup_(new u64[kDedupCap]{}),
        books_(new SymBook[MaxSyms]{}),
        free_(new u32[MaxOrders]),
        pq_(new PruneEntry[MaxOrders]) {
    for (u32 i = 0; i < MaxOrders; ++i) free_[i] = static_cast<u32>(MaxOrders - 1 - i);
    free_top_ = MaxOrders;
  }

  // Grace-period pruning (hot-path mode): a terminal order is recycled only
  // after this much quiet time, because "terminal" does not mean "silent" —
  // the rejection of a cancel that lost its race can arrive after the fill
  // that won it, and pruning eagerly would turn that straggler into a
  // spurious unknown-order alarm. 0 = keep everything (recon walks the
  // corpses at end of day).
  u64 prune_grace_ns = 0;

  // ---- strategy side ---------------------------------------------------------
  // The OMS is a bookkeeper, not a gate (riskgate is the gate): cancel and
  // replace intents are recorded even when local state says "too late" —
  // the venue is the arbiter, and its answer will land in the matrix.
  bool on_request(const Request& r, u64 now = 0) {
    ++cnt_.requests;
    if (prune_grace_ns != 0) pump_prune(now);
    switch (r.type) {
      case ReqType::kNew: {
        if (find_slot(r.cl_id) != kNoSlot || free_top_ == 0) {
          ++cnt_.local_dup_new;
          return false;
        }
        const u32 s = free_[--free_top_];
        Order& o = slots_[s];
        o = Order{};
        o.cl_id = r.cl_id;
        o.qty = r.qty;
        o.px = r.px;
        o.sym = r.sym % MaxSyms;
        o.side = r.side;
        o.st = St::kPendingNew;
        map_insert(r.cl_id, s);
        ++live_orders_;
        retarget(o);  // exposure reserved from the moment of intent
        mix(o, 0xA0);
        return true;
      }
      case ReqType::kCancel: {
        const u32 s = find_slot(r.cl_id);
        if (s == kNoSlot) {
          ++cnt_.local_unknown;
          return false;
        }
        Order& o = slots_[s];
        if (is_terminal(o.st)) {
          ++cnt_.late_intent;  // forwarded anyway; venue will say "too late"
          return true;
        }
        if (o.st != St::kPendingCancel) o.st = St::kPendingCancel;
        retarget(o);
        mix(o, 0xA1);
        return true;
      }
      case ReqType::kReplace: {
        const u32 s = find_slot(r.cl_id);
        if (s == kNoSlot) {
          ++cnt_.local_unknown;
          return false;
        }
        Order& o = slots_[s];
        if (is_terminal(o.st)) {
          ++cnt_.late_intent;
          return true;
        }
        if (o.st == St::kLive || o.st == St::kPendingNew) o.st = St::kPendingReplace;
        o.pending_qty = r.qty;  // conservative exposure: max(old, new) leaves
        retarget(o);
        mix(o, 0xA2);
        return true;
      }
    }
    return false;
  }

  // ---- venue side ------------------------------------------------------------
  void on_report(const ExecReport& e, u64 now = 0) {
    ++cnt_.reports;
    if (prune_grace_ns != 0) pump_prune(now);
    if (!dedup_insert(e.exec_id)) {
      ++cnt_.dup_execs;
      return;
    }
    const u32 s = find_slot(e.cl_id);
    if (s == kNoSlot) {
      ++cnt_.unknown_order;
      return;
    }
    Order& o = slots_[s];
    const Verdict v = kVerdict[unsigned(o.st)][unsigned(e.ev)];
    ++cnt_.verdicts[unsigned(v)];
    if (v == Verdict::kStale) return;
    if (v == Verdict::kVenueErr) {
      if (e.ev == Ev::kFill) cnt_.overfill_qty += e.qty;  // never applied
      return;
    }

    switch (e.ev) {
      case Ev::kAckNew:
        if (o.st == St::kPendingNew) o.st = St::kLive;
        break;  // in race states: order is live at venue, our intent still open
      case Ev::kRejNew:
        o.st = St::kRejected;
        break;
      case Ev::kFill: {
        if (o.st == St::kPendingNew) o.st = St::kLive;  // heal: ack was lost
        o.cum += e.qty;
        if (o.cum > o.qty) {  // beyond known qty: a replace-ack must have been lost
          cnt_.healed_qty_bumps += o.cum - o.qty;
          o.qty = o.cum;
        }
        if (o.cum >= effective_qty(o)) o.st = St::kFilled;
        books_[o.sym].position += o.side == 0 ? i64{e.qty} : -i64{e.qty};
        break;
      }
      case Ev::kAckCancel:
        o.st = St::kCanceled;
        break;
      case Ev::kRejCancel:
        // Only meaningful answer while we wait for one: order lives on.
        if (o.st == St::kPendingCancel) o.st = o.cum >= o.qty ? St::kFilled : St::kLive;
        break;
      case Ev::kAckReplace:
        o.qty = e.qty;
        o.pending_qty = 0;
        if (o.cum >= o.qty) o.st = St::kFilled;
        else if (o.st != St::kPendingCancel) o.st = St::kLive;
        break;
      case Ev::kRejReplace:
        if (o.st == St::kPendingReplace) {
          o.pending_qty = 0;
          o.st = o.cum >= o.qty ? St::kFilled : St::kLive;
        }
        break;
      case Ev::kUnsolCancel:
        o.st = St::kCanceled;
        break;
    }

    retarget(o);
    mix(o, static_cast<u8>(e.ev));
    if (is_terminal(o.st)) {
      --live_orders_;
      if (prune_grace_ns != 0) pq_[pq_tail_++ & (MaxOrders - 1)] = PruneEntry{now, s};
    }
  }

  // ---- inspection ----------------------------------------------------------
  [[nodiscard]] const Order* find(u64 cl_id) const noexcept {
    const u32 s = find_slot(cl_id);
    return s == kNoSlot ? nullptr : &slots_[s];
  }
  [[nodiscard]] const SymBook& book(u32 sym) const noexcept { return books_[sym % MaxSyms]; }
  [[nodiscard]] const Counters& counters() const noexcept { return cnt_; }
  [[nodiscard]] u64 digest() const noexcept { return digest_; }
  [[nodiscard]] u64 open_orders() const noexcept { return live_orders_; }

  template <typename F>
  void for_each_order(F&& f) const {
    for (std::size_t i = 0; i < MaxOrders; ++i)
      if (slots_[i].cl_id != 0) f(slots_[i]);
  }

  // Brute-force invariant check: per-symbol books recomputed from the orders
  // must equal the incrementally maintained ones. (Position needs the fill
  // history, so only open interest is recomputable — that is the leak-prone
  // half anyway.)
  [[nodiscard]] bool audit_open_interest() const {
    std::unique_ptr<SymBook[]> fresh(new SymBook[MaxSyms]{});
    for (std::size_t i = 0; i < MaxOrders; ++i) {
      const Order& o = slots_[i];
      if (o.cl_id == 0) continue;
      (o.side == 0 ? fresh[o.sym].open_buy : fresh[o.sym].open_sell) += o.reserved;
    }
    for (std::size_t s = 0; s < MaxSyms; ++s)
      if (fresh[s].open_buy != books_[s].open_buy || fresh[s].open_sell != books_[s].open_sell)
        return false;
    return true;
  }

 private:
  static constexpr u32 kNoSlot = ~u32{0};
  static constexpr std::size_t kMapCap = MaxOrders * 2;  // load <= 0.5
  static constexpr std::size_t kMapMask = kMapCap - 1;
  static constexpr std::size_t kDedupCap = 1u << 20;

  struct MapSlot {
    u64 key;
    u32 val;
    u32 pad;
  };
  struct PruneEntry {
    u64 t;
    u32 slot;
  };

  [[nodiscard]] static u32 effective_qty(const Order& o) noexcept {
    // While a replace is unresolved, both the old and requested qty are
    // possible outcomes; exposure and fill-completion use the larger.
    return o.st == St::kPendingReplace && o.pending_qty > o.qty ? o.pending_qty : o.qty;
  }

  // The single choke point for exposure accounting.
  void retarget(Order& o) noexcept {
    const u32 eff = effective_qty(o);
    const u32 want = is_terminal(o.st) ? 0 : (eff > o.cum ? eff - o.cum : 0);
    u64& open = o.side == 0 ? books_[o.sym].open_buy : books_[o.sym].open_sell;
    open += want;
    open -= o.reserved;
    o.reserved = want;
  }

  void mix(const Order& o, u8 tag) noexcept {
    digest_mix(digest_, o.cl_id);
    digest_mix(digest_, (u64{tag} << 32) | (u64{static_cast<u8>(o.st)} << 40) | o.cum);
    digest_mix(digest_, (u64{o.qty} << 32) | o.reserved);
  }

  // -- id map: open addressing, linear probe, backshift erase --
  static std::size_t home(u64 key) noexcept {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> 32) & kMapMask;
  }
  void map_insert(u64 key, u32 val) noexcept {
    std::size_t i = home(key);
    while (map_[i].key != 0) i = (i + 1) & kMapMask;
    map_[i].key = key;
    map_[i].val = val;
  }
  [[nodiscard]] u32 find_slot(u64 key) const noexcept {
    std::size_t i = home(key);
    while (map_[i].key != 0) {
      if (map_[i].key == key) return map_[i].val;
      i = (i + 1) & kMapMask;
    }
    return kNoSlot;
  }
  void map_erase(u64 key) noexcept {
    std::size_t i = home(key);
    while (map_[i].key != key) {
      if (map_[i].key == 0) return;
      i = (i + 1) & kMapMask;
    }
    std::size_t hole = i, j = i;
    for (;;) {
      j = (j + 1) & kMapMask;
      if (map_[j].key == 0) break;
      const std::size_t h = home(map_[j].key);
      if (((j - h) & kMapMask) >= ((j - hole) & kMapMask)) {
        map_[hole] = map_[j];
        hole = j;
      }
    }
    map_[hole].key = 0;
  }

  void prune(u32 s) noexcept {
    map_erase(slots_[s].cl_id);
    slots_[s].cl_id = 0;
    free_[free_top_++] = s;
  }

  // Recycle terminal orders whose grace period has elapsed. Each arena slot
  // is in the queue at most once (terminal states never transition again,
  // and a slot is popped before it can be reused), so MaxOrders entries
  // bound it. Amortized O(1) per event.
  void pump_prune(u64 now) noexcept {
    while (pq_head_ != pq_tail_) {
      const PruneEntry& e = pq_[pq_head_ & (MaxOrders - 1)];
      if (e.t + prune_grace_ns > now) break;
      prune(e.slot);
      ++pq_head_;
    }
  }

  // -- exec-id dedup: direct-mapped ring, full id stored (feedarb's trick) --
  bool dedup_insert(u64 id) noexcept {
    u64& slot = dedup_[id & (kDedupCap - 1)];
    if (slot == id) return false;
    slot = id;
    return true;
  }

  std::unique_ptr<Order[]> slots_;
  std::unique_ptr<MapSlot[]> map_;
  std::unique_ptr<u64[]> dedup_;
  std::unique_ptr<SymBook[]> books_;
  std::unique_ptr<u32[]> free_;
  std::unique_ptr<PruneEntry[]> pq_;
  std::size_t free_top_ = 0;
  std::size_t pq_head_ = 0, pq_tail_ = 0;
  u64 live_orders_ = 0;
  u64 digest_ = 0xcbf29ce484222325ull;
  Counters cnt_;
};

}  // namespace om
