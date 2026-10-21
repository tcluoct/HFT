// tests_risk.cpp — the risk desk's riskgate additions (§10.1 #13–#20: the
// futures-side lines and the SSE/SZSE cash-equity rules) and the kill-switch
// throttle bypass: each check fires, doesn't misfire on the
// risk-reducing side, and survives a snapshot round trip.

#include <memory>
#include <utility>
#include <vector>

#include "plat/oms/oms.hpp"
#include "plat/pnl/pnl.hpp"
#include "plat/md/stock_book.hpp"
#include "plat/risk/riskgate.hpp"
#include "plat/risk/stock_rules.hpp"
#include "tests_util.hpp"

namespace {

struct Rig {
  InstrumentTable table = one_future();
  Oms oms;
  PnlLedger pnl{table, FeeSchedule{}};
  std::unique_ptr<RiskGate> gate;
  FutBook book;
  std::uint64_t seq = 500;

  explicit Rig(RiskLimits l = {}) {
    gate = std::make_unique<RiskGate>(table, oms, pnl, l);
    gate->attach_fut_book(0, &book);
    gate->set_phase(Venue::SHFE, Phase::Continuous);
    book.on_snapshot(snap(0, 1, at_time(9, 30), 78'000, 78'010));
  }

  NewOrderReq req(double px, Qty qty, Side side = Side::Buy, Offset off = Offset::Open,
                  StrategyId s = 1) {
    NewOrderReq r{};
    r.cl_id = make_order_id(1, ++seq);
    r.ts = at_time(9, 30, 1);
    r.px = px_from_cny(px);
    r.qty = qty;
    r.inst = 0;
    r.strategy = s;
    r.account = 1;
    r.side = side;
    r.offset = off;
    r.tif = Tif::Day;
    return r;
  }
  // The engine's order: OMS first, then the battery; a denial is terminal.
  RiskCheck send(const NewOrderReq& r, Nanos now) {
    oms.on_new(r);
    const RiskCheck v = gate->check_new(r, now);
    if (v != kRiskPass) report(r, ExecType::Reject, 0);
    return v;
  }
  RiskCheck cancel(OrderId id, Nanos now) {
    const OmsOrder* o = oms.find(id);
    CancelReq c{};
    c.cl_id = id;
    c.ts = now;
    c.inst = 0;
    c.strategy = o ? o->strategy : 1;
    c.account = 1;
    return gate->check_cancel(c, now);
  }
  void report(const NewOrderReq& r, ExecType t, Qty fill) {
    ExecReport e{};
    e.cl_id = r.cl_id;
    e.ts = r.ts;
    e.px = r.px;
    e.qty = fill;
    e.cum = fill;
    e.leaves = t == ExecType::Fill ? r.qty - fill : 0;
    e.inst = r.inst;
    e.strategy = r.strategy;
    e.account = r.account;
    e.type = t;
    e.side = r.side;
    e.offset = r.offset;
    oms.on_exec(e);
    pnl.on_exec(e);
    gate->on_exec(e);
  }
};

void test_loss_limit() {
  RiskLimits l;
  l.max_strategy_loss_e4 = px_from_cny(1'000.0);
  Rig rig(l);
  const Nanos t = at_time(9, 30, 1);
  const auto open = rig.req(78'010.0, 2);
  CHECK(rig.send(open, t) == kRiskPass);
  rig.report(open, ExecType::Fill, 2);
  // 2 lots × 5 × 210 = 2,100 CNY down on the mark: past the 1,000 stop.
  rig.pnl.mark(0, px_from_cny(77'800.0));
  CHECK(rig.pnl.strategy_day_pnl_e4(1) < -px_from_cny(1'000.0));
  CHECK(rig.send(rig.req(77'800.0, 1), t + 1) == RiskCheck::LossLimit);
  CHECK(rig.gate->session().loss_latched(1, 1));
  // The exit stays open: closing today's lots passes.
  CHECK(rig.send(rig.req(77'800.0, 1, Side::Sell, Offset::CloseToday), t + 2) == kRiskPass);
  // Latched: a mark bounce back above the line does not reopen the book.
  rig.pnl.mark(0, px_from_cny(78'200.0));
  CHECK(rig.send(rig.req(78'000.0, 1), t + 3) == RiskCheck::LossLimit);
  // Another strategy on the same account is unaffected (no account limit).
  CHECK(rig.send(rig.req(78'000.0, 1, Side::Sell, Offset::Open, 2), t + 4) == kRiskPass);

  // Survives a snapshot: recovery must not reopen a stopped-out book.
  std::vector<std::uint8_t> snapb;
  rig.gate->save(snapb);
  Rig rig2(l);
  rig2.gate->load(snapb.data(), snapb.size());
  CHECK(rig2.gate->session().loss_latched(1, 1));
  CHECK(rig2.gate->state_hash() == rig.gate->state_hash());
}

void test_loss_baseline_carry() {
  // Carried lots bought far below prev_close: yesterday's gain is not today's.
  Rig rig;
  rig.pnl.carry_in(1, 1, 0, 0, /*long_yd=*/2, 0, px_from_cny(70'000.0));
  CHECK(rig.pnl.strategy_day_pnl_e4(1) == 0);  // marked at prev_close
  rig.pnl.mark(0, px_from_cny(77'900.0));      // −100 × 2 × 5 today
  CHECK(rig.pnl.strategy_day_pnl_e4(1) == -px_from_cny(1'000.0));
  // A second carry into the same key blends cost instead of overwriting it.
  rig.pnl.carry_in(1, 1, 0, 0, 2, 0, px_from_cny(74'000.0));
  CHECK(rig.pnl.find(1, 1, 0)->long_cost_e4 == px_from_cny(72'000.0));
}

void test_inst_open_limit() {
  RiskLimits l;
  l.max_inst_open_qty = 3;
  Rig rig(l);
  const Nanos t = at_time(9, 30, 1);
  const auto a = rig.req(78'000.0, 2);
  CHECK(rig.send(a, t) == kRiskPass);
  CHECK(rig.send(rig.req(77'990.0, 2), t + 1) == RiskCheck::InstOpenLimit);  // 2 working + 2
  rig.report(a, ExecType::Fill, 2);                                          // now 2 filled
  CHECK(rig.send(rig.req(77'990.0, 1), t + 2) == kRiskPass);                 // 2 + 1 = 3
  CHECK(rig.send(rig.req(77'980.0, 1), t + 3) == RiskCheck::InstOpenLimit);
  // Closes are not opens.
  CHECK(rig.send(rig.req(78'000.0, 1, Side::Sell, Offset::CloseToday), t + 4) == kRiskPass);
}

void test_cancel_target_and_inst_cancels() {
  RiskLimits l;
  l.max_inst_cancels = 2;
  Rig rig(l);
  const Nanos t = at_time(9, 30, 1);
  std::vector<NewOrderReq> o;
  for (int i = 0; i < 4; ++i) {
    o.push_back(rig.req(77'900.0 - 10.0 * i, 1));
    CHECK(rig.send(o.back(), t + i) == kRiskPass);
  }
  CHECK(rig.cancel(make_order_id(9, 9), t + 10) == RiskCheck::CancelTarget);  // unknown
  CHECK(rig.cancel(o[0].cl_id, t + 11) == kRiskPass);
  CHECK(rig.cancel(o[0].cl_id, t + 12) == RiskCheck::CancelTarget);  // already in flight
  // Lost cancel: after the retry window the repeat is allowed.
  CHECK(rig.cancel(o[0].cl_id, t + 11 + kNsPerSec) == kRiskPass);
  rig.report(o[0], ExecType::Canceled, 0);
  CHECK(rig.cancel(o[0].cl_id, t + 2 * kNsPerSec) == RiskCheck::CancelTarget);  // terminal
  // Per-contract line: two cancels on this contract today already.
  CHECK(rig.cancel(o[1].cl_id, t + 2 * kNsPerSec) == RiskCheck::InstCancelLimit);
  CHECK(rig.gate->session().cancels(1, 0) == 2);

  // A halt must still pull resting orders, whatever the throttles say.
  KillSwitchCmd k{};
  k.cmd_seq = 1;
  k.scope = KillScope::Strategy;
  k.target = 1;
  k.action = KillAction::Halt;
  rig.gate->kill().apply(k);
  CHECK(rig.cancel(o[1].cl_id, t + 3 * kNsPerSec) == kRiskPass);
  CHECK(rig.cancel(o[2].cl_id, t + 3 * kNsPerSec) == kRiskPass);
  CHECK(rig.gate->session().cancels(1, 0) == 4);  // still counted
}

void test_halt_bypasses_cancel_ratio() {
  // The runaway case: the account's cancel ratio is already over the line
  // when the operator halts. Before the bypass, the halt's own cancels were
  // denied and the resting orders stayed in the market.
  RiskLimits l;
  l.cancel_ratio_pct = 10;
  l.cancel_ratio_min_actions = 2;
  l.max_actions_per_sec = 4;
  l.cancel_retry_after = 0;  // lets the loop below re-cancel inside one second
  Rig rig(l);
  const Nanos t = at_time(9, 30, 1);
  const auto a = rig.req(77'900.0, 1), b = rig.req(77'890.0, 1), c = rig.req(77'880.0, 1);
  CHECK(rig.send(a, t) == kRiskPass);
  CHECK(rig.send(b, t + 1) == kRiskPass);
  CHECK(rig.send(c, t + 2) == kRiskPass);
  CHECK(rig.cancel(a.cl_id, t + 3) == kRiskPass);        // 1/3 = 33% ≥ 10%
  CHECK(rig.cancel(b.cl_id, t + 4) == RiskCheck::RateLimit);  // 4/s window full
  CHECK(rig.cancel(b.cl_id, t + 2 * kNsPerSec) == RiskCheck::CancelRatio);
  KillSwitchCmd k{};
  k.scope = KillScope::Site;
  k.action = KillAction::Halt;
  rig.gate->kill().apply(k);
  CHECK(rig.cancel(b.cl_id, t + 2 * kNsPerSec) == kRiskPass);
  // Twice the per-second cap inside one second: all pass, the rate ring
  // stays bounded (it is sized by the cap), every action is counted.
  for (int i = 0; i < 8; ++i) CHECK(rig.cancel(c.cl_id, t + 2 * kNsPerSec + 1 + i) == kRiskPass);
  CHECK(rig.gate->day_actions(1) == 3 + 1 + 1 + 8);
}

// ------------------------------------------------------- cash equities

// SSE stock fixtures: 0 main board, 1 科创板, 2 ST, 3 ETF.
struct StockRig {
  InstrumentTable table;
  Oms oms;
  PnlLedger pnl;
  std::unique_ptr<RiskGate> gate;
  std::vector<std::unique_ptr<StockBook>> books;
  std::uint64_t seq = 900;
  Nanos clock = at_time(9, 30, 1);

  static InstrumentTable make_table() {
    InstrumentTable t;
    t.add("600000", Venue::SSE, Board::MainBoard, px_from_cny(0.01), 100, 1, px_from_cny(10.0));
    t.add("688001", Venue::SSE, Board::Star, px_from_cny(0.01), 1, 1, px_from_cny(50.0));
    t.add("600001", Venue::SSE, Board::StSpecial, px_from_cny(0.01), 100, 1, px_from_cny(4.0));
    t.add("510300", Venue::SSE, Board::Etf, px_from_cny(0.001), 100, 1, px_from_cny(4.0));
    return t;
  }
  explicit StockRig(RiskLimits l = {}) : table(make_table()), pnl(table, FeeSchedule{}) {
    gate = std::make_unique<RiskGate>(table, oms, pnl, l);
    for (InstrumentId i = 0; i < table.size(); ++i) {
      books.push_back(std::make_unique<StockBook>(&table.at(i)));
      gate->attach_stock_book(i, books.back().get());
    }
    gate->set_phase(Venue::SSE, Phase::Continuous);
    quote(0, 9.99, 10.01);
    quote(1, 49.95, 50.05);
    quote(2, 3.99, 4.01);  // the ETF's book stays empty: cage base falls to prev close
  }
  void quote(InstrumentId i, double bid, double ask) {
    for (const auto& [side, px] : {std::pair{Side::Buy, bid}, std::pair{Side::Sell, ask}}) {
      MdOrderEvt e{};
      e.order_no = ++seq;
      e.ts = at_time(9, 30);
      e.px = px_from_cny(px);
      e.qty = 1'000;
      e.inst = i;
      e.venue = Venue::SSE;
      e.action = MdAction::Add;
      e.side = side;
      books[i]->on_order(e);
    }
  }
  NewOrderReq req(InstrumentId i, double px, Qty qty, Side side = Side::Buy, StrategyId s = 1) {
    NewOrderReq r{};
    r.cl_id = make_order_id(2, ++seq);
    r.ts = clock;
    r.px = px_from_cny(px);
    r.qty = qty;
    r.inst = i;
    r.strategy = s;
    r.account = 1;
    r.side = side;
    r.tif = Tif::Day;
    return r;
  }
  // Each send advances the clock past the duplicate breaker's horizon.
  RiskCheck send(const NewOrderReq& r) {
    clock += 30 * kNsPerMs;
    oms.on_new(r);
    const RiskCheck v = gate->check_new(r, clock);
    if (v != kRiskPass) fill(r, 0, ExecType::Reject);
    return v;
  }
  void fill(const NewOrderReq& r, Qty q, ExecType t = ExecType::Fill) {
    ExecReport e{};
    e.cl_id = r.cl_id;
    e.ts = clock;
    e.px = r.px;
    e.qty = q;
    e.cum = q;
    e.leaves = t == ExecType::Fill ? r.qty - q : 0;
    e.inst = r.inst;
    e.strategy = r.strategy;
    e.account = r.account;
    e.type = t;
    e.side = r.side;
    oms.on_exec(e);
    pnl.on_exec(e);
    gate->on_exec(e);
  }
};

void test_stock_rule_functions() {
  const InstrumentTable t = StockRig::make_table();
  const Instrument& main = t.at(0);
  const Instrument& star = t.at(1);
  CHECK(stock_qty_ok(main, Side::Buy, 100, 0));
  CHECK(!stock_qty_ok(main, Side::Buy, 150, 0));
  CHECK(stock_qty_ok(main, Side::Sell, 150, 250));   // the whole 50-share odd part
  CHECK(!stock_qty_ok(main, Side::Sell, 30, 250));   // a slice of it
  CHECK(stock_qty_ok(main, Side::Buy, 1'000'000, 0));
  CHECK(!stock_qty_ok(main, Side::Buy, 1'000'100, 0));
  CHECK(!stock_qty_ok(star, Side::Buy, 199, 0));
  CHECK(stock_qty_ok(star, Side::Buy, 201, 0));      // 科创板: step of 1 above 200
  CHECK(stock_qty_ok(star, Side::Sell, 150, 150));   // sub-200 remainder, all at once
  CHECK(!stock_qty_ok(star, Side::Sell, 150, 300));
  CHECK(!stock_qty_ok(star, Side::Buy, 100'001, 0));
  CHECK(board_max_order_qty(Board::ChiNext) == 300'000);

  const Price p10 = px_from_cny(10.0), tick = px_from_cny(0.01);
  CHECK(cage_base(Side::Buy, px_from_cny(9.99), px_from_cny(10.01), p10, p10) ==
        px_from_cny(10.01));
  CHECK(cage_base(Side::Buy, px_from_cny(9.99), 0, p10, p10) == px_from_cny(9.99));
  CHECK(cage_base(Side::Sell, 0, 0, px_from_cny(9.5), p10) == px_from_cny(9.5));
  CHECK(cage_base(Side::Sell, 0, 0, 0, p10) == p10);
  CHECK(in_price_cage(Side::Buy, px_from_cny(10.20), p10, tick, 200, 10));   // +2%
  CHECK(!in_price_cage(Side::Buy, px_from_cny(10.21), p10, tick, 200, 10));
  CHECK(in_price_cage(Side::Sell, px_from_cny(9.80), p10, tick, 200, 10));
  CHECK(!in_price_cage(Side::Sell, px_from_cny(9.79), p10, tick, 200, 10));
  // Low-priced stock: 10 ticks (0.10) is wider than 2% of 2.00 (0.04).
  CHECK(in_price_cage(Side::Buy, px_from_cny(2.10), px_from_cny(2.0), tick, 200, 10));
  CHECK(!in_price_cage(Side::Buy, px_from_cny(2.11), px_from_cny(2.0), tick, 200, 10));
}

void test_stock_gate() {
  {  // lot, size, cage on the main board
    StockRig rig;
    CHECK(rig.send(rig.req(0, 10.01, 100)) == kRiskPass);
    CHECK(rig.send(rig.req(0, 10.01, 150)) == RiskCheck::StockQty);
    CHECK(rig.send(rig.req(0, 10.21, 100)) == kRiskPass);       // base 10.01 ×1.02 = 10.2102
    CHECK(rig.send(rig.req(0, 10.22, 100)) == RiskCheck::PriceCage);
    CHECK(rig.send(rig.req(0, 10.0, 200'000)) == RiskCheck::SizeNotional);  // > 10万 shares
    // No tick-count collar on stocks: 60 ticks under the market is a fine bid.
    CHECK(rig.send(rig.req(0, 9.40, 100)) == kRiskPass);
    // Auctions have no cage (the limit band still binds).
    rig.gate->set_phase(Venue::SSE, Phase::OpenAuction);
    CHECK(rig.send(rig.req(0, 10.50, 100)) == kRiskPass);
    CHECK(rig.send(rig.req(0, 11.10, 100)) == RiskCheck::PriceCollar);
  }
  {  // ETFs are outside the cage unless opted in
    StockRig rig;
    CHECK(rig.send(rig.req(3, 4.2, 100)) == kRiskPass);
    RiskLimits l;
    l.stock_cage_etf = true;
    StockRig rig2(l);
    CHECK(rig2.send(rig2.req(3, 4.2, 100)) == RiskCheck::PriceCage);
  }
  {  // odd-lot sells must clear the whole odd remainder
    StockRig rig;
    rig.pnl.carry_in(1, 1, 0, 250);
    CHECK(rig.send(rig.req(0, 10.0, 30, Side::Sell)) == RiskCheck::StockQty);
    CHECK(rig.send(rig.req(0, 10.0, 50, Side::Sell)) == kRiskPass);
  }
  {  // 风险警示股票 daily buy cap: filled + working + this order
    RiskLimits l;
    l.st_daily_buy_qty = 300;
    StockRig rig(l);
    const auto a = rig.req(2, 4.01, 200);
    CHECK(rig.send(a) == kRiskPass);
    CHECK(rig.send(rig.req(2, 4.00, 200)) == RiskCheck::StBuyLimit);  // 200 working + 200
    rig.fill(a, 200);
    CHECK(rig.send(rig.req(2, 4.00, 100)) == kRiskPass);              // 200 filled + 100
    CHECK(rig.send(rig.req(2, 3.99, 100)) == RiskCheck::StBuyLimit);
    CHECK(rig.send(rig.req(0, 10.01, 100)) == kRiskPass);  // the cap is ST-only
  }
  {  // concentration counts every strategy book of the account
    RiskLimits l;
    l.max_stock_holding_qty = 1'000;
    StockRig rig(l);
    rig.pnl.carry_in(1, 1, 0, 800);
    CHECK(rig.pnl.account_holding(1, 0) == 800);
    CHECK(rig.send(rig.req(0, 10.01, 200, Side::Buy, 2)) == kRiskPass);
    CHECK(rig.send(rig.req(0, 10.00, 100, Side::Buy, 2)) == RiskCheck::Concentration);
    // Selling is fine (above strategy 2's resting 10.01 bid: no self-trade).
    CHECK(rig.send(rig.req(0, 10.02, 100, Side::Sell, 1)) == kRiskPass);
    // The derived holding survives a ledger snapshot round trip.
    std::vector<std::uint8_t> b;
    rig.pnl.save(b);
    PnlLedger p2(rig.table, FeeSchedule{});
    p2.load(b.data(), b.size());
    CHECK(p2.account_holding(1, 0) == 800);
  }
  {  // ...and by value
    RiskLimits l;
    l.max_stock_holding_e4 = px_from_cny(5'000.0);
    StockRig rig(l);
    CHECK(rig.send(rig.req(0, 10.01, 500)) == RiskCheck::Concentration);  // 5,005 CNY
    CHECK(rig.send(rig.req(0, 10.01, 400)) == kRiskPass);
  }
}

}  // namespace

void test_risk_desk() {
  test_loss_limit();
  test_loss_baseline_carry();
  test_inst_open_limit();
  test_cancel_target_and_inst_cancels();
  test_halt_bypasses_cancel_ratio();
  test_stock_rule_functions();
  test_stock_gate();
}
