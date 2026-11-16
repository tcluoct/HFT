#pragma once
// plat/testing/harness.hpp — the framework-less scenario-test harness every
// hft test suite uses: the CHECK macro, the shared fixtures (a cu-like
// future, a depth-snapshot builder), and the fake gateway standing at the
// production OrderGateway boundary.
//
// Deliberately strategy-agnostic: the platform engine suite, the desk suite
// and every strategy-team suite (strategies/<team>/src/tests.cpp) include
// this header and compose the Site they need. Each test executable defines
// `int g_checks; int g_fails;` and its own main() — CHECK counts into those
// globals, so a suite needs no test framework at all.

#include <cstdio>
#include <vector>

#include "plat/codec/msgs.hpp"
#include "plat/core/time.hpp"
#include "plat/core/types.hpp"
#include "plat/gw/gateway.hpp"
#include "plat/ref/instrument.hpp"

// The counters every suite's main TU defines.
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

namespace plat::testing {

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

}  // namespace plat::testing

// Suites write the fixtures unqualified, exactly as the old tests_util.hpp
// allowed; only the two fixture helpers and the gateway are exported.
using plat::testing::FakeGateway;
using plat::testing::one_future;
using plat::testing::snap;
