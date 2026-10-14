#pragma once
// plat/rec/inst_enum.hpp — a minimal CTP trader session that does ONE job:
// enumerate every instrument the counter serves, so the recorder knows what
// to subscribe to. It is deliberately NOT the trading TraderClient (no OMS,
// no gateway, no order path, no report ring) — the recorder is a lightweight
// tool and pulls in only what enumeration needs: authenticate → login →
// settlement confirm → ReqQryInstrument(empty id) → the symbol/exchange list.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ThostFtdcTraderApi.h"
#include "plat/core/log.hpp"
#include "plat/ctp/conv.hpp"
#include "plat/ctp/gbk.hpp"

namespace plat::rec {

struct EnumConfig {
  std::string front, broker_id, user_id, password, investor_id, app_id, auth_code, flow_path;
};

struct EnumInstrument {
  char symbol[32]{};
  char exchange[9]{};
  double price_tick = 0;      // CNY
  int volume_multiple = 1;    // contract multiplier
  double long_margin_ratio = 0;  // 0.12 = 12%
};

class InstEnumerator final : public CThostFtdcTraderSpi {
 public:
  explicit InstEnumerator(EnumConfig cfg) : cfg_(std::move(cfg)) {}
  InstEnumerator(const InstEnumerator&) = delete;
  InstEnumerator& operator=(const InstEnumerator&) = delete;
  ~InstEnumerator() { stop(); }

  void start() {
    api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(cfg_.flow_path.c_str());
    api_->RegisterSpi(this);
    api_->SubscribePrivateTopic(THOST_TERT_QUICK);
    api_->SubscribePublicTopic(THOST_TERT_QUICK);
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

  [[nodiscard]] bool wait_until_up(int timeout_ms) {
    std::unique_lock lk(mtx_);
    return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return up_; });
  }
  [[nodiscard]] std::uint32_t trading_day() const noexcept { return trading_day_; }

  // Blocks until the counter finishes the (multi-callback) instrument dump.
  //
  // Two subtleties this handles:
  //   · the SEND is throttled (CTP returns -1 no-net / -2 too-many-
  //     outstanding / -3 per-second-flow — the whole-market query is heavy
  //     and bounces the first tries); retry on the ~1/sec query cadence.
  //   · completion is NOT assumed from bIsLast alone — some counters stream
  //     the entire market and never set it on this query. Completion is
  //     **quiescence**: instruments arrive, then stop for `quiet_ms`. bIsLast
  //     (when the counter does send it) is just the fast path.
  std::vector<EnumInstrument> query_all(int cycles = 6) {
    constexpr int kQuietMs = 3'000;      // stream considered done after this gap
    constexpr int kMaxWaitMs = 120'000;  // absolute cap per cycle
    for (int c = 1; c <= cycles; ++c) {
      {
        std::lock_guard lk(mtx_);
        instruments_.clear();
        done_ = false;
        errored_ = false;
        finished_ = false;
      }
      // 1) Get the send accepted (persistent -1 = no counter connection).
      int ret = -1;
      for (int a = 1; a <= 30; ++a) {
        ret = api_->ReqQryInstrument(&empty_query_, ++req_id_);
        if (ret == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1'100));
      }
      if (ret != 0) {
        logf("[enum] instrument query not accepted (ret %d) — counter unreachable?\n", ret);
        return {};
      }

      // 2) Watch the stream: poll the count; done on bIsLast, on quiescence,
      //    or on error (refused — re-issue). Progress is logged so a slow
      //    counter looks alive, not hung. `harvest()` copies the list under
      //    the lock and latches `finished_` so a counter that keeps firing
      //    callbacks after bIsLast can no longer mutate the vector we return
      //    (that concurrent push-vs-copy was the crash).
      std::size_t last_count = 0;
      int quiet_ms = 0, waited_ms = 0, since_log_ms = 0;
      bool refused = false;
      while (waited_ms < kMaxWaitMs) {
        std::size_t count;
        bool fin;
        {
          std::unique_lock lk(mtx_);
          cv_.wait_for(lk, std::chrono::milliseconds(250),
                       [this] { return done_ || errored_; });
          count = instruments_.size();
          fin = done_;
          refused = errored_ && instruments_.empty();
        }
        if (refused) break;                          // nothing received: re-issue
        if (fin && count > 0) return harvest(count, "bIsLast");        // fast path
        if (count == last_count && count > 0) {
          quiet_ms += 250;
          if (quiet_ms >= kQuietMs) return harvest(count, "quiescence");
        } else {
          quiet_ms = 0;
        }
        last_count = count;
        waited_ms += 250;
        since_log_ms += 250;
        if (since_log_ms >= 5'000) {                 // heartbeat every 5s
          logf("[enum] receiving instruments … %zu so far\n", count);
          since_log_ms = 0;
        }
      }
      logf("[enum] instrument dump %s (cycle %d/%d) — retrying\n",
           refused ? "refused by counter" : "stalled", c, cycles);
      std::this_thread::sleep_for(std::chrono::milliseconds(1'500 * c));
    }
    std::lock_guard lk(mtx_);
    return instruments_;
  }

 private:
  // Copy the accumulated list under the lock and latch `finished_`, so any
  // stray post-bIsLast callback is dropped instead of racing the copy.
  std::vector<EnumInstrument> harvest(std::size_t count, const char* how) {
    logf("[enum] instrument dump complete: %zu instruments (%s)\n", count, how);
    std::lock_guard lk(mtx_);
    finished_ = true;
    return instruments_;
  }

 public:

  // ------------------------------------------------------- CTP callbacks

  void OnFrontConnected() override {
    logf("[enum] front connected %s\n", cfg_.front.c_str());
    CThostFtdcReqAuthenticateField f{};
    put(f.BrokerID, cfg_.broker_id);
    put(f.UserID, cfg_.user_id);
    put(f.AppID, cfg_.app_id);
    put(f.AuthCode, cfg_.auth_code);
    api_->ReqAuthenticate(&f, ++req_id_);
  }
  void OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* info, int,
                         bool) override {
    if (err(info, "authenticate")) return;
    CThostFtdcReqUserLoginField f{};
    put(f.BrokerID, cfg_.broker_id);
    put(f.UserID, cfg_.user_id);
    put(f.Password, cfg_.password);
    api_->ReqUserLogin(&f, ++req_id_);
  }
  void OnRspUserLogin(CThostFtdcRspUserLoginField* login, CThostFtdcRspInfoField* info, int,
                      bool) override {
    if (err(info, "login")) return;
    if (login) trading_day_ = ctp::day_from_ctp(login->TradingDay);
    logf("[enum] logged in, trading day %u\n", trading_day_);
    CThostFtdcSettlementInfoConfirmField f{};
    put(f.BrokerID, cfg_.broker_id);
    put(f.InvestorID, cfg_.investor_id.empty() ? cfg_.user_id : cfg_.investor_id);
    api_->ReqSettlementInfoConfirm(&f, ++req_id_);
  }
  void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*,
                                  CThostFtdcRspInfoField* info, int, bool) override {
    if (err(info, "settlement confirm")) return;
    {
      std::lock_guard lk(mtx_);
      up_ = true;
    }
    cv_.notify_all();
  }
  void OnRspQryInstrument(CThostFtdcInstrumentField* in, CThostFtdcRspInfoField* info, int,
                          bool bIsLast) override {
    std::lock_guard lk(mtx_);
    if (finished_) return;  // query_all already harvested; drop stray callbacks
    if (in && (!info || info->ErrorID == 0) && in->InstrumentID[0]) {
      EnumInstrument e{};
      std::snprintf(e.symbol, sizeof(e.symbol), "%s", in->InstrumentID);
      std::snprintf(e.exchange, sizeof(e.exchange), "%s", in->ExchangeID);
      e.price_tick = in->PriceTick;
      e.volume_multiple = in->VolumeMultiple;
      e.long_margin_ratio = in->LongMarginRatio;
      instruments_.push_back(e);
    }
    if (bIsLast) {
      done_ = true;
      cv_.notify_all();
    }
  }
  void OnRspError(CThostFtdcRspInfoField* info, int, bool) override {
    if (info) logf("[enum] rsp error: %d %s\n", info->ErrorID, ctp::gbk_to_utf8(info->ErrorMsg).c_str());
    {
      std::lock_guard lk(mtx_);
      errored_ = true;  // this query was refused — query_all will re-issue it
      done_ = true;     // wake the waiter rather than let it time out
    }
    cv_.notify_all();
  }
  void OnFrontDisconnected(int reason) override {
    logf("[enum] front disconnected 0x%x\n", reason);
  }

 private:
  template <std::size_t N>
  static void put(char (&dst)[N], const std::string& s) {
    std::snprintf(dst, N, "%s", s.c_str());
  }
  static bool err(const CThostFtdcRspInfoField* info, const char* what) {
    if (info && info->ErrorID != 0) {
      logf("[enum] %s failed: %d %s\n", what, info->ErrorID, ctp::gbk_to_utf8(info->ErrorMsg).c_str());
      return true;
    }
    return false;
  }

  EnumConfig cfg_;
  CThostFtdcTraderApi* api_ = nullptr;
  CThostFtdcQryInstrumentField empty_query_{};  // empty id = every instrument
  std::mutex mtx_;
  std::condition_variable cv_;
  std::vector<EnumInstrument> instruments_;
  std::uint32_t trading_day_ = 0;
  int req_id_ = 0;
  bool up_ = false;
  bool done_ = false;
  bool errored_ = false;
  bool finished_ = false;  // query_all harvested the list; ignore later callbacks
};

}  // namespace plat::rec
