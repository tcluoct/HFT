#pragma once
// plat/codec/msgs.hpp — the platform IDL, hand-rolled (wirecodec graduation,
// §7.4). Every message is a fixed-layout trivially-copyable POD with a stable
// schema id; producers write it once, consumers read fields in place — no
// parse step. Venue-native formats are translated into these at the feed
// handler / gateway boundary and never appear inland.
//
// Layout discipline: explicit-width members, 8-byte members first, static
// asserts on size and triviality. Growing a message means a new schema id.

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "plat/core/phase.hpp"
#include "plat/core/types.hpp"

namespace plat {

// Topics partition the canonical stream for consumers (§7.1).
enum class Topic : std::uint16_t {
  MarketData,  // venue events: 逐笔 orders/trades, futures snapshots
  BookTops,    // book-engine derived tops
  OrderFlow,   // strategy → riskgate → gateway requests
  Execs,       // gateway → inland execution reports
  Risk,        // riskgate denials, kill-switch state changes
  Control,     // phase changes, session events, pre-flight results
  Remote,      // cross-site: hedge intents, basis summaries (§7.3)
};

enum class Schema : std::uint16_t {
  MdOrder = 1,
  MdTrade,
  MdSnapshot,
  BookTop,
  PhaseChange,
  NewOrderReq,
  CancelReq,
  ExecReport,
  RiskDeny,
  HedgeIntent,
  BasisSummary,
  KillSwitch,
  SessionState,
  Timer,
  HedgeDone,
  FeedGap,
  ParamUpdate,
  ManualOrder,
  ManualCancel,
  PositionTransfer,
};

// ---------------------------------------------------------------- market data

enum class MdAction : std::uint8_t { Add, Cancel };

// 逐笔委托 — one order-book event from a stock venue's order-by-order feed
// (SZSE 逐笔委托 semantics: adds and cancels both appear; §8.1).
struct MdOrderEvt {
  std::uint64_t order_no;  // venue-public order number
  std::uint64_t feed_seq;  // per-venue feed sequence (A/B arbitration key)
  Nanos ts;
  Price px;
  Qty qty;
  InstrumentId inst;
  Venue venue;
  MdAction action;
  Side side;
  std::uint8_t pad_[1]{};
};
static_assert(std::is_trivially_copyable_v<MdOrderEvt> && sizeof(MdOrderEvt) == 48);

// 逐笔成交 — one trade print, carrying both resting/aggressing order numbers.
struct MdTradeEvt {
  std::uint64_t trade_no;
  std::uint64_t buy_no;   // buy-side venue order number
  std::uint64_t sell_no;  // sell-side venue order number
  std::uint64_t feed_seq;
  Nanos ts;
  Price px;
  Qty qty;
  InstrumentId inst;
  Venue venue;
  std::uint8_t pad_[3]{};
};
static_assert(std::is_trivially_copyable_v<MdTradeEvt> && sizeof(MdTradeEvt) == 64);

// Futures/options venue snapshot (250–500 ms cadence, §4.2): five levels.
inline constexpr std::size_t kSnapLevels = 5;

struct MdSnapshotEvt {
  std::uint64_t feed_seq;
  Nanos ts;
  Price bid_px[kSnapLevels];
  Qty bid_qty[kSnapLevels];
  Price ask_px[kSnapLevels];
  Qty ask_qty[kSnapLevels];
  Price last_px;
  Qty day_volume;
  Qty open_interest;
  InstrumentId inst;
  Venue venue;
  std::uint8_t pad_[3]{};
};
static_assert(std::is_trivially_copyable_v<MdSnapshotEvt> && sizeof(MdSnapshotEvt) == 208);

// Unrecovered feed gap (§8.1, review R1): a snapshot cannot repair an
// order-by-order hole, so the gap itself is a canonical Source event —
// journaled, so the fail-closed book quarantine it triggers replays
// deterministically.
struct FeedGapEvt {
  Nanos ts;
  std::uint64_t lost;  // events missing from the sequence domain
  Venue venue;
  std::uint8_t pad_[7]{};
};
static_assert(std::is_trivially_copyable_v<FeedGapEvt> && sizeof(FeedGapEvt) == 24);

// Book-engine derived top-of-book (published on change; §8.2).
struct BookTopEvt {
  Nanos ts;        // venue event time driving this top
  Price bid_px;
  Qty bid_qty;
  Price ask_px;
  Qty ask_qty;
  Price last_px;
  InstrumentId inst;
  Venue venue;
  LimitState limit_state;
  std::uint8_t pad_[1]{};
};
static_assert(std::is_trivially_copyable_v<BookTopEvt> && sizeof(BookTopEvt) == 56);

struct PhaseChangeEvt {
  Nanos ts;
  Venue venue;
  Phase phase;
  std::uint8_t pad_[6]{};
};
static_assert(std::is_trivially_copyable_v<PhaseChangeEvt> && sizeof(PhaseChangeEvt) == 16);

// ---------------------------------------------------------------- order path

struct NewOrderReq {
  OrderId cl_id;
  Nanos ts;          // request time
  Nanos trigger_ts;  // md event that caused this order — rides the whole chain
                     // so tick-to-trade is measured on every order (§12.2)
  Price px;
  Qty qty;
  InstrumentId inst;
  StrategyId strategy;
  AccountId account;
  Side side;
  Offset offset;
  Tif tif;
  std::uint8_t pad_[5]{};
};
static_assert(std::is_trivially_copyable_v<NewOrderReq> && sizeof(NewOrderReq) == 56);

struct CancelReq {
  OrderId cl_id;  // order being cancelled
  Nanos ts;
  InstrumentId inst;
  StrategyId strategy;
  AccountId account;
  std::uint8_t pad_[8]{};
};
static_assert(std::is_trivially_copyable_v<CancelReq> && sizeof(CancelReq) == 32);

enum class ExecType : std::uint8_t {
  Ack,           // accepted by venue
  Reject,        // rejected by venue/counter
  Fill,          // partial or full — leaves tells which
  Canceled,      // cancel confirmed
  CancelReject,  // cancel refused (too late / unknown / phase)
};

// Reason taxonomy normalized across counters (§9.4).
enum class ExecReason : std::uint16_t {
  None = 0,
  PriceLimit,
  Phase,
  UnknownOrder,
  SeatFlowControl,
  SessionDown,
  Other,
};

struct ExecReport {
  OrderId cl_id;
  std::uint64_t venue_no;  // venue-public order number (matches MdOrderEvt.order_no)
  Nanos ts;
  Nanos trigger_ts;
  Price px;    // fill price (Fill) or order price otherwise
  Qty qty;     // this event's fill quantity
  Qty leaves;  // remaining open
  Qty cum;     // cumulative filled
  InstrumentId inst;
  StrategyId strategy;
  AccountId account;
  ExecType type;
  Side side;
  Offset offset;
  std::uint8_t pad_[1]{};
  ExecReason reason;
  std::uint8_t pad2_[2]{};
};
static_assert(std::is_trivially_copyable_v<ExecReport> && sizeof(ExecReport) == 80);

// ---------------------------------------------------------------- risk path

// The §10.1 battery, in evaluation order.
enum class RiskCheck : std::uint8_t {
  PhaseGate = 1,
  PriceCollar,
  SizeNotional,
  Exposure,
  SellableInventory,
  Margin,
  RateLimit,
  CancelRatio,
  SelfTrade,
  DuplicateBurst,
  StaleData,
  KillSwitch,
};

struct RiskDeny {
  OrderId cl_id;
  Nanos ts;
  Price px;
  Qty qty;
  InstrumentId inst;
  StrategyId strategy;
  AccountId account;
  RiskCheck check;
  Side side;
  std::uint8_t is_cancel;  // 1 when the denied action was a cancel
  std::uint8_t pad_[5]{};
};
static_assert(std::is_trivially_copyable_v<RiskDeny> && sizeof(RiskDeny) == 48);

enum class KillScope : std::uint8_t { Strategy, Account, Site, Firm };
enum class KillAction : std::uint8_t { Halt, Flatten, Disarm };

struct KillSwitchCmd {
  Nanos ts;
  // Monotonic command version (§10.3, review R1): a stale or replayed control
  // command must never override a newer one. 0 = unversioned (direct/local
  // application, e.g. unit fixtures); versioned commands only apply if newer
  // than the last applied.
  std::uint64_t cmd_seq;
  std::uint16_t target;  // strategy/account id; ignored for Site/Firm
  KillScope scope;
  KillAction action;
  std::uint8_t pad_[4]{};
};
static_assert(std::is_trivially_copyable_v<KillSwitchCmd> && sizeof(KillSwitchCmd) == 24);

// ---------------------------------------------------------------- cross-site

// Hedge intent (§9.3): "my leg filled — execute yours within this budget".
struct HedgeIntent {
  std::uint64_t pair_id;
  Nanos origin_ts;  // stamped at origin site; receiver sees staleness explicitly
  Nanos ts;         // local arrival time (set by bridge on injection)
  Price limit_px;   // max slippage budget expressed as a limit
  Qty qty;
  InstrumentId inst;  // instrument at the *receiving* site
  StrategyId strategy;
  Side side;
  SiteId origin_site;
  std::uint8_t pad_[4]{};
};
static_assert(std::is_trivially_copyable_v<HedgeIntent> && sizeof(HedgeIntent) == 56);

// Compact basis/leg summary exported cross-site instead of raw L2 (§7.3).
struct BasisSummary {
  std::uint64_t pair_id;
  Nanos origin_ts;
  Nanos ts;
  Price leg_bid;
  Price leg_ask;
  Qty leg_bid_qty;
  Qty leg_ask_qty;
  InstrumentId inst;
  SiteId origin_site;
  std::uint8_t pad_[3]{};
};
static_assert(std::is_trivially_copyable_v<BasisSummary> && sizeof(BasisSummary) == 64);

// Hedge completion flowing back to the opportunity ledger at the origin site
// (§9.3): how much got done and at what average price, so unhedged exposure
// and realized slippage are tracked where the pair is managed.
struct HedgeDone {
  std::uint64_t pair_id;
  Nanos origin_ts;  // the intent's origin_ts, echoed for latency attribution
  Nanos ts;
  Price avg_px;
  Qty filled;
  Qty unfilled;  // budget exhausted / rejected remainder
  InstrumentId inst;
  StrategyId strategy;
  Side side;
  SiteId origin_site;  // site that executed the hedge
  std::uint8_t pad_[4]{};
};
static_assert(std::is_trivially_copyable_v<HedgeDone> && sizeof(HedgeDone) == 64);

// Strategy timer callback delivery. Timer firings are scheduler-driven and
// therefore Source-origin: they are journaled and re-injected on replay so a
// strategy's timer-dependent behavior replays exactly (§11.1).
struct TimerEvt {
  Nanos ts;
  std::uint64_t token;
  StrategyId strategy;
  std::uint8_t pad_[6]{};
};
static_assert(std::is_trivially_copyable_v<TimerEvt> && sizeof(TimerEvt) == 24);

// ------------------------------------------------------------- operator ops
// Manual interventions from the ops console / platops web UI (§10.3, §14).
// All four are SOURCE-origin and journaled: an operator action is external
// world, so replay re-injects it and the deterministic set regenerates its
// consequences (the risk-checked order, the book transfer) exactly — which
// is also the attributable audit trail 穿透式监管 expects.

inline constexpr std::size_t kParamKeyLen = 22;

// Live re-tune of ONE whitelisted strategy parameter. Keys use the config
// key names ("lots", "take_ticks", "timeout_ms", ...); each strategy applies
// only the keys it recognizes in on_param. Structural knobs (universe, ids,
// EMA shape) stay restart-only.
struct ParamUpdateEvt {
  Nanos ts;
  std::int64_t value;
  StrategyId strategy;
  char key[kParamKeyLen]{};  // NUL-terminated
};
static_assert(std::is_trivially_copyable_v<ParamUpdateEvt> && sizeof(ParamUpdateEvt) == 40);

// Operator order under a strategy's book. Dispatch turns it into a normal
// Derived NewOrderReq — the full riskgate battery applies; a fat-fingered
// manual order is denied exactly like a strategy's.
struct ManualOrderCmd {
  Nanos ts;
  Price px;
  Qty qty;
  InstrumentId inst;
  StrategyId strategy;
  AccountId account;
  Side side;
  Offset offset;
  std::uint8_t pad_[6]{};
};
static_assert(std::is_trivially_copyable_v<ManualOrderCmd> && sizeof(ManualOrderCmd) == 40);

// Operator cancel of one working order (by client order id).
struct ManualCancelCmd {
  Nanos ts;
  OrderId cl_id;
};
static_assert(std::is_trivially_copyable_v<ManualCancelCmd> && sizeof(ManualCancelCmd) == 16);

// Bookkeeping transfer between two strategy books in the same account — a
// stopped strategy's carry to a live one, a fat finger to the manual book.
// No exchange interaction, nothing realized; cost basis moves with the lots.
struct PositionTransferCmd {
  Nanos ts;
  Qty qty;
  InstrumentId inst;
  StrategyId from_strategy;
  StrategyId to_strategy;
  AccountId account;
  Side side;  // futures: Buy = long book, Sell = short book (stocks ignore)
  std::uint8_t pad_[5]{};
};
static_assert(std::is_trivially_copyable_v<PositionTransferCmd> &&
              sizeof(PositionTransferCmd) == 32);

// ---------------------------------------------------------------- sessions

enum class SessState : std::uint8_t { Down, LoggingIn, Up, Reconciling };

struct SessionStateEvt {
  Nanos ts;
  Venue venue;
  SessState state;
  std::uint8_t pad_[6]{};
};
static_assert(std::is_trivially_copyable_v<SessionStateEvt> && sizeof(SessionStateEvt) == 16);

// ------------------------------------------------------------- schema traits

template <typename T>
struct MsgTraits;  // undefined base: publishing an unregistered type is a compile error

// clang-format off
template <> struct MsgTraits<MdOrderEvt>      { static constexpr Schema schema = Schema::MdOrder;      static constexpr Topic topic = Topic::MarketData; };
template <> struct MsgTraits<MdTradeEvt>      { static constexpr Schema schema = Schema::MdTrade;      static constexpr Topic topic = Topic::MarketData; };
template <> struct MsgTraits<MdSnapshotEvt>   { static constexpr Schema schema = Schema::MdSnapshot;   static constexpr Topic topic = Topic::MarketData; };
template <> struct MsgTraits<BookTopEvt>      { static constexpr Schema schema = Schema::BookTop;      static constexpr Topic topic = Topic::BookTops; };
template <> struct MsgTraits<PhaseChangeEvt>  { static constexpr Schema schema = Schema::PhaseChange;  static constexpr Topic topic = Topic::Control; };
template <> struct MsgTraits<NewOrderReq>     { static constexpr Schema schema = Schema::NewOrderReq;  static constexpr Topic topic = Topic::OrderFlow; };
template <> struct MsgTraits<CancelReq>       { static constexpr Schema schema = Schema::CancelReq;    static constexpr Topic topic = Topic::OrderFlow; };
template <> struct MsgTraits<ExecReport>      { static constexpr Schema schema = Schema::ExecReport;   static constexpr Topic topic = Topic::Execs; };
template <> struct MsgTraits<RiskDeny>        { static constexpr Schema schema = Schema::RiskDeny;     static constexpr Topic topic = Topic::Risk; };
template <> struct MsgTraits<HedgeIntent>     { static constexpr Schema schema = Schema::HedgeIntent;  static constexpr Topic topic = Topic::Remote; };
template <> struct MsgTraits<BasisSummary>    { static constexpr Schema schema = Schema::BasisSummary; static constexpr Topic topic = Topic::Remote; };
template <> struct MsgTraits<KillSwitchCmd>   { static constexpr Schema schema = Schema::KillSwitch;   static constexpr Topic topic = Topic::Risk; };
template <> struct MsgTraits<SessionStateEvt> { static constexpr Schema schema = Schema::SessionState; static constexpr Topic topic = Topic::Control; };
template <> struct MsgTraits<TimerEvt>        { static constexpr Schema schema = Schema::Timer;        static constexpr Topic topic = Topic::Control; };
template <> struct MsgTraits<HedgeDone>       { static constexpr Schema schema = Schema::HedgeDone;    static constexpr Topic topic = Topic::Remote; };
template <> struct MsgTraits<FeedGapEvt>      { static constexpr Schema schema = Schema::FeedGap;      static constexpr Topic topic = Topic::MarketData; };
template <> struct MsgTraits<ParamUpdateEvt>  { static constexpr Schema schema = Schema::ParamUpdate;  static constexpr Topic topic = Topic::Control; };
template <> struct MsgTraits<ManualOrderCmd>  { static constexpr Schema schema = Schema::ManualOrder;  static constexpr Topic topic = Topic::OrderFlow; };
template <> struct MsgTraits<ManualCancelCmd> { static constexpr Schema schema = Schema::ManualCancel; static constexpr Topic topic = Topic::OrderFlow; };
template <> struct MsgTraits<PositionTransferCmd> { static constexpr Schema schema = Schema::PositionTransfer; static constexpr Topic topic = Topic::Control; };
// clang-format on

template <typename T>
concept PlatMsg = std::is_trivially_copyable_v<T> && requires { MsgTraits<T>::schema; };

}  // namespace plat
