#pragma once
// Order-flow simulator: a strategy issuing new/cancel/replace requests and a
// venue answering them, folded into one deterministic tape generator.
//
// Each order's entire lifecycle is generated closed-form at creation time:
// request issue times, venue arrival times (+wire), the venue's own
// price-time walk (which fills happen before the cancel lands, whether the
// replace finds leaves to act on), and the report delivery times back. All
// crossings the OMS matrix exists for — fill-vs-cancel, replace-vs-fill,
// too-late cancels — emerge naturally from the timestamps rather than from
// scripted cases.
//
// Two output streams, one truth:
//   primary   : requests + reports as the OMS sees them, time-merged, with
//               injectable impairments (dropped reports, duplicated reports)
//   drop-copy : every report, complete and per-order ordered — the venue's
//               ground truth as a regulator/clearer would see it
//   Truth[]   : the venue's final word per order (state, cum, qty) plus
//               which primary drops were injected — what recon must find.

#include <algorithm>
#include <cstddef>
#include <vector>

#include "om/core/types.hpp"

namespace om {

struct SimCfg {
  u64 orders = 1'000'000;
  u64 gap_ns = 100;           // a new order every gap
  u64 wire_ns = 5'000;        // one-way firm <-> venue
  u64 dc_lag_ns = 50'000;     // drop-copy lag behind venue time
  u64 maxlife_ns = 200'000;   // lifecycle events scheduled within this window
  u32 cancel_pm = 350;        // strategy cancels this many per mille
  u32 replace_pm = 150;       // ... replaces ...
  u32 unsol_pm = 20;          // venue-initiated cancels
  u32 rej_pm = 5;             // venue rejects this many news
  u32 drop_pm = 0;            // primary reports silently dropped (recon fodder)
  u32 dup_pm = 0;             // primary reports duplicated (dedup fodder)
  u64 alien_orders = 0;       // drop-copy-only phantom orders
  u64 seed = 42;
};

// Venue's final word on one order, for verifying the reconciler.
struct Truth {
  u32 cum = 0;
  u32 qty = 0;
  Cat cat = Cat::kOpen;
  bool dropped_any = false;   // >=1 primary report of this order was dropped
  bool dropped_fill = false;  // >=1 dropped report was a Fill (always material)
};

struct TapeItem {
  u64 t;
  bool is_req;
  Request req;
  ExecReport rep;
};

class VenueSim {
 public:
  VenueSim(const SimCfg& cfg, bool keep_truth, bool keep_dropcopy)
      : cfg_(cfg), rng_(cfg.seed ? cfg.seed : 1), keep_truth_(keep_truth), keep_dc_(keep_dropcopy) {
    if (keep_truth_) truth_.resize(cfg_.orders);
  }

  // Generate lifecycles for orders [first, last) (1-based cl_ids), appending
  // primary items to `out` (unsorted across orders — caller sorts/merges).
  void generate(u64 first, u64 last, std::vector<TapeItem>& out) {
    for (u64 id = first; id < last; ++id) gen_order(id, out);
  }

  // Drop-copy-only phantom orders (a port mixup at the venue): emitted once,
  // after the real orders, with cl_ids the firm never issued.
  void generate_aliens() {
    if (!keep_dc_ || aliens_done_) return;
    aliens_done_ = true;
    for (u64 k = 0; k < cfg_.alien_orders; ++k) {
      const u64 cl_id = cfg_.orders + 1 + k;
      ExecReport a = report(cl_id, Ev::kAckNew, 100, 1'000'000, 0);
      a.exec_id = ++exec_seq_;
      dc_.push_back(a);
      ExecReport fll = report(cl_id, Ev::kFill, 100, 1'000'000, 0);
      fll.exec_id = ++exec_seq_;
      dc_.push_back(fll);
    }
  }

  [[nodiscard]] const std::vector<Truth>& truth() const noexcept { return truth_; }
  [[nodiscard]] const std::vector<ExecReport>& dropcopy() const noexcept { return dc_; }
  [[nodiscard]] u64 reports_emitted() const noexcept { return exec_seq_; }
  [[nodiscard]] u64 dropped() const noexcept { return dropped_; }
  [[nodiscard]] u64 duped() const noexcept { return duped_; }

 private:
  u64 rnd() noexcept {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return rng_;
  }

  static ExecReport report(u64 cl_id, Ev ev, u32 qty, i64 px, u64) noexcept {
    ExecReport e{};
    e.cl_id = cl_id;
    e.qty = qty;
    e.px = px;
    e.ev = ev;
    return e;
  }

  // Emit one venue report: to drop-copy always, to the primary tape unless
  // an injected drop eats it (and never duplicate a dropped report — a dup
  // that survives would self-heal the drop and muddy the ground truth).
  void emit(u64 cl_id, Ev ev, u32 qty, i64 px, u64 venue_t, std::vector<TapeItem>& out,
            Truth* tr) {
    ExecReport e = report(cl_id, ev, qty, px, venue_t);
    e.exec_id = ++exec_seq_;
    if (keep_dc_) dc_.push_back(e);

    if (cfg_.drop_pm != 0 && rnd() % 1000 < cfg_.drop_pm) {
      ++dropped_;
      if (tr) {
        tr->dropped_any = true;
        if (ev == Ev::kFill) tr->dropped_fill = true;
      }
      return;
    }
    out.push_back(TapeItem{venue_t + cfg_.wire_ns, false, {}, e});
    if (cfg_.dup_pm != 0 && rnd() % 1000 < cfg_.dup_pm) {
      ++duped_;
      out.push_back(TapeItem{venue_t + cfg_.wire_ns + 200, false, {}, e});
    }
  }

  void push_req(u64 t, u64 cl_id, ReqType ty, u32 qty, i64 px, u32 sym, u8 side,
                std::vector<TapeItem>& out) {
    Request r{};
    r.cl_id = cl_id;
    r.qty = qty;
    r.px = px;
    r.sym = sym;
    r.type = ty;
    r.side = side;
    out.push_back(TapeItem{t, true, r, {}});
  }

  void gen_order(u64 id, std::vector<TapeItem>& out) {
    const u64 t0 = (id - 1) * cfg_.gap_ns;
    const u32 qty = 100 * (1 + static_cast<u32>(rnd() % 10));
    const i64 px = 1'000'000 + static_cast<i64>(rnd() % 1000);
    const u32 sym = static_cast<u32>(rnd() % 256);
    const u8 side = static_cast<u8>(rnd() & 1);
    Truth* tr = keep_truth_ ? &truth_[id - 1] : nullptr;

    push_req(t0, id, ReqType::kNew, qty, px, sym, side, out);

    // The venue-side timeline for this order.
    struct VE {
      u64 t;
      u8 kind;  // 0 new, 1 fill, 2 cancel, 3 replace, 4 unsol
      u32 q;
    };
    VE ev[8];
    unsigned n = 0;
    const u64 arr_new = t0 + cfg_.wire_ns;
    ev[n++] = {arr_new, 0, 0};

    // Fill opportunities: 0..3, ascending times; quantities are what the
    // market offers, the venue walk below clamps them to the leaves.
    const u64 f = rnd() % 100;
    const unsigned nf = f < 30 ? 0u : f < 70 ? 1u : f < 90 ? 2u : 3u;
    u64 ft = arr_new;
    for (unsigned k = 0; k < nf; ++k) {
      ft += 1 + rnd() % (cfg_.maxlife_ns / 4);
      const u32 fq = (k + 1 == nf && (rnd() & 1)) ? qty  // half of final tries complete
                                                  : 1 + static_cast<u32>(rnd() % qty);
      ev[n++] = {ft, 1, fq};
    }

    if (rnd() % 1000 < cfg_.cancel_pm) {
      const u64 tc = t0 + 1 + rnd() % cfg_.maxlife_ns;
      push_req(tc, id, ReqType::kCancel, 0, 0, sym, side, out);
      ev[n++] = {tc + cfg_.wire_ns, 2, 0};
    }
    if (rnd() % 1000 < cfg_.replace_pm) {
      const u64 tp = t0 + 2 + rnd() % cfg_.maxlife_ns;
      const u32 nq = (rnd() & 1) ? qty + 100 * (1 + static_cast<u32>(rnd() % 5))
                                 : 1 + static_cast<u32>(rnd() % qty);
      push_req(tp, id, ReqType::kReplace, nq, px, sym, side, out);
      ev[n++] = {tp + cfg_.wire_ns, 3, nq};
    }
    if (rnd() % 1000 < cfg_.unsol_pm)
      ev[n++] = {arr_new + 3 + rnd() % cfg_.maxlife_ns, 4, 0};

    std::sort(ev, ev + n, [](const VE& a, const VE& b) {
      return a.t != b.t ? a.t < b.t : a.kind < b.kind;
    });

    // Walk the venue: emit reports, settle the truth.
    const bool rejected = rnd() % 1000 < cfg_.rej_pm;
    bool live = false, filled = false, canceled = false;
    u32 vqty = qty, vcum = 0;
    for (unsigned k = 0; k < n; ++k) {
      const VE& e = ev[k];
      switch (e.kind) {
        case 0:  // arrival of the new order
          if (rejected) {
            emit(id, Ev::kRejNew, 0, px, e.t, out, tr);
          } else {
            live = true;
            emit(id, Ev::kAckNew, qty, px, e.t, out, tr);
          }
          break;
        case 1:  // a fill opportunity
          if (live && vcum < vqty) {
            const u32 fq = e.q < vqty - vcum ? e.q : vqty - vcum;
            vcum += fq;
            emit(id, Ev::kFill, fq, px, e.t, out, tr);
            if (vcum == vqty) {
              live = false;
              filled = true;
            }
          }
          break;
        case 2:  // our cancel arrives
          if (live) {
            live = false;
            canceled = true;
            emit(id, Ev::kAckCancel, 0, 0, e.t, out, tr);
          } else {
            emit(id, Ev::kRejCancel, 0, 0, e.t, out, tr);
          }
          break;
        case 3:  // our replace arrives (cannot shrink below cum)
          if (live && e.q > vcum) {
            vqty = e.q;
            emit(id, Ev::kAckReplace, vqty, px, e.t, out, tr);
          } else {
            emit(id, Ev::kRejReplace, 0, 0, e.t, out, tr);
          }
          break;
        case 4:  // venue-initiated cancel
          if (live) {
            live = false;
            canceled = true;
            emit(id, Ev::kUnsolCancel, 0, 0, e.t, out, tr);
          }
          break;
      }
    }

    if (tr) {
      tr->cum = vcum;
      tr->qty = vqty;
      tr->cat = rejected    ? Cat::kRejected
                : filled    ? Cat::kFilled
                : canceled  ? Cat::kCanceled
                            : Cat::kOpen;
    }
  }

  SimCfg cfg_;
  u64 rng_;
  bool keep_truth_, keep_dc_;
  bool aliens_done_ = false;
  u64 exec_seq_ = 0;
  u64 dropped_ = 0, duped_ = 0;
  std::vector<Truth> truth_;
  std::vector<ExecReport> dc_;
};

// Batched generate/sort/feed with a carry buffer (feedarb's pattern), so the
// tape never has to fit in memory. Order lifecycles overlap, but an item of
// batch N+1 can never be earlier than the first t0 of batch N+1 — everything
// before that horizon is safe to feed; the rest carries. `feed(TapeItem)` is
// invoked in global delivery order, deterministically (full tiebreak).
template <typename F>
inline void run_tape(VenueSim& sim, const SimCfg& cfg, F&& feed, u64 batch = 200'000) {
  auto less = [](const TapeItem& a, const TapeItem& b) {
    if (a.t != b.t) return a.t < b.t;
    if (a.is_req != b.is_req) return a.is_req;  // requests before reports
    if (a.is_req) return a.req.cl_id < b.req.cl_id;
    return a.rep.exec_id < b.rep.exec_id;
  };
  std::vector<TapeItem> items, carry;
  for (u64 first = 1; first <= cfg.orders; first += batch) {
    const u64 last = first + batch < cfg.orders + 1 ? first + batch : cfg.orders + 1;
    items.swap(carry);
    carry.clear();
    sim.generate(first, last, items);
    std::sort(items.begin(), items.end(), less);
    const bool final_batch = last > cfg.orders;
    const u64 horizon = (last - 1) * cfg.gap_ns;  // first t0 of the next batch
    std::size_t i = 0;
    for (; i < items.size() && (final_batch || items[i].t < horizon); ++i) feed(items[i]);
    carry.assign(items.begin() + i, items.end());
  }
}

}  // namespace om
