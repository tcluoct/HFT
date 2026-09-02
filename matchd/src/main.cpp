// matchd harness: drives the matching core with a synthetic order flow,
// measures per-operation latency by class, proves determinism by journaling
// the inbound stream and replaying it through a fresh engine, and reports.
//
// The engine itself is single-threaded; the harness pins that one thread and
// stamps every process() call with hft::rdtscp pairs.

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/tsc_clock.hpp"
#include "me/core/matching_engine.hpp"
#include "me/journal/journal.hpp"

namespace {

using namespace me;

struct Args {
  std::uint64_t ops = 5'000'000;
  std::uint64_t warmup = 500'000;
  std::uint64_t seed = 42;
  unsigned nsym = 1;
  unsigned core = 2;
  bool pin = true;
  bool rt = false;
  bool stp = true;
  const char* journal_out = nullptr;
  const char* replay_in = nullptr;
};

bool parse_u64(const char* s, std::uint64_t& v) {
  const auto r = std::from_chars(s, s + std::strlen(s), v);
  return r.ec == std::errc{} && *r.ptr == '\0';
}

void usage() {
  std::printf(
      "matchd [options]\n"
      "  --ops N        measured operations            (default 5000000)\n"
      "  --warmup N     warmup operations              (default 500000)\n"
      "  --seed N       flow RNG seed                  (default 42)\n"
      "  --symbols N    books to run, 1..4             (default 1)\n"
      "  --core N       core to pin the engine thread  (default 2)\n"
      "  --no-pin / --rt / --no-stp\n"
      "  --journal P    save the inbound journal to file P\n"
      "  --replay P     replay a journal file and print its digest\n");
}

bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const char* s = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    std::uint64_t n{};
    if (!std::strcmp(s, "--ops")) {
      const char* v = next();
      if (!v || !parse_u64(v, a.ops)) return false;
    } else if (!std::strcmp(s, "--warmup")) {
      const char* v = next();
      if (!v || !parse_u64(v, a.warmup)) return false;
    } else if (!std::strcmp(s, "--seed")) {
      const char* v = next();
      if (!v || !parse_u64(v, a.seed)) return false;
    } else if (!std::strcmp(s, "--symbols")) {
      const char* v = next();
      if (!v || !parse_u64(v, n) || n < 1 || n > 4) return false;
      a.nsym = static_cast<unsigned>(n);
    } else if (!std::strcmp(s, "--core")) {
      const char* v = next();
      if (!v || !parse_u64(v, n)) return false;
      a.core = static_cast<unsigned>(n);
    } else if (!std::strcmp(s, "--no-pin")) {
      a.pin = false;
    } else if (!std::strcmp(s, "--rt")) {
      a.rt = true;
    } else if (!std::strcmp(s, "--no-stp")) {
      a.stp = false;
    } else if (!std::strcmp(s, "--journal")) {
      a.journal_out = next();
      if (!a.journal_out) return false;
    } else if (!std::strcmp(s, "--replay")) {
      a.replay_in = next();
      if (!a.replay_in) return false;
    } else {
      usage();
      return false;
    }
  }
  return true;
}

// Open-order list the generator draws cancel/replace targets from.
// Picks are optimistic: an id is removed from the list when picked, even if
// the order already died to a fill — the engine rejects that stale handle,
// which is exactly what cancels racing fills look like in production. The
// list keeps the resting population in equilibrium instead of growing until
// the pool is exhausted.
struct OpenList {
  std::vector<OrderId> ids;
  OpenList() { ids.reserve(1u << 20); }
  void push(OrderId id) noexcept { ids.push_back(id); }
  [[nodiscard]] OrderId take(std::uint64_t r) noexcept {  // random pick + swap-pop
    const std::size_t k =
        static_cast<std::size_t>(((r & 0xFFFFFFFFull) * ids.size()) >> 32);
    const OrderId id = ids[k];
    ids[k] = ids.back();
    ids.pop_back();
    return id;
  }
};

// Counts events and folds every field of every event into an order-sensitive
// digest — the fingerprint that the replay must reproduce exactly.
struct HarnessSink {
  OpenList* open = nullptr;
  Digest dig;
  std::uint64_t accepts = 0, execs = 0, md_events = 0, rejects = 0;

  void on_accepted(ParticipantId p, Token t, OrderId id, std::uint8_t side, Price px,
                   Qty q) noexcept {
    ++accepts;
    if (open) open->push(id);
    dig.mix(1); dig.mix(p); dig.mix(t); dig.mix(id); dig.mix(side);
    dig.mix(static_cast<std::uint64_t>(px)); dig.mix(q);
  }
  void on_canceled(ParticipantId p, Token t, OrderId id, Qty leaves, CancelReason r) noexcept {
    dig.mix(2); dig.mix(p); dig.mix(t); dig.mix(id); dig.mix(leaves);
    dig.mix(static_cast<std::uint64_t>(r));
  }
  void on_replaced(ParticipantId p, Token t, OrderId oid, OrderId nid, Price px,
                   Qty q) noexcept {
    if (open) open->push(nid);  // the generator took oid out when it picked it
    dig.mix(3); dig.mix(p); dig.mix(t); dig.mix(oid); dig.mix(nid);
    dig.mix(static_cast<std::uint64_t>(px)); dig.mix(q);
  }
  void on_rejected(ParticipantId p, Token t, RejectReason r) noexcept {
    ++rejects;
    dig.mix(4); dig.mix(p); dig.mix(t); dig.mix(static_cast<std::uint64_t>(r));
  }
  void on_exec(ParticipantId p, Token t, OrderId id, Qty q, Price px) noexcept {
    ++execs;
    dig.mix(5); dig.mix(p); dig.mix(t); dig.mix(id); dig.mix(q);
    dig.mix(static_cast<std::uint64_t>(px));
  }
  void on_md_add(OrderId id, std::uint8_t side, Price px, Qty q) noexcept {
    ++md_events;
    dig.mix(6); dig.mix(id); dig.mix(side); dig.mix(static_cast<std::uint64_t>(px)); dig.mix(q);
  }
  void on_md_exec(OrderId id, Qty q, Price px) noexcept {
    ++md_events;
    dig.mix(7); dig.mix(id); dig.mix(q); dig.mix(static_cast<std::uint64_t>(px));
  }
  void on_md_reduce(OrderId id, Qty q) noexcept {
    ++md_events;
    dig.mix(8); dig.mix(id); dig.mix(q);
  }
  void on_md_delete(OrderId id) noexcept {
    ++md_events;
    dig.mix(9); dig.mix(id);
  }
};

class FlowGen {
 public:
  FlowGen(std::uint64_t seed, unsigned nsym, Price mid0, OpenList& open)
      : s_(seed | 1), nsym_(nsym), mid0_(mid0), open_(open) {}

  template <class Engine>
  InMsg next(const Engine& eng) noexcept {
    const std::uint64_t r = rng();
    const std::uint32_t roll = bounded(r >> 8, 100);
    const SymbolId sym = static_cast<SymbolId>(bounded(r >> 40, nsym_));
    const ParticipantId part = static_cast<ParticipantId>(1 + bounded(r >> 16, 8));
    const std::uint8_t side = static_cast<std::uint8_t>((r >> 50) & 1);
    const bool have_open = !open_.ids.empty();
    const bool crowded = open_.ids.size() > 100'000;  // equilibrium guard

    InMsg m{};
    // 35% cancel (forced when the resting population grows too large)
    if (have_open && ((roll >= 40 && roll < 75) || crowded)) {
      m.cancel.type = kMsgCancel;
      m.cancel.participant = part;
      m.cancel.order_id = open_.take(rng());
      return m;
    }
    if (have_open && roll >= 75 && roll < 83) {  // 8%: replace
      m.replace.type = kMsgReplace;
      m.replace.participant = part;
      m.replace.order_id = open_.take(rng());
      m.replace.new_price =
          clamp_px(same_side_touch(eng, sym, side) + static_cast<Price>((r >> 52) & 7) - 3);
      m.replace.new_qty = 100u * (1u + static_cast<Qty>((r >> 56) & 7));
      return m;
    }

    EnterOrder& e = m.enter;
    e.type = kMsgEnter;
    e.participant = part;
    e.symbol = sym;
    e.side = side;
    e.token = ++token_seq_;
    if (roll >= 98 && have_open) {  // 2%: FOK crossing the touch
      const Price opp = opposite_touch(eng, sym, side);
      e.price = clamp_px(side == kBuy ? opp + 1 : opp - 1);
      e.qty = 100u * (5u + static_cast<Qty>((r >> 56) & 15));
      e.tif = kTifFok;
    } else if (roll >= 83 && roll < 98) {  // 15%: aggressive, cross the touch
      const Price opp = opposite_touch(eng, sym, side);
      const Price slack = static_cast<Price>((r >> 52) & 1);
      e.price = clamp_px(side == kBuy ? opp + slack : opp - slack);
      e.qty = 100u * (1u + static_cast<Qty>((r >> 56) & 3));
      e.tif = ((r >> 58) & 1) ? kTifIoc : kTifDay;
    } else {  // 40%: passive, join or sit behind the same-side touch
      const Price touch = same_side_touch(eng, sym, side);
      const Price off = static_cast<Price>((r >> 52) & 7);
      e.price = clamp_px(side == kBuy ? touch - off : touch + off);
      e.qty = 100u * (1u + static_cast<Qty>((r >> 56) & 7));
      e.tif = kTifDay;
    }
    return m;
  }

 private:
  HFT_FORCE_INLINE std::uint64_t rng() noexcept {  // splitmix64
    std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  static HFT_FORCE_INLINE std::uint32_t bounded(std::uint64_t x, std::uint32_t n) noexcept {
    return static_cast<std::uint32_t>(((x & 0xFFFFFFFFull) * n) >> 32);
  }
  template <class Engine>
  Price same_side_touch(const Engine& eng, SymbolId sym, std::uint8_t side) const noexcept {
    if (side == kBuy) return eng.has_bid(sym) ? eng.bid_px(sym) : mid0_ - 1;
    return eng.has_ask(sym) ? eng.ask_px(sym) : mid0_ + 1;
  }
  template <class Engine>
  Price opposite_touch(const Engine& eng, SymbolId sym, std::uint8_t side) const noexcept {
    if (side == kBuy) return eng.has_ask(sym) ? eng.ask_px(sym) : mid0_ + 1;
    return eng.has_bid(sym) ? eng.bid_px(sym) : mid0_ - 1;
  }
  [[nodiscard]] Price clamp_px(Price px) const noexcept {
    const Price lo = mid0_ - 2000, hi = mid0_ + 2000;
    return px < lo ? lo : (px > hi ? hi : px);
  }

  std::uint64_t s_;
  unsigned nsym_;
  Price mid0_;
  OpenList& open_;
  Token token_seq_ = 0;
};

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
    std::printf("  %-28s %14s\n", name, "no samples");
    return;
  }
  std::printf("  %-28s %14s %8llu %8llu %8llu %8llu %8llu %10llu\n", name,
              comma(h.count()).c_str(), ns(h.min()), ns(h.percentile(0.50)),
              ns(h.percentile(0.90)), ns(h.percentile(0.99)), ns(h.percentile(0.999)),
              ns(h.max()));
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return 1;

  hft::TscClock::calibrate();
  if (a.pin) hft::pin_current_thread(a.core);
  if (a.rt) hft::boost_current_thread_priority();

  MatchingEngine<HarnessSink>::Params params;
  params.nsym = a.nsym;
  params.self_match_prevention = a.stp;

  if (a.replay_in) {
    const auto msgs = Journal::load(a.replay_in);
    if (msgs.empty()) {
      std::printf("failed to load journal: %s\n", a.replay_in);
      return 1;
    }
    HarnessSink sink;
    const auto eng = std::make_unique<MatchingEngine<HarnessSink>>(sink, params);
    for (const auto& m : msgs) eng->process(m);
    std::printf("replayed %s msgs | digest %016llx\n", comma(msgs.size()).c_str(),
                static_cast<unsigned long long>(sink.dig.h));
    return 0;
  }

  std::printf("matchd | tsc %.3f GHz | %u symbol(s) | stp %s | pin core %u%s | seed %llu\n\n",
              hft::TscClock::cycles_per_ns(), a.nsym, a.stp ? "on" : "off", a.core,
              a.rt ? " (rt)" : "", static_cast<unsigned long long>(a.seed));

  OpenList open;
  HarnessSink sink;
  sink.open = &open;
  const auto eng_ptr = std::make_unique<MatchingEngine<HarnessSink>>(sink, params);
  MatchingEngine<HarnessSink>& eng = *eng_ptr;
  FlowGen gen(a.seed, a.nsym, params.mid0, open);
  Journal journal(a.warmup + a.ops);
  hft::LatencyHistogram h_pass, h_aggr, h_cxl, h_rep, h_rej;

  const auto run = [&](std::uint64_t n, bool measure) {
    for (std::uint64_t i = 0; i < n; ++i) {
      const InMsg m = gen.next(eng);
      journal.append(m);
      const std::uint64_t execs_before = sink.execs;
      const std::uint64_t rejects_before = sink.rejects;
      const std::uint64_t t = hft::rdtscp();
      eng.process(m);
      const std::uint64_t dt = hft::rdtscp() - t;
      if (measure) {
        if (sink.rejects != rejects_before) {
          h_rej.record(dt);  // stale-handle cancels/replaces, validation fails
        } else {
          switch (m.type) {
            case kMsgEnter:
              (sink.execs != execs_before ? h_aggr : h_pass).record(dt);
              break;
            case kMsgCancel:
              h_cxl.record(dt);
              break;
            case kMsgReplace:
              h_rep.record(dt);
              break;
            default:
              break;
          }
        }
      }
    }
  };

  run(a.warmup, false);
  const auto w0 = std::chrono::steady_clock::now();
  run(a.ops, true);
  const auto w1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(w1 - w0).count();

  std::printf("op latency (ns, incl ~20ns timer)     count      min      p50      p90      p99    p99.9        max\n");
  std::printf("----------------------------------------------------------------------------------------------------\n");
  row("enter: passive (no trade)", h_pass);
  row("enter: aggressive (trades)", h_aggr);
  row("cancel", h_cxl);
  row("replace", h_rep);
  row("rejected (stale handle etc)", h_rej);

  std::printf("\nthroughput: %s ops in %.3f s = %s ops/sec\n", comma(a.ops).c_str(), secs,
              comma(static_cast<std::uint64_t>(static_cast<double>(a.ops) / secs)).c_str());

  std::printf("\ncounters (incl. warmup)\n");
  std::printf("  accepts %s | trades %s (qty %s) | execs %s | md events %s\n",
              comma(eng.accepts()).c_str(), comma(eng.trades()).c_str(),
              comma(eng.traded_qty()).c_str(), comma(sink.execs).c_str(),
              comma(sink.md_events).c_str());
  std::printf("  cancels: user %s | ioc %s | fok %s | stp %s\n",
              comma(eng.user_cancels()).c_str(), comma(eng.ioc_kills()).c_str(),
              comma(eng.fok_kills()).c_str(), comma(eng.stp_cancels()).c_str());
  std::printf("  replaces %s | rejects %s (stale-handle cancels/replaces + validation)\n",
              comma(eng.replaces()).c_str(), comma(eng.rejects()).c_str());
  std::printf("  pool: live %s / high-water %s / capacity %s\n",
              comma(eng.pool().live_count()).c_str(), comma(eng.pool().high_water()).c_str(),
              comma(params.pool_capacity).c_str());

  // Determinism proof: replay the journal through a fresh engine and compare
  // outbound digests.
  HarnessSink sink2;
  const auto eng2 = std::make_unique<MatchingEngine<HarnessSink>>(sink2, params);
  for (const auto& m : journal.entries()) eng2->process(m);
  const bool ok = sink2.dig.h == sink.dig.h;
  std::printf("\ndeterminism replay: %s (digest %016llx %s %016llx over %s msgs)\n",
              ok ? "PASS" : "FAIL", static_cast<unsigned long long>(sink.dig.h),
              ok ? "==" : "!=", static_cast<unsigned long long>(sink2.dig.h),
              comma(journal.entries().size()).c_str());

  if (a.journal_out) {
    std::printf("journal: %s (%s bytes)\n",
                journal.save(a.journal_out) ? a.journal_out : "SAVE FAILED",
                comma(journal.entries().size() * sizeof(InMsg)).c_str());
  }
  return ok ? 0 : 1;
}
