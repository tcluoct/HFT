# matchd — a deterministic HFT matching engine

The exchange-side core: a price-time-priority limit-order-book matching engine
of the kind NASDAQ/CME run at their heart, built single-threaded and
deterministic, with an OUCH-style binary order-entry protocol, journaling with
digest-verified replay, a scenario test suite for the matching rules, and a
latency/throughput harness.

Companion project to `C:\work\tick2trade` (the trading-firm side); this is the
venue side. The two compose: matchd could replace tick2trade's toy fill model.

```
inbound (OUCH-style 32-byte msgs)            outbound (via compile-time Sink)
  Enter / Cancel / Replace                     participant: accept/cancel/
        │                                        replace/reject/exec
        ▼                                      market data: add/exec/
┌──────────────────────────────────────┐        reduce/delete (anonymous)
│ MatchingEngine  (ONE thread)         │
│  • flat price ladder per symbol      │      ┌──────────────────────────┐
│  • intrusive FIFO per price level    │      │ Journal (32-B records)   │
│  • slotmap order handles             │─────▶│ replay through a fresh   │
│  • price-time priority matching      │      │ engine == byte-identical │
│  • IOC / FOK / replace / STP         │      │ outbound (digest proof)  │
│  • zero allocation after startup     │      └──────────────────────────┘
└──────────────────────────────────────┘
```

## Measured results

Windows 10, VS2026 (MSVC 14.51) Release + LTO, engine pinned to one core,
time-critical priority, 10M operations against a ~42k-order resting book:

```
op latency (ns, incl ~20ns timer)     count      min      p50      p90      p99    p99.9
-----------------------------------------------------------------------------------------
  enter: passive (no trade)         3,454,830       20       20       20       30       30
  enter: aggressive (trades)        1,402,244       20       20       20       30       30
  cancel                            1,879,638       20       20       20       30       30
  replace                             285,850       20       20       20       30       40
  rejected (stale handle etc)       2,977,438       20       20       20       30       30

throughput: 10,000,000 ops in 1.093 s = 9,147,584 ops/sec
trades: 2,522,287 | resting book steady-state ~42k orders (pool high-water 42,742)
determinism replay: PASS (digest ce58586bf485b380 == ce58586bf485b380 over 11,000,000 msgs)
```

Median cost of any operation sits at the rdtscp measurement floor (~20 ns ≈ 44
cycles): with the book resident in cache and every layer inlined, a match or an
insert is a handful of stores plus the event digest. The meaningful figures are
the p99/p99.9 (30–40 ns) and the ~9M ops/s single-core throughput — roughly 10×
a full production day of NASDAQ order flow per minute.

## Why single-threaded is the design (not a shortcut)

Real matching engines are single-threaded per symbol partition, and it is a
latency *and* correctness technique:

- **No synchronization at all** in the core: no locks, no atomics, no memory-
  ordering arguments. The concurrency problem is designed away.
- **Determinism**: same inbound sequence ⇒ byte-identical outbound events.
  That single property buys journaling (the 32-byte inbound records *are* the
  full state), disaster recovery and hot-standby failover (ship the journal,
  replay it), byte-exact testing, and dispute resolution.
- **Fairness** is decided once, at the front door, by sequencing — not by
  which thread wins a lock race.

The harness *proves* the property on every run: it journals all inbound
messages, replays them through a fresh engine, and compares FNV digests of
every field of every outbound event.

## Matching semantics implemented

| Rule | Behavior |
|---|---|
| Price-time priority | Better price first; at equal price, strict FIFO by arrival. Structural: ladder scan gives price priority, tail-insert/head-match gives time priority. |
| Trade price | Always the **resting** order's price (aggressor gets price improvement). |
| Accept-then-match | Orders are accepted and assigned their id *before* matching (real exchange flow — executions reference the aggressor's id). |
| IOC | Match immediately, cancel the remainder, never rests. |
| FOK | Pre-scan crossable liquidity; kill with zero fills unless fully fillable. |
| Replace, qty decrease @ same price | In place — **time priority kept**, same order id, market data `reduce`. |
| Replace, price change or qty increase | **Priority lost**: leaves the book, old id goes stale, fresh id issued, re-enters through matching (may trade immediately). |
| Self-match prevention | Cancel-resting policy: an aggressor meeting its own resting order cancels it (reason `SelfMatch`) and keeps matching. No self-trades, ever. |
| Stale handles | Cancel/replace of a filled/canceled/replaced order is *detected* and rejected (`UnknownOrder`) — the cancel-racing-a-fill case. |

Every rule above has a scripted scenario test in `src/tests.cpp` asserting the
exact outbound event sequence (`matchd_tests.exe` — 10 scenarios, no framework).

## The technique inventory (what's new vs tick2trade)

| Technique | Where | Why |
|---|---|---|
| **Slotmap / generational-index order handles** | `core/order_pool.hpp` | `OrderId = (generation << 24) \| slot`. Cancel lookup = mask + one compare: O(1), **no hash map at all**, and stale handles are detected (freeing bumps the generation) rather than silently mismatching. Measured: stale-cancel rejection in ~20 ns. |
| **Intrusive index-linked FIFO levels** | `core/order_pool.hpp`, `core/book.hpp` | Orders link into price levels via 32-bit `prev`/`next` indices: half the size of pointers, no per-node allocation, relocatable/serializable, and unlink is O(1). |
| **Single-writer determinism + journal/replay proof** | `core/matching_engine.hpp`, `journal/journal.hpp` | See above — the architectural trick of exchange design. |
| **Compile-time Sink** | `matching_engine.hpp` (template param) | All outbound events are inlined calls; the engine does zero serialization/I-O. Same zero-virtual philosophy as tick2trade's CRTP strategy, applied to the output side. |
| **Accept-before-match with pre-assigned ids** | `enter()` | The id *is* the pool slot, so assignment is free — enabling the real-exchange reporting flow without an id-mapping table. |
| Flat price ladder, freelist pool, fixed-point prices, `[[unlikely]]` rejects, `noexcept` hot path, `/utf-8 /O2 /Ob3 /GR-` + LTO | throughout | Carried over from tick2trade — see its `docs/DESIGN.md` for full explanations. |

## Build & run

```pwsh
cmake --preset vs2026            # or vs2022; or open the folder in VS directly
cmake --build --preset vs2026-release

build\vs2026\Release\matchd_tests.exe          # 10 matching-rule scenarios
build\vs2026\Release\matchd.exe --ops 10000000 --warmup 1000000 --rt
```

```
--ops N / --warmup N   measured / warmup operation counts
--symbols N            1..4 books
--core N / --no-pin / --rt
--no-stp               disable self-match prevention
--seed N               order-flow RNG seed
--journal PATH         save the inbound journal (32 B/msg, binary)
--replay PATH          replay a saved journal, print its digest
```

The harness generates a self-balancing order flow (passive adds, cancels and
replaces drawn from an open-order list, aggressive/IOC crossers, FOK), keeps
the resting book in equilibrium (~42k orders), journals everything, and
finishes with the determinism replay. Cancel targets are picked
*optimistically* — some reference orders that fills already killed, which is
deliberate: that's what cancel/fill races look like, and it exercises the
stale-handle path under load.

## Lab vs production

| Here | Production |
|---|---|
| In-process harness calls `process()` | An order-gateway tier terminates sessions (OUCH/FIX), sequences all inbound into one stream, then feeds the engine. The *sequencer* is the fairness point. |
| Journal in memory / flat file | Journal replicated synchronously to standby engines before acking (that's the real durability boundary), plus wire capture. |
| One engine, up to 4 books | Symbols partitioned across many single-threaded engine instances; a market-data tier fans out the anonymous stream (ITCH). |
| FOK counts all liquidity incl. own | Real STP×FOK interaction is venue-specific and hairier. |
| Uniform random flow | Real flow is bursty and adversarial; production perf tests replay recorded sessions. |

## Layout

```
../common/include/hft/common/   platform, tsc_clock, histogram (solution-wide toolbox)
include/me/
  core/     types.hpp · order_pool.hpp (slotmap) · book.hpp (ladder+FIFO)
            matching_engine.hpp (the core)
  proto/    ouch.hpp   32-byte inbound msgs + Sink interface contract
  journal/  journal.hpp  journal + FNV digest
src/main.cpp   flow generator, bench, determinism replay, report
src/tests.cpp  10 scripted matching-rule scenarios
```

Note: `MatchingEngine` embeds ~1.6 MB of price ladders — always heap-allocate
it (`make_unique`). Constructing it on a default 1 MB Windows thread stack is
an instant 0xC00000FD stack overflow (found the hard way; now documented on
the class).
