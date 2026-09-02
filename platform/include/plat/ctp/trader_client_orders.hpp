#pragma once
// plat/ctp/trader_client_orders.hpp — TraderClient lifecycle, order entry,
// and private-flow callbacks, out-of-line (included by trader_client.hpp;
// never include directly).

#include "plat/ctp/trader_client.hpp"

namespace plat::ctp {

inline void TraderClient::start() {
  api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(cfg_.flow_path.c_str());
  api_->RegisterSpi(this);
  // QUICK: only private flow from connect-time on — the journal is our
  // record of the past; a restart re-queries rather than replaying the
  // counter's day-long private stream.
  api_->SubscribePrivateTopic(THOST_TERT_QUICK);
  api_->SubscribePublicTopic(THOST_TERT_QUICK);
  api_->RegisterFront(const_cast<char*>(cfg_.front.c_str()));
  api_->Init();
}

inline void TraderClient::stop() {
  if (api_) {
    api_->RegisterSpi(nullptr);
    api_->Release();
    api_ = nullptr;
  }
}

inline bool TraderClient::wait_until_up(int timeout_ms) {
  std::unique_lock lk(state_mtx_);
  return state_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [this] { return state_ == SessState::Up; });
}

inline void TraderClient::place(const NewOrderReq& r) {
  if (state() != SessState::Up) {
    bounce_new(r, ExecReason::SessionDown);
    return;
  }
  const std::uint64_t seq = r.cl_id & 0xFFFF'FFFF'FFFFull;
  OrderCtx ctx{};
  ctx.cl_id = r.cl_id;
  ctx.inst = r.inst;
  ctx.strategy = r.strategy;
  ctx.account = r.account;
  ctx.px = r.px;
  ctx.qty = r.qty;
  ctx.trigger_ts = r.trigger_ts;
  ctx.side = r.side;
  ctx.offset = r.offset;
  const InstrumentWire* w = wire_for(r.inst);
  if (!w) {
    bounce_new(r, ExecReason::UnknownOrder);
    return;
  }
  std::snprintf(ctx.symbol, sizeof(ctx.symbol), "%s", w->symbol);
  std::snprintf(ctx.exchange, sizeof(ctx.exchange), "%s", w->exchange);
  {
    std::lock_guard lk(reg_mtx_);
    orders_[seq] = ctx;  // registered before the send: the ack can't race us
  }

  CThostFtdcInputOrderField f{};
  std::snprintf(f.BrokerID, sizeof(f.BrokerID), "%s", cfg_.broker_id.c_str());
  std::snprintf(f.InvestorID, sizeof(f.InvestorID), "%s", cfg_.investor_id.c_str());
  std::snprintf(f.UserID, sizeof(f.UserID), "%s", cfg_.user_id.c_str());
  std::snprintf(f.InstrumentID, sizeof(f.InstrumentID), "%s", w->symbol);
  std::snprintf(f.ExchangeID, sizeof(f.ExchangeID), "%s", w->exchange);
  order_ref_from_seq(seq, f.OrderRef);
  f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
  f.Direction = side_to_ctp(r.side);
  f.CombOffsetFlag[0] = offset_to_ctp(r.offset);
  f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
  f.LimitPrice = px_to_ctp(r.px);
  f.VolumeTotalOriginal = static_cast<int>(r.qty);
  f.TimeCondition = r.tif == Tif::Ioc ? THOST_FTDC_TC_IOC : THOST_FTDC_TC_GFD;
  f.VolumeCondition = THOST_FTDC_VC_AV;
  f.MinVolume = 1;
  f.ContingentCondition = THOST_FTDC_CC_Immediately;
  f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
  f.IsAutoSuspend = 0;
  f.UserForceClose = 0;

  if (api_->ReqOrderInsert(&f, ++req_id_) != 0) bounce_new(r, ExecReason::SessionDown);
}

inline void TraderClient::cancel(const CancelReq& c) {
  if (state() != SessState::Up) {
    bounce_cancel(c, ExecReason::SessionDown);
    return;
  }
  const std::uint64_t seq = c.cl_id & 0xFFFF'FFFF'FFFFull;
  OrderCtx snapshot{};
  {
    std::lock_guard lk(reg_mtx_);
    const auto it = orders_.find(seq);
    if (it == orders_.end()) {
      bounce_cancel(c, ExecReason::UnknownOrder);
      return;
    }
    snapshot = it->second;
  }

  CThostFtdcInputOrderActionField f{};
  std::snprintf(f.BrokerID, sizeof(f.BrokerID), "%s", cfg_.broker_id.c_str());
  std::snprintf(f.InvestorID, sizeof(f.InvestorID), "%s", cfg_.investor_id.c_str());
  std::snprintf(f.UserID, sizeof(f.UserID), "%s", cfg_.user_id.c_str());
  std::snprintf(f.InstrumentID, sizeof(f.InstrumentID), "%s", snapshot.symbol);
  std::snprintf(f.ExchangeID, sizeof(f.ExchangeID), "%s", snapshot.exchange);
  order_ref_from_seq(seq, f.OrderRef);
  f.FrontID = front_id_;
  f.SessionID = session_id_;
  if (snapshot.sys_id[0])
    std::snprintf(f.OrderSysID, sizeof(f.OrderSysID), "%s", snapshot.sys_id);
  f.ActionFlag = THOST_FTDC_AF_Delete;

  if (api_->ReqOrderAction(&f, ++req_id_) != 0) bounce_cancel(c, ExecReason::SessionDown);
}

inline void TraderClient::add_instrument(InstrumentId id, const char* symbol, const char* exchange) {
  InstrumentWire w{};
  w.inst = id;
  std::snprintf(w.symbol, sizeof(w.symbol), "%s", symbol);
  std::snprintf(w.exchange, sizeof(w.exchange), "%s", exchange);
  wires_.push_back(w);
}

inline void TraderClient::adopt_order(OrderId cl_id, InstrumentId inst, StrategyId strategy, AccountId account,
                 Side side, Offset offset, Price px, Qty qty, Qty cum,
                 const char* symbol, const char* exchange, const char* sys_id) {
  const std::uint64_t seq = cl_id & 0xFFFF'FFFF'FFFFull;
  OrderCtx ctx{};
  ctx.cl_id = cl_id;
  ctx.inst = inst;
  ctx.strategy = strategy;
  ctx.account = account;
  ctx.side = side;
  ctx.offset = offset;
  ctx.px = px;
  ctx.qty = qty;
  ctx.cum = cum;
  ctx.acked = true;
  ctx.adopted = true;
  ctx.venue_no = venue_no_from_sys_id(sys_id);
  std::snprintf(ctx.sys_id, sizeof(ctx.sys_id), "%s", sys_id);
  std::snprintf(ctx.symbol, sizeof(ctx.symbol), "%s", symbol);
  std::snprintf(ctx.exchange, sizeof(ctx.exchange), "%s", exchange);
  std::lock_guard lk(reg_mtx_);
  orders_[seq] = ctx;
}

inline void TraderClient::OnFrontConnected() {
  logf("[td] front connected %s\n", cfg_.front.c_str());
  set_state(was_up_ ? SessState::Reconciling : SessState::LoggingIn);
  CThostFtdcReqAuthenticateField f{};
  std::snprintf(f.BrokerID, sizeof(f.BrokerID), "%s", cfg_.broker_id.c_str());
  std::snprintf(f.UserID, sizeof(f.UserID), "%s", cfg_.user_id.c_str());
  std::snprintf(f.AppID, sizeof(f.AppID), "%s", cfg_.app_id.c_str());
  std::snprintf(f.AuthCode, sizeof(f.AuthCode), "%s", cfg_.auth_code.c_str());
  api_->ReqAuthenticate(&f, ++req_id_);
}

inline void TraderClient::OnFrontDisconnected(int nReason) {
  logf("[td] front disconnected, reason 0x%x (api auto-reconnects)\n", nReason);
  set_state(SessState::Down);
}

inline void TraderClient::OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* info,
                       int, bool) {
  if (info && info->ErrorID != 0) {
    logf("[td] authenticate failed: %d %s\n", info->ErrorID, info->ErrorMsg);
    return;
  }
  CThostFtdcReqUserLoginField f{};
  std::snprintf(f.BrokerID, sizeof(f.BrokerID), "%s", cfg_.broker_id.c_str());
  std::snprintf(f.UserID, sizeof(f.UserID), "%s", cfg_.user_id.c_str());
  std::snprintf(f.Password, sizeof(f.Password), "%s", cfg_.password.c_str());
  api_->ReqUserLogin(&f, ++req_id_);
}

inline void TraderClient::OnRspUserLogin(CThostFtdcRspUserLoginField* login, CThostFtdcRspInfoField* info,
                    int, bool) {
  if (info && info->ErrorID != 0) {
    logf("[td] login failed: %d %s\n", info->ErrorID, info->ErrorMsg);
    return;
  }
  if (login) {
    front_id_ = login->FrontID;
    session_id_ = login->SessionID;
    trading_day_ = day_from_ctp(login->TradingDay);
    logf("[td] logged in: trading day %u, front %d session %d\n",
                 trading_day_, front_id_, session_id_);
  }
  CThostFtdcSettlementInfoConfirmField f{};
  std::snprintf(f.BrokerID, sizeof(f.BrokerID), "%s", cfg_.broker_id.c_str());
  std::snprintf(f.InvestorID, sizeof(f.InvestorID), "%s", cfg_.investor_id.c_str());
  api_->ReqSettlementInfoConfirm(&f, ++req_id_);
}

inline void TraderClient::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*,
                                CThostFtdcRspInfoField* info, int, bool) {
  if (info && info->ErrorID != 0) {
    logf("[td] settlement confirm failed: %d %s\n", info->ErrorID,
                 info->ErrorMsg);
    return;
  }
  logf("[td] settlement confirmed — session up\n");
  was_up_ = true;
  set_state(SessState::Up);
}

inline void TraderClient::OnRspError(CThostFtdcRspInfoField* info, int, bool) {
  if (info) logf("[td] rsp error: %d %s\n", info->ErrorID, info->ErrorMsg);
  // A failed request's terminal response can land here instead of the
  // typed callback — wake any pending query as ERRORED, or it would burn
  // its whole timeout in silence (the "counter not ready yet" symptom).
  qry_error_ = info ? info->ErrorID : -1;
  finish_query();
}

inline void TraderClient::OnRspOrderInsert(CThostFtdcInputOrderField* input, CThostFtdcRspInfoField* info,
                      int, bool) {
  if (!input || !info || info->ErrorID == 0) return;
  logf("[td] order insert rejected: %d %s\n", info->ErrorID, info->ErrorMsg);
  reject_by_ref(input->OrderRef);
}

inline void TraderClient::OnErrRtnOrderInsert(CThostFtdcInputOrderField* input,
                         CThostFtdcRspInfoField* info) {
  if (!input || !info || info->ErrorID == 0) return;
  logf("[td] order insert error rtn: %d %s\n", info->ErrorID, info->ErrorMsg);
  reject_by_ref(input->OrderRef);
}

inline void TraderClient::OnRspOrderAction(CThostFtdcInputOrderActionField* action, CThostFtdcRspInfoField* info,
                      int, bool) {
  if (!action || !info || info->ErrorID == 0) return;
  logf("[td] order action rejected: %d %s\n", info->ErrorID, info->ErrorMsg);
  cancel_reject_by_ref(action->OrderRef);
}

inline void TraderClient::OnErrRtnOrderAction(CThostFtdcOrderActionField* action,
                         CThostFtdcRspInfoField* info) {
  if (!action || !info || info->ErrorID == 0) return;
  logf("[td] order action error rtn: %d %s\n", info->ErrorID, info->ErrorMsg);
  cancel_reject_by_ref(action->OrderRef);
}

inline void TraderClient::OnRtnOrder(CThostFtdcOrderField* o) {
  if (!o) return;
  const std::uint64_t seq = seq_from_order_ref(o->OrderRef);
  if (seq == 0) return;

  std::lock_guard lk(reg_mtx_);
  const auto it = orders_.find(seq);
  if (it == orders_.end()) return;
  OrderCtx& ctx = it->second;
  // The private flow is investor-wide: every session (other shards, a
  // crashed predecessor's orders) arrives here. Ours = this session's, or
  // an ADOPTED order (crash recovery) identified by its exchange order id.
  const bool own = o->FrontID == front_id_ && o->SessionID == session_id_;
  const bool adopted =
      ctx.adopted && ctx.sys_id[0] != '\0' && std::strcmp(ctx.sys_id, o->OrderSysID) == 0;
  if (!own && !adopted) return;
  if (ctx.terminal) return;

  // Venue acceptance: the first return carrying an exchange order id.
  if (!ctx.acked && o->OrderSysID[0] != '\0') {
    ctx.acked = true;
    ctx.venue_no = venue_no_from_sys_id(o->OrderSysID);
    std::snprintf(ctx.sys_id, sizeof(ctx.sys_id), "%s", o->OrderSysID);
    publish_report(make_report(ctx, ExecType::Ack, 0, 0, ctx.qty - ctx.cum));
  }

  if (o->OrderStatus == THOST_FTDC_OST_Canceled) {
    ctx.terminal = true;
    if (o->OrderSubmitStatus == THOST_FTDC_OSS_InsertRejected) {
      logf("[td] order %llu rejected by exchange: %s\n",
                   static_cast<unsigned long long>(seq), o->StatusMsg);
      publish_report(make_report(ctx, ExecType::Reject, 0, 0, 0));
    } else {
      publish_report(make_report(ctx, ExecType::Canceled, 0, 0, 0));
    }
  } else if (o->OrderStatus == THOST_FTDC_OST_AllTraded) {
    ctx.terminal = true;  // final fill arrives via OnRtnTrade
  }
}

inline void TraderClient::OnRtnTrade(CThostFtdcTradeField* t) {
  if (!t) return;
  const std::uint64_t seq = seq_from_order_ref(t->OrderRef);
  if (seq == 0) return;

  std::lock_guard lk(reg_mtx_);
  const auto it = orders_.find(seq);
  if (it == orders_.end()) return;
  OrderCtx& ctx = it->second;
  // Trade rows carry no session ids and OrderRefs are small numbers every
  // session reuses — another shard's trade with the same ref number MUST
  // NOT credit our order. Fills only ever follow our ack (or an adoption),
  // both of which recorded the venue-unique OrderSysID: require the match.
  if (!ctx.acked || ctx.sys_id[0] == '\0' ||
      std::strcmp(ctx.sys_id, t->OrderSysID) != 0)
    return;
  ctx.cum += t->Volume;
  const Qty leaves = ctx.qty - ctx.cum;
  ExecReport r = make_report(ctx, ExecType::Fill, px_from_ctp(t->Price), t->Volume,
                             leaves > 0 ? leaves : 0);
  r.offset = offset_from_ctp(t->OffsetFlag);
  publish_report(r);
}

inline const TraderClient::InstrumentWire* TraderClient::wire_for(InstrumentId id) const noexcept {
  for (const auto& w : wires_)
    if (w.inst == id) return &w;
  return nullptr;
}

inline [[nodiscard]] bool TraderClient::trade_is_ours(const CThostFtdcTradeField* t) {
  const std::uint64_t seq = seq_from_order_ref(t->OrderRef);
  if (seq == 0) return false;
  std::lock_guard lk(reg_mtx_);
  const auto it = orders_.find(seq);
  return it != orders_.end() && it->second.sys_id[0] != '\0' &&
         std::strcmp(it->second.sys_id, t->OrderSysID) == 0;
}

inline ExecReport TraderClient::make_report(const OrderCtx& ctx, ExecType type, Price fill_px, Qty fill_qty,
                       Qty leaves) {
  ExecReport r{};
  r.cl_id = ctx.cl_id;
  r.venue_no = ctx.venue_no;
  r.ts = now_();
  r.trigger_ts = ctx.trigger_ts;
  r.px = type == ExecType::Fill ? fill_px : ctx.px;
  r.qty = fill_qty;
  r.leaves = leaves;
  r.cum = ctx.cum;
  r.inst = ctx.inst;
  r.strategy = ctx.strategy;
  r.account = ctx.account;
  r.type = type;
  r.side = ctx.side;
  r.offset = ctx.offset;
  r.reason = ExecReason::None;
  return r;
}

inline void TraderClient::bounce_new(const NewOrderReq& r, ExecReason why) {
  ExecReport rej{};
  rej.cl_id = r.cl_id;
  rej.ts = now_();
  rej.trigger_ts = r.trigger_ts;
  rej.px = r.px;
  rej.inst = r.inst;
  rej.strategy = r.strategy;
  rej.account = r.account;
  rej.type = ExecType::Reject;
  rej.side = r.side;
  rej.offset = r.offset;
  rej.reason = why;
  publish_report(rej);
}

inline void TraderClient::bounce_cancel(const CancelReq& c, ExecReason why) {
  ExecReport rej{};
  rej.cl_id = c.cl_id;
  rej.ts = now_();
  rej.inst = c.inst;
  rej.strategy = c.strategy;
  rej.account = c.account;
  rej.type = ExecType::CancelReject;
  rej.reason = why;
  publish_report(rej);
}

inline void TraderClient::reject_by_ref(const char* order_ref) {
  const std::uint64_t seq = seq_from_order_ref(order_ref);
  if (seq == 0) return;
  std::lock_guard lk(reg_mtx_);
  const auto it = orders_.find(seq);
  if (it == orders_.end() || it->second.terminal) return;
  it->second.terminal = true;
  ExecReport r = make_report(it->second, ExecType::Reject, 0, 0, 0);
  r.reason = ExecReason::Other;
  publish_report(r);
}

inline void TraderClient::cancel_reject_by_ref(const char* order_ref) {
  const std::uint64_t seq = seq_from_order_ref(order_ref);
  if (seq == 0) return;
  std::lock_guard lk(reg_mtx_);
  const auto it = orders_.find(seq);
  if (it == orders_.end()) return;
  const OrderCtx& ctx = it->second;
  ExecReport r{};
  r.cl_id = ctx.cl_id;
  r.ts = now_();
  r.inst = ctx.inst;
  r.strategy = ctx.strategy;
  r.account = ctx.account;
  r.type = ExecType::CancelReject;
  r.reason = ExecReason::Other;
  publish_report(r);
}

inline void TraderClient::publish_report(const ExecReport& r) {
  std::lock_guard lk(ring_mtx_);
  writer_.publish(make_envelope(0, r.ts, r, Origin::Source, 0));
}

inline void TraderClient::set_state(SessState s) {
  {
    std::lock_guard lk(state_mtx_);
    state_ = s;
    state_atomic_.store(s, std::memory_order_release);
  }
  state_cv_.notify_all();
  SessionStateEvt e{};
  e.ts = now_();
  e.venue = Venue::SHFE;  // session is per-counter, not per-venue; tag is cosmetic
  e.state = s;
  std::lock_guard lk(ring_mtx_);
  writer_.publish(make_envelope(0, e.ts, e, Origin::Source, 0));
}

}  // namespace plat::ctp
