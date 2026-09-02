#pragma once
// platops_app.hpp — the mirror layer of the platops web console (§14): one
// MirrorShard per platd shard, each a Replay-mode PackSite fed by tailing
// the shard's live journal (plat/ops/tail.hpp). Zero trading-path impact:
// platd is untouched; the mirror regenerates OMS / PnL / risk state
// deterministically from the journaled Source prefix exactly like
// `platd --replay`, a few milliseconds behind live.
//
// Control goes the other way through the existing cmd-file channel: the web
// UI POSTs an operator line, platops writes `<shard>.cmd`, platd's driver
// picks it up within 100 ms and injects it as a journaled Source envelope —
// which then flows right back into this mirror as the audit record.

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "plat/bus/envelope.hpp"
#include "plat/core/config.hpp"
#include "plat/core/types.hpp"
#include "plat/ops/tail.hpp"
#include "plat/replay/sidecar.hpp"
#include "strat_params.hpp"

namespace platops {

using namespace platcfg;

// Sink-only gateway: Replay mode never sends, but the Site wants the type.
struct MirrorGateway {
  void place(const NewOrderReq&) {}
  void cancel(const CancelReq&) {}
  [[nodiscard]] SessState state() const { return SessState::Up; }
};

using MirrorSite = PackSite<MirrorGateway>;

// Rolling UI feeds, newest first, hard-capped — the console shows recent
// activity; the journal remains the complete record.
struct FillRow {
  Nanos ts;
  Price px;
  Qty qty, leaves;
  InstrumentId inst;
  StrategyId strategy;
  Side side;
  Offset offset;
};
struct DenyRow {
  Nanos ts;
  Price px;
  Qty qty;
  InstrumentId inst;
  StrategyId strategy;
  RiskCheck check;
  bool is_cancel;
};
struct OpsRow {
  Nanos ts;
  std::string text;
};

class MirrorShard {
 public:
  MirrorShard(std::string name, Config cfg)
      : name_(std::move(name)),
        cfg_(std::move(cfg)),
        jnl_path_(cfg_.str("journal", "platd.jnl")),
        cmd_path_(cfg_.str("cmd_file", name_ + ".cmd")),
        account_(static_cast<AccountId>(cfg_.i64("account_id", 1))),
        tail_(jnl_path_) {
    specs_ok_ = parse_strategies(cfg_, account_, specs_);
  }

  // One monitor pass: probe for the journal (and session rotation), build
  // the mirror once journal + sidecar exist, then consume new envelopes.
  bool poll() {
    if (!tail_.probe()) return false;
    if (tail_.generation() != gen_) {  // a fresh session took the path over
      gen_ = tail_.generation();
      site_.reset();
      fills_.clear();
      denies_.clear();
      ops_log_.clear();
      envelopes_ = 0;
      last_env_ts_ = last_md_ts_ = 0;
      phase_ = Phase::Closed;
    }
    if (!site_ && !rebuild()) return false;  // sidecar not written yet — retry
    batch_.clear();
    if (tail_.read(batch_) == 0) return false;
    for (const Envelope& e : batch_) absorb(e);
    return true;
  }

  // ---- read side for the JSON builder (single-threaded with poll()).
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] const std::string& cmd_path() const noexcept { return cmd_path_; }
  [[nodiscard]] const std::string& jnl_path() const noexcept { return jnl_path_; }
  [[nodiscard]] AccountId account() const noexcept { return account_; }
  [[nodiscard]] MirrorSite* site() noexcept { return site_.get(); }
  [[nodiscard]] const InstrumentTable& table() const noexcept { return table_; }
  [[nodiscard]] const std::vector<StratSpec>& specs() const noexcept { return specs_; }
  [[nodiscard]] bool specs_ok() const noexcept { return specs_ok_; }
  [[nodiscard]] const ops::JournalTailer& tail() const noexcept { return tail_; }
  [[nodiscard]] const std::deque<FillRow>& fills() const noexcept { return fills_; }
  [[nodiscard]] const std::deque<DenyRow>& denies() const noexcept { return denies_; }
  [[nodiscard]] const std::deque<OpsRow>& ops_log() const noexcept { return ops_log_; }
  [[nodiscard]] std::uint64_t envelopes() const noexcept { return envelopes_; }
  [[nodiscard]] Nanos last_env_ts() const noexcept { return last_env_ts_; }
  [[nodiscard]] Nanos last_md_ts() const noexcept { return last_md_ts_; }
  [[nodiscard]] Phase phase() const noexcept { return phase_; }
  [[nodiscard]] const std::string& symbol(InstrumentId i) const {
    static const std::string unknown = "?";
    if (i >= sym_cache_.size()) return unknown;
    return sym_cache_[i];
  }

 private:
  bool rebuild() {
    table_ = InstrumentTable{};
    if (!load_sidecar(jnl_path_, table_, /*quiet=*/true)) return false;
    sym_cache_.clear();
    for (InstrumentId i = 0; i < table_.size(); ++i)
      sym_cache_.emplace_back(table_.at(i).sym());
    SiteConfig sc;
    sc.id = static_cast<SiteId>(cfg_.i64("site_id", 1));
    sc.name = name_ + "-mirror";
    sc.day = tail_.header().day;
    for (InstrumentId i = 0; i < table_.size(); ++i) sc.hosted.push_back(i);
    if (table_.size()) sc.venue = table_.at(0).venue;
    sc.limits = limits_from(cfg_);
    sc.fees = fees_from(cfg_);
    // Same key the live site read — order ids must regenerate identically.
    sc.epoch = static_cast<std::uint16_t>(cfg_.i64("epoch", 1));
    site_ = std::make_unique<MirrorSite>(table_, std::move(sc), MirrorSite::Mode::Replay,
                                         Pack::banks(cfg_, table_, specs_));
    return true;
  }

  void absorb(const Envelope& e) {
    ++envelopes_;
    last_env_ts_ = e.ts;
    if (e.origin == Origin::Source) site_->inject(e);  // Derived regenerates inside
    switch (e.schema) {  // UI feeds read the journal record itself
      case Schema::MdSnapshot:
        last_md_ts_ = e.ts;
        break;
      case Schema::PhaseChange:
        phase_ = e.as<PhaseChangeEvt>().phase;
        break;
      case Schema::ExecReport: {
        const auto& r = e.as<ExecReport>();
        if (r.type == ExecType::Fill)
          push(fills_, {r.ts, r.px, r.qty, r.leaves, r.inst, r.strategy, r.side, r.offset},
               100);
        break;
      }
      case Schema::RiskDeny: {
        const auto& d = e.as<RiskDeny>();
        push(denies_, {d.ts, d.px, d.qty, d.inst, d.strategy, d.check, d.is_cancel != 0}, 50);
        break;
      }
      case Schema::ParamUpdate: {
        const auto& u = e.as<ParamUpdateEvt>();
        push_ops(u.ts, "param: strategy " + std::to_string(u.strategy) + " " + u.key + " = " +
                           std::to_string(u.value));
        break;
      }
      case Schema::ManualOrder: {
        const auto& m = e.as<ManualOrderCmd>();
        push_ops(m.ts, "manual order: strategy " + std::to_string(m.strategy) + " " +
                           std::string(side_name(m.side)) + " " + symbol(m.inst) + " x" +
                           std::to_string(m.qty));
        break;
      }
      case Schema::ManualCancel:
        push_ops(e.ts, "manual cancel");
        break;
      case Schema::PositionTransfer: {
        const auto& t = e.as<PositionTransferCmd>();
        push_ops(t.ts, "transfer: " + symbol(t.inst) + " " +
                           (t.side == Side::Buy ? "long" : "short") + " x" +
                           std::to_string(t.qty) + " strategy " +
                           std::to_string(t.from_strategy) + " -> " +
                           std::to_string(t.to_strategy));
        break;
      }
      case Schema::KillSwitch: {
        const auto& k = e.as<KillSwitchCmd>();
        const char* act = k.action == KillAction::Halt      ? "HALT"
                          : k.action == KillAction::Flatten ? "FLATTEN"
                                                            : "disarm";
        const char* scope = k.scope == KillScope::Site      ? "site"
                            : k.scope == KillScope::Strategy ? "strategy"
                            : k.scope == KillScope::Account  ? "account"
                                                             : "firm";
        push_ops(k.ts, std::string(act) + " (" + scope +
                           (k.scope == KillScope::Strategy || k.scope == KillScope::Account
                                ? " " + std::to_string(k.target)
                                : "") +
                           ")");
        break;
      }
      default:
        break;
    }
  }

  template <typename Row>
  static void push(std::deque<Row>& q, Row r, std::size_t cap) {
    q.push_front(std::move(r));
    if (q.size() > cap) q.pop_back();
  }
  void push_ops(Nanos ts, std::string text) {
    ops_log_.push_front({ts, std::move(text)});
    if (ops_log_.size() > 100) ops_log_.pop_back();
  }

  std::string name_;
  Config cfg_;
  std::string jnl_path_, cmd_path_;
  AccountId account_;
  std::vector<StratSpec> specs_;
  bool specs_ok_ = false;
  ops::JournalTailer tail_;
  std::uint64_t gen_ = 0;
  InstrumentTable table_;
  std::vector<std::string> sym_cache_;
  std::unique_ptr<MirrorSite> site_;
  std::vector<Envelope> batch_;
  std::deque<FillRow> fills_;
  std::deque<DenyRow> denies_;
  std::deque<OpsRow> ops_log_;
  std::uint64_t envelopes_ = 0;
  Nanos last_env_ts_ = 0;
  Nanos last_md_ts_ = 0;
  Phase phase_ = Phase::Closed;
};

}  // namespace platops
