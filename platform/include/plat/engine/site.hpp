#pragma once
// plat/engine/site.hpp — the site engine (§5.3): one venue's complete
// trading stack composed on the canonical stream.
//
//   feed adapter ─→ [Source publish] → books → strategies
//                                             │ ctx.place
//   counter ←── gateway ← riskgate ← [Derived publish]┘
//      └─ exec reports ─→ [Source publish] → OMS / PnL / strategies
//
// Compile-time composition (tick2trade's discipline, §9.1 inline mode):
// Site is a template over the concrete gateway type and the strategy pack —
// there is no virtual call, no std::function, and no map lookup between a
// snapshot entering and an order leaving. Books, risk mirrors, and marks are
// dense-id flat arrays (InstrumentId indexes the per-day table); dispatch is
// one switch on the envelope schema, and strategies are a std::tuple of
// per-type BANKS (a vector of configured instances per strategy type) walked
// with a fold — every handler is statically resolved and inlinable; the only
// runtime dimension is how many instances of each type the config asked for.
// A bank may be empty (site doesn't run that type) or hold several instances
// of one type with different ids/params/universes.
//
// Two construction modes with identical deterministic wiring:
//   Live   — external adapters attached; the live driver publishes every
//            boundary crossing as a Source envelope; everything journaled.
//   Replay — deterministic components only; Source envelopes are injected
//            from a journal and every Derived envelope is regenerated. The
//            stream hash proves the regeneration byte-identical (§11.1).
//
// now() inside components is always the timestamp of the envelope being
// dispatched — never a wall clock — which is the entire determinism story.
// The wall clock exists only in the live driver, where it stamps Source
// envelopes as they cross the boundary.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "hft/common/platform.hpp"
#include "plat/bus/journal.hpp"
#include "plat/bus/stream.hpp"
#include "plat/comp/compliance.hpp"
#include "plat/core/log.hpp"
#include "plat/core/phase.hpp"
#include "plat/core/types.hpp"
#include "plat/gw/gateway.hpp"
#include "plat/md/fut_book.hpp"
#include "plat/md/stock_book.hpp"
#include "plat/oms/oms.hpp"
#include "plat/pnl/pnl.hpp"
#include "plat/ref/fees.hpp"
#include "plat/ref/instrument.hpp"
#include "plat/risk/riskgate.hpp"
#include "plat/strat/framework.hpp"
#include "plat/tel/telemetry.hpp"

namespace plat {

struct SiteConfig {
  SiteId id = 0;
  std::string name = "site";
  Venue venue = Venue::SHFE;
  std::vector<InstrumentId> hosted;
  std::uint32_t day = 0;            // yyyymmdd trading day (from counter login)
  std::string journal_path;         // empty → no journal (tests)
  int journal_core = -1;            // pin the journal writer thread (colo)
  RiskLimits limits{};
  FeeSchedule fees{};
  std::uint16_t epoch = 1;          // order-id epoch (§11.2)
};

// Timer scheduling crosses to the driver's wall-clock heap through a plain
// function pointer: it is set once at wiring time and fires at strategy-
// action rate (rare), and a pointer indirection avoids the circular template
// dependency Site ↔ driver. Firings come back as Source TimerEvt envelopes.
using TimerSinkFn = void (*)(void* ctx, Nanos when, std::uint64_t token, StrategyId strategy);

template <OrderGateway Gateway, AnyStrategy... Strats>
class Site {
 public:
  enum class Mode : std::uint8_t { Live, Replay };

  // The bank tuple is what StratPack::banks (engine/pack.hpp) builds: one
  // instance vector per strategy type, in pack order.
  Site(const InstrumentTable& table, SiteConfig cfg, Mode mode,
       std::tuple<std::vector<Strats>...> banks)
      : table_(&table), cfg_(std::move(cfg)), mode_(mode),
        stream_(cfg_.id), pnl_(table, cfg_.fees),
        risk_(table, oms_, pnl_, cfg_.limits),
        fut_books_(table.size()), has_fut_(table.size(), 0),
        stock_books_(table.size()),
        strats_(std::move(banks)) {
    build_books();
    wire_telemetry();
    if (mode_ == Mode::Live && !cfg_.journal_path.empty()) {
      journal_open_ = journal_.open(cfg_.journal_path, cfg_.day, cfg_.id, cfg_.journal_core);
      if (journal_open_) stream_.set_journal(&journal_);
    }
  }

  // Positional convenience (tests, benches): one initializer per bank.
  Site(const InstrumentTable& table, SiteConfig cfg, Mode mode, std::vector<Strats>... banks)
      : Site(table, std::move(cfg), mode,
             std::tuple<std::vector<Strats>...>(std::move(banks)...)) {}

  // ------------------------------------------------------------ composition

  // Live-side adapters, attached by the driver before the session arms.
  void set_gateway(Gateway* gw) noexcept { gw_ = gw; }
  void set_timer_sink(TimerSinkFn fn, void* ctx) noexcept {
    timer_fn_ = fn;
    timer_ctx_ = ctx;
  }

  // Crash recovery (§11.2): while the crashed journal's prefix replays into
  // this LIVE site, the deterministic components run in full — but nothing
  // may leave the building again: risk-passed orders were already sent by
  // the dead process, and journaled TimerEvts already carry the firings.
  void set_recovering(bool r) noexcept { recovering_ = r; }
  [[nodiscard]] bool recovering() const noexcept { return recovering_; }

  [[nodiscard]] PnlLedger& pnl() noexcept { return pnl_; }
  [[nodiscard]] const PnlLedger& pnl() const noexcept { return pnl_; }
  [[nodiscard]] Oms& oms() noexcept { return oms_; }
  [[nodiscard]] RiskGate& risk() noexcept { return risk_; }
  [[nodiscard]] ComplianceEngine& compliance() noexcept { return comp_; }
  [[nodiscard]] Telemetry& tel() noexcept { return tel_; }
  [[nodiscard]] StreamCore& stream() noexcept { return stream_; }
  [[nodiscard]] const SiteConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] const InstrumentTable& table() const noexcept { return *table_; }
  [[nodiscard]] SessState session_state() const noexcept { return sess_state_; }
  // Most recent risk denial (new-order or cancel) — a plain struct copy made
  // in deny(), no I/O, so it costs nothing on the hot path. `status` (cold,
  // operator-cadence) is what turns it into a log line; a default-constructed
  // RiskDeny (check == RiskCheck{}, never a real deny reason) means "none yet".
  [[nodiscard]] const RiskDeny& last_deny() const noexcept { return last_deny_; }
  // cl_id of the most recent operator `order` command (risk-passed or not) —
  // lets the console echo it back so the operator has an id to `cancel`.
  [[nodiscard]] OrderId last_manual_order_cl_id() const noexcept { return last_manual_cl_id_; }
  // Bank I: every configured instance of the I-th strategy type, in config
  // order (possibly empty, possibly several with different ids/params).
  template <std::size_t I>
  [[nodiscard]] auto& strategies() noexcept { return std::get<I>(strats_); }

  // ---------------------------------------------------------- source ingress

  // The one entry point for events that cross the deterministic boundary
  // (venue feed, gateway reports, phase/timer ticks, operator commands).
  // Journaled as Source; replay re-injects them via inject().
  template <PlatMsg T>
  HFT_FORCE_INLINE void source(const T& msg, Nanos ts) {
    dispatch(stream_.seal(ts, msg, Origin::Source));
  }

  // ----------------------------------------------------- session lifecycle

  // Pre-flight checklist (§11.3): every red item blocks the session.
  struct PreFlight {
    bool instruments = false, journal = false, session_up = false, books = false,
         limits = false, kill_disarmed = false;
    [[nodiscard]] bool ok() const {
      return instruments && journal && session_up && books && limits && kill_disarmed;
    }
  };

  [[nodiscard]] PreFlight preflight() const {
    PreFlight p;
    p.instruments = table_->size() > 0 && !cfg_.hosted.empty();
    p.journal = cfg_.journal_path.empty() || journal_open_;
    p.session_up = gw_ && gw_->state() == SessState::Up;
    p.books = books_attached_;
    p.limits = cfg_.limits.max_order_qty > 0 && cfg_.limits.max_actions_per_sec > 0;
    p.kill_disarmed = !risk_.kill().any_armed();
    return p;
  }

  // Arm the session (driver calls this before the open): run pre-flight; on
  // green, start strategies; on red, halt the site through the same
  // mechanism an operator would use (§10.3) — riskgate then denies
  // everything until a versioned Disarm.
  bool arm_session(Nanos now) {
    preflight_result_ = preflight();
    go_ = preflight_result_.ok();
    ++tel_.counter(go_ ? "preflight.pass" : "preflight.fail");
    if (go_) {
      cur_ts_ = now;
      std::apply([&](auto&... bank) { (start_bank(bank), ...); }, strats_);
      dispatching_strategy_ = 0;
      dispatching_account_ = 0;
    } else {
      command_kill(KillScope::Site, KillAction::Halt, 0, now);
    }
    return go_;
  }

  // Operator command entry point (fleet control, §14): kill-switch commands
  // are external world — Source origin, journaled, replayed. Commands carry a
  // monotonic version so a stale/replayed command can never override a newer
  // one (§10.3, review R1).
  void command_kill(KillScope scope, KillAction action, std::uint16_t target, Nanos now) {
    KillSwitchCmd k{};
    k.ts = now;
    k.cmd_seq = risk_.kill().last_cmd_seq() + 1;
    k.scope = scope;
    k.action = action;
    k.target = target;
    dispatch(stream_.seal(k.ts, k, Origin::Source));
  }

  struct EodResult {
    std::size_t order_breaks = 0;
    bool fills_ok = false;
    Qty venue_buy = 0, venue_sell = 0;
    bool ran = false;
  };
  [[nodiscard]] const EodResult& eod() const noexcept { return eod_; }
  [[nodiscard]] const PreFlight& preflight_result() const noexcept { return preflight_result_; }
  [[nodiscard]] bool armed() const noexcept { return go_; }

  // End-of-day reconciliation (§11.3) against the counter's own view: the
  // driver queries open orders and fill aggregates from the counter (CTP
  // ReqQryOrder / ReqQryTrade) and hands them in; the venue is the source of
  // truth and any break blocks the next session's re-enable.
  void run_eod(const std::vector<VenueOpenOrder>& venue_open, Qty venue_buy, Qty venue_sell) {
    eod_.order_breaks = oms_.reconcile(venue_open).size();
    const auto fills = pnl_.reconcile_fills(venue_buy, venue_sell);
    eod_.fills_ok = fills.ok;
    eod_.venue_buy = venue_buy;
    eod_.venue_sell = venue_sell;
    eod_.ran = true;
    if (journal_open_) journal_.finish();  // drain the ring, seal, join
  }

  // False once the journal thread was lapped: the audit record has a hole —
  // the driver halts trading on this (fail closed, §7.2).
  [[nodiscard]] bool journal_healthy() const noexcept {
    return !journal_open_ || journal_.healthy();
  }

  // --------------------------------------------------------------- recovery

  // Inject a journaled envelope (Replay mode): Source entries drive the
  // deterministic set; Derived entries are skipped — they are regenerated by
  // dispatch and verified via the stream hash.
  void inject(const Envelope& e) {
    if (e.origin != Origin::Source) return;
    stream_.absorb(e);
    dispatch(e);
  }

  // Deterministic state fingerprint and snapshot serialization (§11.1/§16);
  // definitions in site_state.hpp.
  [[nodiscard]] std::uint64_t state_hash() const;
  void save_state(std::vector<std::uint8_t>& out) const;
  void load_state(const std::vector<std::uint8_t>& in);

  // --------------------------------------------------------- strategy ctx
  // Site *is* the strategy context: every read/action below is a direct,
  // statically-dispatched member call at the strategy's call site.

  OrderId place(InstrumentId inst, Side side, Offset offset, Price px, Qty qty, Tif tif,
                Nanos trigger_ts) {
    NewOrderReq r{};
    r.cl_id = make_order_id(cfg_.epoch, ++next_order_seq_);
    r.ts = cur_ts_;
    r.trigger_ts = trigger_ts;
    r.px = px;
    r.qty = qty;
    r.inst = inst;
    r.strategy = dispatching_strategy_;
    r.account = dispatching_account_;
    r.side = side;
    r.offset = offset;
    r.tif = tif;
    dispatch(stream_.seal(cur_ts_, r, Origin::Derived));
    return r.cl_id;
  }

  void cancel(OrderId id) {
    const OmsOrder* o = oms_.find(id);
    if (!o) return;
    CancelReq c{};
    c.cl_id = id;
    c.ts = cur_ts_;
    c.inst = o->inst;
    c.strategy = o->strategy;
    c.account = o->account;
    dispatch(stream_.seal(cur_ts_, c, Origin::Derived));
  }

  void set_timer(Nanos when, std::uint64_t token) {
    // Replay/recovery: TimerEvt firings come from the journal, not the heap.
    if (mode_ != Mode::Live || recovering_) return;
    if (timer_fn_) timer_fn_(timer_ctx_, when, token, dispatching_strategy_);
  }

  void send_intent(const HedgeIntent& h) {
    HedgeIntent out = h;
    out.origin_site = cfg_.id;
    out.ts = cur_ts_;
    dispatch(stream_.seal(cur_ts_, out, Origin::Derived));
  }
  void send_basis(const BasisSummary& b) {
    BasisSummary out = b;
    out.origin_site = cfg_.id;
    out.ts = cur_ts_;
    dispatch(stream_.seal(cur_ts_, out, Origin::Derived));
  }
  void send_hedge_done(const HedgeDone& d) {
    HedgeDone out = d;
    out.origin_site = cfg_.id;
    out.ts = cur_ts_;
    dispatch(stream_.seal(cur_ts_, out, Origin::Derived));
  }

  [[nodiscard]] Nanos now() const noexcept { return cur_ts_; }
  [[nodiscard]] const Instrument& instrument(InstrumentId i) const noexcept {
    return table_->at(i);
  }
  [[nodiscard]] const Position* position(InstrumentId i) const {
    return pnl_.find(dispatching_account_, dispatching_strategy_, i);
  }
  [[nodiscard]] const StockBook* stock_book(InstrumentId i) const noexcept {
    return stock_books_[i].get();
  }
  [[nodiscard]] const FutBook* fut_book(InstrumentId i) const noexcept {
    return has_fut_[i] ? &fut_books_[i] : nullptr;
  }

 private:
  // Wiring, the dispatch chain, and strategy routing are defined in
  // site_dispatch.hpp; snapshot serialization in site_state.hpp. Both are
  // included at the bottom of this header — never include them directly.

  void build_books();
  void wire_telemetry();

  void dispatch(const Envelope& e);
  HFT_FORCE_INLINE void on_new_order(const NewOrderReq& r);
  HFT_FORCE_INLINE void on_cancel_req(const CancelReq& c);
  HFT_FORCE_INLINE void on_exec_report(const Envelope& e);
  HFT_FORCE_INLINE void route(const Envelope& e);
  HFT_FORCE_INLINE void deliver(const Envelope& e);
  template <typename Bank>
  HFT_FORCE_INLINE void deliver_bank(Bank& bank, const Envelope& e);
  template <typename S>
  HFT_FORCE_INLINE void deliver_one(S& s, const Envelope& e);
  template <typename Bank>
  void start_bank(Bank& bank);
  template <typename S>
  void start_one(S& s);
  void publish_top(InstrumentId inst, Nanos ts);
  [[nodiscard]] std::vector<const OmsOrder*> open_orders_snapshot() const;
  static bool in_kill_scope(const KillSwitchCmd& k, const OmsOrder& o);
  void deny(OrderId id, RiskCheck check, const NewOrderReq& r, bool is_cancel);

  // Snapshot helpers (site_state.hpp).
  template <typename T>
  static void append_pod(std::vector<std::uint8_t>& out, const T& v);
  template <typename T>
  static std::size_t read_pod(const std::vector<std::uint8_t>& in, std::size_t off, T& v);
  template <typename F>
  static void save_blob(std::vector<std::uint8_t>& out, F&& fill);
  template <typename F>
  static std::size_t load_blob(const std::vector<std::uint8_t>& in, std::size_t off, F&& use);

  const InstrumentTable* table_;
  SiteConfig cfg_;
  Mode mode_;
  StreamCore stream_;
  AsyncJournal journal_;
  bool journal_open_ = false;

  Oms oms_;
  PnlLedger pnl_;
  RiskGate risk_;
  ComplianceEngine comp_;
  Telemetry tel_;

  // Dense-id books: futures by value (one indexed load per snapshot),
  // stocks behind pointers (constructed only for hosted stock instruments).
  std::vector<FutBook> fut_books_;
  std::vector<std::uint8_t> has_fut_;
  std::vector<std::unique_ptr<StockBook>> stock_books_;

  Gateway* gw_ = nullptr;           // owned by main
  TimerSinkFn timer_fn_ = nullptr;  // owned by the driver
  void* timer_ctx_ = nullptr;
  std::tuple<std::vector<Strats>...> strats_;  // per-type instance banks

  Nanos cur_ts_ = 0;
  std::uint64_t next_order_seq_ = 0;
  StrategyId dispatching_strategy_ = 0;
  AccountId dispatching_account_ = 0;
  SessState sess_state_ = SessState::Down;
  bool recovering_ = false;

  // Fixed-capacity re-entrancy ring (power of two): drained before any event
  // returns, so it is never part of a snapshot.
  static constexpr std::size_t kStratQueueCap = 64;
  Envelope strat_q_[kStratQueueCap];
  std::uint64_t q_head_ = 0;
  std::size_t q_count_ = 0;
  int strat_depth_ = 0;
  std::uint64_t q_overflow_ = 0;

  // Preresolved telemetry slots (§14): registered at wiring, raw increments
  // on the hot path.
  std::uint64_t* c_envelopes_ = nullptr;
  std::uint64_t* c_fills_ = nullptr;
  std::uint64_t* c_feed_gap_ = nullptr;
  std::uint64_t* c_risk_pass_ = nullptr;
  std::uint64_t* c_risk_deny_ = nullptr;
  std::uint64_t* c_deny_check_[static_cast<int>(kRiskCheckLast) + 1] = {};
  LatencyHisto* h_t2t_ = nullptr;
  LatencyHisto* h_rtt_ = nullptr;
  RiskDeny last_deny_{};
  OrderId last_manual_cl_id_ = kNoOrder;
  bool books_attached_ = false;
  bool go_ = false;
  PreFlight preflight_result_{};
  EodResult eod_{};
};

}  // namespace plat

// Out-of-line member definitions (kept separate for file-size discipline).
#include "plat/engine/site_dispatch.hpp"  // IWYU pragma: keep
#include "plat/engine/site_state.hpp"     // IWYU pragma: keep
