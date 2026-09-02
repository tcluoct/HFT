// Scenario tests for the session layer. The contract under test: across any
// pattern of partial reads, coalesced frames, heartbeat silences, and
// connection kills (including mid-frame, with bytes in flight destroyed),
//   * the client's sequenced stream is byte-identical to the server journal
//     (digest), with no gap and no duplicate — replay works;
//   * every order the firm ever sent is executed by the venue EXACTLY once
//     and eventually acked — resend + token dedup works;
//   * silence is detected: both sides declare a quiet peer dead on schedule.
// All over the deterministic MemLink transport — the hard interleavings are
// constructed, not hoped for. No test framework — a CHECK macro.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "sl/core/session.hpp"
#include "sl/core/wire.hpp"
#include "sl/sim/apps.hpp"
#include "sl/sim/mem_link.hpp"

using namespace sl;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_fail;                                                     \
    }                                                               \
  } while (0)

u64 g_rng = 0x9E3779B97F4A7C15ull;
u64 rnd() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}

OrderMsg order(u64 token) {
  OrderMsg o{};
  o.token = token;
  o.symbol = 7;
  o.qty = 100;
  o.px = 1000000 + static_cast<i64>(token % 100);
  o.side = 'B';
  return o;
}

// One client + one (recreatable) server session over one MemLink.
struct Rig {
  MemLink link;
  ServerCore<VenueApp> core;
  FirmApp firm;
  ClientSession<MemEnd, FirmApp> cli;
  std::unique_ptr<ServerSession<MemEnd, VenueApp>> srv;
  u64 now = 0;

  explicit Rig(HeartbeatCfg hb = {}) : cli(firm, /*session_id=*/1, hb), hb_(hb) {
    srv = std::make_unique<ServerSession<MemEnd, VenueApp>>(core, link.server, now, hb_);
    cli.attach(link.client, now);
  }

  void pump(int rounds = 4, u64 dt = 1000) {
    for (int i = 0; i < rounds; ++i) {
      now += dt;
      cli.poll(now);
      srv->poll(now);
    }
  }

  // Crash the connection (in-flight bytes vanish), then dial back in.
  void kill_and_reconnect() {
    link.kill();
    now += 1000;
    cli.poll(now);   // sees kClosed -> on_link_down
    srv->poll(now);  // sees kClosed -> dead
    link.reset();
    srv = std::make_unique<ServerSession<MemEnd, VenueApp>>(core, link.server, now, hb_);
    cli.attach(link.client, now);
  }

  // Pump until fully caught up (client at journal tail, no open orders).
  bool drain(std::size_t max_pumps = 100000) {
    for (std::size_t i = 0; i < max_pumps; ++i) {
      pump(1);
      if (cli.next_seq() == core.journal.next_seq() && firm.open.empty()) return true;
    }
    return false;
  }

 private:
  HeartbeatCfg hb_;
};

// ---- framing ---------------------------------------------------------------

void t_framing_chunked() {
  std::puts("framing: byte-by-byte, coalesced, split at every boundary");
  // Build 100 frames of varying payload sizes back to back.
  std::vector<u8> wire;
  std::vector<std::pair<u8, std::size_t>> expect;
  for (int i = 0; i < 100; ++i) {
    u8 payload[64];
    const std::size_t len = static_cast<std::size_t>(i % 64);
    for (std::size_t b = 0; b < len; ++b) payload[b] = static_cast<u8>(i + b);
    u8 tmp[3 + 64];
    const std::size_t n = write_frame(tmp, static_cast<u8>('a' + i % 26), payload, len);
    wire.insert(wire.end(), tmp, tmp + n);
    expect.emplace_back(static_cast<u8>('a' + i % 26), len);
  }

  // Deliver in chunk sizes 1, 2, 3, 7, and all-at-once; same frames out.
  for (const std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7},
                                  wire.size()}) {
    FrameReader fr;
    std::vector<std::pair<u8, std::size_t>> got;
    std::size_t off = 0;
    bool ok = true;
    while (off < wire.size() && ok) {
      const std::size_t n = chunk < wire.size() - off ? chunk : wire.size() - off;
      ok = fr.on_bytes(wire.data() + off, n,
                       [&](u8 ty, const u8*, std::size_t len) { got.emplace_back(ty, len); });
      off += n;
    }
    CHECK(ok);
    CHECK(got == expect);
  }

  // A zero length or an oversized length poisons the stream.
  FrameReader fr;
  const u8 bad_zero[2] = {0, 0};
  CHECK(!fr.on_bytes(bad_zero, 2, [](u8, const u8*, std::size_t) {}));
  FrameReader fr2;
  const u8 bad_huge[2] = {0xff, 0xff};
  CHECK(!fr2.on_bytes(bad_huge, 2, [](u8, const u8*, std::size_t) {}));
}

// ---- login -----------------------------------------------------------------

void t_login_happy() {
  std::puts("login: connect, establish, order round-trip");
  Rig r;
  r.pump();
  CHECK(r.cli.state() == CliState::kEstablished);
  CHECK(r.firm.logins == 1);
  CHECK(r.core.logins == 1);

  r.firm.send_order(r.cli, order(1));
  CHECK(r.drain());
  CHECK(r.core.app.accepted == 1);
  CHECK(r.firm.acked == 1);
  CHECK(r.firm.stream_digest == r.core.journal.digest());
}

void t_login_reject_bad_session() {
  std::puts("login: wrong session id is rejected");
  MemLink link;
  ServerCore<VenueApp> core;  // session_id = 1
  FirmApp firm;
  ClientSession<MemEnd, FirmApp> cli{firm, /*session_id=*/42};
  ServerSession<MemEnd, VenueApp> srv(core, link.server, 0);
  cli.attach(link.client, 0);
  for (int i = 1; i <= 6; ++i) {
    cli.poll(static_cast<u64>(i) * 1000);
    srv.poll(static_cast<u64>(i) * 1000);
  }
  CHECK(firm.login_rejects == 1);
  CHECK(firm.last_reject_reason == kRejBadSession);
  CHECK(cli.state() == CliState::kDead);
  CHECK(core.rejects == 1);
}

void t_login_reject_seq_ahead() {
  std::puts("login: claiming unseen sequences is rejected");
  MemLink link;
  ServerCore<VenueApp> core;
  ServerSession<MemEnd, VenueApp> srv(core, link.server, 0);
  // Raw client: request to resume from seq 100 of an empty journal.
  const LoginReq lr{1, 100};
  u8 tmp[3 + sizeof lr];
  const std::size_t n = write_frame(tmp, kLoginReq, &lr, sizeof lr);
  link.client.send_some(tmp, n);
  srv.poll(1000);
  srv.poll(2000);
  CHECK(core.rejects == 1);
  // The reject frame is on the wire for the client to read.
  u8 rx[64];
  const std::ptrdiff_t got = link.client.recv_some(rx, sizeof rx);
  CHECK(got == 3 + 1 && rx[2] == kLoginReject && rx[3] == kRejSeqAhead);
  srv.poll(3000);
  CHECK(!srv.alive());  // drain-then-die completed
}

// ---- sequenced delivery ------------------------------------------------------

void t_replay_gapless_under_chunking() {
  std::puts("replay: 5000 journaled messages, delivered gapless at 1..7-byte reads");
  Rig r;
  // Pre-fill the journal before the client ever logs in: pure replay.
  for (u64 i = 1; i <= 5000; ++i) {
    ExecMsg e{};
    e.token = i;
    e.kind = kExecAck;
    e.exec_id = i;
    r.core.journal.append(&e, sizeof e);
  }
  while (r.cli.next_seq() != r.core.journal.next_seq()) {
    r.link.client.chunk_cap = 1 + rnd() % 7;  // adversarial read sizes
    r.pump(1);
  }
  CHECK(r.firm.seq_seen == 5000);
  CHECK(r.firm.stream_digest == r.core.journal.digest());
}

// ---- heartbeats ---------------------------------------------------------------

void t_heartbeats_and_timeout() {
  std::puts("heartbeats: idle link stays up; silent peer is declared dead");
  const HeartbeatCfg hb{1'000'000, 5'000'000};  // 1ms interval, 5ms timeout (test units)
  Rig r(hb);
  r.pump(4, 1000);
  CHECK(r.cli.state() == CliState::kEstablished);

  // Idle for 20 intervals with both sides polling: heartbeats keep it alive.
  for (int i = 0; i < 20; ++i) r.pump(1, hb.interval_ns);
  CHECK(r.cli.state() == CliState::kEstablished);
  CHECK(r.srv->alive());

  // Server goes silent (stops being polled): client notices within timeout.
  u64 t = r.now;
  for (int i = 0; i < 10 && r.cli.state() == CliState::kEstablished; ++i) {
    t += hb.interval_ns;
    r.cli.poll(t);
  }
  CHECK(r.cli.state() == CliState::kDead);
  CHECK(r.firm.link_downs == 1);

  // And the mirror image: client silent, server notices.
  Rig r2(hb);
  r2.pump(4, 1000);
  t = r2.now;
  for (int i = 0; i < 10 && r2.srv->alive(); ++i) {
    t += hb.interval_ns;
    r2.srv->poll(t);
  }
  CHECK(!r2.srv->alive());
}

// ---- recovery ------------------------------------------------------------------

void t_reconnect_replays_lost_acks() {
  std::puts("recovery: acks lost with the connection come back via replay");
  Rig r;
  r.pump();
  for (u64 t = 1; t <= 10; ++t) r.firm.send_order(r.cli, order(t));
  r.now += 1000;
  r.cli.poll(r.now);  // flush the orders out
  // Let the server execute everything, but deliver NOTHING back: the client
  // never reads again, and the connection dies with all 10 acks in flight.
  for (int i = 0; i < 8; ++i) {
    r.now += 1000;
    r.srv->poll(r.now);
  }
  CHECK(r.core.app.accepted == 10);
  CHECK(r.firm.acked == 0);

  r.kill_and_reconnect();
  CHECK(r.drain());
  CHECK(r.firm.acked == 10);
  CHECK(r.core.app.accepted == 10);          // resends all deduped...
  CHECK(r.core.app.dup_dropped == 10);       // ...and counted as such
  CHECK(r.firm.stream_digest == r.core.journal.digest());
}

void t_reconnect_resends_lost_orders() {
  std::puts("recovery: orders lost with the connection go out again");
  Rig r;
  r.pump();
  // Queue 10 orders but kill the link before the server ever polls: the
  // order bytes evaporate. This is the "did my order make it?" nightmare.
  for (u64 t = 1; t <= 10; ++t) r.firm.send_order(r.cli, order(t));
  r.link.kill();
  r.now += 1000;
  r.cli.poll(r.now);
  r.link.reset();
  r.srv = std::make_unique<ServerSession<MemEnd, VenueApp>>(r.core, r.link.server, r.now);
  r.cli.attach(r.link.client, r.now);

  CHECK(r.drain());
  CHECK(r.core.app.accepted == 10);     // executed exactly once each
  CHECK(r.core.app.dup_dropped == 0);   // nothing to dedup: originals died
  CHECK(r.firm.acked == 10);
  CHECK(r.firm.resent == 10);
}

void t_partial_delivery_kill() {
  std::puts("recovery: kill mid-stream with a torn frame in the rx buffer");
  Rig r;
  r.pump();
  for (u64 t = 1; t <= 100; ++t) r.firm.send_order(r.cli, order(t));
  r.now += 1000;
  r.cli.poll(r.now);  // flush the orders out
  // Server executes all; client then reads a byte count guaranteed to leave
  // a torn frame in its rx buffer (185 is not a multiple of 35).
  for (int i = 0; i < 8; ++i) {
    r.now += 1000;
    r.srv->poll(r.now);
  }
  r.link.client.recv_budget = 185;
  r.now += 1000;
  r.cli.poll(r.now);
  r.link.client.recv_budget = ~std::size_t{0};
  const u64 acked_before = r.firm.acked;
  CHECK(acked_before > 0 && acked_before < 100);  // some acks landed, most in flight

  r.kill_and_reconnect();  // torn frame must not poison the new stream
  CHECK(r.drain());
  CHECK(r.cli.proto_errors() == 0);
  CHECK(r.firm.acked == 100);
  CHECK(r.core.app.accepted == 100);
  CHECK(r.firm.stream_digest == r.core.journal.digest());
}

void t_explicit_dup_is_ignored() {
  std::puts("dedup: a raw duplicate of an executed token is dropped");
  Rig r;
  r.pump();
  r.firm.send_order(r.cli, order(77));
  CHECK(r.drain());
  CHECK(r.core.app.accepted == 1);
  // Inject the same token again, bypassing the firm's own bookkeeping.
  const OrderMsg o = order(77);
  u8 tmp[3 + sizeof o];
  const std::size_t n = write_frame(tmp, kUnseqData, &o, sizeof o);
  r.link.client.send_some(tmp, n);
  r.pump();
  CHECK(r.core.app.accepted == 1);
  CHECK(r.core.app.dup_dropped == 1);
}

// ---- the chaos sweep -------------------------------------------------------------

void t_chaos() {
  std::puts("chaos: 50k orders, random kills, random read sizes — exactly-once holds");
  Rig r;
  r.pump();
  const u64 kOrders = 50'000;
  u64 next_token = 1;
  u64 kills = 0;
  u64 next_kill_at = 500 + rnd() % 1500;

  u64 safety = 0;
  while (next_token <= kOrders || !r.firm.open.empty() ||
         r.cli.next_seq() != r.core.journal.next_seq()) {
    if (++safety > 10'000'000) {
      CHECK(!"chaos sweep did not converge");
      break;
    }
    // Random adversarial read granularity, occasionally unlimited.
    const u64 dice = rnd() % 10;
    r.link.client.chunk_cap = dice < 3 ? 1 + rnd() % 200 : ~std::size_t{0};
    r.link.server.chunk_cap = dice < 2 ? 1 + rnd() % 200 : ~std::size_t{0};

    if (r.cli.state() == CliState::kEstablished) {
      // Keep up to 64 in flight.
      while (next_token <= kOrders && r.firm.open.size() < 64)
        r.firm.send_order(r.cli, order(next_token++));
    } else if (r.cli.state() == CliState::kDead) {
      r.link.reset();
      r.srv = std::make_unique<ServerSession<MemEnd, VenueApp>>(r.core, r.link.server, r.now);
      r.cli.attach(r.link.client, r.now);
    }

    r.pump(1);

    if (next_token > next_kill_at && kills < 60) {
      r.link.kill();
      r.pump(1);  // both sides observe the crash
      ++kills;
      next_kill_at = next_token + 500 + rnd() % 1500;
    }
  }

  std::printf("  kills %llu, logins %llu, resent %llu, dups absorbed %llu, fills %llu\n",
              static_cast<unsigned long long>(kills),
              static_cast<unsigned long long>(r.firm.logins),
              static_cast<unsigned long long>(r.firm.resent),
              static_cast<unsigned long long>(r.core.app.dup_dropped),
              static_cast<unsigned long long>(r.core.app.fills));
  CHECK(kills >= 20);                          // the sweep actually hurt
  CHECK(r.core.app.accepted == kOrders);       // every order executed exactly once
  CHECK(r.firm.acked == kOrders);              // every order eventually acked
  CHECK(r.firm.open.empty());
  CHECK(r.firm.stream_digest == r.core.journal.digest());  // views identical
  CHECK(r.cli.proto_errors() == 0);
  CHECK(r.srv->proto_errors() == 0);
}

}  // namespace

int main() {
  t_framing_chunked();
  t_login_happy();
  t_login_reject_bad_session();
  t_login_reject_seq_ahead();
  t_replay_gapless_under_chunking();
  t_heartbeats_and_timeout();
  t_reconnect_replays_lost_acks();
  t_reconnect_resends_lost_orders();
  t_partial_delivery_kill();
  t_explicit_dup_is_ignored();
  t_chaos();

  if (g_fail == 0) {
    std::puts("\nALL TESTS PASSED");
    return 0;
  }
  std::printf("\n%d CHECK(S) FAILED\n", g_fail);
  return 1;
}
