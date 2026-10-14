#pragma once
// plat/ctp/md_client.hpp — the production market-data adapter: SFIT CTP
// (thostmduserapi_se) → MdSnapshotEvt envelopes on the broadcast ring.
//
// Threading model (§7.1): CTP delivers every callback on the api's own
// network thread; the only thing that thread does here is convert the depth
// record to the platform snapshot and publish it to the ring. The ring is
// the seqbus broadcast protocol — single writer, any number of independent
// readers — though the trading topology attaches exactly ONE consumer: the
// engine thread, which seals each event into the canonical stream and fans
// it out to every strategy in-process. Strategy fan-out happens *after* the
// single sequencing point, never at the transport — that is what makes the
// journal a total order and replay byte-identical (§7.1/§11.1). Additional
// readers (capture, telemetry, a mirror process over shm) attach with their
// own cursors without the writer or the engine ever knowing.
//
// CTP transport is TCP with no application sequence numbers, so there is no
// A/B arbitration at this layer; per-instrument synthesized feed_seq gives
// FutBook its stale/duplicate rejection. Front disconnects are not gaps:
// futures books re-baseline on the next snapshot by construction (§8.1), and
// the riskgate stale-data interlock (#11) refuses trading while snapshots
// are missing.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ThostFtdcMdApi.h"
#include "hft/common/platform.hpp"
#include "plat/bus/ring.hpp"
#include "plat/core/log.hpp"
#include "plat/codec/msgs.hpp"
#include "plat/core/types.hpp"
#include "plat/ctp/conv.hpp"
#include "plat/ctp/gbk.hpp"

namespace plat::ctp {

struct MdConfig {
  std::string front;        // "tcp://ip:port"
  std::string broker_id;
  std::string user_id;
  std::string password;
  std::string flow_path;    // directory for CTP's .con state files
};

class MdClient final : public CThostFtdcMdSpi {
 public:
  // now_fn: the driver's session clock (ns since midnight) — snapshots are
  // stamped with local receive time so the staleness interlock measures our
  // own data age, immune to venue/host clock skew.
  using NowFn = Nanos (*)();

  MdClient(MdConfig cfg, Ring& out, NowFn now_fn) : cfg_(std::move(cfg)), writer_(out), now_(now_fn) {}
  MdClient(const MdClient&) = delete;
  MdClient& operator=(const MdClient&) = delete;
  ~MdClient() { stop(); }

  // Instruments must be registered before start(): the callback thread does
  // an allocation-free linear match (subscription lists are tens of names).
  void add_instrument(const char* symbol, InstrumentId id, Venue venue) {
    Sub s{};
    std::snprintf(s.symbol, sizeof(s.symbol), "%s", symbol);
    std::memcpy(s.w, s.symbol, sizeof(s.w));
    s.id = id;
    s.venue = venue;
    subs_.push_back(s);
  }

  void start() {
    api_ = CThostFtdcMdApi::CreateFtdcMdApi(cfg_.flow_path.c_str());
    api_->RegisterSpi(this);
    api_->RegisterFront(const_cast<char*>(cfg_.front.c_str()));
    api_->Init();
  }

  void stop() {
    if (api_) {
      api_->RegisterSpi(nullptr);
      api_->Release();
      api_ = nullptr;
    }
  }

  [[nodiscard]] bool logged_in() const noexcept { return logged_in_.load(std::memory_order_acquire); }
  [[nodiscard]] std::uint64_t snapshots() const noexcept { return snapshots_.load(std::memory_order_relaxed); }

  // ------------------------------------------------------- CTP callbacks

  void OnFrontConnected() override {
    logf("[md] front connected %s\n", cfg_.front.c_str());
    CThostFtdcReqUserLoginField req{};
    std::snprintf(req.BrokerID, sizeof(req.BrokerID), "%s", cfg_.broker_id.c_str());
    std::snprintf(req.UserID, sizeof(req.UserID), "%s", cfg_.user_id.c_str());
    std::snprintf(req.Password, sizeof(req.Password), "%s", cfg_.password.c_str());
    api_->ReqUserLogin(&req, ++req_id_);
  }

  void OnFrontDisconnected(int nReason) override {
    logged_in_.store(false, std::memory_order_release);
    logf("[md] front disconnected, reason 0x%x (api auto-reconnects)\n", nReason);
  }

  void OnRspUserLogin(CThostFtdcRspUserLoginField* login, CThostFtdcRspInfoField* info,
                      int, bool) override {
    if (info && info->ErrorID != 0) {
      logf("[md] login failed: %d %s\n", info->ErrorID, ctp::gbk_to_utf8(info->ErrorMsg).c_str());
      return;
    }
    logf("[md] logged in, trading day %s\n",
                 login ? login->TradingDay : "?");
    logged_in_.store(true, std::memory_order_release);
    subscribe_all();
  }

  void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* inst,
                          CThostFtdcRspInfoField* info, int, bool) override {
    if (info && info->ErrorID != 0)
      logf("[md] subscribe %s failed: %d %s\n",
                   inst ? inst->InstrumentID : "?", info->ErrorID, ctp::gbk_to_utf8(info->ErrorMsg).c_str());
    else
      logf("[md] subscribe %s acked — waiting on OnRtnDepthMarketData\n",
                   inst ? inst->InstrumentID : "?");
  }

  void OnRspError(CThostFtdcRspInfoField* info, int, bool) override {
    if (info) logf("[md] rsp error: %d %s\n", info->ErrorID, ctp::gbk_to_utf8(info->ErrorMsg).c_str());
  }

  void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* d) override {
    if (!d) return;
    Sub* sub = find(d->InstrumentID);
    if (!sub) return;

    MdSnapshotEvt s{};
    s.feed_seq = ++sub->seq;
    s.ts = now_();
    s.bid_px[0] = px_from_ctp(d->BidPrice1);  s.bid_qty[0] = d->BidVolume1;
    s.bid_px[1] = px_from_ctp(d->BidPrice2);  s.bid_qty[1] = d->BidVolume2;
    s.bid_px[2] = px_from_ctp(d->BidPrice3);  s.bid_qty[2] = d->BidVolume3;
    s.bid_px[3] = px_from_ctp(d->BidPrice4);  s.bid_qty[3] = d->BidVolume4;
    s.bid_px[4] = px_from_ctp(d->BidPrice5);  s.bid_qty[4] = d->BidVolume5;
    s.ask_px[0] = px_from_ctp(d->AskPrice1);  s.ask_qty[0] = d->AskVolume1;
    s.ask_px[1] = px_from_ctp(d->AskPrice2);  s.ask_qty[1] = d->AskVolume2;
    s.ask_px[2] = px_from_ctp(d->AskPrice3);  s.ask_qty[2] = d->AskVolume3;
    s.ask_px[3] = px_from_ctp(d->AskPrice4);  s.ask_qty[3] = d->AskVolume4;
    s.ask_px[4] = px_from_ctp(d->AskPrice5);  s.ask_qty[4] = d->AskVolume5;
    s.last_px = px_from_ctp(d->LastPrice);
    s.day_volume = d->Volume;
    s.open_interest = static_cast<Qty>(d->OpenInterest);
    s.inst = sub->id;
    s.venue = sub->venue;

    writer_.publish(make_envelope(0, s.ts, s, Origin::Source, 0));
    snapshots_.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  // Symbols as zero-padded u64 wire words (the symtab lab's trick): the
  // per-tick lookup is four u64 compares per candidate, no strcmp loop.
  struct Sub {
    std::uint64_t w[4];     // zero-padded symbol, 32 bytes
    char symbol[32];        // original spelling for the subscribe call
    InstrumentId id;
    Venue venue;
    std::uint64_t seq = 0;  // synthesized per-instrument feed sequence
  };

  void subscribe_all() {
    std::vector<char*> names;
    names.reserve(subs_.size());
    for (auto& s : subs_) names.push_back(s.symbol);
    if (!names.empty())
      api_->SubscribeMarketData(names.data(), static_cast<int>(names.size()));
  }

  [[nodiscard]] HFT_FORCE_INLINE Sub* find(const char* symbol) noexcept {
    std::uint64_t w[4] = {};
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%s", symbol);
    std::memcpy(w, buf, sizeof(w));
    for (auto& s : subs_) {
      if (((s.w[0] == w[0]) & (s.w[1] == w[1]) & (s.w[2] == w[2]) & (s.w[3] == w[3])) != 0)
        return &s;
    }
    return nullptr;
  }

  MdConfig cfg_;
  Ring::Writer writer_;
  NowFn now_;
  CThostFtdcMdApi* api_ = nullptr;
  std::vector<Sub> subs_;
  std::atomic<bool> logged_in_{false};
  std::atomic<std::uint64_t> snapshots_{0};
  int req_id_ = 0;
};

}  // namespace plat::ctp
