# sesslink — a resumable, exactly-once order-entry session layer

The first project in the family whose hot path crosses the kernel: a
SoupBinTCP-style session protocol over real non-blocking TCP. Every firm's
orders reach every venue through a layer like this, and it exists to answer
the ugliest question in trading: *the connection died with orders and acks in
flight — what state am I in?* Framing over a byte stream, login/heartbeat
state machines, journal-cursor replay, and token-dedup'd resends turn TCP's
"at most once, no receipt" into "exactly once, provably". Eighth project in
the family (`tick2trade` = the trading hot path, `matchd` = the exchange,
`ticklab` = research infra, `seqbus` = the transport tier, `riskgate` =
pre-trade checks, `feedarb` = feed arbitration, `wirecodec` = the codec).

```
 firm (client)                                        venue (server)
 ClientSession ── LoginReq{session, next_seq} ──────► ServerSession
   counts seq   ◄─ LoginAccept ◄─ S,S,S ... ────────── journal cursor chases
   implicitly      (replay == live: cursor catch-up)   the tail: REPLAY and
   orders ──────── U{token,...} ────────────────────►  LIVE are one loop
   resend all   ◄─ S{ack token} ◄──────────────────── SeenRing dedup: resent
   unacked on      heartbeats both ways; silence ►     token == executed
   every login     peer declared dead                  exactly once
```

## Measured results

Windows 10 VM on AMD EPYC 7552 (20 vCPUs, no SMT), VS2026 (MSVC 14.51)
Release + LTO, venue and firm threads pinned to separate cores at
time-critical priority, real Winsock TCP over loopback, TCP_NODELAY on.

```
framing (no sockets): incremental parse of 2M 35-byte frames
  chunk 1448 B (MTU-ish)   183.4M frames/s   6.4 GB/s     (~5.5 ns/frame)
  chunk 64 KiB             157.9M frames/s   5.5 GB/s
  chunk 16 B (adversarial)  52.7M frames/s   1.8 GB/s

rtt: order -> venue -> sequenced ack, wire-to-wire (ns)
  round trip           p50   7169   p90  10375   p99  14805   p99.9  39637

window: 500k orders, 64 in flight
  2.80M orders/s round-tripped, 3.64M sequenced msgs/s delivered

recover: RST with ~32 orders + acks in flight, 200 drills (ns)
  crash to whole       p50 436484   p90 484982   p99 693897   max  1396549

integrity (whole run): 616,400 tokens sent | executed exactly once OK
  all acked OK | client stream digest == venue journal digest OK
  201 sessions | 6,400 resends -> 6,400 dups absorbed | 0 proto errors
```

Three readings:

- **The kernel tax, priced.** The session *logic* costs ~5.5 ns/frame; the
  loopback TCP round trip costs ~7,200 ns — over a real NIC it would be worse.
  seqbus moves a message between processes in ~200 ns. That three-orders-of-
  magnitude gap between "what the protocol costs" and "what the socket costs"
  is the entire business case for kernel-bypass NICs, and now it has a
  number on this box. (The ~µs max outliers are hypervisor scheduling, per
  the family note.)
- **Recovery is half a millisecond, proven whole.** Each drill RSTs the
  socket mid-burst (a crash, not a goodbye — in-flight bytes destroyed),
  then: notice, reconnect, log in with `next_seq`, replay the missed acks,
  resend everything unacked, converge. 200 crashes, 6,400 orders resent,
  6,400 duplicates absorbed by the venue's dedup ring — and the final audit
  still shows every one of 616,400 tokens executed exactly once, with the
  client's received stream digest-identical to the venue's journal.
- **Nagle didn't bite — by design, and that's the lesson.** `--nagle` runs
  measure no penalty here: strict ping-pong never has unacked data when it
  sends (the reply's ACK piggybacks), and the tx ring coalesces every burst
  into one `send()`. The classic write-write-read Nagle disaster needs
  chatty small writes on a real network; batching your writes is the actual
  production defense, TCP_NODELAY is the belt to those suspenders.

## Why the design is what it is

- **The journal is the send queue.** The server appends every sequenced
  message to a journal; each connection is just a cursor chasing the tail.
  A freshly reconnected client is not in a special "recovery mode" — it is
  merely *behind*, exactly like a slow client. One loop, no mode switch, no
  replay-vs-live handoff bug, because there is no handoff.
- **Sequence numbers are implicit.** Like SoupBinTCP, `S` frames carry no
  seq field: the client counts from its login point. A gap is not detected
  and handled — it is *unrepresentable*. The one number the client persists
  across reconnects (`next_seq`) is the entire session state.
- **In-flight ambiguity has a two-part answer.** Downstream (acks lost with
  the connection): journal replay restores them. Upstream (orders that may
  or may not have arrived): resend all unacked, and the venue's SeenRing —
  one probe against the full 64-bit token, feedarb's ABA-proof dedup ring
  reused for idempotency — drops what it already executed. Both halves are
  exercised 200 times per benchmark run and audited exactly.
- **Framing is an incremental state machine, not a parser.** TCP hands you
  half a header, three frames glued together, or one byte at a time — the
  FrameReader stitches across reads with a fixed buffer and delivers whole
  frames only. The tests deliver 5,000 messages at 1..7-byte read sizes and
  kill connections with a torn frame in the buffer; `reset()` on reattach is
  what keeps old-stream garbage from poisoning the new one.
- **Heartbeats are the liveness layer TCP doesn't give you.** A dead peer
  can stay invisible to TCP for minutes; a trading session declares death
  after 5 quiet intervals and moves on. Both sides send-on-idle and
  timeout-on-silence, driven by an injected clock — so the tests script
  "server goes silent for exactly 5 intervals" deterministically.
- **Sessions are transport-generic.** The state machines are written against
  a 2-function ByteTransport concept; Winsock satisfies it in the benchmark,
  and a deterministic in-memory pipe satisfies it in the tests — with knobs
  for read chunking, byte-budget starvation, and crash-with-data-in-flight.
  The chaos sweep (50k orders, ~40 random kills, random read sizes) holds
  the exactly-once invariant on every run.

## Build & run

```
cmake --preset vs2026            # or configure from the solution root
cmake --build --preset vs2026-release
build\vs2026\Release\sesslink_tests.exe
build\vs2026\Release\sesslink.exe --rt
```

Options: `--rtt/--warmup/--win-orders/--window/--recover/--fill-pm/--port/
--nagle/--core-srv/--core-cli/--no-pin/--rt`.

## Layout

```
include/sl/core/wire.hpp       frame types, payloads, incremental FrameReader
include/sl/core/transport.hpp  ByteTransport concept (+would-block/closed)
include/sl/core/flat_hash.hpp  SeenRing dedup; FlatHash w/ backshift deletion
include/sl/core/session.hpp    Journal, TxRing, ServerSession, ClientSession
include/sl/net/tcp.hpp         non-blocking Winsock/POSIX TCP + RST abort()
include/sl/sim/mem_link.hpp    deterministic in-memory transport for tests
include/sl/sim/apps.hpp        VenueApp (dedup+ack+fill), FirmApp (resend)
src/main.cpp                   parse/rtt/window/recover benchmark + audit
src/tests.cpp                  scenario + chaos tests over MemLink
```
