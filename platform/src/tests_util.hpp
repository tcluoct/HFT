#pragma once
// tests_util.hpp — the plat_tests harness: CHECK macro, shared fixtures
// (a cu-like future, snapshot builder, the fake gateway standing at the
// OrderGateway boundary), and the TestSite composition both suites drive.

#include <cstdio>
#include <vector>

#include "plat/codec/msgs.hpp"
#include "plat/core/time.hpp"
#include "plat/core/types.hpp"
#include "plat/engine/site.hpp"
#include "plat/ref/instrument.hpp"
#include "plat/strat/momentum.hpp"

using namespace plat;

extern int g_checks;
extern int g_fails;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_fails;                                                             \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                        \
  } while (0)

// --------------------------------------------------------------- fixtures

inline InstrumentTable one_future() {
  InstrumentTable t;
  // cu-like: tick 10 CNY, multiplier 5, prev settlement 78000, margin 10%.
  t.add("cu2609", Venue::SHFE, Board::Future, px_from_cny(10.0), 1, 5,
        px_from_cny(78'000.0), 1'000);
  return t;
}

inline MdSnapshotEvt snap(InstrumentId inst, std::uint64_t seq, Nanos ts, double bid,
                          double ask, Qty bq = 10, Qty aq = 10, Qty day_vol = 0) {
  MdSnapshotEvt s{};
  s.feed_seq = seq;
  s.ts = ts;
  s.bid_px[0] = px_from_cny(bid);
  s.bid_qty[0] = bq;
  s.ask_px[0] = px_from_cny(ask);
  s.ask_qty[0] = aq;
  s.last_px = px_from_cny((bid + ask) / 2);
  s.day_volume = day_vol;
  s.inst = inst;
  s.venue = Venue::SHFE;
  return s;
}


// Stands where the CTP trader client stands in production: accepts
// risk-passed orders, and the test feeds the venue's responses back through
// the same Source path the live driver uses. Satisfies the OrderGateway
// concept — no virtual anything.
struct FakeGateway {
  std::vector<NewOrderReq> placed;
  std::vector<CancelReq> cancels;
  SessState st = SessState::Up;
  void place(const NewOrderReq& r) { placed.push_back(r); }
  void cancel(const CancelReq& c) { cancels.push_back(c); }
  [[nodiscard]] SessState state() const { return st; }
};
static_assert(OrderGateway<FakeGateway>);

using TestSite = Site<FakeGateway, SnapMomentum>;
// The whole hot path is compile-time composed: no vtables anywhere.
static_assert(!std::is_polymorphic_v<SnapMomentum>);
static_assert(!std::is_polymorphic_v<TestSite>);
static_assert(!std::is_polymorphic_v<FutBook>);
static_assert(!std::is_polymorphic_v<RiskGate>);
