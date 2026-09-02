// sesslink harness: a venue thread and a firm thread talk real TCP over
// loopback — the first project in the family whose hot path crosses the
// kernel. Four phases, each answering one question:
//   parse    : what does framing itself cost? (nanoseconds — the stitch
//              buffer and length walk, no sockets)
//   rtt      : what does the kernel charge for a round trip? (microseconds —
//              this gap, session-logic-ns vs socket-us, is the entire
//              business case for kernel-bypass NICs)
//   window   : how many orders/s does one session sustain with N in flight?
//   recover  : crash the connection with orders and acks in flight (RST, no
//              goodbye), reconnect, replay, resend — how long until the firm
//              is provably whole again?
// End-to-end integrity is asserted after every run: the firm's sequenced
// stream digest must equal the venue journal digest, and every token must
// have been executed exactly once.

#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/tsc_clock.hpp"
#include "sl/core/session.hpp"
#include "sl/core/wire.hpp"
#include "sl/net/tcp.hpp"
#include "sl/sim/apps.hpp"

namespace {

using namespace sl;

struct Args {
  u64 rtt_orders = 100'000;
  u64 warmup = 10'000;
  u64 win_orders = 500'000;
  u64 window = 64;
  u64 recover_cycles = 200;
  u64 fill_pm = 300;
  u64 port = 47654;
  unsigned core_srv = 2;
  unsigned core_cli = 3;
  bool pin = true;
  bool rt = false;
  bool nagle = false;  // leave Nagle ON to demonstrate why everyone turns it off
};

bool parse_u64(const char* s, u64& v) {
  const auto r = std::from_chars(s, s + std::strlen(s), v);
  return r.ec == std::errc{} && *r.ptr == '\0';
}

void usage() {
  std::printf(
      "sesslink [options]\n"
      "  --rtt N        ping-pong round trips          (default 100000)\n"
      "  --warmup N     unmeasured leading round trips (default 10000)\n"
      "  --win-orders N windowed-phase orders          (default 500000)\n"
      "  --window N     orders in flight               (default 64)\n"
      "  --recover N    crash/recover drills           (default 200)\n"
      "  --fill-pm N    orders filled, per mille       (default 300)\n"
      "  --port N       loopback TCP port              (default 47654)\n"
      "  --nagle        do NOT set TCP_NODELAY (see README)\n"
      "  --core-srv N / --core-cli N / --no-pin / --rt\n");
}

bool parse_args(int argc, char** argv, Args& a) {
  u64 core_srv = a.core_srv, core_cli = a.core_cli;
  struct Opt {
    const char* name;
    u64* dst;
  };
  const Opt opts[] = {
      {"--rtt", &a.rtt_orders},   {"--warmup", &a.warmup},
      {"--win-orders", &a.win_orders}, {"--window", &a.window},
      {"--recover", &a.recover_cycles}, {"--fill-pm", &a.fill_pm},
      {"--port", &a.port},        {"--core-srv", &core_srv},
      {"--core-cli", &core_cli},
  };
  for (int i = 1; i < argc; ++i) {
    const char* s = argv[i];
    if (!std::strcmp(s, "--no-pin")) {
      a.pin = false;
      continue;
    }
    if (!std::strcmp(s, "--rt")) {
      a.rt = true;
      continue;
    }
    if (!std::strcmp(s, "--nagle")) {
      a.nagle = true;
      continue;
    }
    bool hit = false;
    for (const auto& o : opts)
      if (!std::strcmp(s, o.name)) {
        if (i + 1 >= argc || !parse_u64(argv[++i], *o.dst)) {
          usage();
          return false;
        }
        hit = true;
        break;
      }
    if (!hit) {
      usage();
      return false;
    }
  }
  a.core_srv = static_cast<unsigned>(core_srv);
  a.core_cli = static_cast<unsigned>(core_cli);
  return true;
}

u64 now_ns() { return hft::TscClock::to_ns(hft::rdtsc()); }

OrderMsg mk_order(u64 token) {
  OrderMsg o{};
  o.token = token;
  o.symbol = static_cast<u32>(token % 512);
  o.qty = 100;
  o.px = 1'000'000 + static_cast<i64>(token % 1000);
  o.side = (token & 1) ? 'B' : 'S';
  return o;
}

void print_hist_ns(const char* label, const hft::LatencyHistogram& h) {
  auto ns = [](u64 cyc) {
    return static_cast<unsigned long long>(hft::TscClock::to_ns(cyc));
  };
  std::printf("  %-22s p50 %8llu  p90 %8llu  p99 %8llu  p99.9 %8llu  max %9llu\n", label,
              ns(h.percentile(0.50)), ns(h.percentile(0.90)), ns(h.percentile(0.99)),
              ns(h.percentile(0.999)), ns(h.max()));
}

// ---- phase 0: framing micro-bench (no sockets) ------------------------------

void parse_bench() {
  constexpr u64 kFrames = 2'000'000;
  std::vector<u8> wire;
  wire.reserve(kFrames * 35);
  for (u64 i = 1; i <= kFrames; ++i) {
    ExecMsg e{};
    e.token = i;
    e.kind = kExecAck;
    e.exec_id = i;
    u8 tmp[64];
    const std::size_t n = write_frame(tmp, kSeqData, &e, sizeof e);
    wire.insert(wire.end(), tmp, tmp + n);
  }

  std::printf("framing: incremental parse of %lluM 35-byte frames\n",
              static_cast<unsigned long long>(kFrames / 1'000'000));
  for (const std::size_t chunk : {std::size_t{1448}, std::size_t{65536}, std::size_t{16}}) {
    FrameReader fr;
    u64 frames = 0, tokens = 0;
    const u64 c0 = hft::rdtscp();
    std::size_t off = 0;
    while (off < wire.size()) {
      const std::size_t n = chunk < wire.size() - off ? chunk : wire.size() - off;
      (void)fr.on_bytes(wire.data() + off, n, [&](u8, const u8* p, std::size_t) {
        ExecMsg e;
        std::memcpy(&e, p, sizeof e);
        tokens += e.token;  // touch the payload like a real handler would
        ++frames;
      });
      off += n;
    }
    const u64 c1 = hft::rdtscp();
    const double secs = static_cast<double>(c1 - c0) / (hft::TscClock::cycles_per_ns() * 1e9);
    std::printf("  chunk %5zu B: %6.1fM frames/s  (%.2f GB/s)%s\n", chunk,
                static_cast<double>(frames) / secs / 1e6,
                static_cast<double>(wire.size()) / secs / 1e9,
                tokens == kFrames * (kFrames + 1) / 2 ? "" : "  BAD PAYLOADS");
  }
}

// ---- the venue thread ---------------------------------------------------------

struct Venue {
  ServerCore<VenueApp> core;
  TcpListener listener;
  std::atomic<bool> stop{false};
  std::atomic<u64> sessions{0};

  void run(const Args& a) {
    if (a.pin && !hft::pin_current_thread(a.core_srv)) std::puts("warn: server pin failed");
    if (a.rt && !hft::boost_current_thread_priority()) std::puts("warn: server rt failed");
    while (!stop.load(std::memory_order_relaxed)) {
      TcpConn conn;
      if (!listener.accept_into(conn, !a.nagle)) {
        hft::cpu_relax();
        continue;
      }
      sessions.fetch_add(1, std::memory_order_relaxed);
      ServerSession<TcpConn, VenueApp> sess(core, conn, now_ns());
      while (sess.alive() && !stop.load(std::memory_order_relaxed)) sess.poll(now_ns());
    }
  }
};

// ---- the firm side --------------------------------------------------------------

struct Firm {
  FirmApp app;
  ClientSession<TcpConn, FirmApp> session{app};
  std::unique_ptr<TcpConn> conn;
  const Args* a;
  u64 next_token = 1;

  bool connect_and_login() {
    conn = std::make_unique<TcpConn>();
    for (int tries = 0; tries < 20000; ++tries) {
      if (conn->connect_to("127.0.0.1", static_cast<std::uint16_t>(a->port), !a->nagle)) break;
      hft::cpu_relax();
    }
    if (!conn->valid()) return false;
    session.attach(*conn, now_ns());
    while (session.state() == CliState::kLoginSent) session.poll(now_ns());
    return session.state() == CliState::kEstablished;
  }

  // One measured round trip: order out, spin until its ack is back.
  bool round_trip(u64& cycles) {
    const u64 want_acked = app.acked + 1;
    const u64 t0 = hft::rdtscp();
    OrderMsg o = mk_order(next_token);
    if (!app.send_order(session, o)) return false;
    ++next_token;
    session.poll(now_ns());
    while (app.acked < want_acked) {
      if (session.state() != CliState::kEstablished) return false;
      session.poll(now_ns());
    }
    cycles = hft::rdtscp() - t0;
    return true;
  }
};

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return 1;

  std::printf("sesslink: rtt %llu (+%llu warmup), window %llu x %llu, recover %llu, "
              "fill %llu/1000, TCP_NODELAY %s\n\n",
              static_cast<unsigned long long>(a.rtt_orders),
              static_cast<unsigned long long>(a.warmup),
              static_cast<unsigned long long>(a.window),
              static_cast<unsigned long long>(a.win_orders),
              static_cast<unsigned long long>(a.recover_cycles),
              static_cast<unsigned long long>(a.fill_pm), a.nagle ? "OFF (Nagle!)" : "on");

  hft::TscClock::calibrate();
  parse_bench();

  if (!net_init()) {
    std::puts("winsock init failed");
    return 1;
  }

  Venue venue;
  venue.core.app.fill_pm = static_cast<u32>(a.fill_pm);
  if (!venue.listener.listen_on(static_cast<std::uint16_t>(a.port))) {
    std::puts("listen failed");
    return 1;
  }
  std::thread srv([&] { venue.run(a); });

  if (a.pin && !hft::pin_current_thread(a.core_cli)) std::puts("warn: client pin failed");
  if (a.rt && !hft::boost_current_thread_priority()) std::puts("warn: client rt failed");

  Firm firm;
  firm.a = &a;
  int rc = 0;
  if (!firm.connect_and_login()) {
    std::puts("connect/login failed");
    venue.stop = true;
    srv.join();
    return 1;
  }

  // ---- phase 1: RTT ping-pong -------------------------------------------------
  {
    hft::LatencyHistogram rtt;
    bool ok = true;
    for (u64 i = 0; ok && i < a.warmup + a.rtt_orders; ++i) {
      u64 cyc = 0;
      ok = firm.round_trip(cyc);
      if (ok && i >= a.warmup) rtt.record(cyc);
    }
    std::printf("\nrtt: order -> venue -> sequenced ack, wire-to-wire over loopback TCP (ns)\n");
    if (!ok) {
      std::puts("  RTT PHASE FAILED (link died)");
      rc = 1;
    } else {
      print_hist_ns("round trip", rtt);
    }
  }

  // ---- phase 2: windowed throughput --------------------------------------------
  {
    const u64 first_token = firm.next_token;
    const u64 seq_start = firm.app.seq_seen;
    const u64 c0 = hft::rdtscp();
    u64 target = first_token + a.win_orders;
    bool ok = true;
    while (ok && (firm.next_token < target || !firm.app.open.empty())) {
      while (firm.next_token < target && firm.app.open.size() < a.window) {
        OrderMsg o = mk_order(firm.next_token);
        if (!firm.app.send_order(firm.session, o)) break;  // tx full: drain first
        ++firm.next_token;
      }
      firm.session.poll(now_ns());
      ok = firm.session.state() == CliState::kEstablished;
    }
    const u64 c1 = hft::rdtscp();
    const double secs = static_cast<double>(c1 - c0) / (hft::TscClock::cycles_per_ns() * 1e9);
    std::printf("\nwindow: %llu orders, %llu in flight\n",
                static_cast<unsigned long long>(a.win_orders),
                static_cast<unsigned long long>(a.window));
    if (!ok) {
      std::puts("  WINDOW PHASE FAILED (link died)");
      rc = 1;
    } else {
      std::printf("  %.2fM orders/s round-tripped; %.2fM sequenced msgs/s delivered\n",
                  static_cast<double>(a.win_orders) / secs / 1e6,
                  static_cast<double>(firm.app.seq_seen - seq_start) / secs / 1e6);
    }
  }

  // ---- phase 3: crash / recover drill ---------------------------------------------
  {
    hft::LatencyHistogram rec;
    u64 drills = 0;
    for (u64 k = 0; k < a.recover_cycles; ++k) {
      // Put a burst in flight so the crash strands both orders and acks.
      for (int i = 0; i < 32 && firm.app.open.size() < 64; ++i) {
        OrderMsg o = mk_order(firm.next_token);
        if (firm.app.send_order(firm.session, o)) ++firm.next_token;
      }
      firm.session.poll(now_ns());

      const u64 t0 = hft::rdtscp();
      firm.conn->abort();  // RST: a crash, not a goodbye
      // Notice, reconnect, login, replay, resend, converge.
      while (firm.session.state() == CliState::kEstablished) firm.session.poll(now_ns());
      if (!firm.connect_and_login()) break;
      while (!firm.app.recovered(firm.session.next_seq()) &&
             firm.session.state() == CliState::kEstablished)
        firm.session.poll(now_ns());
      const u64 t1 = hft::rdtscp();
      if (!firm.app.recovered(firm.session.next_seq())) break;
      rec.record(t1 - t0);
      ++drills;
    }
    std::printf("\nrecover: RST with orders+acks in flight -> reconnect+replay+resend (ns)\n");
    print_hist_ns("crash to whole", rec);
    if (drills != a.recover_cycles) {
      std::printf("  RECOVERY DRILLS INCOMPLETE: %llu of %llu\n",
                  static_cast<unsigned long long>(drills),
                  static_cast<unsigned long long>(a.recover_cycles));
      rc = 1;
    }
  }

  // Let the last acks drain, then stop the venue and audit.
  for (int i = 0; i < 1000 && !firm.app.open.empty(); ++i) firm.session.poll(now_ns());
  const u64 tail = venue.core.journal.next_seq();
  while (firm.session.next_seq() < tail && firm.session.state() == CliState::kEstablished)
    firm.session.poll(now_ns());
  venue.stop = true;
  firm.conn->close();
  srv.join();

  const u64 tokens_sent = firm.next_token - 1;
  const bool once = venue.core.app.accepted == tokens_sent;
  const bool acked = firm.app.acked == tokens_sent && firm.app.open.empty();
  const bool digest_ok = firm.app.stream_digest == venue.core.journal.digest() &&
                         firm.session.next_seq() == venue.core.journal.next_seq();
  std::printf(
      "\nintegrity: %llu tokens sent | executed exactly once %s | all acked %s | "
      "stream digest == journal digest %s\n"
      "           sessions %llu | logins %llu | resends %llu | dups absorbed %llu | "
      "fills %llu | proto errors %llu\n",
      static_cast<unsigned long long>(tokens_sent), once ? "OK" : "VIOLATED",
      acked ? "OK" : "VIOLATED", digest_ok ? "OK" : "VIOLATED",
      static_cast<unsigned long long>(venue.sessions.load()),
      static_cast<unsigned long long>(firm.app.logins),
      static_cast<unsigned long long>(firm.app.resent),
      static_cast<unsigned long long>(venue.core.app.dup_dropped),
      static_cast<unsigned long long>(firm.app.fills_seen),
      static_cast<unsigned long long>(firm.session.proto_errors()));
  return (rc == 0 && once && acked && digest_ok) ? 0 : 1;
}
