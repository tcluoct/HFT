// Scenario tests for the pre-trade risk gate. A risk gate is the one
// component that is *required* to be correct by regulation (SEC 15c3-5):
// every rule gets a scenario proving it fires, proving it doesn't overfire,
// and proving rejects leave no state behind. No test framework — a CHECK
// macro, same as the rest of the family.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "rg/core/gate.hpp"

using namespace rg;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_fail;                                                     \
    }                                                               \
  } while (0)

constexpr std::size_t kSyms = 64;
using Table = LimitsTable<kSyms>;
using Gate = RiskGate<kSyms>;

SymbolLimits base_limits() {
  return SymbolLimits{
      .max_order_qty = 1'000,
      .max_order_notional = 20'000'000,  // 1000 sh * 11_000 ticks fits, bigger doesn't
      .band_lo = 9'000,
      .band_hi = 11'000,
      .max_long_pos = 5'000,
      .max_short_pos = 5'000,
      .enabled = 1,
      .pad_ = 0,
  };
}

// Throttle that never rejects, for tests that aren't about the throttle.
void open_throttle(Gate& g) { g.throttle().configure_raw(TokenBucket::kOne, 1'000'000); }

OrderReq buy(SymbolId s, Qty q, PriceTicks px) { return {s, Side::kBuy, q, px}; }
OrderReq sell(SymbolId s, Qty q, PriceTicks px) { return {s, Side::kSell, q, px}; }

void t_clean_accept() {
  std::puts("clean order accepted, position reserved");
  Table t;
  t.publish(0, base_limits());
  Gate g(t);
  open_throttle(g);
  CHECK(g.check(buy(0, 100, 10'000), 1000) == 0);
  CHECK(g.position(0) == 100);
  CHECK(g.check(sell(0, 30, 10'000), 1001) == 0);
  CHECK(g.position(0) == 70);
}

void t_each_rule_fires_alone() {
  std::puts("each rule fires exactly its own bit");
  Table t;
  t.publish(0, base_limits());
  auto disabled = base_limits();
  disabled.enabled = 0;
  t.publish(1, disabled);
  Gate g(t);
  open_throttle(g);

  CHECK(g.check(buy(0, 0, 10'000), 1) == kQtyZero);
  CHECK(g.check(buy(0, 1'001, 10'000), 2) == kQtyTooLarge);
  CHECK(g.check(buy(0, 1'000, 8'999), 3) == kPriceBandLow);
  CHECK(g.check(buy(0, 1'000, 11'001), 4) == kPriceBandHigh);
  // Notional can't breach alone under the base limits (band × qty caps it),
  // so tighten it on its own symbol to prove the bit in isolation:
  auto tight = base_limits();
  tight.max_order_notional = 500'000;  // 100 sh * 10_000 = 1M > 500k
  t.publish(2, tight);
  CHECK(g.check(buy(2, 100, 10'000), 5) == kNotionalTooLarge);
  CHECK(g.check(buy(1, 100, 10'000), 6) == kUnknownSymbol);   // disabled
  CHECK(g.check(buy(kSyms, 100, 10'000), 7) == kUnknownSymbol);  // out of range
  CHECK(g.position(0) == 0);
}

void t_multi_violation_mask() {
  std::puts("multiple violations OR into one mask");
  Table t;
  t.publish(0, base_limits());
  Gate g(t);
  open_throttle(g);
  const auto m = g.check(buy(0, 2'000, 12'000), 1);
  CHECK((m & kQtyTooLarge) != 0);
  CHECK((m & kPriceBandHigh) != 0);
  CHECK((m & kPriceBandLow) == 0);
}

void t_notional_overflow() {
  std::puts("notional check survives 64-bit overflow (fat-finger proof)");
  Table t;
  auto l = base_limits();
  l.max_order_qty = ~std::uint64_t{0};  // qty rule out of the way
  l.band_hi = std::int64_t{1} << 62;    // band rule out of the way
  t.publish(0, l);
  Gate g(t);
  open_throttle(g);
  // px * qty wraps 2^64: a naive u64 product would come out tiny and pass.
  const auto m = g.check(buy(0, std::uint64_t{1} << 33, std::int64_t{1} << 33), 1);
  CHECK((m & kNotionalTooLarge) != 0);
}

void t_position_long_boundary() {
  std::puts("long position: exactly-at-limit accepted, one over rejected");
  Table t;
  t.publish(0, base_limits());  // max_long_pos 5000
  Gate g(t);
  open_throttle(g);
  for (int i = 0; i < 5; ++i) CHECK(g.check(buy(0, 1'000, 10'000), i) == 0);
  CHECK(g.position(0) == 5'000);
  CHECK(g.check(buy(0, 1, 10'000), 10) == kPositionLong);
  CHECK(g.position(0) == 5'000);                     // reject reserved nothing
  CHECK(g.check(sell(0, 100, 10'000), 11) == 0);     // reducing is fine
  CHECK(g.position(0) == 4'900);
}

void t_position_short_boundary() {
  std::puts("short position symmetric");
  Table t;
  t.publish(0, base_limits());
  Gate g(t);
  open_throttle(g);
  for (int i = 0; i < 5; ++i) CHECK(g.check(sell(0, 1'000, 10'000), i) == 0);
  CHECK(g.position(0) == -5'000);
  CHECK(g.check(sell(0, 1, 10'000), 10) == kPositionShort);
  CHECK(g.check(buy(0, 100, 10'000), 11) == 0);
  CHECK(g.position(0) == -4'900);
}

void t_unwind() {
  std::puts("unwind hands a dead order's reservation back");
  Table t;
  t.publish(0, base_limits());
  Gate g(t);
  open_throttle(g);
  const auto o = buy(0, 1'000, 10'000);
  CHECK(g.check(o, 1) == 0);
  CHECK(g.position(0) == 1'000);
  g.unwind(o);  // exchange rejected it
  CHECK(g.position(0) == 0);
}

void t_kill_switch() {
  std::puts("kill switch rejects everything, releases cleanly");
  Table t;
  t.publish(0, base_limits());
  Gate g(t);
  open_throttle(g);
  g.set_kill(true);
  const auto m = g.check(buy(0, 100, 10'000), 1);
  CHECK((m & kKillSwitch) != 0);
  CHECK(g.position(0) == 0);
  g.set_kill(false);
  CHECK(g.check(buy(0, 100, 10'000), 2) == 0);
}

void t_throttle() {
  std::puts("token bucket: burst, refill, and rejects don't spend tokens");
  Table t;
  t.publish(0, base_limits());
  Gate g(t);
  // 1 token per 128 cycles (exact in 32.32), burst of 3, bucket starts full.
  g.throttle().configure_raw(TokenBucket::kOne >> 7, 3);

  const std::uint64_t t0 = 1 << 20;
  CHECK(g.check(buy(0, 1, 10'000), t0) == 0);
  CHECK(g.check(buy(0, 1, 10'000), t0) == 0);
  CHECK(g.check(buy(0, 1, 10'000), t0) == 0);
  CHECK(g.check(buy(0, 1, 10'000), t0) == kThrottled);   // burst spent
  CHECK(g.check(buy(0, 1, 10'000), t0 + 128) == 0);      // exactly one refilled
  CHECK(g.check(buy(0, 1, 10'000), t0 + 128) == kThrottled);

  // A band-violating order while the bucket refills must not spend the token.
  CHECK(g.check(buy(0, 1, 99'000), t0 + 256) == kPriceBandHigh);
  CHECK(g.check(buy(0, 1, 10'000), t0 + 256) == 0);  // token still there
}

void t_hot_reload() {
  std::puts("limits hot-reload is visible to the very next check");
  Table t;
  t.publish(0, base_limits());
  Gate g(t);
  open_throttle(g);
  CHECK(g.check(buy(0, 100, 10'500), 1) == 0);
  auto tighter = base_limits();
  tighter.band_hi = 10'400;
  t.publish(0, tighter);
  CHECK(g.check(buy(0, 100, 10'500), 2) == kPriceBandHigh);
  t.publish(0, base_limits());
  CHECK(g.check(buy(0, 100, 10'500), 3) == 0);
}

void t_seqlock_stress() {
  std::puts("seqlock: reader never sees a torn limits record (2s stress)");
  Table t;
  // Every record ever published (including the initial one) satisfies:
  // band_hi == band_lo + 2000, max_long_pos == band_lo * 3 == max_short_pos.
  auto make = [](std::int64_t k) {
    auto nl = base_limits();
    nl.band_lo = k;
    nl.band_hi = k + 2'000;
    nl.max_long_pos = k * 3;
    nl.max_short_pos = k * 3;
    return nl;
  };
  t.publish(0, make(9'000));
  std::atomic<bool> stop{false};

  std::thread w([&] {
    std::int64_t k = 9'000;
    while (!stop.load(std::memory_order_relaxed)) {
      t.publish(0, make(k));
      k = (k == 9'000) ? 909'000 : 9'000;  // large jump: torn reads would show
    }
  });

  std::uint64_t reads = 0, torn = 0, retries = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    for (int i = 0; i < 10'000; ++i) {
      const auto r = t.read(0, &retries);
      torn += (r.band_hi != r.band_lo + 2'000) | (r.max_long_pos != r.band_lo * 3) |
              (r.max_short_pos != r.max_long_pos);
      ++reads;
    }
  }
  stop.store(true);
  w.join();
  std::printf("    %llu reads, %llu retries, %llu torn\n",
              static_cast<unsigned long long>(reads), static_cast<unsigned long long>(retries),
              static_cast<unsigned long long>(torn));
  CHECK(torn == 0);
  CHECK(reads > 0);
}

}  // namespace

int main() {
  std::puts("riskgate scenario tests");
  std::puts("=======================");
  t_clean_accept();
  t_each_rule_fires_alone();
  t_multi_violation_mask();
  t_notional_overflow();
  t_position_long_boundary();
  t_position_short_boundary();
  t_unwind();
  t_kill_switch();
  t_throttle();
  t_hot_reload();
  t_seqlock_stress();
  std::puts("=======================");
  if (g_fail == 0) {
    std::puts("ALL TESTS PASSED");
    return 0;
  }
  std::printf("%d CHECKS FAILED\n", g_fail);
  return 1;
}
