# feedarb — A/B feed-line arbitration

The front door of every production feed handler. Venues broadcast the same
sequence-numbered stream on two multicast lines (A and B) over disjoint
network paths; subscribers listen to both and take whichever copy of each
packet arrives first. Arbitration is where three ugly realities of UDP
multicast — loss, duplication, reordering — get turned back into the gapless
in-order stream everything downstream (tick2trade's book, ticklab's capture)
silently assumes. Sixth project in the family.

```
 venue ──► line A ──► ─┐        first copy wins ──► gapless, in-order ──► book
   seq 1,2,3,...       ├──► arbiter:
 venue ──► line B ──► ─┘     · seen-ring dedup (full seq stored, no ABA)
   (same packets,            · 1024-seq reorder window: bitmap + slot ring
    different path)          · hole outlives timeout ► on_gap(lo,hi), move on
                             · seq beyond window ► fast-forward resync
```

## Measured results

Windows 10 VM on AMD EPYC 7552 (20 vCPUs, no SMT), VS2026 (MSVC 14.51)
Release + LTO, arbiter thread pinned at time-critical priority. 20M venue
seqs → ~40M arrivals, venue sending every 100 ns (10M pkt/s per line), line A
5 µs / line B 7 µs propagation, ±2 µs jitter, 50 µs gap timeout. `rdtscp`
pairs around each `on_packet()+poll()` (~20 ns floor on this box).

```
                            per-arrival cost (ns)              sustained
impairment                  p50   p90   p99   p99.9            throughput
clean lines                  30    50    81     129          17.5M pkt/s
2% loss + 1% dups per line   30    40    90     162          17.8M pkt/s
```

**The impaired run, accounted for exactly** (nothing lost silently):

```
line A: received 19,795,800   wins 98.0%   dups    195,696
line B: received 19,795,163   wins  2.0%   dups 19,403,247
inter-line skew of raced seqs: p50 2.0 µs, p99 3.7 µs (= the configured A/B offset)
delivered 19,992,020 + declared lost 7,980 == 20,000,000     ✓
out-of-order deliveries 0 · corrupted payloads 0             ✓
```

Line B wins exactly when line A dropped (2%): the B line pays for itself as
insurance, not speed — precisely how real A/B pairs behave. The tests push
much harder: a 200k-seq sweep at 10% loss + 2% dups per line with 5 µs jitter
still ends with every sequence number accounted for exactly once (delivered
or inside a reported gap), verified by an exact-coverage walk and a full
internal-state audit after every arrival.

## Why first-wins + bounded window + declared loss is the design

- **Dedup must be O(1) and ABA-proof.** A `seen` ring indexed by
  `seq & (W-1)` storing the *full* sequence number answers "have I already
  got this?" with one probe — no hash, no ordered set, and a recycled slot
  can never impersonate an old seq because the comparison is against the
  full 64-bit value.
- **Reordering is not an error.** Jitter means seq n+3 legitimately beats
  seq n. Ahead-of-sequence packets park in the window (presence bitmap +
  slot array); holes usually fill from the other line a few µs later, and
  delivery order is restored for free by draining consecutively-parked seqs.
- **A hole must not stall the market.** When both copies are lost, waiting
  forever optimizes for the wrong side (the same freshness-beats-completeness
  contract as seqbus's lapped reader). A hole gets `timeout` from the moment
  it blocks the stream; then it is *declared* — `on_gap(lo, hi)`, exact
  range, downstream triggers snapshot recovery — and the stream moves on.
- **The failure mode is audited, not assumed.** The arbiter's invariant —
  every seq delivered exactly once or reported lost exactly once, in order —
  is checked by the harness on every run (`delivered + lost == sent`,
  out-of-order = 0) and by an `audit()` sweep of bitmap/slots/state in tests.
  This caught a real bug during development: a fast-forward that landed on a
  parked seq left `next_` sitting on it until a timeout "resolved" it as an
  empty gap.

## The technique inventory (what's new vs the other five projects)

| Technique | Where | Why |
|---|---|---|
| **Seq-window presence bitmap + slot ring** | `core/arbiter.hpp` | 1024-seq reorder buffer in 128 bytes of bitmap + a flat slot array: park, probe, and drain are mask-and-bit-test operations; `countr_zero` word scans find the lowest survivor when a gap is declared. |
| **Full-seq dedup ring (no ABA)** | `core/arbiter.hpp` | The twin identifies itself by matching the stored 64-bit seq — the same trick matchd's slotmap uses for stale order handles, applied to packet identity. |
| **Gap state machine with declared loss** | `core/arbiter.hpp` | kSynced ⇄ kGapPending with a deadline; loss is a *reported range*, never a silent skip — and a fast-forward path bounds worst-case recovery after an outage. |
| **Two-clock harness** | `src/main.cpp` | Sim time (ns) drives loss/jitter/timeout deterministically; real `rdtscp` measures only the arbitration work. Replayable *and* honest. |
| **Deterministic per-(seq,line) impairment RNG** | `sim/line_sim.hpp` | RNG re-seeded per packet: arrivals are identical regardless of batch boundaries, so 40M-arrival benches stream through a small carry buffer instead of materializing 1.6 GB. |
| **Exact-coverage verification** | `src/tests.cpp` | The delivered list and gap ranges must tile [1, N] with no overlap — an invariant strong enough to catch off-by-one resync bugs that throughput stats would hide. |
| **Inter-line skew/win telemetry** | `core/arbiter.hpp` | The dup path (the race loser) donates a skew sample per raced seq — line-health monitoring for free, in the histogram the family already uses. |

## Build & run

```pwsh
cmake --preset vs2026            # standalone; or build the whole hft solution from ..
cmake --build --preset vs2026-release

build\vs2026\Release\feedarb_tests.exe   # 10 scenarios + audited adversarial sweep
build\vs2026\Release\feedarb.exe --rt    # 20M seqs, 2% loss + 1% dups per line
build\vs2026\Release\feedarb.exe --rt --drop-pm 0 --dup-pm 0    # clean lines
```

```
--seqs N / --warmup N / --seed N      --drop-pm N   per-line loss, per mille
--delay-a N / --delay-b N (ns)        --dup-pm N    per-line dups, per mille
--jitter N (ns)                       --send-gap N  venue spacing (ns)
--timeout N   gap declaration (ns)    --core N / --no-pin / --rt
```

The harness always ends with the computed invariants: `delivered + lost ==
sent`, zero out-of-order deliveries, zero corrupted payloads (every packet
carries a splitmix64-derived payload verified at delivery).

## Lab vs production

| Here | Production feed handler front-end |
|---|---|
| Simulated lines (deterministic impairments) | Two NICs, kernel-bypass sockets (Onload/ef_vi/DPDK), real multicast joins |
| Declared gap → downstream's problem | Gap triggers the venue's recovery protocol: TCP retransmission request or snapshot channel re-prime |
| One arbiter thread | One arbiter per line pair per core, NUMA-pinned next to its NIC |
| 1024-seq fixed window | Sized from venue burst specs; some venues need per-channel windows (ITCH MoldUDP64 sessions) |
| Payload = one checksummed word | Full packet payloads handed off zero-copy to the book builder |
| ~30 ns arbitration on a VM | Same algorithm in single-digit ns on bare metal, or inside the FPGA before the host ever sees the packet |

## Layout

```
../common/include/hft/common/   platform, tsc_clock, histogram (solution-wide toolbox)
include/fa/
  core/     packet.hpp    the arbitration unit + payload mix
            arbiter.hpp   seen-ring dedup, window, gap FSM, audit() (the core)
  sim/      line_sim.hpp  deterministic dual-line impairment generator
src/main.cpp   bench: batched sim-time feed, real-time measurement, invariants
src/tests.cpp  10 scenarios: rescue, dead line, declared gap, reorder, stale,
               fast-forward, race stats, determinism, audited 200k sweep
```
