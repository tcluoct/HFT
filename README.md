# hft — a family of low-latency trading-infrastructure labs

Fourteen standalone C++23 projects, one per layer of an HFT plant, sharing
one toolbox. Each project is a working, benchmarked implementation of a core
piece of trading infrastructure, deliberately built with *different*
low-latency techniques so the family doubles as a technique catalog. Every
README reports real numbers measured on this machine, and every core is
proven by framework-less scenario tests.

`platform/` is the production system those labs graduate into. Its strategy
lineup is not part of the engine: strategy teams own separate static-library
projects under `strategies/`, and a `desk` project composes the teams into
the lineup the platform binaries link. See
[strategies/README.md](strategies/README.md) and [desk/README.md](desk/README.md).

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

## Production composition: platform, strategy teams, desk

The platform is one execution system many strategy teams trade on, and the
build graph says so:

```
plat_core → strat_sdk → strategies/<team>          one static_lib per team
                ↑                      ↓
         platform_lib (engine)  →   desk   →  platd / platbt / platops / …
```

| Project | Role |
|---|---|
| [platform](platform/) | Engine + strategy SDK. Ships `plat/sdk/` (the CRTP contract and primitives) and the trading stack; ships **no concrete strategy**. |
| [strategies/trend](strategies/trend/) | `strat_trend` — snapshot momentum + session breakout. |
| [strategies/fade](strategies/fade/) | `strat_fade` — snapshot mean-reversion. |
| [strategies/basis](strategies/basis/) | `strat_basis` — futures–spot arbitrage legs (Phase 2). |
| [strategies/cash](strategies/cash/) | `strat_cash` — T+0 alpha on carried stock inventory (Phase 2). |
| [desk](desk/) | The composition root: appends the fielded teams' `Def`s to a `StratPack` and exports the `Pack`/`PackSite` the platform apps link. |

A team library links only `strat_sdk` and never the engine; the platform
engine never includes a strategy header. A new strategy type is a team-local
header plus one append-only line in `desk/pack.hpp` — no platform change.

## Building

The whole solution (every project + one `hft.sln` for Visual Studio):

```pwsh
cmake --preset vs2026                    # or vs2022; or open the folder in VS
cmake --build --preset vs2026-release
build\vs2026\<project>\Release\<project>.exe          # bench harness
build\vs2026\<project>\Release\<project>_tests.exe    # scenario tests
build\vs2026\strategies\<team>\Release\strat_<team>_tests.exe   # team suite
build\vs2026\desk\Release\desk_tests.exe              # desk/lineup suite
```

One-command QA gate (configure + full build + every suite + `platd --check`):

```pwsh
pwsh -File tools/run-qa.ps1              # -SkipConfigure / -SkipBuild for incremental runs
```

CI runs the same checks on Windows (`.github/workflows/ci.yml`).

Each project also configures standalone — run the same two commands inside
its directory; it pulls in `common/` and the shared build config itself.

On Windows with Visual Studio 2026, `build-vs.cmd` locates the installed IDE
and bundled CMake, configures the solution, and builds Release x64.
Use `open-vs.cmd` to open the generated solution with the same environment.
The startup project is `platd`: F5 runs it
with `--fleet <repo>\platform\platd.fleet.example.cfg`, working from its
output directory, where the CTP DLLs and the runtime journals/logs live. The
config is read from the source tree, so editing
`platform/platd.fleet.example.cfg` in the IDE takes effect on the very next
F5, and a copy of both example configs is refreshed beside the exe on every
build for runs started outside the IDE. This starts the configured CTP
sessions; an expired contract symbol in the fleet config aborts a shard at
startup (the counter answers "not found", leaving no instrument to trade),
and `platd_fleet.log`, `shardN.log` and `shardN.jnl.log` in the output
directory record why. Use `--check platd.example.cfg` in the project's
Debugging arguments for an offline configuration/SDK check instead.
Both launchers restore missing standard Windows folder variables locally;
they do not change registry or machine settings.

If MSBuild reports `MSB4018` in `FileTracker.InitializeCommonApplicationDataPaths`
with an invalid-path exception, its parent process may have omitted
`ProgramData` or `SystemDrive`, leaving the registered `%ProgramData%` path
unexpanded. Save your work, close the existing HFT Visual Studio window,
and reopen using `open-vs.cmd`. An already running IDE retains its old
environment. Do not disable compiler dependency tracking to work around this.

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
