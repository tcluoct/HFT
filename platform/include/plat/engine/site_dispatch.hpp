#pragma once
// plat/engine/site_dispatch.hpp — Site's dispatch chain, out-of-line
// (included by site.hpp; never include directly). One switch, fixed consumer
// order per schema — books before OMS before riskgate before PnL before
// strategies — as direct inlinable calls. dispatch() itself is not
// force-inlined: it recurses through nested publishes (strategy → order →
// deny → strategy), so it is the one deliberate call frame on the path.

#include "plat/engine/site.hpp"

namespace plat {

template <OrderGateway Gateway, AnyStrategy... Strats>
void Site<Gateway, Strats...>::build_books() {
  for (const InstrumentId i : cfg_.hosted) {
    if (table_->at(i).is_stock()) {
      stock_books_[i] = std::make_unique<StockBook>(&table_->at(i));
      risk_.attach_stock_book(i, stock_books_[i].get());
    } else {
      has_fut_[i] = 1;
      risk_.attach_fut_book(i, &fut_books_[i]);
    }
  }
  books_attached_ = true;
}

template <OrderGateway Gateway, AnyStrategy... Strats>
void Site<Gateway, Strats...>::wire_telemetry() {
  // Telemetry slots registered once — the hot path increments raw u64s
  // and records into preresolved histograms; no strings past this point.
  c_envelopes_ = &tel_.counter("stream.envelopes");
  c_fills_ = &tel_.counter("fills");
  c_feed_gap_ = &tel_.counter("feed.gap");
  c_risk_pass_ = &tel_.counter("risk.pass");
  c_risk_deny_ = &tel_.counter("risk.deny");
  for (int c = 1; c <= static_cast<int>(RiskCheck::TickSize); ++c)
    c_deny_check_[c] = &tel_.counter("risk.deny." + std::to_string(c));
  h_t2t_ = &tel_.histo("tick_to_trade_ns");
  h_rtt_ = &tel_.histo("order_roundtrip_ns");
}

template <OrderGateway Gateway, AnyStrategy... Strats>
void Site<Gateway, Strats...>::dispatch(const Envelope& e) {
  cur_ts_ = e.ts;
  ++*c_envelopes_;
  switch (e.schema) {
    case Schema::MdSnapshot: {
      const auto& m = e.as<MdSnapshotEvt>();
      if (m.last_px) pnl_.mark(m.inst, m.last_px);
      if (has_fut_[m.inst]) [[likely]] fut_books_[m.inst].on_snapshot(m);
      route(e);
      break;
    }
    case Schema::MdOrder: {
      const auto& m = e.as<MdOrderEvt>();
      if (StockBook* b = stock_books_[m.inst].get())
        if (b->on_order(m)) publish_top(m.inst, m.ts);
      break;  // book deltas surface to strategies as BookTopEvt
    }
    case Schema::MdTrade: {
      const auto& m = e.as<MdTradeEvt>();
      pnl_.mark(m.inst, m.px);
      if (StockBook* b = stock_books_[m.inst].get())
        if (b->on_trade(m)) publish_top(m.inst, m.ts);
      route(e);
      break;
    }
    case Schema::FeedGap: {
      // Unrecovered gap in the order-by-order feed (§8.1, review R1): the
      // periodic snapshot cannot reconstruct the missing orders, so every
      // book built from this feed fails closed — riskgate's stale-data
      // interlock (#11) refuses the instruments until an authoritative
      // re-baseline clears the quarantine. Futures books need no flag:
      // each venue snapshot is already a full re-baseline.
      for (const InstrumentId i : cfg_.hosted)
        if (stock_books_[i]) stock_books_[i]->set_quarantined(true);
      ++*c_feed_gap_;
      break;
    }
    case Schema::BookTop:
      route(e);
      break;
    case Schema::NewOrderReq:
      on_new_order(e.as<NewOrderReq>());
      break;
    case Schema::CancelReq:
      on_cancel_req(e.as<CancelReq>());
      break;
    case Schema::ExecReport:
      on_exec_report(e);
      break;
    case Schema::RiskDeny: {
      const auto& d = e.as<RiskDeny>();
      if (!d.is_cancel) {
        // A denied order never left the firm: terminal state in the OMS.
        ExecReport synth{};
        synth.cl_id = d.cl_id;
        synth.ts = d.ts;
        synth.type = ExecType::Reject;
        synth.reason = ExecReason::Other;
        synth.inst = d.inst;
        synth.strategy = d.strategy;
        synth.account = d.account;
        oms_.on_exec(synth);
      }
      comp_.on_deny(d);
      route(e);
      break;
    }
    case Schema::KillSwitch: {
      const auto& k = e.as<KillSwitchCmd>();
      risk_.kill().apply(k);
      // Halt/Flatten pull resting orders (§10.3). Cancels ride the normal
      // path — riskgate permits cancels under halt. Position flattening is
      // the strategies' close-out logic against a halted book, kept manual
      // in v1 (§10.3 lists it as the second, human-confirmed stage).
      if (k.action != KillAction::Disarm) {
        for (const OmsOrder* o : open_orders_snapshot())  // cold: operator action
          if (in_kill_scope(k, *o)) cancel(o->cl_id);
      }
      break;
    }
    case Schema::PhaseChange:
      risk_.set_phase(e.as<PhaseChangeEvt>().venue, e.as<PhaseChangeEvt>().phase);
      route(e);
      break;
    case Schema::SessionState:
      sess_state_ = e.as<SessionStateEvt>().state;
      break;
    case Schema::Timer:
      route(e);
      break;
    case Schema::HedgeIntent:
    case Schema::BasisSummary:
    case Schema::HedgeDone:
      // Only *injected* remote traffic reaches strategies; locally
      // originated Remote messages are for the site bridge (§7.3).
      if (e.origin == Origin::Source) route(e);
      break;
    // Operator interventions (all cold; Source-journaled so replay and the
    // audit record carry them):
    case Schema::ParamUpdate:
      route(e);  // the target strategy picks it up by id in deliver_one
      break;
    case Schema::ManualOrder: {
      // Becomes a normal Derived NewOrderReq under the given strategy id —
      // the full riskgate battery applies to the operator too.
      const auto& m = e.as<ManualOrderCmd>();
      dispatching_strategy_ = m.strategy;
      dispatching_account_ = m.account;
      last_manual_cl_id_ = place(m.inst, m.side, m.offset, m.px, m.qty, Tif::Day, m.ts);
      dispatching_strategy_ = 0;
      dispatching_account_ = 0;
      break;
    }
    case Schema::ManualCancel:
      cancel(e.as<ManualCancelCmd>().cl_id);
      break;
    case Schema::PositionTransfer: {
      const auto& t = e.as<PositionTransferCmd>();
      const Qty moved =
          pnl_.transfer(t.account, t.from_strategy, t.to_strategy, t.inst, t.side, t.qty);
      logf("[site] transfer: %lld/%lld %s inst %u strategy %u -> %u\n",
           static_cast<long long>(moved), static_cast<long long>(t.qty),
           t.side == Side::Buy ? "long" : "short", t.inst, t.from_strategy, t.to_strategy);
      break;
    }
  }
}

template <OrderGateway Gateway, AnyStrategy... Strats>
HFT_FORCE_INLINE void Site<Gateway, Strats...>::on_new_order(const NewOrderReq& r) {
  // OMS first: the order exists before its fate is decided, and riskgate's
  // open-order reads must exclude the in-flight request itself.
  oms_.on_new(r);
  const RiskCheck verdict = risk_.check_new(r, cur_ts_);
  if (verdict == kRiskPass) [[likely]] {
    // Gateway first — the wire outranks our own bookkeeping (§12.2);
    // telemetry and compliance accounting happen behind the send.
    if (mode_ == Mode::Live && gw_ && !recovering_) gw_->place(r);
    ++*c_risk_pass_;
    comp_.on_new(r);
    if (cur_ts_ >= r.trigger_ts)
      h_t2t_->record(static_cast<std::uint64_t>(cur_ts_ - r.trigger_ts));
  } else {
    deny(r.cl_id, verdict, r, false);
  }
}

template <OrderGateway Gateway, AnyStrategy... Strats>
HFT_FORCE_INLINE void Site<Gateway, Strats...>::on_cancel_req(const CancelReq& c) {
  const RiskCheck verdict = risk_.check_cancel(c, cur_ts_);
  if (verdict == kRiskPass) [[likely]] {
    if (mode_ == Mode::Live && gw_ && !recovering_) gw_->cancel(c);
    comp_.on_cancel(c);
  } else {
    RiskDeny d{};
    d.cl_id = c.cl_id;
    d.ts = cur_ts_;
    d.inst = c.inst;
    d.strategy = c.strategy;
    d.account = c.account;
    d.check = verdict;
    d.is_cancel = 1;
    ++*c_risk_deny_;
    ++*c_deny_check_[static_cast<int>(verdict)];
    last_deny_ = d;
    dispatch(stream_.seal(cur_ts_, d, Origin::Derived));
  }
}

template <OrderGateway Gateway, AnyStrategy... Strats>
HFT_FORCE_INLINE void Site<Gateway, Strats...>::on_exec_report(const Envelope& e) {
  const auto& r = e.as<ExecReport>();
  oms_.on_exec(r);
  pnl_.on_exec(r);
  if (r.type == ExecType::Fill) ++*c_fills_;
  comp_.on_exec(r);
  if (r.trigger_ts && r.type == ExecType::Ack && r.ts >= r.trigger_ts)
    h_rtt_->record(static_cast<std::uint64_t>(r.ts - r.trigger_ts));
  // Own-order queue tracking in stock books: acks carry venue numbers.
  if (r.type == ExecType::Ack && r.venue_no != 0)
    if (StockBook* b = stock_books_[r.inst].get()) b->track_own(r.venue_no);
  route(e);
}

// Strategies never re-enter: an envelope produced *during* a strategy
// handler (its own order's RiskDeny, a book top from its own fill's feed
// echo) is queued and delivered after the outer handler chain finishes —
// matching production, where each hop is a queue. `ctx.place()` therefore
// always returns the order id before any verdict about it arrives. The
// queue is a fixed ring: strategy fan-out per event is bounded by design,
// so overflow is a logic error, not a load condition.
template <OrderGateway Gateway, AnyStrategy... Strats>
HFT_FORCE_INLINE void Site<Gateway, Strats...>::route(const Envelope& e) {
  if (strat_depth_ > 0) {
    if (q_count_ < kStratQueueCap) [[likely]] {
      strat_q_[(q_head_ + q_count_++) & (kStratQueueCap - 1)] = e;
    } else {
      ++q_overflow_;  // unreachable by construction; loud if it ever isn't
      logf("[site] FATAL: strategy queue overflow\n");
    }
    return;
  }
  ++strat_depth_;
  deliver(e);
  while (q_count_ > 0) {
    const Envelope q = strat_q_[q_head_ & (kStratQueueCap - 1)];
    ++q_head_;
    --q_count_;
    deliver(q);
  }
  --strat_depth_;
}

template <OrderGateway Gateway, AnyStrategy... Strats>
HFT_FORCE_INLINE void Site<Gateway, Strats...>::deliver(const Envelope& e) {
  std::apply([&](auto&... bank) { (deliver_bank(bank, e), ...); }, strats_);
  dispatching_strategy_ = 0;
  dispatching_account_ = 0;
}

// One bank = every configured instance of one strategy type, walked in
// config order. The handler body is statically resolved per type; instance
// count is the only runtime dimension (typically 1–3, cache-resident).
template <OrderGateway Gateway, AnyStrategy... Strats>
template <typename Bank>
HFT_FORCE_INLINE void Site<Gateway, Strats...>::deliver_bank(Bank& bank, const Envelope& e) {
  for (auto& s : bank) deliver_one(s, e);
}

template <OrderGateway Gateway, AnyStrategy... Strats>
template <typename S>
HFT_FORCE_INLINE void Site<Gateway, Strats...>::deliver_one(S& s, const Envelope& e) {
  dispatching_strategy_ = s.id();
  dispatching_account_ = s.account();
  switch (e.schema) {
    case Schema::BookTop: s.on_book(*this, e.as<BookTopEvt>()); break;
    case Schema::MdTrade: s.on_trade(*this, e.as<MdTradeEvt>()); break;
    case Schema::MdSnapshot: s.on_snapshot(*this, e.as<MdSnapshotEvt>()); break;
    case Schema::PhaseChange: s.on_phase(*this, e.as<PhaseChangeEvt>()); break;
    case Schema::ExecReport: {
      const auto& r = e.as<ExecReport>();
      if (r.strategy == s.id()) s.on_exec(*this, r);
      break;
    }
    case Schema::RiskDeny: {
      const auto& d = e.as<RiskDeny>();
      if (d.strategy == s.id()) s.on_deny(*this, d);
      break;
    }
    case Schema::Timer: {
      const auto& t = e.as<TimerEvt>();
      if (t.strategy == s.id()) s.on_timer(*this, t);
      break;
    }
    case Schema::HedgeIntent: s.on_hedge_intent(*this, e.as<HedgeIntent>()); break;
    case Schema::BasisSummary: s.on_basis(*this, e.as<BasisSummary>()); break;
    case Schema::HedgeDone: s.on_hedge_done(*this, e.as<HedgeDone>()); break;
    case Schema::ParamUpdate: {
      const auto& u = e.as<ParamUpdateEvt>();
      if (u.strategy == s.id()) s.on_param(*this, u);
      break;
    }
    default: break;
  }
}

template <OrderGateway Gateway, AnyStrategy... Strats>
template <typename Bank>
void Site<Gateway, Strats...>::start_bank(Bank& bank) {
  for (auto& s : bank) start_one(s);
}

template <OrderGateway Gateway, AnyStrategy... Strats>
template <typename S>
void Site<Gateway, Strats...>::start_one(S& s) {
  dispatching_strategy_ = s.id();
  dispatching_account_ = s.account();
  s.on_start(*this);
}

template <OrderGateway Gateway, AnyStrategy... Strats>
void Site<Gateway, Strats...>::publish_top(InstrumentId inst, Nanos ts) {
  const StockBook* b = stock_books_[inst].get();
  if (!b) return;
  const auto t = b->top();
  BookTopEvt e{};
  e.ts = ts;
  e.bid_px = t.bid_px;
  e.bid_qty = t.bid_qty;
  e.ask_px = t.ask_px;
  e.ask_qty = t.ask_qty;
  e.last_px = t.last_px;
  e.inst = inst;
  e.venue = cfg_.venue;
  e.limit_state = t.limit;
  // Envelope time is the dispatch time of the triggering event; the
  // payload keeps the venue event time as the tick-to-trade trigger.
  dispatch(stream_.seal(cur_ts_, e, Origin::Derived));
}

template <OrderGateway Gateway, AnyStrategy... Strats>
std::vector<const OmsOrder*> Site<Gateway, Strats...>::open_orders_snapshot() const {
  std::vector<const OmsOrder*> out;  // cold path: kill-switch sweep only
  for (const InstrumentId i : cfg_.hosted)
    for (const OmsOrder* o : oms_.open_orders(i)) out.push_back(o);
  return out;
}

template <OrderGateway Gateway, AnyStrategy... Strats>
bool Site<Gateway, Strats...>::in_kill_scope(const KillSwitchCmd& k, const OmsOrder& o) {
  switch (k.scope) {
    case KillScope::Firm:
    case KillScope::Site: return true;
    case KillScope::Account: return o.account == k.target;
    case KillScope::Strategy: return o.strategy == k.target;
  }
  return false;
}

template <OrderGateway Gateway, AnyStrategy... Strats>
void Site<Gateway, Strats...>::deny(OrderId id, RiskCheck check, const NewOrderReq& r,
                                    bool is_cancel) {
  RiskDeny d{};
  d.cl_id = id;
  d.ts = cur_ts_;
  d.px = r.px;
  d.qty = r.qty;
  d.inst = r.inst;
  d.strategy = r.strategy;
  d.account = r.account;
  d.check = check;
  d.side = r.side;
  d.is_cancel = is_cancel ? 1 : 0;
  ++*c_risk_deny_;
  ++*c_deny_check_[static_cast<int>(check)];
  last_deny_ = d;
  dispatch(stream_.seal(cur_ts_, d, Origin::Derived));
}

}  // namespace plat
