// tests_ops.cpp — the operator intervention paths behind the platops web
// console: live parameter re-tune, manual (risk-checked) orders and cancels,
// bookkeeping position transfers, and the journal tailer the console's
// mirror sites are fed from. Everything enters as a journaled Source
// envelope — the same contract as every other boundary crossing.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "plat/bus/journal.hpp"
#include "plat/ops/tail.hpp"
#include "plat/pnl/pnl.hpp"
#include "plat/replay/sidecar.hpp"
#include "platops_app.hpp"
#include "platops_json.hpp"
#include "tests_util.hpp"

namespace {

ParamUpdateEvt param_evt(Nanos ts, StrategyId s, const char* key, std::int64_t value) {
  ParamUpdateEvt u{};
  u.ts = ts;
  u.strategy = s;
  u.value = value;
  std::memcpy(u.key, key, std::strlen(key) + 1);
  return u;
}

}  // namespace

// A journaled ParamUpdateEvt re-tunes exactly the addressed instance.
void test_ops_param_update() {
  InstrumentTable table = one_future();
  SiteConfig sc;
  sc.id = 1;
  sc.name = "ops";
  sc.venue = Venue::SHFE;
  sc.hosted = {0};
  sc.day = 20'260'717;
  RiskLimits wide;
  wide.max_actions_per_sec = 1'000;
  wide.duplicate_horizon = 0;
  sc.limits = wide;

  MomentumParams mp;
  mp.imb_gate_e4 = 1'000;
  TestSite site(table, sc, TestSite::Mode::Live,
                {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})});
  FakeGateway gw;
  site.set_gateway(&gw);

  PhaseChangeEvt ph{};
  ph.ts = at_time(9, 0);
  ph.venue = Venue::SHFE;
  ph.phase = Phase::Continuous;
  site.source(ph, ph.ts);

  // lots 1 → 4 before the first entry; a wrong id and an unknown key no-op.
  site.source(param_evt(at_time(9, 0), 1, "lots", 4), at_time(9, 0));
  site.source(param_evt(at_time(9, 0), 9, "lots", 9), at_time(9, 0));   // not ours
  site.source(param_evt(at_time(9, 0), 1, "warp", 9), at_time(9, 0));   // unknown key

  site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010, 50, 10), at_time(9, 0, 1));
  site.source(snap(0, 2, at_time(9, 0, 2), 78'010, 78'020, 50, 10), at_time(9, 0, 2));
  CHECK(gw.placed.size() == 1);
  CHECK(gw.placed[0].qty == 4);
}

// A ManualOrderCmd becomes a normal risk-checked order under the given
// strategy id; a ManualCancelCmd pulls it through the normal cancel path.
void test_ops_manual_order_cancel() {
  InstrumentTable table = one_future();
  SiteConfig sc;
  sc.id = 1;
  sc.name = "ops";
  sc.venue = Venue::SHFE;
  sc.hosted = {0};
  sc.day = 20'260'717;
  RiskLimits wide;
  wide.max_actions_per_sec = 1'000;
  wide.duplicate_horizon = 0;
  sc.limits = wide;

  MomentumParams mp;
  mp.imb_gate_e4 = 9'000'000;  // strategy stays quiet; the operator trades
  TestSite site(table, sc, TestSite::Mode::Live,
                {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})});
  FakeGateway gw;
  site.set_gateway(&gw);

  PhaseChangeEvt ph{};
  ph.ts = at_time(9, 0);
  ph.venue = Venue::SHFE;
  ph.phase = Phase::Continuous;
  site.source(ph, ph.ts);
  site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010), at_time(9, 0, 1));

  ManualOrderCmd m{};
  m.ts = at_time(9, 0, 2);
  m.px = px_from_cny(78'010.0);
  m.qty = 2;
  m.inst = 0;
  m.strategy = 7;  // the manual book: no live instance needed
  m.account = 1;
  m.side = Side::Buy;
  m.offset = Offset::Open;
  site.source(m, m.ts);
  CHECK(gw.placed.size() == 1);
  CHECK(gw.placed[0].strategy == 7 && gw.placed[0].qty == 2);
  CHECK(gw.placed[0].px == px_from_cny(78'010.0));

  ManualCancelCmd c{};
  c.ts = at_time(9, 0, 3);
  c.cl_id = gw.placed[0].cl_id;
  site.source(c, c.ts);
  CHECK(gw.cancels.size() == 1 && gw.cancels[0].cl_id == gw.placed[0].cl_id);

  // Unknown id: dropped in Site::cancel, nothing sent.
  ManualCancelCmd bogus{};
  bogus.ts = at_time(9, 0, 4);
  bogus.cl_id = 0xDEAD;
  site.source(bogus, bogus.ts);
  CHECK(gw.cancels.size() == 1);
}

// Transfers move lots yd-first with their cost basis; nothing is realized.
void test_ops_position_transfer() {
  InstrumentTable table = one_future();
  PnlLedger pnl(table, FeeSchedule{});
  // Strategy 1 carries 3 lots long yd @78000 and opens 2 more today @78100.
  pnl.carry_in(1, 1, 0, 0, /*fut_long_yd=*/3, 0, px_from_cny(78'000.0));
  ExecReport fill{};
  fill.cl_id = 42;
  fill.ts = at_time(9, 1);
  fill.px = px_from_cny(78'100.0);
  fill.qty = 2;
  fill.cum = 2;
  fill.inst = 0;
  fill.strategy = 1;
  fill.account = 1;
  fill.type = ExecType::Fill;
  fill.side = Side::Buy;
  fill.offset = Offset::Open;
  pnl.on_exec(fill);

  const std::int64_t src_cost = pnl.find(1, 1, 0)->long_cost_e4;
  const std::int64_t realized_before = pnl.find(1, 1, 0)->realized_e4;

  // Move 4: drains yd (3) first, then 1 from td.
  CHECK(pnl.transfer(1, 1, 2, 0, Side::Buy, 4) == 4);
  const Position* src = pnl.find(1, 1, 0);
  const Position* dst = pnl.find(1, 2, 0);
  CHECK(src && src->long_yd == 0 && src->long_td == 1);
  CHECK(dst && dst->long_yd == 3 && dst->long_td == 1);
  CHECK(dst->long_cost_e4 == src_cost);            // basis travelled with the lots
  CHECK(src->realized_e4 == realized_before);      // nothing realized
  CHECK(dst->realized_e4 == 0);

  // Clamped to what the source still holds; self/empty transfers no-op.
  CHECK(pnl.transfer(1, 1, 2, 0, Side::Buy, 99) == 1);
  CHECK(pnl.transfer(1, 1, 2, 0, Side::Buy, 1) == 0);   // source now empty
  CHECK(pnl.transfer(1, 2, 2, 0, Side::Buy, 1) == 0);   // from == to
  CHECK(pnl.transfer(1, 9, 2, 0, Side::Sell, 1) == 0);  // no such book

  // And through the site: the command is an envelope like everything else.
  SiteConfig sc;
  sc.id = 1;
  sc.name = "ops";
  sc.venue = Venue::SHFE;
  sc.hosted = {0};
  sc.day = 20'260'717;
  MomentumParams mp;
  TestSite site(table, sc, TestSite::Mode::Live,
                {SnapMomentum(StrategyId{1}, AccountId{1}, mp, table.size(), {0})});
  site.pnl().carry_in(1, 1, 0, 0, 2, 0, px_from_cny(78'000.0));
  PositionTransferCmd t{};
  t.ts = at_time(9, 2);
  t.qty = 2;
  t.inst = 0;
  t.from_strategy = 1;
  t.to_strategy = 3;
  t.account = 1;
  t.side = Side::Buy;
  site.source(t, t.ts);
  CHECK(site.pnl().find(1, 3, 0) && site.pnl().find(1, 3, 0)->long_yd == 2);
  CHECK(site.pnl().find(1, 1, 0)->long_yd == 0);
}

// End-to-end mirror (platops' whole read side): a LIVE site journals a
// session with strategy fills, a resting manual order, a live param re-tune
// and a position transfer; a MirrorShard then rebuilds the identical state
// from the journal + sidecar alone — the same determinism claim --replay
// makes, expressed through the ops console's own machinery.
void test_ops_mirror() {
  const char* cfg_path = "platops_mirror_test.cfg";
  const char* jnl_path = "platops_mirror_test.jnl";
  {
    std::FILE* f = std::fopen(cfg_path, "wb");
    const char* text =
        "journal = platops_mirror_test.jnl\n"
        "site_name = mirror_test\n"
        "account_id = 1\n"
        "instruments = cu2609\n"
        "momentum.imb_gate_e4 = 1000\n";
    std::fwrite(text, 1, std::strlen(text), f);
    std::fclose(f);
  }
  plat::Config cfg;
  CHECK(cfg.load(cfg_path));
  std::vector<StratSpec> specs;
  CHECK(platcfg::parse_strategies(cfg, AccountId{1}, specs));

  std::uint64_t live_hash = 0;
  OrderId manual_id = kNoOrder;
  {
    InstrumentTable table = one_future();
    CHECK(save_sidecar(jnl_path, table));
    SiteConfig sc;
    sc.id = 1;
    sc.name = "mirror_test";
    sc.venue = Venue::SHFE;
    sc.hosted = {0};
    sc.day = 20'260'717;
    sc.journal_path = jnl_path;
    sc.limits = platcfg::limits_from(cfg);
    sc.fees = platcfg::fees_from(cfg);
    using LiveTestSite = platcfg::PackSite<FakeGateway>;
    LiveTestSite site(table, sc, LiveTestSite::Mode::Live,
                      platcfg::Pack::banks(cfg, table, specs));
    FakeGateway gw;
    site.set_gateway(&gw);

    PhaseChangeEvt ph{};
    ph.ts = at_time(9, 0);
    ph.venue = Venue::SHFE;
    ph.phase = Phase::Continuous;
    site.source(ph, ph.ts);
    // Operator re-tune first, then a rising tape: momentum enters with the
    // NEW lot size, proving the update rode the journal.
    ParamUpdateEvt u{};
    u.ts = at_time(9, 0, 0, kNsPerMs);
    u.strategy = 1;
    u.value = 2;
    std::memcpy(u.key, "lots", 5);
    site.source(u, u.ts);
    site.source(snap(0, 1, at_time(9, 0, 1), 78'000, 78'010, 50, 10), at_time(9, 0, 1));
    site.source(snap(0, 2, at_time(9, 0, 2), 78'010, 78'020, 50, 10), at_time(9, 0, 2));
    CHECK(gw.placed.size() == 1 && gw.placed[0].qty == 2);
    if (gw.placed.empty()) return;  // fail closed: the CHECK above reported it
    ExecReport fill{};
    fill.cl_id = gw.placed[0].cl_id;
    fill.ts = at_time(9, 0, 2, kNsPerMs);
    fill.px = gw.placed[0].px;
    fill.qty = 2;
    fill.cum = 2;
    fill.inst = 0;
    fill.strategy = 1;
    fill.account = 1;
    fill.type = ExecType::Fill;
    fill.side = gw.placed[0].side;
    fill.offset = gw.placed[0].offset;
    site.source(fill, fill.ts);
    // A manual order that stays resting, and a transfer of the fresh fill.
    ManualOrderCmd m{};
    m.ts = at_time(9, 0, 3);
    m.px = px_from_cny(78'000.0);
    m.qty = 1;
    m.inst = 0;
    m.strategy = 7;
    m.account = 1;
    m.side = Side::Buy;
    m.offset = Offset::Open;
    site.source(m, m.ts);
    CHECK(gw.placed.size() == 2);
    manual_id = gw.placed[1].cl_id;
    PositionTransferCmd t{};
    t.ts = at_time(9, 0, 4);
    t.qty = 2;
    t.inst = 0;
    t.from_strategy = 1;
    t.to_strategy = 2;
    t.account = 1;
    t.side = Side::Buy;
    site.source(t, t.ts);

    live_hash = site.state_hash();
    site.run_eod({}, 2, 0);  // drain + seal so the bytes are on disk
  }

  // The mirror sees only the files.
  platops::MirrorShard sh("mirror_test", cfg);
  int spins = 0;
  while (sh.poll() && ++spins < 100) {}
  CHECK(sh.site() != nullptr);
  CHECK(sh.site()->state_hash() == live_hash);  // byte-identical reconstruction
  CHECK(sh.fills().size() == 1);
  CHECK(sh.ops_log().size() == 3);  // param + manual order + transfer
  const Position* moved = sh.site()->pnl().find(1, 2, 0);
  CHECK(moved && moved->long_td == 2);
  CHECK(sh.site()->oms().open_orders(0).size() == 1);
  CHECK(sh.site()->oms().open_orders(0)[0]->cl_id == manual_id);

  // And the JSON document the web page polls carries the essentials.
  std::vector<std::unique_ptr<platops::MirrorShard>> shards;
  shards.push_back(std::make_unique<platops::MirrorShard>("mirror_test", cfg));
  while (shards[0]->poll()) {}
  const std::string j = platops::build_state_json(shards, at_time(15, 0));
  CHECK(j.find("\"status\":\"sealed\"") != std::string::npos);
  CHECK(j.find("cu2609") != std::string::npos);
  CHECK(j.find("\"strategy\":7") != std::string::npos);   // the resting manual order
  CHECK(j.find("transfer") != std::string::npos);         // operator audit feed

  // PLATOPS_KEEP=1 leaves the journal/sidecar/config for a manual UI smoke:
  //   set PLATOPS_KEEP=1 && plat_tests && platops platops_mirror_test.cfg
  if (std::getenv("PLATOPS_KEEP") == nullptr) {
    std::remove(cfg_path);
    std::remove(jnl_path);
    std::remove((std::string(jnl_path) + ".ref").c_str());
  }
}

// The platops tailer: incremental consumption of a growing journal, torn
// tails left alone, and a fresh session at the same path detected as a
// generation bump.
void test_ops_journal_tailer() {
  const char* path = "plat_ops_tail_test.jnl";
  const auto phase_env = [](std::uint64_t seq, Nanos ts) {
    PhaseChangeEvt p{};
    p.ts = ts;
    p.venue = Venue::SHFE;
    p.phase = Phase::Continuous;
    return make_envelope(seq, ts, p, Origin::Source, 1);
  };

  JournalWriter w;
  CHECK(w.open(path, 20'260'717, 1));
  for (std::uint64_t i = 1; i <= 3; ++i) w.append(phase_env(i, at_time(9, 0, static_cast<int>(i))));
  w.flush();

  ops::JournalTailer tail(path);
  std::vector<Envelope> out;
  CHECK(tail.probe());
  CHECK(tail.read(out) == 3);
  CHECK(out.size() == 3 && out[2].seq == 3);
  CHECK(tail.read(out) == 0);  // nothing new

  for (std::uint64_t i = 4; i <= 5; ++i) w.append(phase_env(i, at_time(9, 1, static_cast<int>(i))));
  w.flush();
  CHECK(tail.read(out) == 2 && out.size() == 5);
  w.close();  // seal
  CHECK(tail.probe());
  CHECK(tail.header().sealed == 1);
  CHECK(tail.generation() == 0);  // same session throughout

  // A new day takes the path over: generation bumps, offset resets.
  JournalWriter w2;
  CHECK(w2.open(path, 20'260'718, 1));
  w2.append(phase_env(1, at_time(21, 0)));
  w2.flush();
  CHECK(tail.probe());
  CHECK(tail.generation() == 1);
  out.clear();
  CHECK(tail.read(out) == 1 && out[0].seq == 1);
  w2.close();
  std::remove(path);
}
