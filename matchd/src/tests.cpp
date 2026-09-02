// Scenario tests for the matching core. A matching engine is correctness-
// critical infrastructure: every price-time rule, TIF behavior, and handle-
// staleness case gets an explicit scripted scenario asserting the exact
// outbound event sequence. No test framework dependency — a CHECK macro and
// a recording sink.

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "me/core/matching_engine.hpp"

using namespace me;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

struct Ev {
  char k;
  std::uint64_t a, b, c, d, e;
};

struct RecSink {
  std::vector<Ev> evs;

  void on_accepted(ParticipantId p, Token t, OrderId id, std::uint8_t side, Price px,
                   Qty q) noexcept {
    (void)p;
    evs.push_back(Ev{'A', t, id, side, static_cast<std::uint64_t>(px), q});
  }
  void on_canceled(ParticipantId p, Token t, OrderId id, Qty leaves, CancelReason r) noexcept {
    (void)p;
    evs.push_back(Ev{'C', t, id, leaves, static_cast<std::uint64_t>(r), 0});
  }
  void on_replaced(ParticipantId p, Token t, OrderId oid, OrderId nid, Price px,
                   Qty q) noexcept {
    (void)p;
    (void)px;
    evs.push_back(Ev{'U', t, oid, nid, q, 0});
  }
  void on_rejected(ParticipantId p, Token t, RejectReason r) noexcept {
    (void)p;
    evs.push_back(Ev{'R', t, static_cast<std::uint64_t>(r), 0, 0, 0});
  }
  void on_exec(ParticipantId p, Token t, OrderId id, Qty q, Price px) noexcept {
    evs.push_back(Ev{'E', t, id, q, static_cast<std::uint64_t>(px), p});
  }
  void on_md_add(OrderId id, std::uint8_t side, Price px, Qty q) noexcept {
    evs.push_back(Ev{'a', id, side, static_cast<std::uint64_t>(px), q, 0});
  }
  void on_md_exec(OrderId id, Qty q, Price px) noexcept {
    evs.push_back(Ev{'e', id, q, static_cast<std::uint64_t>(px), 0, 0});
  }
  void on_md_reduce(OrderId id, Qty q) noexcept { evs.push_back(Ev{'r', id, q, 0, 0, 0}); }
  void on_md_delete(OrderId id) noexcept { evs.push_back(Ev{'d', id, 0, 0, 0, 0}); }

  [[nodiscard]] std::vector<Ev> of(char k) const {
    std::vector<Ev> v;
    for (const auto& e : evs)
      if (e.k == k) v.push_back(e);
    return v;
  }
  [[nodiscard]] const Ev* first(char k) const {
    for (const auto& e : evs)
      if (e.k == k) return &e;
    return nullptr;
  }
};

using Engine = MatchingEngine<RecSink>;

Engine::Params prm() {
  Engine::Params p;
  p.nsym = 1;
  return p;
}

InMsg enter(std::uint8_t side, Price px, Qty q, ParticipantId part = 1,
            std::uint8_t tif = kTifDay, Token tok = 0) {
  InMsg m{};
  m.enter.type = kMsgEnter;
  m.enter.side = side;
  m.enter.tif = tif;
  m.enter.participant = part;
  m.enter.symbol = 0;
  m.enter.token = tok;
  m.enter.price = px;
  m.enter.qty = q;
  return m;
}

InMsg cancel(OrderId id, ParticipantId part = 1) {
  InMsg m{};
  m.cancel.type = kMsgCancel;
  m.cancel.participant = part;
  m.cancel.order_id = id;
  return m;
}

InMsg replace(OrderId id, Price px, Qty q, ParticipantId part = 1) {
  InMsg m{};
  m.replace.type = kMsgReplace;
  m.replace.participant = part;
  m.replace.order_id = id;
  m.replace.new_price = px;
  m.replace.new_qty = q;
  return m;
}

// --- scenarios -------------------------------------------------------------

void test_price_time_priority() {
  std::printf("price-time priority\n");
  RecSink s;
  const auto eng = std::make_unique<Engine>(s, prm());  // ~1.6 MB: heap it
  Engine& e = *eng;
  e.process(enter(kSell, 10001, 60, 1, kTifDay, 11));   // A: first at the price
  e.process(enter(kSell, 10001, 60, 2, kTifDay, 22));   // B: second, same price
  e.process(enter(kBuy, 10001, 100, 3, kTifDay, 33));   // crosses both

  const auto execs = s.of('E');
  CHECK(execs.size() == 4);                     // 2 trades x (aggressor + resting)
  CHECK(execs[1].a == 11 && execs[1].c == 60);  // A fills FIRST, fully
  CHECK(execs[3].a == 22 && execs[3].c == 40);  // then B, partially
  CHECK(s.of('a').size() == 2);                 // only A and B ever rested
  CHECK(s.of('e').size() == 2);                 // two md executions
}

void test_price_improvement() {
  std::printf("trade prints at resting price\n");
  RecSink s;
  const auto eng = std::make_unique<Engine>(s, prm());  // ~1.6 MB: heap it
  Engine& e = *eng;
  e.process(enter(kSell, 10005, 100, 1, kTifDay, 11));
  e.process(enter(kBuy, 10010, 100, 2, kTifDay, 22));   // limit above the ask
  const auto execs = s.of('E');
  CHECK(execs.size() == 2);
  CHECK(execs[0].d == 10005 && execs[1].d == 10005);    // price improvement
}

void test_ioc() {
  std::printf("IOC kills remainder\n");
  RecSink s;
  const auto eng = std::make_unique<Engine>(s, prm());  // ~1.6 MB: heap it
  Engine& e = *eng;
  e.process(enter(kSell, 10001, 50, 1, kTifDay, 11));
  e.process(enter(kBuy, 10001, 100, 2, kTifIoc, 22));
  const auto execs = s.of('E');
  CHECK(execs.size() == 2);                     // filled the available 50
  const Ev* c = s.first('C');
  CHECK(c && c->a == 22 && c->c == 50);         // remainder 50 canceled...
  CHECK(c->d == static_cast<std::uint64_t>(CancelReason::Ioc));
  CHECK(s.of('a').size() == 1);                 // aggressor never rested
}

void test_fok() {
  std::printf("FOK: all-or-nothing\n");
  RecSink s;
  const auto eng = std::make_unique<Engine>(s, prm());  // ~1.6 MB: heap it
  Engine& e = *eng;
  e.process(enter(kSell, 10001, 50, 1, kTifDay, 11));
  e.process(enter(kBuy, 10001, 100, 2, kTifFok, 22));   // needs 100, only 50 there
  CHECK(s.of('E').empty());                              // no partial fills
  const Ev* c = s.first('C');
  CHECK(c && c->d == static_cast<std::uint64_t>(CancelReason::Fok) && c->c == 100);
  e.process(enter(kBuy, 10001, 50, 3, kTifFok, 33));    // exactly available: fills
  CHECK(s.of('E').size() == 2);
}

void test_cancel_and_stale_handle() {
  std::printf("cancel + stale-handle rejection\n");
  RecSink s;
  const auto eng = std::make_unique<Engine>(s, prm());  // ~1.6 MB: heap it
  Engine& e = *eng;
  e.process(enter(kSell, 10002, 100, 1, kTifDay, 11));
  const OrderId id = s.first('A')->b;
  e.process(cancel(id));
  const Ev* c = s.first('C');
  CHECK(c && c->b == id && c->c == 100);
  CHECK(c->d == static_cast<std::uint64_t>(CancelReason::User));
  CHECK(s.of('d').size() == 1);
  e.process(cancel(id));                                 // duplicate: handle is stale
  const auto rejects = s.of('R');
  CHECK(rejects.size() == 1);
  CHECK(rejects[0].b == static_cast<std::uint64_t>(RejectReason::UnknownOrder));
}

void test_replace_qty_decrease_keeps_priority() {
  std::printf("replace qty-down keeps time priority\n");
  RecSink s;
  const auto eng = std::make_unique<Engine>(s, prm());  // ~1.6 MB: heap it
  Engine& e = *eng;
  e.process(enter(kSell, 10001, 100, 1, kTifDay, 11));  // A
  e.process(enter(kSell, 10001, 100, 2, kTifDay, 22));  // B behind A
  const OrderId idA = s.of('A')[0].b;
  e.process(replace(idA, 10001, 50));                   // shrink A in place
  const auto ups = s.of('U');
  CHECK(ups.size() == 1 && ups[0].b == idA && ups[0].c == idA);  // same id kept
  CHECK(s.of('r').size() == 1 && s.of('r')[0].b == 50);          // md reduce by 50
  e.process(enter(kBuy, 10001, 30, 3, kTifDay, 33));
  const auto execs = s.of('E');
  CHECK(execs.size() == 2 && execs[1].a == 11);         // A still matches first
}

void test_replace_price_change_loses_priority() {
  std::printf("replace price-change loses priority + old id goes stale\n");
  RecSink s;
  const auto eng = std::make_unique<Engine>(s, prm());  // ~1.6 MB: heap it
  Engine& e = *eng;
  e.process(enter(kSell, 10001, 100, 1, kTifDay, 11));  // A
  e.process(enter(kSell, 10001, 100, 2, kTifDay, 22));  // B
  const OrderId idA = s.of('A')[0].b;
  e.process(replace(idA, 10001, 150));                  // qty UP: priority lost
  const auto ups = s.of('U');
  CHECK(ups.size() == 1 && ups[0].b == idA && ups[0].c != idA);  // new id issued
  const OrderId nidA = ups[0].c;
  e.process(enter(kBuy, 10001, 30, 3, kTifDay, 33));
  const auto execs = s.of('E');
  CHECK(execs.size() == 2 && execs[1].a == 22);         // B now matches first
  e.process(cancel(idA));                               // old handle: stale
  CHECK(s.of('R').size() == 1);
  e.process(cancel(nidA));                              // new handle: works
  CHECK(s.of('R').size() == 1);
}

void test_replace_cross_triggers_match() {
  std::printf("replace across the spread trades immediately\n");
  RecSink s;
  const auto eng = std::make_unique<Engine>(s, prm());  // ~1.6 MB: heap it
  Engine& e = *eng;
  e.process(enter(kBuy, 9999, 100, 1, kTifDay, 11));    // resting bid
  e.process(enter(kSell, 10005, 100, 2, kTifDay, 22));  // ask far away
  const OrderId idAsk = s.of('A')[1].b;
  e.process(replace(idAsk, 9999, 50));                  // reprice through the bid
  const auto execs = s.of('E');
  CHECK(execs.size() == 2);
  CHECK(execs[0].c == 50 && execs[0].d == 9999);        // trades at resting bid px
}

void test_self_match_prevention() {
  std::printf("self-match prevention (cancel-resting)\n");
  RecSink s;
  const auto eng = std::make_unique<Engine>(s, prm());  // ~1.6 MB: heap it
  Engine& e = *eng;
  e.process(enter(kSell, 10001, 100, 7, kTifDay, 11));  // participant 7 resting
  e.process(enter(kBuy, 10001, 100, 7, kTifDay, 22));   // same participant crosses
  CHECK(s.of('E').empty());                             // NO self-trade
  const Ev* c = s.first('C');
  CHECK(c && c->a == 11);                               // the RESTING order pulled
  CHECK(c->d == static_cast<std::uint64_t>(CancelReason::SelfMatch));
  const auto adds = s.of('a');
  CHECK(adds.size() == 2 && adds[1].b == kBuy);         // aggressor rested after
}

void test_determinism() {
  std::printf("determinism: identical runs, identical events\n");
  const auto script = [] {
    std::vector<InMsg> v;
    v.push_back(enter(kSell, 10001, 60, 1, kTifDay, 1));
    v.push_back(enter(kBuy, 10001, 100, 2, kTifIoc, 2));
    v.push_back(enter(kBuy, 9999, 300, 3, kTifDay, 3));
    v.push_back(enter(kSell, 9999, 500, 4, kTifDay, 4));
    return v;
  }();
  RecSink s1, s2;
  const auto eng1 = std::make_unique<Engine>(s1, prm());
  const auto eng2 = std::make_unique<Engine>(s2, prm());
  Engine& e1 = *eng1;
  Engine& e2 = *eng2;
  for (const auto& m : script) e1.process(m);
  for (const auto& m : script) e2.process(m);
  CHECK(s1.evs.size() == s2.evs.size());
  for (std::size_t i = 0; i < s1.evs.size() && i < s2.evs.size(); ++i)
    CHECK(std::memcmp(&s1.evs[i], &s2.evs[i], sizeof(Ev)) == 0);
}

}  // namespace

int main() {
  std::printf("matchd scenario tests\n=====================\n");
  test_price_time_priority();
  test_price_improvement();
  test_ioc();
  test_fok();
  test_cancel_and_stale_handle();
  test_replace_qty_decrease_keeps_priority();
  test_replace_price_change_loses_priority();
  test_replace_cross_triggers_match();
  test_self_match_prevention();
  test_determinism();
  if (g_fail == 0) {
    std::printf("=====================\nALL TESTS PASSED\n");
    return 0;
  }
  std::printf("=====================\n%d CHECK(S) FAILED\n", g_fail);
  return 1;
}
