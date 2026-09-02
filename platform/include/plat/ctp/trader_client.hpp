#pragma once
// plat/ctp/trader_client.hpp — the production counter gateway: SFIT CTP
// (thosttraderapi_se) behind the plat::OrderGateway boundary (§9.4).
//
// Lifecycle (all driven by CTP callbacks on the api's network thread):
//   OnFrontConnected → ReqAuthenticate → ReqUserLogin (captures FrontID/
//   SessionID/TradingDay) → ReqSettlementInfoConfirm → session Up.
//   A front disconnect re-runs the same chain when the api reconnects; the
//   session reports Reconciling until the login handshake completes again.
//
// Threading:
//   · place()/cancel() are called on the engine thread; every CTP callback
//     arrives on the api thread. The order registry is mutex-guarded (cold:
//     tens of entries, hundreds of actions/day by design — riskgate's rate
//     cap sits far below any contention level).
//   · All outcomes — venue reports and local bounces alike — are published
//     to one broadcast ring through one mutex-serialized writer, so the
//     engine sees a single FIFO report stream (the session-FIFO contract: a
//     fill can never arrive after its IOC's cancel-remainder). The ring is
//     single-writer/multi-reader; the engine is its one trading consumer.
//   · Startup/EOD queries are synchronous helpers with the CTP 1-flow/sec
//     query throttle respected by the caller (main/driver, never the engine
//     loop).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ThostFtdcTraderApi.h"
#include "plat/bus/ring.hpp"
#include "plat/core/log.hpp"
#include "plat/codec/msgs.hpp"
#include "plat/core/types.hpp"
#include "plat/ctp/conv.hpp"
#include "plat/gw/gateway.hpp"
#include "plat/oms/oms.hpp"

namespace plat::ctp {

struct TraderConfig {
  std::string front;      // "tcp://ip:port"
  std::string broker_id;
  std::string user_id;
  std::string password;
  std::string investor_id;  // usually == user_id
  std::string app_id;       // terminal 穿透式监管 credentials
  std::string auth_code;
  std::string flow_path;    // directory for CTP's .con state files
  std::uint16_t epoch = 1;  // order-id epoch — must match SiteConfig.epoch
};

// Reference/position rows produced by the startup queries. `answered`
// distinguishes "the counter replied cleanly" from "timed out / errored" —
// right after login many fronts need seconds before queries serve, and a
// warming-up counter must read as RETRY, never as "instrument absent".
struct InstrumentMeta {
  bool answered = false;
  bool found = false;
  char exchange_id[9]{};
  char symbol[32]{};
  double price_tick = 0;
  int volume_multiple = 0;
  double long_margin_ratio = 0;
};
struct DepthMeta {
  bool answered = false;
  bool found = false;
  double pre_close = 0;
  double pre_settlement = 0;
  double upper_limit = 0;
  double lower_limit = 0;
};
struct PositionRow {
  char symbol[32]{};
  bool is_long = false;
  Qty total = 0;  // current position (lots)
  Qty yd = 0;     // carried from yesterday
  double open_cost = 0;  // CTP PositionCost (total, CNY)
};
struct TradeTotals {
  Qty buy_qty = 0;
  Qty sell_qty = 0;
};
// Crash-recovery rows (account-wide, session-agnostic): what the counter
// knows about orders and fills, for reconciling a resumed site.
struct AccountOpenOrder {
  std::uint64_t ref_seq = 0;   // OrderRef parsed as our sequence (0 = not numeric)
  std::uint64_t venue_no = 0;  // parsed OrderSysID
  char sys_id[21]{};
  char exchange[9]{};
  char symbol[32]{};
  Qty leaves = 0;
};
struct AccountTrade {
  std::uint64_t venue_no = 0;  // parsed OrderSysID of the owning order
  std::uint64_t ref_seq = 0;
  Price px = 0;
  Qty qty = 0;
  Side side = Side::Buy;
  Offset offset = Offset::Open;
  char symbol[32]{};
};
struct AccountFunds {
  bool found = false;
  double balance = 0;
  double available = 0;
  double margin = 0;
};

class TraderClient final : public CThostFtdcTraderSpi {
 public:
  using NowFn = Nanos (*)();

  TraderClient(TraderConfig cfg, Ring& out, NowFn now_fn)
      : cfg_(std::move(cfg)), writer_(out), now_(now_fn) {}
  TraderClient(const TraderClient&) = delete;
  TraderClient& operator=(const TraderClient&) = delete;
  ~TraderClient() { stop(); }

  // ------------------------------------------------------------- lifecycle

  void start();

  void stop();

  bool wait_until_up(int timeout_ms);

  [[nodiscard]] SessState state() const {
    return state_atomic_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint32_t trading_day() const noexcept { return trading_day_; }

  // ------------------------------------------------------------ order entry

  void place(const NewOrderReq& r);

  void cancel(const CancelReq& c);

  // The wire identity of each hosted instrument, registered by main after
  // the instrument queries and before any order flow.
  void add_instrument(InstrumentId id, const char* symbol, const char* exchange);

  // -------------------------------------------------------------- queries
  // Synchronous, one at a time, engine-external threads only. Each respects
  // the counter's 1 query/sec flow control by spacing itself.

  InstrumentMeta query_instrument(const char* symbol);

  DepthMeta query_depth(const char* symbol);

  std::vector<PositionRow> query_positions();

  std::vector<VenueOpenOrder> query_open_orders();

  TradeTotals query_trade_totals();

  // ------------------------------------------------- crash-recovery queries
  // Account-wide views (NOT session-filtered — the crashed session is gone;
  // its orders now look foreign). Used only by --resume reconciliation.

  std::vector<AccountOpenOrder> query_account_open_orders();

  std::vector<AccountTrade> query_account_trades();

  // Adopt a crashed session's still-open order into this session's registry:
  // subsequent OnRtnOrder/OnRtnTrade for it route here (matched by its
  // exchange order id), and cancel() reaches it via ExchangeID+OrderSysID.
  void adopt_order(OrderId cl_id, InstrumentId inst, StrategyId strategy, AccountId account,
                   Side side, Offset offset, Price px, Qty qty, Qty cum,
                   const char* symbol, const char* exchange, const char* sys_id);

  AccountFunds query_funds();

  // ------------------------------------------------------- CTP callbacks

  void OnFrontConnected() override;

  void OnFrontDisconnected(int nReason) override;

  void OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* info,
                         int, bool) override;

  void OnRspUserLogin(CThostFtdcRspUserLoginField* login, CThostFtdcRspInfoField* info,
                      int, bool) override;

  void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*,
                                  CThostFtdcRspInfoField* info, int, bool) override;

  void OnRspError(CThostFtdcRspInfoField* info, int, bool) override;

  // Front/exchange rejected the insert before it ever rested.
  void OnRspOrderInsert(CThostFtdcInputOrderField* input, CThostFtdcRspInfoField* info,
                        int, bool) override;
  void OnErrRtnOrderInsert(CThostFtdcInputOrderField* input,
                           CThostFtdcRspInfoField* info) override;

  void OnRspOrderAction(CThostFtdcInputOrderActionField* action, CThostFtdcRspInfoField* info,
                        int, bool) override;
  void OnErrRtnOrderAction(CThostFtdcOrderActionField* action,
                           CThostFtdcRspInfoField* info) override;

  void OnRtnOrder(CThostFtdcOrderField* o) override;

  void OnRtnTrade(CThostFtdcTradeField* t) override;

  // ------------------------------------------------------ query callbacks

  // Some fronts treat the query's InstrumentID as a prefix filter and return
  // the whole family (the future plus every option series on it) — only the
  // exact match is the instrument we asked about.
  void OnRspQryInstrument(CThostFtdcInstrumentField* in, CThostFtdcRspInfoField* info,
                          int, bool bIsLast) override;

  void OnRspQryDepthMarketData(CThostFtdcDepthMarketDataField* d, CThostFtdcRspInfoField* info,
                               int, bool bIsLast) override;

  void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* p, CThostFtdcRspInfoField* info,
                                int, bool bIsLast) override;

  // EOD queries return ACCOUNT-level rows; several shard processes share one
  // account (one CTP session each), so reconciliation must be session-scoped:
  // another shard's open orders and fills are its own shard's business (and
  // the firm aggregator's), not a break here.
  void OnRspQryOrder(CThostFtdcOrderField* o, CThostFtdcRspInfoField* info, int,
                     bool bIsLast) override;

  void OnRspQryTrade(CThostFtdcTradeField* t, CThostFtdcRspInfoField* info, int,
                     bool bIsLast) override;

  void OnRspQryTradingAccount(CThostFtdcTradingAccountField* a, CThostFtdcRspInfoField* info,
                              int, bool bIsLast) override;

 private:
  struct OrderCtx {
    OrderId cl_id = kNoOrder;
    std::uint64_t venue_no = 0;
    Nanos trigger_ts = 0;
    Price px = 0;
    Qty qty = 0;
    Qty cum = 0;
    InstrumentId inst = kNoInstrument;
    StrategyId strategy = 0;
    AccountId account = 0;
    Side side = Side::Buy;
    Offset offset = Offset::Open;
    bool acked = false;
    bool terminal = false;
    bool adopted = false;  // inherited from a crashed session (--resume)
    char sys_id[21]{};
    char exchange[9]{};
    char symbol[32]{};
  };
  struct InstrumentWire {
    InstrumentId inst = kNoInstrument;
    char symbol[32]{};
    char exchange[9]{};
  };

  [[nodiscard]] const InstrumentWire* wire_for(InstrumentId id) const noexcept;

  // A trade row has no FrontID/SessionID; ownership = its OrderRef parses to
  // a sequence in OUR registry AND the exchange order id matches what our
  // ack recorded (two shards both use small numeric OrderRefs, so the ref
  // alone is ambiguous — the venue-unique OrderSysID disambiguates).
  [[nodiscard]] bool trade_is_ours(const CThostFtdcTradeField* t);

  ExecReport make_report(const OrderCtx& ctx, ExecType type, Price fill_px, Qty fill_qty,
                         Qty leaves);

  void bounce_new(const NewOrderReq& r, ExecReason why);

  void bounce_cancel(const CancelReq& c, ExecReason why);

  void reject_by_ref(const char* order_ref);

  void cancel_reject_by_ref(const char* order_ref);

  void publish_report(const ExecReport& r);

  void set_state(SessState s);

  // One outstanding query; retry sends that hit the counter's flow control.
  // Returns whether the counter answered at all (a timeout means "ask
  // again", never "the thing you asked about does not exist").
  template <typename SendFn>
  bool run_query(SendFn&& send);
  void finish_query();

  TraderConfig cfg_;
  Ring::Writer writer_;
  NowFn now_;
  CThostFtdcTraderApi* api_ = nullptr;

  std::mutex ring_mtx_;   // serializes venue reports and engine-side bounces
  std::mutex reg_mtx_;    // order registry
  std::map<std::uint64_t, OrderCtx> orders_;
  std::vector<InstrumentWire> wires_;

  std::mutex state_mtx_;
  std::condition_variable state_cv_;
  SessState state_ = SessState::Down;
  std::atomic<SessState> state_atomic_{SessState::Down};
  bool was_up_ = false;

  std::mutex qry_mtx_;        // one query at a time
  std::mutex qry_state_mtx_;  // completion handshake
  std::condition_variable qry_cv_;
  bool qry_done_ = false;
  int qry_error_ = 0;         // ErrorID of the pending query's response (0 = clean)
  InstrumentMeta qry_instrument_{};
  char qry_instrument_symbol_[32]{};
  DepthMeta qry_depth_{};
  char qry_depth_symbol_[32]{};
  std::vector<PositionRow> qry_positions_;
  std::vector<VenueOpenOrder> qry_open_orders_;
  std::uint32_t qry_foreign_open_ = 0;
  TradeTotals qry_trades_{};
  AccountFunds qry_funds_{};
  std::vector<AccountOpenOrder> qry_acct_open_;
  std::vector<AccountTrade> qry_acct_trades_;
  bool qry_acct_open_active_ = false;
  bool qry_acct_trades_active_ = false;

  int front_id_ = 0;
  int session_id_ = 0;
  std::uint32_t trading_day_ = 0;
  int req_id_ = 0;
};

static_assert(OrderGateway<TraderClient>,
              "TraderClient must satisfy the site engine's gateway concept");

}  // namespace plat::ctp

// Out-of-line member definitions (kept separate for file-size discipline).
#include "plat/ctp/trader_client_orders.hpp"   // IWYU pragma: keep
#include "plat/ctp/trader_client_queries.hpp"  // IWYU pragma: keep
