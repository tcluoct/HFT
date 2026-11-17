#pragma once
// plat/ctp/trader_client_queries.hpp — TraderClient's synchronous query
// machinery and its response callbacks, out-of-line (included by
// trader_client.hpp; never include directly).

#include "plat/ctp/trader_client.hpp"

namespace plat::ctp {

inline InstrumentMeta TraderClient::query_instrument(const char* symbol) {
  std::lock_guard qlk(qry_mtx_);
  qry_instrument_ = InstrumentMeta{};
  std::snprintf(qry_instrument_symbol_, sizeof(qry_instrument_symbol_), "%s", symbol);
  CThostFtdcQryInstrumentField q{};
  std::snprintf(q.InstrumentID, sizeof(q.InstrumentID), "%s", symbol);
  const bool done = run_query([&](int request_id) { return api_->ReqQryInstrument(&q, request_id); });
  qry_instrument_.answered = done && qry_error_ == 0;
  return qry_instrument_;
}

inline DepthMeta TraderClient::query_depth(const char* symbol) {
  std::lock_guard qlk(qry_mtx_);
  qry_depth_ = DepthMeta{};
  std::snprintf(qry_depth_symbol_, sizeof(qry_depth_symbol_), "%s", symbol);
  CThostFtdcQryDepthMarketDataField q{};
  std::snprintf(q.InstrumentID, sizeof(q.InstrumentID), "%s", symbol);
  const bool done = run_query([&](int request_id) { return api_->ReqQryDepthMarketData(&q, request_id); });
  qry_depth_.answered = done && qry_error_ == 0;
  return qry_depth_;
}

inline std::optional<std::vector<PositionRow>> TraderClient::query_positions() {
  std::lock_guard qlk(qry_mtx_);
  qry_positions_.clear();
  CThostFtdcQryInvestorPositionField q{};
  std::snprintf(q.BrokerID, sizeof(q.BrokerID), "%s", cfg_.broker_id.c_str());
  std::snprintf(q.InvestorID, sizeof(q.InvestorID), "%s", cfg_.investor_id.c_str());
  const bool ok = run_query([&](int request_id) { return api_->ReqQryInvestorPosition(&q, request_id); });
  if (!ok) return std::nullopt;
  return qry_positions_;
}

inline std::optional<std::vector<VenueOpenOrder>> TraderClient::query_open_orders() {
  std::lock_guard qlk(qry_mtx_);
  qry_open_orders_.clear();
  qry_foreign_open_ = 0;
  CThostFtdcQryOrderField q{};
  std::snprintf(q.BrokerID, sizeof(q.BrokerID), "%s", cfg_.broker_id.c_str());
  std::snprintf(q.InvestorID, sizeof(q.InvestorID), "%s", cfg_.investor_id.c_str());
  const bool ok = run_query([&](int request_id) { return api_->ReqQryOrder(&q, request_id); });
  if (!ok) return std::nullopt;
  return qry_open_orders_;
}

inline std::optional<TradeTotals> TraderClient::query_trade_totals() {
  std::lock_guard qlk(qry_mtx_);
  qry_trades_ = TradeTotals{};
  CThostFtdcQryTradeField q{};
  std::snprintf(q.BrokerID, sizeof(q.BrokerID), "%s", cfg_.broker_id.c_str());
  std::snprintf(q.InvestorID, sizeof(q.InvestorID), "%s", cfg_.investor_id.c_str());
  const bool ok = run_query([&](int request_id) { return api_->ReqQryTrade(&q, request_id); });
  if (!ok) return std::nullopt;
  return qry_trades_;
}

inline std::optional<std::vector<AccountOpenOrder>> TraderClient::query_account_open_orders() {
  std::lock_guard qlk(qry_mtx_);
  qry_acct_open_.clear();
  qry_acct_open_active_ = true;
  CThostFtdcQryOrderField q{};
  std::snprintf(q.BrokerID, sizeof(q.BrokerID), "%s", cfg_.broker_id.c_str());
  std::snprintf(q.InvestorID, sizeof(q.InvestorID), "%s", cfg_.investor_id.c_str());
  const bool ok = run_query([&](int request_id) { return api_->ReqQryOrder(&q, request_id); });
  qry_acct_open_active_ = false;
  if (!ok) return std::nullopt;
  return qry_acct_open_;
}

inline std::optional<std::vector<AccountTrade>> TraderClient::query_account_trades() {
  std::lock_guard qlk(qry_mtx_);
  qry_acct_trades_.clear();
  qry_acct_trades_active_ = true;
  CThostFtdcQryTradeField q{};
  std::snprintf(q.BrokerID, sizeof(q.BrokerID), "%s", cfg_.broker_id.c_str());
  std::snprintf(q.InvestorID, sizeof(q.InvestorID), "%s", cfg_.investor_id.c_str());
  const bool ok = run_query([&](int request_id) { return api_->ReqQryTrade(&q, request_id); });
  qry_acct_trades_active_ = false;
  if (!ok) return std::nullopt;
  return qry_acct_trades_;
}

inline std::optional<AccountFunds> TraderClient::query_funds() {
  std::lock_guard qlk(qry_mtx_);
  qry_funds_ = AccountFunds{};
  CThostFtdcQryTradingAccountField q{};
  std::snprintf(q.BrokerID, sizeof(q.BrokerID), "%s", cfg_.broker_id.c_str());
  std::snprintf(q.InvestorID, sizeof(q.InvestorID), "%s", cfg_.investor_id.c_str());
  const bool ok = run_query([&](int request_id) { return api_->ReqQryTradingAccount(&q, request_id); });
  if (!ok) return std::nullopt;
  return qry_funds_;
}

inline void TraderClient::OnRspQryInstrument(CThostFtdcInstrumentField* in, CThostFtdcRspInfoField* info,
                        int request_id, bool bIsLast) {
  std::lock_guard state_lock(qry_state_mtx_);
  if (request_id != qry_request_id_ || qry_request_id_ == 0) return;
  if (info && info->ErrorID) qry_error_ = info->ErrorID;
  if (info && info->ErrorID != 0) {
    logf("[td] instrument query error: %d %s\n", info->ErrorID, ctp::gbk_to_utf8(info->ErrorMsg).c_str());
  }
  if (in && (!info || info->ErrorID == 0) &&
      std::strcmp(in->InstrumentID, qry_instrument_symbol_) == 0) {
    qry_instrument_.found = true;
    std::snprintf(qry_instrument_.symbol, sizeof(qry_instrument_.symbol), "%s",
                  in->InstrumentID);
    std::snprintf(qry_instrument_.exchange_id, sizeof(qry_instrument_.exchange_id), "%s",
                  in->ExchangeID);
    qry_instrument_.price_tick = in->PriceTick;
    qry_instrument_.volume_multiple = in->VolumeMultiple;
    qry_instrument_.long_margin_ratio = in->LongMarginRatio;
  }
  if (bIsLast) finish_query();
}

inline void TraderClient::OnRspQryDepthMarketData(CThostFtdcDepthMarketDataField* d, CThostFtdcRspInfoField* info,
                             int request_id, bool bIsLast) {
  std::lock_guard state_lock(qry_state_mtx_);
  if (request_id != qry_request_id_ || qry_request_id_ == 0) return;
  if (info && info->ErrorID) qry_error_ = info->ErrorID;
  if (info && info->ErrorID != 0) {
    logf("[td] depth query error: %d %s\n", info->ErrorID, ctp::gbk_to_utf8(info->ErrorMsg).c_str());
  }
  // The query can return every instrument on some fronts — filter to ours.
  if (d && (!info || info->ErrorID == 0) &&
      std::strcmp(d->InstrumentID, qry_depth_symbol_) == 0) {
    qry_depth_.found = true;
    qry_depth_.pre_close = d->PreClosePrice;
    qry_depth_.pre_settlement = d->PreSettlementPrice;
    qry_depth_.upper_limit = d->UpperLimitPrice;
    qry_depth_.lower_limit = d->LowerLimitPrice;
  }
  if (bIsLast) finish_query();
}

inline void TraderClient::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* p, CThostFtdcRspInfoField* info,
                              int request_id, bool bIsLast) {
  std::lock_guard state_lock(qry_state_mtx_);
  if (request_id != qry_request_id_ || qry_request_id_ == 0) return;
  if (info && info->ErrorID) qry_error_ = info->ErrorID;
  if (p && (!info || info->ErrorID == 0) && p->Position > 0) {
    PositionRow row{};
    std::snprintf(row.symbol, sizeof(row.symbol), "%s", p->InstrumentID);
    row.is_long = p->PosiDirection == THOST_FTDC_PD_Long;
    row.total = p->Position;
    // SHFE/INE split rows by PositionDate; other venues report one row
    // whose YdPosition is the carry.
    row.yd = p->PositionDate == THOST_FTDC_PSD_History ? p->Position : p->YdPosition;
    row.open_cost = p->PositionCost;
    qry_positions_.push_back(row);
  }
  if (bIsLast) finish_query();
}

inline void TraderClient::OnRspQryOrder(CThostFtdcOrderField* o, CThostFtdcRspInfoField* info, int request_id,
                   bool bIsLast) {
  std::lock_guard state_lock(qry_state_mtx_);
  if (request_id != qry_request_id_ || qry_request_id_ == 0) return;
  if (info && info->ErrorID) qry_error_ = info->ErrorID;
  if (o && (!info || info->ErrorID == 0)) {
    const bool open = o->OrderStatus == THOST_FTDC_OST_PartTradedQueueing ||
                      o->OrderStatus == THOST_FTDC_OST_NoTradeQueueing ||
                      o->OrderStatus == THOST_FTDC_OST_Unknown;
    if (open && qry_acct_open_active_) {
      AccountOpenOrder a{};
      a.ref_seq = seq_from_order_ref(o->OrderRef);
      a.venue_no = venue_no_from_sys_id(o->OrderSysID);
      std::snprintf(a.sys_id, sizeof(a.sys_id), "%s", o->OrderSysID);
      std::snprintf(a.exchange, sizeof(a.exchange), "%s", o->ExchangeID);
      std::snprintf(a.symbol, sizeof(a.symbol), "%s", o->InstrumentID);
      a.leaves = o->VolumeTotal;
      qry_acct_open_.push_back(a);
    } else if (open) {
      const std::uint64_t seq = seq_from_order_ref(o->OrderRef);
      const bool ours = seq != 0 && o->FrontID == front_id_ && o->SessionID == session_id_;
      if (ours) {
        VenueOpenOrder v{};
        v.cl_id = make_order_id(cfg_.epoch, seq);
        v.order_no = venue_no_from_sys_id(o->OrderSysID);
        v.leaves = o->VolumeTotal;
        qry_open_orders_.push_back(v);
      } else {
        ++qry_foreign_open_;  // reported, not flagged: another session's order
      }
    }
  }
  if (bIsLast) {
    if (qry_foreign_open_)
      logf("[td] EOD: %u open orders belong to other sessions/shards\n",
                   qry_foreign_open_);
    finish_query();
  }
}

inline void TraderClient::OnRspQryTrade(CThostFtdcTradeField* t, CThostFtdcRspInfoField* info, int request_id,
                   bool bIsLast) {
  std::lock_guard state_lock(qry_state_mtx_);
  if (request_id != qry_request_id_ || qry_request_id_ == 0) return;
  if (info && info->ErrorID) qry_error_ = info->ErrorID;
  if (t && (!info || info->ErrorID == 0)) {
    if (qry_acct_trades_active_) {
      AccountTrade a{};
      a.venue_no = venue_no_from_sys_id(t->OrderSysID);
      a.ref_seq = seq_from_order_ref(t->OrderRef);
      a.px = px_from_ctp(t->Price);
      a.qty = t->Volume;
      a.side = side_from_ctp(t->Direction);
      a.offset = offset_from_ctp(t->OffsetFlag);
      std::snprintf(a.symbol, sizeof(a.symbol), "%s", t->InstrumentID);
      qry_acct_trades_.push_back(a);
    } else if (trade_is_ours(t)) {
      if (t->Direction == THOST_FTDC_D_Buy) qry_trades_.buy_qty += t->Volume;
      else qry_trades_.sell_qty += t->Volume;
    }
  }
  if (bIsLast) finish_query();
}

inline void TraderClient::OnRspQryTradingAccount(CThostFtdcTradingAccountField* a, CThostFtdcRspInfoField* info,
                            int request_id, bool bIsLast) {
  std::lock_guard state_lock(qry_state_mtx_);
  if (request_id != qry_request_id_ || qry_request_id_ == 0) return;
  if (info && info->ErrorID) qry_error_ = info->ErrorID;
  if (a && (!info || info->ErrorID == 0)) {
    qry_funds_.found = true;
    qry_funds_.balance = a->Balance;
    qry_funds_.available = a->Available;
    qry_funds_.margin = a->CurrMargin;
    std::snprintf(qry_funds_.account_id, sizeof(qry_funds_.account_id), "%s", a->AccountID);
  }
  if (bIsLast) finish_query();
}

template <typename SendFn>
bool TraderClient::run_query(SendFn&& send, std::chrono::milliseconds timeout) {
  for (int attempt = 0; attempt < 30; ++attempt) {
    const int request_id = ++req_id_;
    {
      std::lock_guard lk(qry_state_mtx_);
      qry_done_ = false;
      qry_error_ = 0;
      qry_request_id_ = request_id;
    }
    const int ret = send(request_id);
    std::unique_lock lk(qry_state_mtx_);
    if (ret == 0) {
      const bool done = qry_cv_.wait_for(lk, timeout, [this] { return qry_done_; });
      qry_request_id_ = 0; // retire before releasing the result to the caller
      const bool ok = done && qry_error_ == 0;
      if (!ok) logf("[td] query %d failed (timeout=%d, error=%d)\n",
                    request_id, !done, qry_error_);
      return ok;
    }
    qry_request_id_ = 0;
    if (ret != -2 && ret != -3) return false;
    lk.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  }
  return false;
}

inline void TraderClient::finish_query() {
  qry_done_ = true;
  qry_request_id_ = 0;
  qry_cv_.notify_all();
}

}  // namespace plat::ctp
