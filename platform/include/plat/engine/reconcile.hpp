#pragma once
#include <algorithm>
#include "plat/core/time.hpp"
#include "plat/core/log.hpp"
#include "plat/ctp/trader_client.hpp"
#include "plat/ref/instrument.hpp"

namespace plat {
// Crash-recovery reconciliation (§11.2): after the journal replay restored
// state to the crash point, true it up against the counter — fills that
// landed while we were dead become synthesized Source ExecReports (journaled,
// so the recovery itself replays), orders still working at the venue are
// adopted into the new session's registry, and orders the venue no longer
// knows are closed out locally.
template <typename SiteT, typename TraderT>
bool reconcile_resume(SiteT& site, TraderT& trader, const InstrumentTable& table, Nanos now) {
  const auto acct_open = trader.query_account_open_orders();
  const auto acct_trades = trader.query_account_trades();
  const auto counter_pos = trader.query_positions();
  if (!acct_open || !acct_trades || !counter_pos) {
    site.block_trading();
    logf("[resume] incomplete counter queries: trading blocked until repair/restart\n");
    return false;
  }
  int unknown = 0;
  for (const auto& t : *acct_trades) {
    const auto inst = table.find(t.symbol);
    if (std::find(site.config().hosted.begin(), site.config().hosted.end(), inst) ==
        site.config().hosted.end()) continue;
    bool known = false;
    site.oms().for_each([&](const OmsOrder& o) {
      if (o.inst == inst && o.venue_no && o.venue_no == t.venue_no) known = true;
    });
    if (!known) {
      ++unknown;
      logf("[resume] UNKNOWN TRADE %s: no durable order identity; manual review required\n", t.symbol);
    }
  }
  // Never guess strategy ownership for an order absent from the durable log.
  // This covers an order sent in the asynchronous journal's crash window.
  for (const auto& a : *acct_open) {
    const auto inst = table.find(a.symbol);
    if (std::find(site.config().hosted.begin(), site.config().hosted.end(), inst) ==
        site.config().hosted.end()) continue;
    bool known = false;
    for (const auto* o : site.oms().open_orders(inst)) {
      if (o->venue_no && o->venue_no == a.venue_no) known = true;
    }
    if (!known) {
      ++unknown;
      logf("[resume] UNKNOWN LIVE ORDER %s sys_id=%s: resolve at counter before restart\n",
           a.symbol, a.sys_id);
    }
  }
  for (const auto inst : site.config().hosted)
    for (const auto* o : site.oms().open_orders(inst))
      if (!o->venue_no) {
        ++unknown;
        logf("[resume] unacknowledged order has no durable venue identity: manual review required\n");
      }
  if (unknown) {
    site.block_trading();
    return false;
  }
  int adopted = 0, synth_fills = 0, closed = 0;

  for (const InstrumentId inst : site.config().hosted) {
    for (const OmsOrder* o : site.oms().open_orders(inst)) {
      const Instrument& in = table.at(inst);
      const auto is_ours_row = [&](std::uint64_t row_venue_no,
                                   const char* row_sym) {
        return in.sym() == row_sym && row_venue_no == o->venue_no;
      };

      // Fills the dead process never saw: venue trades beyond our cum.
      Qty applied = o->cum;
      Qty skip = o->cum;
      const ctp::AccountOpenOrder* live_row = nullptr;
      for (const auto& a : *acct_open)
        if (is_ours_row(a.venue_no, a.symbol)) live_row = &a;

      for (const auto& t : *acct_trades) {
        if (!is_ours_row(t.venue_no, t.symbol)) continue;
        Qty take = t.qty;
        if (skip > 0) {
          const Qty s = skip < take ? skip : take;
          skip -= s;
          take -= s;
        }
        if (take <= 0) continue;
        ExecReport fill{};
        fill.cl_id = o->cl_id;
        fill.venue_no = o->venue_no;
        fill.ts = now;
        fill.px = t.px;
        fill.qty = take;
        fill.cum = applied + take;
        fill.leaves = o->qty - (applied + take);
        fill.inst = inst;
        fill.strategy = o->strategy;
        fill.account = o->account;
        fill.type = ExecType::Fill;
        fill.side = o->side;
        fill.offset = o->offset;
        site.source(fill, now);
        applied += take;
        ++synth_fills;
      }

      if (applied >= o->qty) continue;  // fully filled while dead: OMS closed it above
      if (live_row) {
        trader.adopt_order(o->cl_id, inst, o->strategy, o->account, o->side, o->offset,
                           o->px, o->qty, applied, in.symbol, live_row->exchange,
                           live_row->sys_id);
        ++adopted;
      } else {
        // The venue no longer knows it (canceled/expired while dead, or it
        // never arrived): terminal locally so no strategy waits forever.
        ExecReport cxl{};
        cxl.cl_id = o->cl_id;
        cxl.venue_no = o->venue_no;
        cxl.ts = now;
        cxl.px = o->px;
        cxl.cum = applied;
        cxl.leaves = 0;
        cxl.inst = inst;
        cxl.strategy = o->strategy;
        cxl.account = o->account;
        cxl.type = ExecType::Canceled;
        cxl.side = o->side;
        cxl.offset = o->offset;
        site.source(cxl, now);
        ++closed;
      }
    }
  }

  // Compare gross position buckets: equal net positions can hide exposure.
  int breaks = 0;
  for (const InstrumentId inst : site.config().hosted) {
    Qty long_yd = 0, long_td = 0, short_yd = 0, short_td = 0;
    site.pnl().for_each([&](const PosKey& k, const Position& p) {
      if (k.inst == inst) {
        long_yd += p.long_yd; long_td += p.long_td;
        short_yd += p.short_yd; short_td += p.short_td;
      }
    });
    for (const auto& p : *counter_pos) {
      if (table.at(inst).sym() != p.symbol) continue;
      (p.is_long ? long_yd : short_yd) -= p.yd;
      (p.is_long ? long_td : short_td) -= p.total - p.yd;
    }
    if (long_yd || long_td || short_yd || short_td) {
      ++breaks;
      logf("[resume] POSITION BREAK %s: gross yd/td buckets differ\n", table.at(inst).symbol);
    }
  }

  logf("[resume] reconciled with counter: %d fills synthesized, %d orders adopted, "
               "%d closed locally, %d position breaks\n",
               synth_fills, adopted, closed, breaks);
  if (breaks) site.block_trading();
  return breaks == 0;
}


} // namespace plat
