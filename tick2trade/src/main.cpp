// tick2trade: a four-stage HFT hot path with per-hop latency attribution.
//
//   exchange sim --MdRing--> feed handler --BookRing--> strategy
//        ^                                                 |
//        |                                              OrderRing
//        +---- OrderRing (wire) <---- order gateway <------+
//        +---- ExecRing  ----> strategy (fills)
//
// Each stage runs on its own pinned core and busy-spins. Timestamps:
//   t0 exchange wire  t1 book applied  t2 decision  t3 gateway out  t4 exchange in

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "hft/common/async_logger.hpp"
#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/run_control.hpp"
#include "hft/common/tsc_clock.hpp"
#include "t2t/config.hpp"
#include "t2t/md/feed_handler.hpp"
#include "t2t/oe/order_gateway.hpp"
#include "t2t/sim/exchange_sim.hpp"
#include "t2t/strategy/imbalance_taker.hpp"

namespace {

struct Config {
  double seconds = 10.0;
  double warmup = 2.0;
  std::uint64_t rate = 1'000'000;
  unsigned nsym = 2;
  bool pin = true;
  bool rt = true;
  // Even-numbered logical CPUs: on SMT boxes Windows pairs siblings as
  // (2n, 2n+1), so this lands each stage on its own physical core.
  unsigned cores[4] = {2, 4, 6, 8};  // exchange, feed, strategy, gateway
  std::uint64_t seed = 42;
  const char* logpath = "t2t.log";
};

bool parse_u64(const char* s, std::uint64_t& v) {
  const auto r = std::from_chars(s, s + std::strlen(s), v);
  return r.ec == std::errc{} && *r.ptr == '\0';
}

bool parse_cores(const char* s, unsigned (&cores)[4]) {
  const char* p = s;
  const char* end = s + std::strlen(s);
  for (int i = 0; i < 4; ++i) {
    unsigned v{};
    const auto r = std::from_chars(p, end, v);
    if (r.ec != std::errc{}) return false;
    cores[i] = v;
    p = r.ptr;
    if (i < 3) {
      if (p == end || *p != ',') return false;
      ++p;
    }
  }
  return p == end;
}

void usage() {
  std::printf(
      "tick2trade [options]\n"
      "  --seconds N     measured run length in seconds        (default 10)\n"
      "  --warmup N      warmup seconds before measuring       (default 2)\n"
      "  --rate N        market-data msgs/sec, 0 = max speed   (default 1000000)\n"
      "  --symbols N     symbols to simulate, 1..%u            (default 2)\n"
      "  --cores A,B,C,D pin exchange,feed,strategy,gateway    (default 2,4,6,8)\n"
      "  --no-pin        do not pin threads\n"
      "  --rt            raise thread priority to time-critical\n"
      "  --seed N        RNG seed                              (default 42)\n"
      "  --log PATH      async log file                        (default t2t.log)\n",
      t2t::kMaxSymbols);
}

bool parse_args(int argc, char** argv, Config& cfg) {
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (!std::strcmp(a, "--seconds")) {
      const char* v = next();
      if (!v) return false;
      cfg.seconds = std::strtod(v, nullptr);
    } else if (!std::strcmp(a, "--warmup")) {
      const char* v = next();
      if (!v) return false;
      cfg.warmup = std::strtod(v, nullptr);
    } else if (!std::strcmp(a, "--rate")) {
      const char* v = next();
      if (!v || !parse_u64(v, cfg.rate)) return false;
    } else if (!std::strcmp(a, "--symbols")) {
      std::uint64_t n{};
      const char* v = next();
      if (!v || !parse_u64(v, n) || n < 1 || n > t2t::kMaxSymbols) return false;
      cfg.nsym = static_cast<unsigned>(n);
    } else if (!std::strcmp(a, "--cores")) {
      const char* v = next();
      if (!v || !parse_cores(v, cfg.cores)) return false;
    } else if (!std::strcmp(a, "--no-pin")) {
      cfg.pin = false;
    } else if (!std::strcmp(a, "--rt")) {
      cfg.rt = true;
    } else if (!std::strcmp(a, "--seed")) {
      const char* v = next();
      if (!v || !parse_u64(v, cfg.seed)) return false;
    } else if (!std::strcmp(a, "--log")) {
      const char* v = next();
      if (!v) return false;
      cfg.logpath = v;
    } else {
      usage();
      return false;
    }
  }
  return true;
}

std::string comma(std::uint64_t v) {
  char raw[24];
  const auto r = std::to_chars(raw, raw + sizeof raw, v);
  std::string s(raw, r.ptr);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
  return s;
}

void row(const char* name, const hft::LatencyHistogram& h) {
  const auto ns = [](std::uint64_t cyc) {
    return static_cast<unsigned long long>(hft::TscClock::to_ns(cyc));
  };
  if (h.count() == 0) {
    std::printf("  %-34s %14s\n", name, "no samples");
    return;
  }
  std::printf("  %-34s %14s %8llu %8llu %8llu %8llu %8llu %10llu\n", name, comma(h.count()).c_str(),
              ns(h.min()), ns(h.percentile(0.50)), ns(h.percentile(0.90)), ns(h.percentile(0.99)),
              ns(h.percentile(0.999)), ns(h.max()));
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (!parse_args(argc, argv, cfg)) return 1;

  using namespace t2t;

  hft::TscClock::calibrate();
  const unsigned hw = std::thread::hardware_concurrency();
  if (cfg.pin && hw > 0 && hw < 6) {
    std::printf("note: only %u hardware threads; disabling pinning\n", hw);
    cfg.pin = false;
  }

  std::printf("tick2trade | tsc %.3f GHz | %u hw threads | cache line %zu B | %u symbol(s)\n",
              hft::TscClock::cycles_per_ns(), hw, hft::kCacheLine, cfg.nsym);
  std::printf("md rate %s msg/s | warmup %.1f s | measure %.1f s | pin %s | rt %s\n\n",
              cfg.rate ? comma(cfg.rate).c_str() : "max", cfg.warmup, cfg.seconds,
              cfg.pin ? "yes" : "no", cfg.rt ? "yes" : "no");

  // Rings live on the heap (2 MiB+); components hold references.
  auto md_ring = std::make_unique<MdRing>();
  auto book_ring = std::make_unique<BookRing>();
  auto order_ring = std::make_unique<OrderRing>();   // strategy -> gateway
  auto wire_ring = std::make_unique<OrderRing>();    // gateway -> exchange
  auto exec_ring = std::make_unique<ExecRing>();     // exchange -> strategy

  hft::AsyncLogger logger(cfg.logpath);

  const Price mid0 = 10'000;  // $100.00 at $0.01 tick
  ExchangeSim::Params sp;
  sp.rate = cfg.rate;
  sp.mid0 = mid0;
  sp.seed = cfg.seed;
  sp.nsym = cfg.nsym;
  auto sim = std::make_unique<ExchangeSim>(*md_ring, *wire_ring, *exec_ring, logger.queue(0), sp);
  auto feed = std::make_unique<FeedHandler>(*md_ring, *book_ring, logger.queue(1), mid0, cfg.nsym);
  auto strat = std::make_unique<ImbalanceTaker>(*book_ring, *exec_ring, *order_ring,
                                                logger.queue(2), ImbalanceTaker::Params{});
  OrderGateway::Risk risk;
  risk.px_min = mid0 / 2;
  risk.px_max = mid0 * 2;
  auto gw = std::make_unique<OrderGateway>(*order_ring, *wire_ring, logger.queue(3), risk);

  hft::RunControl rc;
  const auto pinboost = [&cfg](unsigned core) {
    if (cfg.pin) hft::pin_current_thread(core);
    if (cfg.rt) hft::boost_current_thread_priority();
  };

  std::jthread t_sim([&] { pinboost(cfg.cores[0]); sim->run(rc); });
  std::jthread t_feed([&] { pinboost(cfg.cores[1]); feed->run(rc); });
  std::jthread t_strat([&] { pinboost(cfg.cores[2]); strat->run(rc); });
  std::jthread t_gw([&] { pinboost(cfg.cores[3]); gw->run(rc); });

  const auto sleep_s = [](double s) {
    std::this_thread::sleep_for(std::chrono::duration<double>(s));
  };
  sleep_s(cfg.warmup);
  rc.phase.store(hft::kMeasure);
  sleep_s(cfg.seconds);
  rc.phase.store(hft::kStop);

  t_sim.join();
  t_feed.join();
  t_strat.join();
  t_gw.join();

  std::printf("hop (all values ns)                          count      min      p50      p90      p99    p99.9        max\n");
  std::printf("--------------------------------------------------------------------------------------------------------\n");
  row("wire->book applied      (t1-t0)", feed->hist());
  row("book->decision made     (t2-t1)", strat->hist());
  row("decision->gateway wire  (t3-t2)", gw->hist());
  row("TICK-TO-TRADE wire-wire (t4-t0)", sim->tick_to_trade_hist());

  std::printf("\ncounters (measured window)\n");
  std::printf("  md msgs sent %s (drops %s) | book msgs %s | tob updates %s (drops %s)\n",
              comma(sim->md_sent()).c_str(), comma(sim->md_drops()).c_str(),
              comma(feed->msgs()).c_str(), comma(feed->updates()).c_str(),
              comma(feed->dropped_updates()).c_str());
  std::printf("  strategy events %s | orders sent %s (ring drops %s)\n",
              comma(strat->events()).c_str(), comma(strat->sent()).c_str(),
              comma(strat->order_drops()).c_str());
  std::printf("  gateway passed %s | rejected %s | throttled %s | ring drops %s\n",
              comma(gw->sent()).c_str(), comma(gw->rejects()).c_str(),
              comma(gw->throttled()).c_str(), comma(gw->drops()).c_str());
  std::printf("  exchange orders %s | fills %s | kills %s | exec-ring drops %s\n",
              comma(sim->orders_seen()).c_str(), comma(sim->fills()).c_str(),
              comma(sim->kills()).c_str(), comma(sim->exec_drops()).c_str());
  std::printf("  strategy position %lld shares | fills seen %s / kills %s\n",
              static_cast<long long>(strat->position()), comma(strat->fills()).c_str(),
              comma(strat->kills()).c_str());
  std::printf("  async-log drops %s (log: %s)\n", comma(logger.total_drops()).c_str(),
              cfg.logpath);
  return 0;
}
