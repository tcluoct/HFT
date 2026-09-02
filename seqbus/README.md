# seqbus — a wait-free shared-memory broadcast bus

The transport tier of an HFT plant: one writer publishes fixed-size records
into a shared-memory ring; any number of reader **processes** follow along at
a few hundred nanoseconds of one-way latency. This is the backbone that
connects feed handler → strategy → gateway when they are separate processes —
the role LMAX's Disruptor and Aeron IPC play in real firms, and the layer
tick2trade's in-process SPSC ring cannot cover (its messages go to exactly
one consumer in the same address space; seqbus gives *every* reader *every*
message across address spaces). Fourth project in the family (`tick2trade` =
the trading hot path, `matchd` = the exchange, `ticklab` = research infra).

```
 process A (writer)               shared memory (named mapping)
 publish(msg) ────────────▶ ┌───────────────────────────────────────┐
   fixed sequence of         │ header page: magic | capacity | pub_seq │
   stores; never waits,      │ ring of 64-B versioned slots:           │
   never aware of readers    │  [stamp|payload][stamp|payload] ...     │
                             └───────────────────────────────────────┘
                                  ▲             ▲             ▲
                              reader P1     reader P2     reader P3
                              busy-spin on next slot's stamp;
                              torn reads and overruns detected by
                              stamp arithmetic → count loss, resync
```

## Measured results

Windows 10 VM on AMD EPYC 7552 (20 vCPUs, no SMT), VS2026 (MSVC 14.51)
Release + LTO, every participant pinned to its own core at time-critical
priority. 20M measured messages (2M warmup), 65,536-slot ring, hop latency =
reader `rdtscp` at delivery − writer `rdtscp` at publish (the invariant TSC
is package-wide, so cross-core and cross-process deltas are valid).

**Fan-out — writer at full speed, N reader threads:**

```
readers   writer throughput   ns/publish     hop p50 (ns)   hop p99 (ns)   dropped
      1     16.11M msg/s          62             90              231           0
      3      5.19M msg/s         193            209 / 324 / 331  302–440       0
      6      4.80M msg/s         208            231–546          302–765       0
```

**Threads vs processes — 3 readers, full speed:**

```
                     writer throughput    hop p50 (ns)       hop p99 (ns)
reader threads         5.19M msg/s        209 / 324 / 331    302 / 433 / 440
reader PROCESSES       5.26M msg/s        209 / 331 / 331    251 / 404 / 404
```

The headline: **crossing a process boundary costs nothing.** Cache coherence
works on physical addresses; once the pages are mapped, the protocol cannot
tell threads from processes (the process run is marginally *cleaner* —
separate address spaces share less incidental state). p99.9 sits at 2–5 µs
and the ~210 µs max is hypervisor scheduling, visible identically in every
run on this box; on bare metal with isolated cores both collapse.

**Overload — 64-slot (4 KiB) ring, writer at full speed, 10M messages:**

```
reader    delivered      dropped   gaps   integrity failures
     0   11,999,554          447      2                    0
     1   11,996,056        3,945     38                    0
     2   11,996,287        3,714     13                    0
                                          delivered + dropped == published, all readers
```

Loss under overrun is *detected, counted, and exact* — never silent, never
corrupt. Every delivered message carries a checksum over its own body;
integrity is measured by the harness, not assumed.

## Why single-writer broadcast is the design (not a shortcut)

The defining property: **`publish()` is wait-free and reader-oblivious** — a
fixed, bounded sequence of stores with no locks, no RMW atomics, no reader
bookkeeping, and no backpressure. A reader that stalls, deschedules, or dies
cannot slow the market-data path by one cycle. matchd designs away
concurrency by giving the matching core one thread; seqbus designs away
*flow control* by making loss the reader's problem:

- A reader that falls more than one ring behind is **lapped**. It detects
  this from stamp arithmetic alone, counts exactly what it missed, and
  resyncs to the live edge (`kGap` tells the application to trigger
  recovery — in production, a snapshot request).
- For market data this is the correct contract: the value of a quote decays
  in microseconds (ticklab measured the same alpha losing money at 100 µs),
  so **freshness beats completeness** — a transport that blocks the producer
  to spare a slow consumer would be optimizing for the wrong side.

## The protocol (four lines of memory ordering)

Each 64-byte slot is its own seqlock whose stamp also encodes *which*
sequence number it holds: `2s+1` = write of `s` in progress, `2s+2` = `s`
published. The writer visits a slot with `s, s+N, s+2N, …`, so stamps
strictly increase — there is no ABA, and one comparison classifies a slot as
not-yet-written, ready, or overwritten (reader lapped):

```
writer                                   reader (expecting seq s)
  stamp ← 2s+1        relaxed             v1 ← stamp              acquire
  fence(release)                          words → copy            relaxed
  words ← payload     relaxed             fence(acquire)
  stamp ← 2s+2        release             v2 ← stamp              relaxed
  pub_seq ← s+1       release             valid iff v1 == v2 == 2s+2
```

The payload is seven *relaxed atomic* words, not raw bytes — that makes the
seqlock formally data-race-free under the C++ memory model (Boehm, MSPC'12)
while compiling to the same plain `MOV`s on x86-64, where TSO hardware makes
every fence above compiler-only. The same source is correct, with real fence
instructions, on ARM.

## The technique inventory (what's new vs the other three projects)

| Technique | Where | Why |
|---|---|---|
| **Per-slot seqlock with lap-encoding stamps** | `core/record.hpp` | One u64 answers "in progress?", "published?", *and* "was I lapped?" by pure arithmetic. Torn reads and overruns are detected, never delivered — proven by a stress test hammering a 64-slot ring at full speed (0 bad checksums in 2M msgs, 30k+ laps). |
| **Wait-free reader-oblivious publish** | `core/bus.hpp` | The transport analogue of matchd's single-threaded engine: the hard problem (backpressure) is designed away, not synchronized away. Measured: publish cost is flat from 3 to 6 readers. |
| **Relaxed-atomic payload words** | `core/record.hpp` | The seqlock that is formally correct in the C++ memory model *and* free on x86 — the difference between "works on this compiler" and works. |
| **std::atomic in shared memory, cross-process** | `core/shm.hpp`, `core/bus.hpp` | Named mappings (Windows `CreateFileMapping` / POSIX `shm_open`); `is_always_lock_free` static_assert because a lock-based atomic would put the lock in one process only. Exclusive create rejects stale buses from crashed runs. Header magic stored release-last so attach-vs-format races fail cleanly. |
| **Cross-process one-way latency via invariant TSC** | `harness` | Writer stamps `rdtscp` into the payload; readers difference it against their own. Package-wide invariant TSC makes the delta valid across cores *and* processes — no clock handshake protocol needed. |
| **Self-verifying messages** | `harness/msg.hpp` | Every payload's last word is a splitmix64 mix of the rest: any torn or misdelivered message is detectable in isolation. The integrity line in every report is computed, not asserted. |
| **C++20 concept-constrained payloads** | `BusPayload` | Trivially copyable, ≤ 56 bytes, ≤ 8-byte alignment — enforced at compile time, so the wire contract is a type error rather than a runtime surprise. |
| Flat power-of-two ring, mask indexing, `[[likely]]` hot path, cache-line `alignas`, `/O2 /Ob3 /GR-` + LTO | throughout | Carried over from the family — see tick2trade `docs/DESIGN.md`. |

## Build & run

```pwsh
cmake --preset vs2026            # or vs2022; or open the folder in VS directly
cmake --build --preset vs2026-release

build\vs2026\Release\seqbus_tests.exe    # 11 protocol scenarios + 2 stress invariants
build\vs2026\Release\seqbus.exe          # 1 writer + 3 reader threads, paced 5M msg/s
build\vs2026\Release\seqbus.exe --proc --rate 0 --rt   # separate PROCESSES, full speed
```

```
--readers N    reader count, 1..8            --core N     writer core; reader i on N+1+i
--msgs N / --warmup N                        --no-pin / --rt
--rate N       msg/s, 0 = full speed         --name S     shm segment base name
--ring P       capacity 2^P slots (16 = 4 MiB)
--proc         writer + readers as separate processes (harness respawns itself;
               they meet only through the two named segments)
```

The harness writer publishes checksummed messages (warmup, measured, stop);
readers verify every delivery, histogram the measured hops, and ship results
back through a second small shm segment — in `--proc` mode nothing but shared
memory connects the participants. The report always ends with the two
computed invariants: zero bad deliveries, and `delivered + dropped ==
published` for every reader.

## Lab vs production

| Here | Production (Aeron IPC / Disruptor-class) |
|---|---|
| Fixed 64-B records, one cache line | Variable-length framed messages in term buffers; batching amortizes per-message cost far below our 62 ns |
| Lapped reader jumps to the live edge and counts the hole | Gap triggers a recovery protocol: snapshot service or retransmission channel re-primes the reader |
| One writer per bus, by architecture | Multi-producer variants exist (CAS to claim slots); single-writer remains the latency configuration |
| Readers busy-spin, always | Tiered idle strategies (spin → yield → park) for buses that must coexist with other work |
| Two ad-hoc named segments | Media driver owns mappings, lifecycle, and counters; monitoring reads the same shm |
| EPYC VM, ~210 µs hypervisor outliers | Bare metal, isolated cores, sub-100 ns hops, no outlier past a few µs |

## Layout

```
../common/include/hft/common/   platform, tsc_clock, histogram (solution-wide toolbox)
include/sb/
  core/     shm.hpp     named shared-memory segments (Win32 / POSIX)
            record.hpp  the 64-B versioned slot + stamp arithmetic
            bus.hpp     Bus format/attach · Writer · Reader (the core)
  harness/  msg.hpp     self-verifying 56-B message
src/main.cpp   bench: thread and multi-process modes, ctl segment, report
src/tests.cpp  11 scenarios: ordering, wrap, late join, lap/resync,
               in-progress visibility, torn-read + slow-reader stress
```
