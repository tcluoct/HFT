#pragma once
// Strategy interface + the reference strategy + the backtest runner.
//
// The strategy is a plain class with `on_tick(const Tick&, Ctx&)`, statically
// dispatched (the backtest loop is templated on the strategy type — same
// zero-virtual philosophy as the live tick2trade engine). The live/backtest
// parity idea: the strategy only ever talks to a Context (bbo/position/now/
// send_ioc); wire the same class to a live context and it trades for real.

#include <chrono>
#include <cstdint>

#include "tl/bt/context.hpp"
#include "tl/data/replayer.hpp"
#include "tl/data/tick.hpp"

namespace tl {

// Same signal family as tick2trade's live strategy: when the touch is tight
// and one side displays much more size, take the thin side.
class ImbalanceTaker {
 public:
  struct Params {
    std::uint64_t imb_num = 2, imb_den = 1;  // heavy side >= 2x thin side
    Price max_spread = 3;
    Qty clip = 100;
    std::uint64_t cooldown_ns = 100'000;
    std::int64_t pos_limit = 2'000;
  };

  explicit ImbalanceTaker(Params p = {}) : p_(p) {}

  HFT_FORCE_INLINE void on_tick(const Tick& t, BtContext& ctx) noexcept {
    if (t.kind != kQuote) return;
    const BtContext::Bbo& b = ctx.bbo(t.symbol);
    const Price spread = b.ask_px - b.bid_px;
    if (spread <= 0 || spread > p_.max_spread) return;
    if (ctx.now() - last_send_ < p_.cooldown_ns) return;

    const std::uint64_t bq = b.bid_qty, aq = b.ask_qty;
    if (bq * p_.imb_den >= aq * p_.imb_num) {  // heavy bid: buy the ask
      if (ctx.position(t.symbol) + static_cast<std::int64_t>(p_.clip) <= p_.pos_limit) {
        ctx.send_ioc(t.symbol, 0, b.ask_px, p_.clip);
        last_send_ = ctx.now();
      }
    } else if (aq * p_.imb_den >= bq * p_.imb_num) {  // heavy ask: sell the bid
      if (ctx.position(t.symbol) - static_cast<std::int64_t>(p_.clip) >= -p_.pos_limit) {
        ctx.send_ioc(t.symbol, 1, b.bid_px, p_.clip);
        last_send_ = ctx.now();
      }
    }
  }

  void reset() noexcept { last_send_ = 0; }

 private:
  Params p_;
  std::uint64_t last_send_ = 0;
};

struct BtStats {
  std::uint64_t ticks = 0;
  std::uint64_t sent = 0, fills = 0, kills = 0, volume = 0;
  double net_pnl = 0, fees = 0, max_dd = 0, sharpe = 0;
  std::uint64_t digest = 0;
  double secs = 0;
};

template <class Strategy>
BtStats run_backtest(Replayer& r, Strategy& strat, BtContext::Params p) {
  BtContext ctx(p);
  Tick t;
  std::uint64_t n = 0;
  const auto w0 = std::chrono::steady_clock::now();
  while (r.next(t)) {
    ctx.on_tick(t);
    strat.on_tick(t, ctx);
    ++n;
  }
  ctx.finish();
  const auto w1 = std::chrono::steady_clock::now();

  BtStats s;
  s.ticks = n;
  s.sent = ctx.sent();
  s.fills = ctx.fills();
  s.kills = ctx.kills();
  s.volume = ctx.volume();
  s.net_pnl = ctx.equity();
  s.fees = ctx.fees();
  s.max_dd = ctx.max_drawdown();
  s.sharpe = ctx.sharpe();
  s.digest = ctx.digest();
  s.secs = std::chrono::duration<double>(w1 - w0).count();
  return s;
}

}  // namespace tl
