#include <filesystem>
#include <optional>
#include <thread>

#include "tests_util.hpp"
#include "platd_app.hpp"
#include "plat/ctp/trader_client.hpp"
#include "plat/engine/reconcile.hpp"
#include "plat/engine/session_marker.hpp"

namespace plat {
struct SiteTestAccess {
  static void fault(TestSite& s) { s.journal_.simulate_fault_for_test(); }
};
}
namespace plat::ctp {
struct TraderClientTestAccess {
  template <typename Send>
  static bool query(TraderClient& c, Send send) {
    return c.run_query(send, std::chrono::milliseconds(5));
  }
  static std::size_t positions(const TraderClient& c) { return c.qry_positions_.size(); }
  static void set_state(TraderClient& c, SessState state) { c.set_state(state); }
};
}

namespace {
SiteConfig config() {
  SiteConfig c;
  c.id = 1; c.day = 20261117; c.hosted = {0};
  c.limits.duplicate_horizon = 0;
  return c;
}
void ready(TestSite& site, FakeGateway& gw) {
  site.set_gateway(&gw);
  SessionStateEvt up{};
  up.venue = Venue::SHFE; up.state = SessState::Up;
  site.source(up, at_time(9, 0));
  PhaseChangeEvt phase{};
  phase.venue = Venue::SHFE; phase.phase = Phase::Continuous;
  site.source(phase, at_time(9, 0));
  site.source(snap(0, 1, at_time(9, 0), 78000, 78010, 10, 10), at_time(9, 0));
}
void order(TestSite& site) {
  ManualOrderCmd m{};
  m.strategy = 1; m.account = 1; m.inst = 0;
  m.px = px_from_cny(78010); m.qty = 1;
  m.side = Side::Buy; m.offset = Offset::Open;
  site.source(m, at_time(9, 0));
}
struct Counter {
  std::optional<std::vector<ctp::AccountOpenOrder>> orders{std::in_place};
  std::optional<std::vector<ctp::AccountTrade>> trades{std::in_place};
  std::optional<std::vector<ctp::PositionRow>> positions{std::in_place};
  int adopted = 0;
  auto query_account_open_orders() { return orders; }
  auto query_account_trades() { return trades; }
  auto query_positions() { return positions; }
  template <typename... Args> void adopt_order(Args&&...) { ++adopted; }
};
}

void test_trade_ring_transport() {
  TradeRing ring;
  ctp::TraderClient client({}, ring, +[]() -> Nanos { return 123; });
  ctp::TraderClientTestAccess::set_state(client, SessState::Up);
  const OrderId cl_id = make_order_id(1, 17);
  client.adopt_order(cl_id, 0, 4, 7, Side::Buy, Offset::Open, px_from_cny(78'000),
                     4, 1, "cu2701", "SHFE", "12345", {"1000"});
  CThostFtdcTradeField fill{};
  ctp::order_ref_from_seq(17, fill.OrderRef);
  std::memcpy(fill.OrderSysID, "12345", sizeof("12345"));
  std::memcpy(fill.ExchangeID, "SHFE", sizeof("SHFE"));
  std::memcpy(fill.TradeID, "1000", sizeof("1000"));
  fill.Price = 78'010.0;
  fill.Volume = 2;
  fill.OffsetFlag = THOST_FTDC_OF_Open;
  client.OnRtnTrade(&fill);
  std::memcpy(fill.TradeID, "1001", sizeof("1001"));
  client.OnRtnTrade(&fill);
  client.OnRtnTrade(&fill);
  std::memcpy(fill.TradeID, "1002", sizeof("1002"));
  fill.Volume = 1;
  client.OnRtnTrade(&fill);

  TradeEvent event;
  CHECK(ring.poll(event) == TradeRing::Poll::Event && event.kind == TradeEvent::Kind::Session);
  if (event.kind == TradeEvent::Kind::Session)
    CHECK(event.session.state == SessState::Up && event.session.ts == 123);
  CHECK(ring.poll(event) == TradeRing::Poll::Event && event.kind == TradeEvent::Kind::Report);
  if (event.kind == TradeEvent::Kind::Report) {
    const auto& report = event.report;
    CHECK(report.type == ExecType::Fill && report.cl_id == cl_id);
    CHECK(report.account == 7 && report.strategy == 4 && report.inst == 0);
    CHECK(report.qty == 2 && report.cum == 3 && report.leaves == 1);
    CHECK(report.ts == 123 && report.px == px_from_cny(fill.Price));
  }
  CHECK(ring.poll(event) == TradeRing::Poll::Event && event.kind == TradeEvent::Kind::Report);
  if (event.kind == TradeEvent::Kind::Report)
    CHECK(event.report.qty == 1 && event.report.cum == 4 && event.report.leaves == 0);
  CHECK(ring.poll(event) == TradeRing::Poll::Empty);

  const TradeEvent session{SessionStateEvt{123, Venue::SHFE, SessState::Up}};
  for (std::size_t i = 0; i < TradeRing::kSlots - 1; ++i) ring.publish(session);
  CHECK(ring.poll(event) == TradeRing::Poll::Event);
  ring.publish(session);  // reuse the freed slot
  ring.publish(session);  // overflow invalidates the stream
  CHECK(ring.poll(event) == TradeRing::Poll::Fault);
  CHECK(ring.poll(event) == TradeRing::Poll::Fault);
  TradeRing malformed;
  ctp::TraderClient invalid_client({}, malformed, +[]() -> Nanos { return 123; });
  invalid_client.adopt_order(cl_id, 0, 4, 7, Side::Buy, Offset::Open, px_from_cny(78'000),
                             1, 0, "cu2701", "SHFE", "12345");
  fill.Volume = 2;
  invalid_client.OnRtnTrade(&fill);
  CHECK(malformed.poll(event) == TradeRing::Poll::Fault);
}

void test_query_fail_closed() {
  using Access = ctp::TraderClientTestAccess;
  TradeRing ring;
  ctp::TraderClient client({}, ring, +[]() -> Nanos { return 1; });
  int expired = 0;
  CHECK(!Access::query(client, [&](int id) { expired = id; return 0; }));
  CThostFtdcInvestorPositionField row{};
  row.Position = 3;
  client.OnRspQryInvestorPosition(&row, nullptr, expired, true);
  CHECK(Access::positions(client) == 0); // late callback cannot mutate retired result
  CHECK(Access::query(client, [&](int id) {
    CThostFtdcRspInfoField error{}; error.ErrorID = 7;
    client.OnRspError(&error, expired, true); // unrelated request must not abort this query
    client.OnRspQryInvestorPosition(&row, nullptr, expired, true);
    client.OnRspQryInvestorPosition(nullptr, nullptr, id, true); // valid empty account
    return 0;
  }));
  CHECK(Access::positions(client) == 0);
  CHECK(!Access::query(client, [&](int id) {
    CThostFtdcRspInfoField error{}; error.ErrorID = 9;
    client.OnRspQryInvestorPosition(&row, nullptr, id, false); // partial rows
    client.OnRspQryInvestorPosition(nullptr, &error, id, true);
    return 0;
  }));
  CHECK(!Access::query(client, [&](int id) {
    CThostFtdcRspInfoField error{}; error.ErrorID = 10;
    client.OnRspError(&error, id, true);
    return 0;
  }));
  CHECK(!Access::query(client, [](int) { return -1; }));
  CHECK(!Access::query(client, [&](int id) {
    client.OnRspQryOrder(nullptr, nullptr, id - 1, true);
    return 0; // unrelated completion must not satisfy the current query
  }));
}

void test_safety_interlocks() {
  auto table = one_future();
  auto c = config();
  c.journal_path = "safety_interlock_test.jnl";
  {
    TestSite site(table, c, TestSite::Mode::Live, std::vector<SnapMomentum>{});
    FakeGateway gw;
    ready(site, gw);
    CHECK(site.arm_session(at_time(9, 0)));
    order(site);
    CHECK(gw.placed.size() == 1);
    const auto id = site.last_manual_order_cl_id();
    SiteTestAccess::fault(site);
    CHECK(!site.journal_healthy());
    // Check at order submission, even before the driver has observed the fault.
    order(site);
    CHECK(gw.placed.size() == 1);
    site.command_kill(KillScope::Site, KillAction::Halt, 0, at_time(9, 0));
    site.command_kill(KillScope::Site, KillAction::Disarm, 0, at_time(9, 0));
    CHECK(site.risk().kill().any_armed());
    CHECK(!site.arm_session(at_time(9, 0)));
    site.cancel(id); // cancellation remains available to reduce risk
    CHECK(!gw.cancels.empty());
    order(site);
    CHECK(gw.placed.size() == 1);
  }
  JournalReader jr;
  CHECK(jr.open(c.journal_path));
  CHECK(!jr.sealed());
  std::filesystem::remove(c.journal_path);
  c.journal_path = "missing_safety_test_directory/journal";
  TestSite missing(table, c, TestSite::Mode::Live, std::vector<SnapMomentum>{});
  FakeGateway gw;
  ready(missing, gw);
  CHECK(!missing.arm_session(at_time(9, 0)));
  missing.command_kill(KillScope::Site, KillAction::Disarm, 0, at_time(9, 0));
  order(missing);
  CHECK(gw.placed.empty());

  TestSite query_failure(table, config(), TestSite::Mode::Live, std::vector<SnapMomentum>{});
  query_failure.run_eod({}, 0, 0, false);
  CHECK(query_failure.eod().ran && !query_failure.eod().fills_ok);

  // A recovery lock with a healthy journal must remain deterministic on replay.
  c.journal_path = "safety_replay_test.jnl";
  {
    TestSite blocked(table, c, TestSite::Mode::Live, std::vector<SnapMomentum>{});
    FakeGateway blocked_gw;
    blocked.block_trading();
    ready(blocked, blocked_gw);
    blocked.command_kill(KillScope::Site, KillAction::Disarm, 0, at_time(9, 0));
    order(blocked);
    CHECK(blocked_gw.placed.empty());
    blocked.run_eod({}, 0, 0);
  }
  JournalReader record;
  CHECK(record.open(c.journal_path) && record.verified());
  TestSite replay(table, c, TestSite::Mode::Replay, std::vector<SnapMomentum>{});
  for (const auto& e : record.envelopes()) replay.inject(e);
  CHECK(replay.stream().stream_hash() == record.header().stream_hash);
  std::filesystem::remove(c.journal_path);
}

void test_recovery_counter_audit() {
  const auto table = one_future();
  const auto make_site = [&] {
    return std::make_unique<TestSite>(table, config(), TestSite::Mode::Live,
                                     std::vector<SnapMomentum>{});
  };
  Counter counter;
  auto clean = make_site();
  CHECK(reconcile_resume(*clean, counter, table, at_time(9, 0)));
  CHECK(!clean->safety_blocked());
  auto failed = make_site();
  counter.orders.reset();
  CHECK(!reconcile_resume(*failed, counter, table, at_time(9, 0)));
  CHECK(failed->safety_blocked());
  counter = Counter{};
  ctp::AccountOpenOrder unknown{};
  std::snprintf(unknown.symbol, sizeof(unknown.symbol), "%s", table.at(0).symbol);
  unknown.venue_no = 999; unknown.leaves = 1;
  counter.orders->push_back(unknown);
  auto orphan = make_site();
  CHECK(!reconcile_resume(*orphan, counter, table, at_time(9, 0)));
  CHECK(orphan->safety_blocked() && counter.adopted == 0);
  FakeGateway gw;
  ready(*orphan, gw);
  orphan->command_kill(KillScope::Site, KillAction::Disarm, 0, at_time(9, 0));
  CHECK(!orphan->arm_session(at_time(9, 0)));
  order(*orphan);
  CHECK(gw.placed.empty());
  counter = Counter{};
  ctp::AccountTrade trade{};
  std::snprintf(trade.symbol, sizeof(trade.symbol), "%s", table.at(0).symbol);
  trade.venue_no = 999; trade.qty = 1;
  counter.trades->push_back(trade);
  auto unknown_fill = make_site();
  CHECK(!reconcile_resume(*unknown_fill, counter, table, at_time(9, 0)));
  CHECK(unknown_fill->safety_blocked());
  counter = Counter{};
  ctp::PositionRow pos{};
  std::snprintf(pos.symbol, sizeof(pos.symbol), "%s", table.at(0).symbol);
  pos.total = pos.yd = 1;
  pos.is_long = true; counter.positions->push_back(pos);
  pos.is_long = false; counter.positions->push_back(pos);
  auto gross = make_site();
  CHECK(!reconcile_resume(*gross, counter, table, at_time(9, 0))); // net zero is not flat
  CHECK(gross->safety_blocked());

  // A known resting order remains recoverable and cancellable.
  counter = Counter{};
  auto known = make_site();
  FakeGateway known_gw;
  ready(*known, known_gw);
  order(*known);
  CHECK(known_gw.placed.size() == 1);
  const auto id = known->last_manual_order_cl_id();
  ExecReport ack{};
  ack.cl_id = id; ack.venue_no = 123; ack.inst = 0;
  ack.account = 1; ack.strategy = 1; ack.leaves = 1; ack.type = ExecType::Ack;
  known->source(ack, at_time(9, 0));
  unknown.venue_no = 123;
  counter.orders->push_back(unknown);
  CHECK(reconcile_resume(*known, counter, table, at_time(9, 0)));
  CHECK(counter.adopted == 1 && !known->safety_blocked());
  known->run_eod({VenueOpenOrder{id, 123, 1}}, 0, 0);
  CHECK(known->eod().order_breaks == 0 && !known->eod().clean());

  // Reused OrderRef alone cannot establish which crashed session owned it.
  auto unacked = make_site();
  FakeGateway unacked_gw;
  ready(*unacked, unacked_gw);
  order(*unacked);
  counter.orders->front().ref_seq = unacked->last_manual_order_cl_id() & 0xFFFF'FFFF'FFFFull;
  CHECK(!reconcile_resume(*unacked, counter, table, at_time(9, 0)));
  CHECK(unacked->safety_blocked());

  // Query failure must not synthesize a cancellation of a known live order.
  counter.orders.reset();
  CHECK(!reconcile_resume(*known, counter, table, at_time(9, 0)));
  CHECK(known->oms().find(id)->state == OrderState::Live);
}

void test_session_marker() {
  Config fleet;
  fleet.set_default("user_id", "desk");
  fleet.set_default("shard1.instruments", "cu, rb");
  fleet.set_default("shard2.instruments", "rb");
  CHECK(!platcfg::unique_fleet_instruments(fleet));
  fleet.set_default("shard2.investor_id", "second");
  CHECK(platcfg::unique_fleet_instruments(fleet));
  Config lineup;
  lineup.set_default("strategies", "first");
  lineup.set_default("first.type", "momentum");
  lineup.set_default("first.id", "2");
  lineup.set_default("first.account", "2");
  std::vector<StratSpec> specs;
  CHECK(!platd::parse_live_strategies(lineup, 1, specs));
  const std::string path = "safety_test.eod-breaks";
  std::filesystem::remove(path);
  SessionMarker first(path);
  CHECK(first.begin() && !first.preexisting());
  CHECK(std::filesystem::exists(path));
  CHECK(!first.complete(false)); // failed query/disk/reconciliation leaves the gate
  SessionMarker restarted(path);
  CHECK(restarted.begin() && restarted.preexisting());
  CHECK(!restarted.complete(true)); // a later empty session cannot erase the incident
  CHECK(std::filesystem::exists(path));
  CHECK(first.complete(true));
  CHECK(!std::filesystem::exists(path));
  SessionMarker bad("missing_safety_test_directory/marker");
  CHECK(!bad.begin());
  CHECK(!bad.complete(true));
  SessionMarker directory(".");
  CHECK(!directory.begin());
#ifdef _WIN32
  SessionMarker locked(path);
  CHECK(locked.begin());
  HANDLE hold = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(hold != INVALID_HANDLE_VALUE);
  CHECK(!locked.complete(true));
  CHECK(std::filesystem::exists(path));
  if (hold != INVALID_HANDLE_VALUE) CloseHandle(hold);
  CHECK(locked.complete(true));
#endif
}

void test_carry_attribution() {
  auto table = one_future();
  platd::LiveSite site(table, config(), platd::LiveSite::Mode::Replay, desk::Pack::Banks{});
  PositionsRow owner{};
  owner.strategy = 2;
  owner.long_lots = 2;
  ctp::PositionRow broker{};
  std::snprintf(owner.symbol, sizeof(owner.symbol), "%s", table.at(0).symbol);
  std::snprintf(broker.symbol, sizeof(broker.symbol), "%s", owner.symbol);
  broker.is_long = true;
  broker.total = broker.yd = 1;
  const auto now = at_time(9, 0);
  CHECK(platd::apply_carry(site, table, 1, {owner}, {broker, broker}, now));
  CHECK(site.pnl().at(1, 2, 0).long_yd == 2);
  CHECK(!platd::apply_carry(site, table, 1, {}, {broker}, now));
  CHECK(site.pnl().at(1, 0, 0).long_yd == 1 && site.pnl().at(1, 2, 0).long_yd == 2);
  CHECK(!platd::apply_carry(site, table, 1, {owner}, {}, now));
}
