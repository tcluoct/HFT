# hft — a family of low-latency trading-infrastructure labs

Fourteen standalone C++23 projects, one per layer of an HFT plant, sharing
one toolbox. Each project is a working, benchmarked implementation of a core
piece of trading infrastructure, deliberately built with *different*
low-latency techniques so the family doubles as a technique catalog. Every
README reports real numbers measured on this machine, and every core is
proven by framework-less scenario tests.

```
                        ┌───────────────────────────────────────────────────┐
 venue side             │                firm side                          │
┌──────────┐  A/B UDP   │ ┌─────────┐   ┌──────────┐  ┌──────┐ ┌─────────┐  │
│  matchd  │ ══════════▶│ │ feedarb │──▶│tick2trade│─▶│ omsd │▶│riskgate │──┼─▶ orders
│ matching │            │ │ A/B arb │   │ book+strat│ │ OMS  │ │pre-trade│  │   (wirecodec
│  engine  │◀═══════════┼─┴─────────┘   └──────────┘  └──────┘ └─────────┘  │    frames,
└──────────┘  sesslink: │       │  seqbus: shared-memory transport       │  │    sesslink
              resumable │       ▼             between processes          ▼  │    sessions)
              exactly-  │   ticklab: capture ▸ replay ▸ backtest            │
              once TCP  │─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─│
                        │ symtab: wire symbol ▸ dense id   sord: SOR        │
                        │ sigcalc: ticks ▸ signal vector   quotd: MM quotes │
                        │ pnld: fills ▸ positions ▸ P&L (feeds risk/quotes) │
                        └───────────────────────────────────────────────────┘
```

| Project | Layer | Signature techniques |
|---|---|---|
| [tick2trade](tick2trade/) | Firm hot path: feed → book → strategy → gateway | SPSC rings, flat-ladder L3 book, async deferred-format logger, full-pipeline tick-to-trade measurement |
| [matchd](matchd/) | Venue: price-time matching engine | Slotmap order handles, intrusive FIFO levels, single-threaded determinism + journal/replay digest proof |
| [ticklab](ticklab/) | Research: capture / replay / backtest | Columnar frame-of-reference codec, mmap I/O, latency-sweep alpha-decay measurement |
| [seqbus](seqbus/) | Transport: shared-memory broadcast bus | Per-slot seqlock stamps, wait-free reader-oblivious publish, cross-process TSC latency |
| [riskgate](riskgate/) | Compliance: pre-trade risk gate (15c3-5) | Branchless reject-bitmask rules, write-rare seqlock'd limits hot-reload, TSC token bucket, 128-bit notional |
| [feedarb](feedarb/) | Feed front door: A/B line arbitration | Seq-window bitmap reorder ring, full-seq dedup, gap-declaration state machine, two-clock harness |
| [wirecodec](wirecodec/) | Wire boundary: schema-driven codec | NTTP + consteval compile-time layouts, flyweight zero-copy decode, `std::byteswap`, hardware CRC32-C |
| [sesslink](sesslink/) | Session layer: resumable exactly-once order entry | Real non-blocking TCP, incremental stream framing, journal-cursor replay, token-dedup resend, heartbeat liveness, transport-as-concept chaos tests |
| [omsd](omsd/) | Order management: state machine + drop-copy recon | Exhaustive 7x8 verdict matrix, choke-point exposure accounting, grace-period prune queue, exact 4-class reconciliation |
| [symtab](symtab/) | Reference data: the instrument master | Hash-and-displace minimal perfect hash, wire-word symbol keys, fastrange, epoch-based snapshot reclamation, batched prefetch lookups |
| [sigcalc](sigcalc/) | Signals: per-tick microstructure features | Monotonic-wedge O(1) extrema, exact integer rolling windows, shift-only EWMA lattice, first explicit AVX2 (srlv + bias trick) proven bit-equal to scalar |
| [pnld](pnld/) | Accounting: positions & P&L | FIFO lot ring, dual FIFO/WAC attribution, rule-independent equity identity audit, O(1) firm rollup deltas |
| [sord](sord/) | Routing: smart order router | Compile-time Batcher sorting networks, packed u64 rank keys, all-in venue cost model, largest-remainder splits, naive-vs-model quality duel |
| [quotd](quotd/) | Market making: two-sided quote engine | Desired-vs-working diff engine, queue-priority hysteresis band, inventory skew LUT, O(1) epoch mass-pull, adverse-selection fill sim |

## Building

The whole solution (every project + one `hft.sln` for Visual Studio):

```pwsh
cmake --preset vs2026                    # or vs2022; or open the folder in VS
cmake --build --preset vs2026-release
build\vs2026\<project>\Release\<project>.exe          # bench harness
build\vs2026\<project>\Release\<project>_tests.exe    # scenario tests
```

Each project also configures standalone — run the same two commands inside
its directory; it pulls in `common/` and the shared build config itself.

## Shared pieces

- `common/` — the `hft::common` interface library (namespace `hft`):
  `platform.hpp` (pinning, cache-line, pause), `tsc_clock.hpp` (calibrated
  rdtsc/rdtscp), `histogram.hpp` (log-linear latency histogram),
  `spsc_ring.hpp`, `async_logger.hpp`, `run_control.hpp`, `bench.hpp`
  (signal-fenced bulk timing — MSVC's `__rdtscp` is not a compiler barrier,
  and unfenced measured loops read as 0 ns under LTO).
- `cmake/HftConfig.cmake` — one `hft_configure(target)` applying the family
  build contract: C++23, `/W4 /O2 /Ob3 /GR- /arch:AVX2` + LTO on MSVC.
- Conventions: header-only cores under `include/<abbrev>/`, a bench harness
  (`src/main.cpp`) and scenario tests (`src/tests.cpp`, CHECK macro, no
  framework) per project, measured-results READMEs, deterministic seeds.

## Measurement environment

All numbers in the READMEs: Windows 10 VM on AMD EPYC 7552 (20 vCPUs, no
SMT), VS2026 (MSVC 14.51) Release + LTO, threads pinned, time-critical
priority. `rdtscp` has a ~20 ns floor here, and every run shows rare
hundreds-of-µs max outliers from hypervisor scheduling — called out in the
reports rather than chased. tick2trade's `docs/DESIGN.md` holds the long-form
rationale the other projects reference.
