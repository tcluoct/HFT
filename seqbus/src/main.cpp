// seqbus harness: one writer broadcasts checksummed 56-byte messages to N
// readers, either as pinned threads (default) or as separate PROCESSES
// (--proc) that meet only through two named shared-memory segments — the bus
// itself and a small control block used for the start barrier and for
// shipping each reader's results back to the orchestrator.
//
// Latency is one-way: the writer stamps hft::rdtscp into the payload, each reader
// stamps hft::rdtscp at delivery; the invariant TSC is package-wide, so the delta
// is a valid cross-core (and cross-process) hop time. Every reader verifies
// every delivered message's checksum and sequence — the report's integrity
// line is computed, not assumed.

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/tsc_clock.hpp"
#include "sb/core/bus.hpp"
#include "sb/core/shm.hpp"
#include "sb/harness/msg.hpp"

#if !defined(_WIN32)
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace {

using namespace sb;

constexpr unsigned kMaxReaders = 8;
constexpr std::uint64_t kCtlMagic = 0x0100'4C54'4353'4253ull;  // "SBCTL" + v1

enum class Role : std::uint8_t { kOrchestrator, kWriter, kReader };

struct Args {
  std::uint64_t msgs = 20'000'000;
  std::uint64_t warmup = 2'000'000;
  std::uint64_t rate = 5'000'000;  // msg/s; 0 = publish at full speed
  unsigned ring_pow = 16;           // 2^16 slots x 64 B = 4 MiB
  unsigned readers = 3;
  unsigned core = 2;  // writer core; reader i on core+1+i
  bool pin = true;
  bool rt = false;
  bool proc = false;
  std::string name;  // shm base name (default bus<pid>)
  Role role = Role::kOrchestrator;
  unsigned id = 0;  // reader index (child role only)
};

// ---------------------------------------------------------------------------
// Control block: start barrier + per-participant results, in its own segment.
// Readers do all histogramming privately and write only the ns summary here.

struct alignas(hft::kCacheLine) ReaderStat {
  std::atomic<std::uint64_t> ready{0};
  std::atomic<std::uint64_t> done{0};
  std::atomic<std::uint64_t> delivered{0};
  std::atomic<std::uint64_t> dropped{0};
  std::atomic<std::uint64_t> gaps{0};
  std::atomic<std::uint64_t> bad{0};  // checksum or sequence-echo failures
  std::uint64_t measured = 0;         // messages behind the latency numbers
  std::uint64_t min_ns = 0, p50_ns = 0, p90_ns = 0, p99_ns = 0, p999_ns = 0, max_ns = 0;
};

struct alignas(hft::kCacheLine) WriterStat {
  std::atomic<std::uint64_t> done{0};
  std::uint64_t published = 0;    // total, including warmup and the stop message
  std::uint64_t elapsed_ns = 0;   // measured phase only
};

struct CtlBlock {
  std::atomic<std::uint64_t> magic{0};
  alignas(hft::kCacheLine) std::atomic<std::uint32_t> stop{0};  // fallback exit for lapped readers
  WriterStat writer;
  ReaderStat readers[kMaxReaders];
};

// ---------------------------------------------------------------------------

bool parse_u64(const char* s, std::uint64_t& v) {
  const auto r = std::from_chars(s, s + std::strlen(s), v);
  return r.ec == std::errc{} && *r.ptr == '\0';
}

void usage() {
  std::printf(
      "seqbus [options]\n"
      "  --readers N    reader count, 1..%u                (default 3)\n"
      "  --msgs N       measured messages                  (default 20000000)\n"
      "  --warmup N     warmup messages                    (default 2000000)\n"
      "  --rate N       publish rate msg/s, 0 = full speed (default 5000000)\n"
      "  --ring P       ring capacity = 2^P slots          (default 16 = 4 MiB)\n"
      "  --proc         readers + writer as separate processes (default: threads)\n"
      "  --core N       writer core; reader i on N+1+i     (default 2)\n"
      "  --name S       shm base name                      (default bus<pid>)\n"
      "  --no-pin / --rt\n",
      kMaxReaders);
}

bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const char* s = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    std::uint64_t n{};
    if (!std::strcmp(s, "--msgs")) {
      const char* v = next();
      if (!v || !parse_u64(v, a.msgs)) return false;
    } else if (!std::strcmp(s, "--warmup")) {
      const char* v = next();
      if (!v || !parse_u64(v, a.warmup)) return false;
    } else if (!std::strcmp(s, "--rate")) {
      const char* v = next();
      if (!v || !parse_u64(v, a.rate)) return false;
    } else if (!std::strcmp(s, "--ring")) {
      const char* v = next();
      if (!v || !parse_u64(v, n) || n < 3 || n > 26) return false;
      a.ring_pow = static_cast<unsigned>(n);
    } else if (!std::strcmp(s, "--readers")) {
      const char* v = next();
      if (!v || !parse_u64(v, n) || n < 1 || n > kMaxReaders) return false;
      a.readers = static_cast<unsigned>(n);
    } else if (!std::strcmp(s, "--core")) {
      const char* v = next();
      if (!v || !parse_u64(v, n)) return false;
      a.core = static_cast<unsigned>(n);
    } else if (!std::strcmp(s, "--id")) {
      const char* v = next();
      if (!v || !parse_u64(v, n)) return false;
      a.id = static_cast<unsigned>(n);
    } else if (!std::strcmp(s, "--name")) {
      const char* v = next();
      if (!v) return false;
      a.name = v;
    } else if (!std::strcmp(s, "--role")) {
      const char* v = next();
      if (!v) return false;
      if (!std::strcmp(v, "writer")) a.role = Role::kWriter;
      else if (!std::strcmp(v, "reader")) a.role = Role::kReader;
      else return false;
    } else if (!std::strcmp(s, "--proc")) {
      a.proc = true;
    } else if (!std::strcmp(s, "--no-pin")) {
      a.pin = false;
    } else if (!std::strcmp(s, "--rt")) {
      a.rt = true;
    } else {
      usage();
      return false;
    }
  }
  if (a.name.empty()) a.name = "bus" + std::to_string(hft::current_pid());
  return true;
}

std::string comma3(std::uint64_t v) {
  std::string s = std::to_string(v);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
  return s;
}

void pin_and_boost(const Args& a, unsigned core) {
  if (a.pin) {
    if (core < hft::logical_core_count())
      hft::pin_current_thread(core);
    else
      std::printf("  (core %u > machine's last core — not pinning this participant)\n", core);
  }
  if (a.rt) hft::boost_current_thread_priority();
}

// ---------------------------------------------------------------------------
// Roles. Identical code runs as a thread (default) or as a child process
// (--proc) — the bus cannot tell the difference, which is rather the point.

void run_writer(Bus bus, CtlBlock& ctl, const Args& a) {
  pin_and_boost(a, a.core);
  // In --proc mode every process owns its hft::TscClock and calibrates here; in
  // thread mode the orchestrator calibrated once before launching us (the
  // calibration constant is process-wide state).
  if (a.role != Role::kOrchestrator) hft::TscClock::calibrate();

  for (unsigned i = 0; i < a.readers; ++i)
    while (!ctl.readers[i].ready.load(std::memory_order_acquire)) hft::cpu_relax();

  Writer w(bus);
  const bool paced = a.rate > 0;
  const double cycles_per_msg =
      paced ? hft::TscClock::cycles_per_ns() * 1e9 / static_cast<double>(a.rate) : 0.0;
  double next_emit = static_cast<double>(hft::rdtsc());
  std::uint64_t seq = 0;

  auto emit = [&](MsgKind kind) {
    if (paced) {
      next_emit += cycles_per_msg;
      while (static_cast<double>(hft::rdtsc()) < next_emit) hft::cpu_relax();
    }
    w.publish(make_msg(seq, kind, hft::rdtscp()));
    ++seq;
  };

  for (std::uint64_t i = 0; i < a.warmup; ++i) emit(MsgKind::kWarmup);
  const std::uint64_t t0 = hft::rdtscp();
  for (std::uint64_t i = 0; i < a.msgs; ++i) emit(MsgKind::kMeasured);
  const std::uint64_t t1 = hft::rdtscp();
  emit(MsgKind::kStop);
  ctl.stop.store(1, std::memory_order_release);

  ctl.writer.published = seq;
  ctl.writer.elapsed_ns = hft::TscClock::to_ns(t1 - t0);
  ctl.writer.done.store(1, std::memory_order_release);
}

void run_reader(Bus bus, CtlBlock& ctl, const Args& a, unsigned id) {
  pin_and_boost(a, a.core + 1 + id);
  if (a.role != Role::kOrchestrator) hft::TscClock::calibrate();

  ReaderStat& st = ctl.readers[id];
  Reader r(bus, Reader::Start::kOrigin);
  hft::LatencyHistogram hist;
  std::uint64_t delivered = 0, bad = 0, measured = 0, idle = 0;
  BusMsg m{};

  st.ready.store(1, std::memory_order_release);
  for (;;) {
    const Poll p = r.poll(m);
    if (p.kind == PollKind::kMsg) [[likely]] {
      const std::uint64_t t1 = hft::rdtscp();
      ++delivered;
      if (!verify_msg(m) || m.seq != r.next_seq() - 1) [[unlikely]] ++bad;
      if (m.kind == static_cast<std::uint32_t>(MsgKind::kMeasured)) [[likely]] {
        hist.record(t1 >= m.t0_tsc ? t1 - m.t0_tsc : 0);
        ++measured;
      } else if (m.kind == static_cast<std::uint32_t>(MsgKind::kStop)) {
        break;
      }
      continue;
    }
    if (p.kind == PollKind::kGap) continue;  // Reader already counted it; keep draining
    hft::cpu_relax();
    if ((++idle & 0xFFFu) == 0) [[unlikely]] {
      // Fallback exit: if we were lapped past the stop message itself, leave
      // once the writer is finished and we have drained to the live edge.
      if (ctl.stop.load(std::memory_order_acquire) &&
          r.next_seq() >= bus.header().pub_seq.load(std::memory_order_acquire))
        break;
    }
  }

  st.delivered.store(delivered, std::memory_order_relaxed);
  st.dropped.store(r.dropped(), std::memory_order_relaxed);
  st.gaps.store(r.gaps(), std::memory_order_relaxed);
  st.bad.store(bad, std::memory_order_relaxed);
  st.measured = measured;
  st.min_ns = hft::TscClock::to_ns(hist.min());
  st.p50_ns = hft::TscClock::to_ns(hist.percentile(0.50));
  st.p90_ns = hft::TscClock::to_ns(hist.percentile(0.90));
  st.p99_ns = hft::TscClock::to_ns(hist.percentile(0.99));
  st.p999_ns = hft::TscClock::to_ns(hist.percentile(0.999));
  st.max_ns = hft::TscClock::to_ns(hist.max());
  st.done.store(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Child-process plumbing for --proc.

#if defined(_WIN32)
struct Child {
  PROCESS_INFORMATION pi{};
};

bool spawn(const char*, const std::string& args, Child& c) {
  char exe[MAX_PATH];
  ::GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string cmd = "\"" + std::string(exe) + "\" " + args;
  std::vector<char> buf(cmd.begin(), cmd.end());
  buf.push_back('\0');
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  return ::CreateProcessA(exe, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                          &c.pi) != 0;
}

int wait_child(Child& c) {
  ::WaitForSingleObject(c.pi.hProcess, INFINITE);
  DWORD code = 1;
  ::GetExitCodeProcess(c.pi.hProcess, &code);
  ::CloseHandle(c.pi.hThread);
  ::CloseHandle(c.pi.hProcess);
  return static_cast<int>(code);
}
#else
struct Child {
  pid_t pid = -1;
};

bool spawn(const char* argv0, const std::string& args, Child& c) {
  std::vector<std::string> tok{argv0};
  for (std::size_t b = 0; b < args.size();) {
    std::size_t e = args.find(' ', b);
    if (e == std::string::npos) e = args.size();
    if (e > b) tok.emplace_back(args.substr(b, e - b));
    b = e + 1;
  }
  const pid_t p = ::fork();
  if (p < 0) return false;
  if (p == 0) {
    std::vector<char*> av;
    for (auto& t : tok) av.push_back(t.data());
    av.push_back(nullptr);
    ::execvp(argv0, av.data());
    ::_exit(127);
  }
  c.pid = p;
  return true;
}

int wait_child(Child& c) {
  int status = 0;
  ::waitpid(c.pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
#endif

std::string child_args(const Args& a, const char* role, unsigned id) {
  std::string s = "--role ";
  s += role;
  s += " --name " + a.name;
  s += " --msgs " + std::to_string(a.msgs);
  s += " --warmup " + std::to_string(a.warmup);
  s += " --rate " + std::to_string(a.rate);
  s += " --ring " + std::to_string(a.ring_pow);
  s += " --readers " + std::to_string(a.readers);
  s += " --core " + std::to_string(a.core);
  if (!std::strcmp(role, "reader")) s += " --id " + std::to_string(id);
  if (!a.pin) s += " --no-pin";
  if (a.rt) s += " --rt";
  return s;
}

// Child entry: attach both segments (they exist before we were spawned),
// run the role, and report only through the control block.
int run_child(const Args& a) {
  SharedSegment data = SharedSegment::open(a.name, bus_bytes(std::uint64_t{1} << a.ring_pow));
  SharedSegment ctlseg = SharedSegment::open(a.name + ".ctl", sizeof(CtlBlock));
  if (!data.valid() || !ctlseg.valid()) {
    std::fprintf(stderr, "seqbus child: cannot open shm '%s'\n", a.name.c_str());
    return 2;
  }
  Bus bus = Bus::attach(data.data(), data.size());
  auto* ctl = static_cast<CtlBlock*>(ctlseg.data());
  if (!bus.valid() || ctl->magic.load(std::memory_order_acquire) != kCtlMagic) {
    std::fprintf(stderr, "seqbus child: bus/ctl not formatted\n");
    return 2;
  }
  if (a.role == Role::kWriter)
    run_writer(bus, *ctl, a);
  else
    run_reader(bus, *ctl, a, a.id);
  return 0;
}

// ---------------------------------------------------------------------------

void report(const CtlBlock& ctl, const Args& a) {
  const std::uint64_t published = ctl.writer.published;
  const double secs = static_cast<double>(ctl.writer.elapsed_ns) / 1e9;
  const double rate = secs > 0 ? static_cast<double>(a.msgs) / secs : 0;

  std::printf("\nwriter : %s published (%s warmup + %s measured + 1 stop)\n",
              comma3(published).c_str(), comma3(a.warmup).c_str(), comma3(a.msgs).c_str());
  std::printf("         measured phase %.3f s = %.2fM msg/s (%.1f ns/publish)\n", secs,
              rate / 1e6, rate > 0 ? 1e9 / rate : 0.0);

  std::printf("\nhop latency (ns, one-way, writer rdtscp -> reader rdtscp)\n");
  std::printf("reader    delivered      dropped  gaps      min      p50      p90      p99"
              "    p99.9      max\n");
  bool all_accounted = true;
  std::uint64_t total_bad = 0;
  for (unsigned i = 0; i < a.readers; ++i) {
    const ReaderStat& st = ctl.readers[i];
    const std::uint64_t delivered = st.delivered.load(std::memory_order_relaxed);
    const std::uint64_t dropped = st.dropped.load(std::memory_order_relaxed);
    total_bad += st.bad.load(std::memory_order_relaxed);
    if (delivered + dropped != published) all_accounted = false;
    std::printf("%6u %12s %12s %5s %8s %8s %8s %8s %8s %8s\n", i, comma3(delivered).c_str(),
                comma3(dropped).c_str(), comma3(st.gaps.load(std::memory_order_relaxed)).c_str(),
                comma3(st.min_ns).c_str(), comma3(st.p50_ns).c_str(), comma3(st.p90_ns).c_str(),
                comma3(st.p99_ns).c_str(), comma3(st.p999_ns).c_str(), comma3(st.max_ns).c_str());
  }
  std::printf("\nintegrity: %s bad checksum/sequence deliveries; delivered+dropped %s published"
              " for every reader\n",
              comma3(total_bad).c_str(), all_accounted ? "==" : "!=");
}

int run_orchestrator(const Args& a, const char* argv0) {
  const std::uint64_t capacity = std::uint64_t{1} << a.ring_pow;

  SharedSegment data = SharedSegment::create(a.name, bus_bytes(capacity));
  SharedSegment ctlseg = SharedSegment::create(a.name + ".ctl", sizeof(CtlBlock));
  if (!data.valid() || !ctlseg.valid()) {
    std::fprintf(stderr,
                 "seqbus: cannot create shm '%s' (name already in use? try --name)\n",
                 a.name.c_str());
    return 2;
  }
  Bus bus = Bus::format(data.data(), data.size(), capacity);
  auto* ctl = new (ctlseg.data()) CtlBlock{};
  ctl->magic.store(kCtlMagic, std::memory_order_release);
  if (!a.proc) hft::TscClock::calibrate();  // one calibration shared by all threads

  std::printf("seqbus — single-writer shared-memory broadcast\n");
  std::printf("mode: %u reader %s | ring %s x %zu B (%.1f MiB) | rate %s | %s%s\n", a.readers,
              a.proc ? "processes" : "threads", comma3(capacity).c_str(), sizeof(Slot),
              static_cast<double>(bus_bytes(capacity)) / (1024.0 * 1024.0),
              a.rate ? (comma3(a.rate) + " msg/s").c_str() : "full speed",
              a.pin ? "pinned" : "not pinned", a.rt ? ", time-critical" : "");

  if (!a.proc) {
    std::vector<std::jthread> threads;
    threads.reserve(a.readers + 1);
    for (unsigned i = 0; i < a.readers; ++i)
      threads.emplace_back([&, i] { run_reader(bus, *ctl, a, i); });
    threads.emplace_back([&] { run_writer(bus, *ctl, a); });
  } else {
    std::vector<Child> kids(a.readers + 1);
    for (unsigned i = 0; i < a.readers; ++i)
      if (!spawn(argv0, child_args(a, "reader", i), kids[i])) {
        std::fprintf(stderr, "seqbus: failed to spawn reader %u\n", i);
        return 2;
      }
    if (!spawn(argv0, child_args(a, "writer", 0), kids[a.readers])) {
      std::fprintf(stderr, "seqbus: failed to spawn writer\n");
      return 2;
    }
    int rc = 0;
    for (auto& k : kids) rc |= wait_child(k);
    if (rc) {
      std::fprintf(stderr, "seqbus: a child failed (%d)\n", rc);
      return rc;
    }
  }

  report(*ctl, a);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return 2;
  if (a.role != Role::kOrchestrator) return run_child(a);
  return run_orchestrator(a, argv[0]);
}
