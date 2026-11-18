#pragma once

#include <span>
#include <utility>

#include "hft/common/spsc_ring.hpp"
#include "plat/engine/site.hpp"

namespace plat {

enum class RunnerAction : std::uint8_t { Place, Cancel };
enum class IntentStatus : std::uint8_t {
  Processed, Invalid, Fenced, Stale, AccountUnavailable, RiskDenied
};

struct RunnerIntent {
  std::uint64_t generation;
  std::uint64_t request_seq;
  std::uint64_t feed_seq;  // zero for triggers independent of market data
  Nanos trigger_ts;       // ingress timestamp in the account engine's clock domain
  OrderId cancel_id;
  Price px;
  Qty qty;
  InstrumentId inst;
  RunnerAction action;
  Side side;
  Offset offset;
  Tif tif;
};
static_assert(sizeof(RunnerIntent) == 64);

struct IntentOutcome {
  std::uint64_t request_seq;
  OrderId cl_id;
  IntentStatus status;
  RiskCheck check{};
  std::uint8_t pad_[6]{};
};

struct RunnerMailbox {
  hft::SpscRing<RunnerIntent, 256> intents;
  hft::SpscRing<IntentOutcome, 256> outcomes;
  hft::SpscRing<ExecReport, 256> reports;
};

// Engine-owned binding for a single mapped runner mailbox. A restart uses a
// new mailbox and generation. Polling a fenced or slow runner never waits.
struct RunnerBinding {
  RunnerMailbox& mailbox;
  StrategyId strategy;
  std::uint64_t generation;
  std::uint64_t last_request = 0;
  bool fenced = false;
};

template <OrderGateway Counter, AnyStrategy... Strats>
class AccountEngine : public Site<Counter, Strats...> {
  using Core = Site<Counter, Strats...>;

 public:
  using Core::Core;
  AccountEngine(const InstrumentTable& table, SiteConfig cfg, typename Core::Mode mode,
                std::tuple<std::vector<Strats>...> banks, AccountId account, Nanos max_trigger_age)
      : Core(table, std::move(cfg), mode, std::move(banks)), account_(account),
        max_trigger_age_(max_trigger_age) {}

  AccountEngine(const InstrumentTable& table, SiteConfig cfg, typename Core::Mode mode,
                AccountId account, Nanos max_trigger_age)
      : Core(table, account_config(std::move(cfg)), mode), account_(account),
        max_trigger_age_(max_trigger_age) {}

  // Call after applying a feed event to the authoritative books.
  void market_applied(std::uint64_t seq) noexcept { market_seq_ = seq; }

  // Apply reports even if a runner died, disconnected or stopped reading.
  // Resolve ownership from OMS so the private queue cannot be selected by
  // an untrusted attribution field in a report.
  [[nodiscard]] bool receive_report(ExecReport report, std::span<RunnerBinding> runners,
                                    Nanos now, Nanos received = 0) {
    const OmsOrder* order = this->oms().find(report.cl_id);
    if (!order || order->account != account_) return false;
    report.account = order->account;
    report.strategy = order->strategy;
    report.inst = order->inst;
    this->source(report, now, received);
    for (auto& runner : runners) {
      if (runner.strategy != report.strategy) continue;
      if (!runner.fenced && !runner.mailbox.reports.try_push(report)) runner.fenced = true;
      break;
    }
    return true;
  }

  // One intent per visit, so callers can round-robin across runner mailboxes.
  [[nodiscard]] bool poll(RunnerBinding& runner, Nanos now) {
    if (runner.fenced) return false;
    const RunnerIntent* req = runner.mailbox.intents.front();
    if (!req) return false;
    if (!runner.mailbox.outcomes.can_push()) {
      runner.fenced = true;
      return false;
    }
    IntentOutcome outcome{req->request_seq, kNoOrder, IntentStatus::Invalid};
    if (req->generation != runner.generation || req->request_seq <= runner.last_request) {
      outcome.status = IntentStatus::Fenced;
    } else if (req->action == RunnerAction::Place) {
      if (req->feed_seq != 0) {
        if (req->trigger_ts <= 0 || now - req->trigger_ts > max_trigger_age_) {
          outcome.status = IntentStatus::Stale;
        } else if (req->trigger_ts > now || req->feed_seq > market_seq_) {
          return false;
        }
      }
      if (outcome.status != IntentStatus::Stale && valid_order(*req)) {
        outcome.status = IntentStatus::AccountUnavailable;
        if (this->armed() && this->trading_permitted() &&
            this->session_state() == SessState::Up) {
          submit(runner.strategy, *req, now);
          outcome.cl_id = this->last_strategy_order_cl_id();
          outcome.status = IntentStatus::Processed;
          if (const auto& deny = this->last_deny(); deny.cl_id == outcome.cl_id) {
            outcome.status = IntentStatus::RiskDenied;
            outcome.check = deny.check;
          }
        }
      }
    } else if (req->action == RunnerAction::Cancel) {
      const OmsOrder* order = this->oms().find(req->cancel_id);
      if (order && order->open() && order->account == account_ &&
          order->strategy == runner.strategy) {
        this->source(StrategyCancelCmd{req->generation, req->request_seq, req->cancel_id,
                                      runner.strategy, account_}, now);
        outcome.cl_id = req->cancel_id;
        outcome.status = IntentStatus::Processed;
      }
    }
    if (outcome.status != IntentStatus::Fenced) runner.last_request = req->request_seq;
    runner.mailbox.intents.pop();
    if (!runner.mailbox.outcomes.try_push(outcome)) runner.fenced = true;
    return true;
  }

 private:
  static SiteConfig account_config(SiteConfig cfg) {
    cfg.shared_account_risk = true;
    return cfg;
  }

  [[nodiscard]] bool valid_order(const RunnerIntent& req) const noexcept {
    return req.inst < this->table().size() && req.px > 0 && req.qty > 0 &&
           req.side <= Side::Sell && req.offset <= Offset::CloseYesterday && req.tif <= Tif::Ioc;
  }

  void submit(StrategyId strategy, const RunnerIntent& req, Nanos now) {
    this->source(StrategyOrderCmd{req.generation, req.request_seq, req.feed_seq, req.trigger_ts,
                                 req.px, req.qty, req.inst, strategy, account_, req.side,
                                 req.offset, req.tif}, now);
  }

  AccountId account_ = 0;
  Nanos max_trigger_age_ = 0;
  std::uint64_t market_seq_ = 0;
};

}  // namespace plat
