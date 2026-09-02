# tick2trade — an HFT tick-to-trade hot path in modern C++

A four-stage, wire-to-wire trading system of the shape every proprietary
trading firm runs, instrumented to attribute latency to each hop with TSC
timestamps. Everything on the hot path is allocation-free, lock-free,
virtual-call-free, and busy-spinning on a pinned core.

> **Deep dive:** [`docs/DESIGN.md`](docs/DESIGN.md) walks through the code
> logic of every component and explains all 26 low-latency techniques in depth
> (mechanism → cost avoided → where in the source), including the memory-model
> correctness argument for the SPSC ring and the order-book algorithms.

```
┌─────────────────────────────────────────────────────────────────┐
│ ExchangeSim (core A)                                            │
│  order-by-order market data generator + IOC fill engine         │
│  stamps t0 on md at "the wire", t4 on order arrival             │
└──────┬───────────────────────────────────────────▲──────────────┘
       │ MdRing (SPSC, 32-B msgs)                  │ OrderRing (wire)
       ▼                                           │
┌──────────────────────┐   BookRing   ┌────────────┴─────────────┐
│ FeedHandler (core B) │─────────────▶│ OrderGateway (core D)    │
│  L3 book: flat price │              │  pre-trade risk checks   │
│  ladder + open-addr  │              │  (15c3-5 style), t3      │
│  order-id map, t1    │              └────────────▲─────────────┘
└──────────────────────┘                           │ OrderRing
                       ┌───────────────────────────┴──────────────┐
        BookRing ─────▶│ ImbalanceTaker strategy (core C)         │
        ExecRing ─────▶│  CRTP dispatch, position + rate limits,  │
                       │  decision stamp t2                       │
                       └──────────────────────────────────────────┘

  t0 exchange wire → t1 book applied → t2 decision → t3 gateway out → t4 exchange in
  TICK-TO-TRADE = t4 - t0
```

## Measured results

Windows 10, 20 hardware threads, TSC 2.196 GHz, MSVC `/O2 /Ob3 /arch:AVX2` + LTO,
threads pinned to distinct physical cores, `--rt` (time-critical priority).

Paced at 1,000,000 md msgs/s for 10 s (10M messages, zero drops):

```
hop (all values ns)                          count      min      p50      p90      p99    p99.9        max
--------------------------------------------------------------------------------------------------------
  wire->book applied      (t1-t0)        10,014,145      140      517      677     1092     5887     238467
  book->decision made     (t2-t1)           212,802      100      151      209      404     1355     205335
  decision->gateway wire  (t3-t2)               235      551      648     1209     1894     3905       3917
  TICK-TO-TRADE wire-wire (t4-t0)               235     1733     2069     2885     4604     6003       6041
```

**Tick-to-trade p50 ≈ 2.1 µs, p99 ≈ 4.6 µs** — in software, through five SPSC
rings and four threads, with full L3 book maintenance on the way.

Burst mode (`--rate 0`) pushes ~**4.1M msgs/s** through the book on one core
(20.4M messages in 5 s). At saturation the classic queueing-theory picture
appears on cue: the median holds (~590 ns) while p99 explodes to milliseconds
and the md ring sheds 0.17% — when arrival rate ≥ service rate, latency is
queue depth, not code speed. Real feed handlers are sized so this never
happens (and shard symbols across cores when it would).

## Build & run

Requires a C++23-capable compiler. `CMakePresets.json` ships presets for
**Visual Studio 2026** (`vs2026`) and VS2022 (`vs2022`).

> Note: VS2026 must have the **"Desktop development with C++"** workload
> installed (the MSVC v145 toolset). Without it there is no `cl.exe` — the
> preset will fail to configure.

```pwsh
cmake --preset vs2026            # or vs2022
cmake --build --preset vs2026-release
build\vs2026\Release\tick2trade.exe --seconds 10 --warmup 2 --rt
```

Or just open the folder in Visual Studio — it detects CMakePresets natively.

```
--seconds N     measured run length (default 10)
--warmup N      warmup before measuring (default 2)   caches/branch predictors warm up;
                                                      the histograms reset at the boundary
--rate N        md msgs/sec, 0 = max speed (default 1,000,000)
--symbols N     1..4 (default 2)
--cores A,B,C,D pin exchange,feed,strategy,gateway (default 2,4,6,8)
--no-pin / --rt pinning off / time-critical priority
--seed N        market RNG seed
--log PATH      async binary-ish log (default t2t.log)
```

Default cores are even-numbered on purpose: Windows enumerates SMT siblings as
(2n, 2n+1), so 2,4,6,8 = four distinct physical cores. Putting two spinning
stages on hyperthread siblings of one core roughly triples the tail (measured
here: p99.9 went from ~900 µs to ~6 µs when unpaired).

## The low-latency technique inventory

Every technique below is used somewhere real in this codebase — file
references included.

### Concurrency & memory model
| Technique | Where | Why |
|---|---|---|
| Wait-free SPSC rings, acquire/release only | `common/spsc_ring.hpp` | The whole pipeline is SPSC by design: no CAS loops, no seq_cst fences. Producer release-publishes a slot; consumer acquire-observes it. |
| Cached counterpart index (rigtorp trick) | `spsc_ring.hpp` | Producer re-reads the consumer's index **only when the ring looks full** — steady-state push/pop causes zero cross-core coherence traffic on the counterpart's line. |
| False-sharing elimination | `spsc_ring.hpp` | Producer state and consumer state on separate cache lines via `alignas(std::hardware_destructive_interference_size)`. |
| Busy-spin + `_mm_pause` | every `run()` loop | No syscalls, no futexes, no wakeup latency on the hot path; PAUSE frees pipeline resources for the SMT sibling and saves power. |
| Thread pinning | `platform.hpp`, `main.cpp` | Warm L1/L2/TLB, no migrations. (Prod Linux: `isolcpus`/`nohz_full`/cpusets to evict the kernel too.) |
| Amortized control-plane checks | every `run()` loop | Phase flag sampled with one relaxed load every 4096 iterations — coordination costs nothing in steady state. |

### Data structures
| Technique | Where | Why |
|---|---|---|
| Flat price ladder | `md/order_book.hpp` | Price levels in a plain array indexed by tick offset: O(1) access, no tree, no pointers, touch area stays in L1. This is the canonical HFT book layout. |
| Open-addressing order-id map, SoA layout | `order_book.hpp` | Keys and values in separate arrays — the probe scan touches dense key lines only. Fibonacci (multiplicative) hashing. |
| Backward-shift deletion | `order_book.hpp` | No tombstones ⇒ probe lengths cannot degrade over a full trading day of adds/deletes. |
| Fixed-point prices (`int64` ticks) | `proto/wire.hpp` | Floats on a hot path: slower compares, non-associative math, phantom price levels. Integer ticks all the way. |
| Cache-line-conscious message sizes | `wire.hpp` (static_asserts) | `MdMsg` = 32 B (two per line), `NewOrder` = exactly 64 B (one line). |
| Pre-sized everything, zero hot-path allocation | all components | `vector::reserve` at startup, swap-pop removal, fixed-capacity rings/maps. The allocator is never called after init. |

### Dispatch & codegen
| Technique | Where | Why |
|---|---|---|
| CRTP static dispatch | `strategy/imbalance_taker.hpp` | The strategy inlines into the polling loop: no vtable, no indirect-branch misprediction per event. |
| `[[likely]] / [[unlikely]]` | throughout | Keeps the hot path fall-through and I-cache dense; error/full/empty paths out of line. |
| `T2T_FORCE_INLINE` on tiny hot functions | `platform.hpp` users | rdtsc/pause/ring ops must never become calls. |
| Compile-time capacities (`constexpr`, template params) | `config.hpp` | Index masks and bounds fold into immediates; `requires std::has_single_bit(Capacity)` enforces mask-wrap legality at compile time. |
| No RTTI, LTO, `/arch:AVX2` | `CMakeLists.txt` | Whole-program inlining across the header-only components. |
| Power-of-two + mask, Lemire bounded reduction | rings, sim | Division/modulo (20–40 cycles) never on the hot path. |

### Measurement (the part most projects get wrong)
| Technique | Where | Why |
|---|---|---|
| `rdtsc`/`rdtscp` timestamps (~6 ns) | `common/tsc_clock.hpp` | `steady_clock` costs 20–30 ns and syscall risk; rdtscp is used at measurement end-points because it won't drift ahead of the work. |
| Startup TSC calibration | `tsc_clock.hpp` | Cycles converted to ns **once at report time**; hot threads record raw cycle deltas. |
| Log-linear histograms (HdrHistogram-style) | `common/histogram.hpp` | O(1) record, fixed footprint, real percentiles. Averages lie; p99.9 is the product. |
| Per-hop timestamps riding in the messages | `wire.hpp` | Latency is *attributed*, not just totaled: t1-t0 decode, t2-t1 decision, t3-t2 risk, t4-t0 headline. |
| Warmup phase + histogram reset | `run_control.hpp` users | Cold caches, cold branch predictors and page faults belong to warmup, not to the report. |
| Deferred-formatting async logger | `common/async_logger.hpp` | Hot threads enqueue {literal-pointer, 3 ints} in ~10 ns; formatting/I-O happen on a background thread; full queue ⇒ drop + count, never block. (NanoLog's core idea.) |

### Trading-specific correctness
- **Pre-trade risk in the gateway** (`oe/order_gateway.hpp`): max-qty, price
  collar, rate throttle, session kill switch — a regulatory requirement (SEC
  15c3-5), implemented as a handful of integer compares ordered so the pass
  path is straight-line.
- **Position & cooldown limits in the strategy**: no runaway inventory, no
  self-inflicted order storms.
- **Backpressure counted, never blocking**: every ring push failure increments
  a drop counter that appears in the report. A real feed handler would conflate
  (keep latest state per symbol) — noted where it applies.

## What's simulated vs. production reality

| Here | Production |
|---|---|
| SPSC ring as "the wire" | UDP multicast feeds + kernel bypass (Solarflare ef_vi/Onload, DPDK, or FPGA NICs); TCP order entry with the same bypass. The in-proc ring stands in for the NIC-to-userspace path. |
| Single md format, little-endian | Real ITCH 5.0 / CME MDP3: per-type packed layouts, big-endian fields — decode is `bswap` + `std::bit_cast` per type; plus arbitration between A/B feed copies and sequence-gap recovery. |
| `SetThreadAffinityMask` | Linux: `isolcpus`, `nohz_full`, IRQ steering away from trading cores, NUMA-local memory, huge pages (`madvise`/explicit 2M pages) to kill TLB misses. |
| TSC timestamps in one host | PTP-synchronized NIC hardware timestamps; wire-tap capture (Corvil-style) for compliance-grade latency numbers. |
| Toy IOC fill model | A real matching engine with price-time priority queues — the natural next project on top of this one. |
| Strategy: one-signal taker | Real alpha, obviously. The *plumbing* around it — book, queues, risk, measurement — is the part that transfers. |

## Experiments worth running

1. `--no-pin` vs pinned, and pinned-on-SMT-siblings (`--cores 2,3,4,5`) vs
   distinct cores — watch the tail, not the median.
2. `--rate 0` — find the throughput cliff and watch queueing delay take over
   p99 while p50 barely moves.
3. Set the ring's cached-index optimization off (re-load the atomic every call)
   and measure the coherence-traffic cost.
4. Replace CRTP with a `virtual on_book()` and measure the per-event cost.
5. Swap the flat-ladder book for `std::map<Price, Level>` and watch decode
   latency triple. This is the single most persuasive data-structure demo.
6. Make `NewOrder` 72 bytes (break the static_assert) and look for the
   cache-line-crossing cost in the gateway hop.

## Layout

```
../common/include/hft/common/   platform, tsc_clock, spsc_ring, histogram,
                                async_logger, run_control (solution-wide toolbox)
include/t2t/
  proto/    wire.hpp        fixed-size POD messages, static_assert'd sizes
  md/       order_book.hpp  flat-ladder L3 book · feed_handler.hpp
  strategy/ imbalance_taker.hpp   CRTP base + concrete strategy
  oe/       order_gateway.hpp     pre-trade risk + t3 stamping
  sim/      exchange_sim.hpp      md generator + IOC fills + t4/tick-to-trade
src/main.cpp                wiring, pinning, phases, report
```
