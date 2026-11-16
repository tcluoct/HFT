// strategies/cash/src/tests.cpp — the cash team's suite: T0Alpha works a
// carried 底仓 (T+1 inventory), sells first into ask pressure, and therefore
// must do nothing without sellable inventory. That is the channel's most
// important safety property, so it is asserted directly.
//
// The engine is only the host: the test grants inventory through the PnL
// ledger's carry-in, then feeds order-book tops through the Source path.

#include <cstdio>

#include "plat/engine/site.hpp"
#include "plat/testing/harness.hpp"
#include "strat/cash/t0alpha.hpp"

using namespace plat;

int g_checks = 0;
int g_fails = 0;

namespace {

BookTopEvt top(InstrumentId inst, Nanos ts, double bid, double ask, Qty bq, Qty aq) {
  BookTopEvt t{};
  t.ts = ts;
  t.inst = inst;
  t.venue = Venue::SSE;
  t.bid_px = px_from_cny(bid);
  t.bid_qty = bq;
  t.ask_px = px_from_cny(ask);
  t.ask_qty = aq;
  t.last_px = px_from_cny((bid + ask) / 2);
  t.limit_state = LimitState::Normal;
  return t;
}

}  // namespace

static void test_t0_cash() {
  using CashSite = Site<FakeGateway, T0Alpha>;
  InstrumentTable table;
  // ETF-like: tick 0.001 CNY, board lot 100.
  table.add("510300", Venue::SSE, Board::Etf, px_from_cny(0.001), 100, 1, px_from_cny(4.0));
  SiteConfig sc;
  sc.id = 1;
  sc.name = "cash";
  sc.venue = Venue::SSE;
  sc.hosted = {0};
  sc.day = 20'260'713;

  T0Params p;
  p.ema_den = 8;
  p.entry_e4 = -4'000;  // sell when the imbalance EMA is ask-heavy
  p.clip = 200;
  CashSite site(table, sc, CashSite::Mode::Live,
                {T0Alpha(StrategyId{5}, AccountId{1}, p, table.size(), {0})});
  FakeGateway gw;
  site.set_gateway(&gw);

  PhaseChangeEvt ph{};
  ph.ts = at_time(9, 30);
  ph.venue = Venue::SSE;
  ph.phase = Phase::Continuous;
  site.source(ph, ph.ts);

  // Ask pressure, but NO carried inventory: T+1 forbids the sell. This is the
  // safety property — the signal must not create a naked short.
  site.source(top(0, at_time(9, 30, 1), 4.000, 4.001, 10, 90), at_time(9, 30, 1));
  CHECK(gw.placed.empty());

  // Grant the carried 底仓 (200 sellable shares) through the ledger, exactly
  // as platd's carry-in does at startup, then replay the same pressure.
  site.pnl().carry_in(1, 5, 0, 200, 0, 0, px_from_cny(4.000));
  site.source(top(0, at_time(9, 30, 2), 4.000, 4.001, 10, 90), at_time(9, 30, 2));
  CHECK(gw.placed.size() == 1);
  CHECK(gw.placed[0].side == Side::Sell);
  CHECK(gw.placed[0].qty == 200);
  CHECK(gw.placed[0].offset == Offset::Open);
  CHECK(gw.placed[0].strategy == 5);

  // Fill the sell; the position's sellable inventory is consumed and the
  // strategy is now waiting for a buy-back, not another sell.
  ExecReport fill{};
  fill.cl_id = gw.placed[0].cl_id;
  fill.ts = at_time(9, 30, 3);
  fill.px = gw.placed[0].px;
  fill.qty = gw.placed[0].qty;
  fill.cum = gw.placed[0].qty;
  fill.inst = 0;
  fill.strategy = 5;
  fill.account = 1;
  fill.type = ExecType::Fill;
  fill.side = Side::Sell;
  fill.offset = Offset::Open;
  site.source(fill, fill.ts);
  const Position* pos = site.pnl().find(1, 5, 0);
  CHECK(pos && pos->sellable == 0 && pos->total == 0);
  site.source(top(0, at_time(9, 30, 4), 4.000, 4.001, 10, 90), at_time(9, 30, 4));
  CHECK(gw.placed.size() == 1);  // no second sell without inventory
}

int main() {
  test_t0_cash();
  std::printf("strat_cash_tests: %d checks, %d failures\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
