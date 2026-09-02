# ticklab — tick capture, deterministic replay, latency-aware backtesting

The research-infrastructure side of an HFT firm: record market data into
compressed columnar files, replay it deterministically at any speed, and run
strategies against history with an execution model honest enough that the
results mean something. Third project in the family (`tick2trade` = the
trading hot path, `matchd` = the exchange).

```
SessionGen (synthetic market            ticklab record
 WITH real short-horizon alpha) ──────▶ Recorder ── chunked columnar file
                                                     (FOR codec, 12 B/tick)
                                                        │  mmap, chunk index
                                       ┌────────────────┴───────────────┐
                                       ▼                                ▼
                              ticklab replay                   ticklab backtest
                              • as-fast: 258M ticks/s          • BtContext: BBO state,
                              • paced 1x/Nx, jitter hist         latency-modeled orders,
                              • seek by time (binary             fills, PnL/dd/sharpe
                                search over chunk index)       • latency sweep table
                                                               • determinism digest
```

## Measured results

Windows 10, VS2026 (MSVC 14.51) Release + LTO, pinned core. Capture: 20M
ticks, 2 symbols, 240 MB.

**Storage / replay:**

```
record   : 30,967,906 ticks/s write (372 MB/s, generate+encode+write)
           12.00 bytes/tick columnar-FOR vs 36 raw row = 3.0x compression
replay   : pass 1 (cold pages) 130,982,460 ticks/s   (1.6 GB/s decoded)
           pass 2 (page cache) 257,603,820 ticks/s   (3.1 GB/s decoded)
seek     : 5,265 random time-seeks/s  (~190 us each = one chunk decode, by design)
paced 5x : delivery lateness p50 = 23 ns; p99 373 us (chunk-decode stalls, see below)
```

**The headline — alpha decay vs latency.** Same strategy, same 20M-tick
capture, only the simulated decision-to-exchange latency changes:

```
latency      sent    fills  fill%      volume        net PnL   PnL/fill    sharpe
---------------------------------------------------------------------------------
      0 us  116,570  116,570  100.0  11,657,000      9,573,400     82.13     7.17
      1 us  114,148  101,015   88.5  10,101,500      8,020,600     79.40     5.25
      5 us  113,385   89,444   78.9   8,944,400      6,933,900     77.52     4.48
     20 us  117,441   76,421   65.1   7,642,100      4,501,700     58.91     3.19
    100 us  121,139   54,182   44.7   5,418,200       -971,200    -17.92    -0.81
    500 us  120,634   49,166   40.8   4,916,600     -6,807,200   -138.45    -4.86

(PnL in tick*share units: with a $0.01 tick, 10,000 = $100)
backtest engine rate: 93,697,353 ticks/s through strategy+fills
determinism: PASS (identical fill digest and PnL on repeated runs)
```

The same signal that makes +9.6M at zero latency **loses money at 100 µs** —
fill rate decays (the price you wanted is gone) and the fills you do get are
adversely selected (you only get filled when the market is coming through
you). This monotonic decay curve is the reason HFT firms spend what they
spend on latency, reproduced end-to-end on synthetic data with a known,
embedded alpha horizon.

## Why the backtest is honest (the part most backtests get wrong)

1. **No look-ahead, mechanically enforced.** An order sent at decision time T
   takes effect at `T + latency` and fills against the book prevailing *just
   before* the first tick with `ts >= T + latency`. Pending orders are
   processed at the top of `on_tick()`, before the new tick touches the book —
   the strategy can never trade against prices it caused itself or prices
   from its own future. There is a unit test that fails if this ordering
   breaks (`test_no_lookahead_fill`).
2. **Conservative fill sizing.** IOC fills are capped at the displayed size.
   Assuming unlimited liquidity at the touch is the second-most-common way
   backtests manufacture fake PnL.
3. **Deterministic by construction.** Storage order = session order; the fill
   digest is compared across repeated runs on every `backtest` invocation.
4. **The synthetic market has *known* alpha.** `SessionGen` embeds a
   size-imbalance regime that genuinely predicts short-horizon drift (regime
   half-life ~180 µs). A generator without real alpha would show PnL ≈ 0 at
   every latency and validate nothing — you need ground truth to test the
   tester.

## Technique inventory (what's new in this project)

| Technique | Where | Why |
|---|---|---|
| **Columnar (SoA) on-disk layout** | `data/codec.hpp` | Fields stored as separate columns per 65,536-tick chunk. Columnar makes values self-similar — which is what makes the next row work. |
| **Frame-of-reference (FOR) encoding** | `data/codec.hpp` | Per column per chunk: store `min` + `(value − min)` at 0/1/2/4/8 bytes, chosen by range. Width 0 = constant column costs zero bytes. Decode is a branch-predictable base+widen loop, no entropy coding: 3.0× compression at 3.1 GB/s decode. Same family of ideas as kdb+/Parquet encodings. |
| **Memory-mapped read path** | `data/mapped_file.hpp` | The capture becomes addressable memory backed by the page cache: zero-copy decode straight from the mapping, `FILE_FLAG_SEQUENTIAL_SCAN`/`MADV_SEQUENTIAL` readahead hints, and the warm second pass runs at RAM speed (measured: 2× the cold pass). |
| **Chunk index + footer** | `data/format.hpp` | Per-chunk `{offset, ts_first, ts_last}` written at close, found via fixed-size footer. Time-seek = binary search + one chunk decode; no scan of 240 MB. |
| **Sequential-append write path** | `data/recorder.hpp` | Recording is buffered sequential writes only (what disks are best at); all random-access structure is built for the *read* side. |
| **Latency-modeled fill simulator** | `bt/context.hpp` | The no-look-ahead rule above; order effect times, displayed-size caps, fees, equity curve, drawdown, Sharpe. |
| **TSC spin-pacer for paced replay** | `src/main.cpp` | Delivery at `(ts − ts0)/speed` on the wall clock via rdtsc spin: p50 lateness 23 ns. The p99 tail is the synchronous chunk decode (~250 µs every 65k ticks) — the documented fix is double-buffered decode on a second thread. |
| **Alpha-bearing synthetic market** | `sim/session_gen.hpp` | Imbalance regimes with biased mid drift: ground truth for validating that the latency model measures something real. |
| **Live/backtest strategy parity** | `bt/strategy.hpp` | The strategy sees only a Context (bbo/position/now/send_ioc), statically dispatched — the same class can be wired to live rings (tick2trade-style) or the simulator. |

Carried over from the earlier projects: integer-tick prices, splitmix64 +
Lemire bounded reduction, log-linear histograms, zero hot-path allocation,
`/utf-8 /O2 /Ob3 /GR-` + LTO, heap-allocation of multi-MB objects.

## Build & run

```pwsh
cmake --preset vs2026            # or vs2022; or open the folder in VS directly
cmake --build --preset vs2026-release

build\vs2026\Release\ticklab_tests.exe     # 7 test groups

build\vs2026\Release\ticklab.exe record   --out session.tkl --ticks 20000000 --symbols 2
build\vs2026\Release\ticklab.exe replay   --in session.tkl              # as-fast + seek bench
build\vs2026\Release\ticklab.exe replay   --in session.tkl --speed 5    # paced, jitter histogram
build\vs2026\Release\ticklab.exe backtest --in session.tkl              # latency sweep + determinism
```

Options: `--latency-ns L` (single point instead of the sweep), `--fee F`
(ticks/share), `--seed`, `--rt`, `--no-pin`, `--core N`.

Note: paced replay must satisfy `speed × session tick rate < ~30M ticks/s`
(the delivery loop's own cost); beyond that the pacer saturates and lateness
measures backlog, not jitter — as any pacing system does past capacity.

## Tests (`src/tests.cpp`)

1. Codec round-trip at every width, including constant columns
2. Record→replay round-trip is byte-exact (200k ticks vs regenerated stream)
3. Time-seek correctness (exact hit, between-ticks, start, past-end)
4. **No-look-ahead fill rule** — the same order fills at 5 µs latency and is
   killed at 20 µs when the quote runs away in between
5. Fill size capped at displayed liquidity
6. PnL accounting identity (cash + position × mid = equity; half-spread paid)
7. Backtest determinism on a real capture

## Lab vs production

| Here | Production |
|---|---|
| Synthetic session generator | Capture from live feed handlers (tick2trade's) or exchange pcaps; L3 order-by-order storage, not just L1. |
| FOR codec only | FOR/delta + a general compressor (zstd) per chunk; dictionary encoding for symbols; often kdb+/kx or in-house columnar stores. |
| Single-file, single-day session | Date/symbol-partitioned datasets, TB/year scale, distributed backtest farms sweeping parameters in parallel. |
| L1 fill model (touch + displayed size) | Queue-position models against L3 data (your place in the FIFO decides your fill), market-impact models, venue fee schedules. |
| Sync chunk decode on the replay path | Double-buffered / prefetching decode thread; the p99 lateness above is exactly this gap, measured. |

## Layout

```
../common/include/hft/common/   platform, tsc_clock, histogram (solution-wide toolbox)
include/tl/
  data/     tick.hpp · codec.hpp (FOR) · format.hpp · mapped_file.hpp
            recorder.hpp · replayer.hpp
  sim/      session_gen.hpp   alpha-bearing synthetic market
  bt/       context.hpp (latency fill model, PnL) · strategy.hpp (+ runner)
src/main.cpp    record / replay / backtest subcommands
src/tests.cpp   7 test groups
```
