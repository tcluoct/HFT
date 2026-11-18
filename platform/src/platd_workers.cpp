#include "platd_workers.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <thread>

#include "plat/bus/shared_channel.hpp"
#include "plat/core/thread.hpp"
#include "plat/replay/sidecar.hpp"

#ifndef _WIN32
#include <csignal>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/wait.h>
extern char** environ;
#endif

using namespace plat;

namespace platd {
namespace {
using Feed = SharedChannel<Ring::SharedStorage<1 << 14>, 0x48465410>;

struct WorkerStorage {
  RunnerMailbox orders;
  hft::SpscRing<Envelope, 256> events;
  std::atomic<bool> stop{false};
  std::atomic<bool> ready{false};
  std::atomic<std::uint64_t> progress{0};
  std::atomic<std::uint64_t> processed_feed{0};
  std::uint64_t parent = 0;
  StrategyId strategy = 0;
  AccountId account = 0;
};
using Mailbox = SharedChannel<WorkerStorage, 0x48465413>;

struct WorkerProcess {
  explicit WorkerProcess(Mailbox channel, StrategyId id)
      : mailbox(std::move(channel)), binding{mailbox.get()->orders, id, 1} {}
  ~WorkerProcess() {
    mailbox.get()->stop.store(true, std::memory_order_release);
#ifdef _WIN32
    if (job) CloseHandle(job);
    if (process) CloseHandle(process);
#else
    if (pid > 0) {
      kill(pid, SIGKILL);
      while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
#endif
  }
  bool launch(const std::string& mailbox_name, const std::string& feed,
              const std::string& config, const std::string& journal) {
#ifdef _WIN32
    mailbox.get()->parent = GetCurrentProcessId();
    mailbox.get()->ready.store(true, std::memory_order_release);
    wchar_t exe[32768];
    const DWORD length = GetModuleFileNameW(nullptr, exe, 32768);
    if (!length || length == 32768) return false;
    const auto quote = [](const std::string& arg) {
      return L"\"" + std::filesystem::path(arg).wstring() + L"\"";
    };
    std::wstring cmd = L"\"" + std::wstring(exe, length) + L"\" --strategy-worker " +
                      quote(mailbox_name) + L" " + quote(feed) + L" " + quote(config) +
                      L" " + quote(journal);
    job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                  sizeof(limits))) return false;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup,
                        &child)) return false;
    process = child.hProcess;
    const bool ok = AssignProcessToJobObject(job, process) &&
                    ResumeThread(child.hThread) != static_cast<DWORD>(-1);
    if (!ok) TerminateProcess(process, 1);
    CloseHandle(child.hThread);
    return ok;
#else
    mailbox.get()->parent = getpid();
    mailbox.get()->ready.store(true, std::memory_order_release);
    std::string args[] = {"platd", "--strategy-worker", mailbox_name, feed, config, journal};
    char* argv[] = {args[0].data(), args[1].data(), args[2].data(), args[3].data(),
                    args[4].data(), args[5].data(), nullptr};
    return posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, argv, environ) == 0;
#endif
  }
  Mailbox mailbox;
  RunnerBinding binding;
  std::uint64_t last_progress = 0;
  Nanos last_activity = 0;
#ifdef _WIN32
  HANDLE process = nullptr;
  HANDLE job = nullptr;
#else
  pid_t pid = -1;
#endif
};

// The deployed desk's futures callbacks use an instrument table, a local
// book and asynchronous order entry. Account state stays in the parent.
class WorkerContext {
 public:
  WorkerContext(WorkerStorage& storage, const InstrumentTable& table)
      : storage_(storage), table_(table), books_(table.size()) {}
  [[nodiscard]] const Instrument& instrument(InstrumentId id) const { return table_.at(id); }
  [[nodiscard]] const FutBook* fut_book(InstrumentId id) const { return &books_[id]; }
  void apply(const MdSnapshotEvt& tick, std::uint64_t seq, Nanos received) {
    books_[tick.inst].on_snapshot(tick);
    feed_seq_ = seq;
    received_ = received;
  }
  OrderId place(InstrumentId inst, Side side, Offset offset, Price px, Qty qty, Tif tif, Nanos) {
    const auto next = request_ + 1;
    auto& pending = orders_[next % orders_.size()];
    if (pending.local) return kNoOrder;
    RunnerIntent intent{};
    intent.generation = 1;
    intent.request_seq = next;
    intent.feed_seq = feed_seq_;
    intent.trigger_ts = received_;
    intent.inst = inst;
    intent.side = side;
    intent.offset = offset;
    intent.px = px;
    intent.qty = qty;
    intent.tif = tif;
    if (!storage_.orders.intents.try_push(intent)) return kNoOrder;
    pending = {next, kNoOrder, inst};
    return ++request_;
  }
  template <typename S>
  void outcomes(S& strategy) {
    IntentOutcome result;
    while (storage_.orders.outcomes.try_pop(result)) {
      auto& pending = orders_[result.request_seq % orders_.size()];
      if (pending.local != result.request_seq) continue;
      if (result.status == IntentStatus::Processed) {
        pending.canonical = result.cl_id;
      } else {
        RiskDeny deny{};
        deny.cl_id = pending.local;
        deny.inst = pending.inst;
        deny.strategy = strategy.id();
        deny.account = strategy.account();
        deny.check = result.check;
        strategy.on_deny(*this, deny);
        pending = {};
      }
    }
  }
  template <typename S>
  void report(S& strategy, ExecReport report) {
    for (auto& pending : orders_) {
      if (!pending.local || pending.canonical != report.cl_id) continue;
      report.cl_id = pending.local;
      strategy.on_exec(*this, report);
      if (report.type == ExecType::Reject || report.type == ExecType::Canceled ||
          (report.type == ExecType::Fill && report.leaves == 0)) pending = {};
      return;
    }
  }
 private:
  struct Pending { OrderId local; OrderId canonical; InstrumentId inst; };
  WorkerStorage& storage_;
  const InstrumentTable& table_;
  std::vector<FutBook> books_;
  std::array<Pending, 256> orders_{};
  std::uint64_t request_ = 0, feed_seq_ = 0;
  Nanos received_ = 0;
};

template <typename S>
bool run_worker(S& strategy, WorkerStorage& storage, Ring& market, const InstrumentTable& table,
                bool spin) {
  WorkerContext context(storage, table);
  Ring::Reader feed(market, true, Ring::LossPolicy::Intolerant);
  strategy.on_start(context);
  bool phase_known = false;
  std::uint64_t rounds = 0;
  while (!storage.stop.load(std::memory_order_acquire)) {
    context.outcomes(strategy);
    Envelope event;
    bool work = storage.events.try_pop(event);
    if (work) {
      if (event.is<PhaseChangeEvt>()) {
        strategy.on_phase(context, event.as<PhaseChangeEvt>());
        phase_known = true;
      } else if (event.is<ExecReport>()) {
        context.outcomes(strategy);
        context.report(strategy, event.as<ExecReport>());
      } else if (event.is<ParamUpdateEvt>()) strategy.on_param(context, event.as<ParamUpdateEvt>());
    }
    Nanos received = 0;
    if (phase_known) {
      const auto result = feed.poll(event, &received);
      if (result.kind == Ring::PollKind::Fault) return false;
      if (result.kind == Ring::PollKind::Msg && event.is<MdSnapshotEvt>()) {
        const auto& tick = event.as<MdSnapshotEvt>();
        if (tick.inst >= table.size()) return false;
        context.apply(tick, event.seq + 1, received);
        strategy.on_snapshot(context, tick);
        storage.processed_feed.store(event.seq + 1, std::memory_order_release);
        work = true;
      }
    }
    ++rounds;
    if (work || (rounds & 255u) == 1)
      storage.progress.store(rounds, std::memory_order_relaxed);
    if (!work) {
      if (spin) hft::cpu_relax();
      else std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
  return true;
}
}  // namespace

struct StrategyProcesses::State {
  Feed feed;
  Ring market;
  LiveSite& site;
  std::vector<std::unique_ptr<WorkerProcess>> workers;
  Nanos timeout;
  State(Feed mapping, LiveSite& engine, Nanos stall)
      : feed(std::move(mapping)), market(*feed.get()), site(engine), timeout(stall) {}
};

StrategyProcesses::StrategyProcesses() = default;
StrategyProcesses::~StrategyProcesses() = default;

bool StrategyProcesses::launch(LiveSite& site, const Config& cfg,
                              const std::vector<StratSpec>& specs, const std::string& config) {
  const auto name = "plat-live-" + std::to_string(perf_now_ns());
  auto feed = Feed::create(name, 1);
  if (!feed.get()) return false;
  state_ = std::make_unique<State>(std::move(feed), site,
                                   cfg.i64("strategy_stall_ms", 5000) * kNsPerMs);
  for (const auto& spec : specs) {
    const auto worker_name = name + "-" + std::to_string(spec.id);
    auto mailbox = Mailbox::create(worker_name, 1);
    if (!mailbox.get()) return false;
    mailbox.get()->strategy = spec.id;
    mailbox.get()->account = spec.account;
    auto worker = std::make_unique<WorkerProcess>(std::move(mailbox), spec.id);
    if (!worker->launch(worker_name, name, config, site.config().journal_path)) return false;
    state_->workers.push_back(std::move(worker));
    logf("[worker] launched strategy %u (%s)\n", spec.id, spec.name.c_str());
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  const auto running = [](const auto& worker) {
    return worker->mailbox.get()->progress.load(std::memory_order_relaxed) != 0;
  };
  for (;;) {
    if (std::all_of(state_->workers.begin(), state_->workers.end(), running)) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

Ring& StrategyProcesses::market() { return state_->market; }

bool StrategyProcesses::poll(void* self, Nanos now) {
  auto& state = *static_cast<StrategyProcesses*>(self)->state_;
  bool work = false;
  for (auto& worker : state.workers) {
    auto& binding = worker->binding;
    auto& shared = *worker->mailbox.get();
    const auto progress = shared.progress.load(std::memory_order_relaxed);
    if (!worker->last_activity || progress != worker->last_progress) {
      worker->last_progress = progress;
      worker->last_activity = now;
    }
    if (binding.fenced || now - worker->last_activity >= state.timeout) {
      if (!shared.stop.exchange(true, std::memory_order_release))
        logf("[worker] strategy %u fenced, check process and resting orders\n", binding.strategy);
      binding.fenced = true;
      continue;
    }
    work |= state.site.poll(binding, now);
  }
  return work;
}

void StrategyProcesses::publish(void* self, const Envelope& event) {
  StrategyId target = 0;
  if (event.is<ExecReport>()) target = event.as<ExecReport>().strategy;
  else if (event.is<ParamUpdateEvt>()) target = event.as<ParamUpdateEvt>().strategy;
  else if (!event.is<PhaseChangeEvt>()) return;
  auto& state = *static_cast<StrategyProcesses*>(self)->state_;
  for (auto& worker : state.workers) {
    auto& binding = worker->binding;
    if (!binding.fenced && (event.is<PhaseChangeEvt>() || target == binding.strategy) &&
        !worker->mailbox.get()->events.try_push(event)) binding.fenced = true;
  }
}

int StrategyProcesses::check(const Config& cfg, const std::string& config_path) {
  const auto account = static_cast<AccountId>(cfg.i64("account_id", 1));
  std::vector<StratSpec> specs;
  if (!parse_strategies(cfg, account, specs) || specs.empty()) return 1;
  InstrumentTable table;
  SiteConfig sc;
  sc.journal_path = "plat-worker-check-" + std::to_string(perf_now_ns());
  for (const auto& symbol : cfg.list("instruments")) {
    const auto id = table.add(symbol, Venue::SHFE, Board::Future, px_from_cny(10), 1, 1,
                              px_from_cny(1000));
    sc.hosted.push_back(id);
  }
  if (sc.hosted.empty() || !save_sidecar(sc.journal_path, table)) return 1;
  LiveSite site(table, sc, LiveSite::Mode::Replay, Pack::Banks{});
  StrategyProcesses processes;
  const bool started = processes.launch(site, cfg, specs, config_path);
  bool received = false;
  if (started) {
    const Nanos now = at_time(9, 0, 1);
    publish(&processes, make_envelope(0, now, PhaseChangeEvt{now, Venue::SHFE, Phase::Continuous},
                                     Origin::Source, 0));
    MdSnapshotEvt tick{};
    tick.ts = now;
    tick.inst = sc.hosted.front();
    tick.venue = Venue::SHFE;
    tick.last_px = px_from_cny(1000);
    tick.bid_px[0] = px_from_cny(990);
    tick.ask_px[0] = px_from_cny(1010);
    tick.bid_qty[0] = 50;
    tick.ask_qty[0] = 10;
    Ring::Writer writer(processes.market());
    writer.publish(make_envelope(0, now, tick, Origin::Source, 0), now);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!received && std::chrono::steady_clock::now() < deadline) {
      received = true;
      for (const auto& child : processes.state_->workers)
        received &= child->mailbox.get()->processed_feed.load(std::memory_order_acquire) != 0;
      if (!received) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  processes.state_.reset();
  std::remove((sc.journal_path + ".ref").c_str());
  logf("[worker] offline startup and shared tick delivery: %s (%zu configured workers)\n",
       received ? "PASS" : "FAIL", specs.size());
  return received ? 0 : 1;
}

int run_strategy_worker(const char* mailbox_name, const char* feed_name, const char* config,
                        const char* journal) {
  auto mailbox = Mailbox::open(mailbox_name, 1);
  auto feed = Feed::open(feed_name, 1);
  if (!mailbox.get() || !feed.get()) return 1;
  auto& storage = *mailbox.get();
  if (!storage.ready.load(std::memory_order_acquire)) return 1;
#ifndef _WIN32
  if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 ||
      static_cast<std::uint64_t>(getppid()) != storage.parent) return 1;
#endif
  Config cfg;
  InstrumentTable table;
  std::vector<StratSpec> specs;
  if (!cfg.load(config) || !load_sidecar(journal, table) ||
      !parse_strategies(cfg, storage.account, specs)) return 1;
  for (const auto& spec : specs) if (spec.id == storage.strategy) {
    const auto core = static_cast<int>(cfg.i64(spec.name + ".core", -1));
    if (core >= 0 && !pin_thread_to_core(core)) return 1;
    auto banks = Pack::banks(cfg, table, {spec});
    Ring market(*feed.get());
    bool ok = false;
    std::apply([&](auto&... bank) {
      const auto run = [&](auto& instances) {
        if (!instances.empty()) ok = run_worker(instances.front(), storage, market, table,
                                                cfg.flag("spin", false));
      };
      (run(bank), ...);
    }, banks);
    return ok ? 0 : 1;
  }
  return 1;
}

}  // namespace platd
