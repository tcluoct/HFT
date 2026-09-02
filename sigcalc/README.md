# sigcalc — a microstructure signal engine in exact integer arithmetic

The layer between the book and the strategy: every market-data event becomes
a feature vector — microprice, top imbalance, order-flow imbalance, trade
flow, drift, realized variance, range position, EWMA momentum — and one
combined score, in O(1) per event with no allocation and *no floating
point*. Integers make the windows exact: a rolling sum after ten million
pushes is bit-identical to a from-scratch recompute, so every feature is
provable against a dumb reference loop, and the whole run folds into one
deterministic digest. sigcalc is also the family's first explicit-SIMD
project: the 8-horizon EWMA lattice runs scalar or AVX2, proven equal bit
for bit. Eleventh project in the family (`tick2trade` = the hot path,
`matchd` = the exchange, `ticklab` = research infra, `seqbus` = transport,
`riskgate` = pre-trade checks, `feedarb` = feed arbitration, `wirecodec` =
the codec, `sesslink` = the session layer, `omsd` = order state, `symtab` =
the instrument master).

```
                 ┌──────────────────────────────────────────────┐
 Tick            │ SignalEngine (per symbol, O(1) per event)    │
  bid/ask px,qty─┤  microprice, imbalance   2 int divides       │
  trade qty,side │  OFI / tflow / drift / rvar                  │
                 │      4 x RollingSum: ring + running sum      │──► Features
                 │  range position                              │    + score
                 │      RollingExtrema: 2 monotonic wedges      │    + digest
                 │  momentum                                    │
                 │      EwmaLattice: 8 horizons, m+=(x-m)>>k    │
                 │      scalar or AVX2 (srlv + bias trick)      │
                 └──────────────────────────────────────────────┘
```

## Measured results

Windows 10 VM on AMD EPYC 7552 (20 vCPUs, no SMT), VS2026 (MSVC 14.51)
Release + LTO, pinned, time-critical priority. Simulator: random-walk mid
with calm/hot volatility regimes, stochastic touch sizes, 25% of events
carry trades. Window W = 256 events.

**Hot path** — 10M streamed events, full feature vector per event:

```
per event (ns, rdtscp pairs, ~20 ns floor): p50 40  p90 50  p99 61  p99.9 81
engine-busy throughput 24.1M events/s | digest 8702471da13b829c (reproducible)
```

**Backend duel** — same 1M-tick tape through both backends (bulk-timed,
fenced, best of 5):

```
full engine   scalar 27.4 ns | avx2 25.1 ns   (1.09x)
lattice only  scalar 3.56 ns | avx2 2.60 ns   (1.37x)  [32M updates]
backend digests EQUAL
```

**Where the nanoseconds go** (each part in isolation, 8M ops):

```
rolling sum push        2.7 ns
wedge push + min + max 17.1 ns
microprice + imbalance 15.9 ns   (the two 64-bit divides)
```

Three readings:

- **A full signal update is ~25 ns.** The p50 of 40 ns per event carries the
  ~20 ns rdtscp floor; the bulk-timed number is 25–27 ns for all eight
  features plus the score — 24M events/s on one core, faster than any real
  feed bursts. The two integer divides (microprice and imbalance
  normalization) are the single most expensive thing in the engine; the
  windows the features actually ride on cost ~3 ns each.
- **Vector code loses until you feed it properly.** The first AVX2 lattice
  *lost* to scalar (0.72x): its shift/bias constants were built with
  `_mm256_setr_epi64x` inside the update, and MSVC re-materialized them per
  call. Moving them to `static constexpr` aligned tables (one .rdata load,
  hoisted out of loops) made the same arithmetic 1.37x faster than scalar in
  isolation — and only 1.09x inside the full engine, where the scalar ports
  the lattice frees up were partly idle anyway. Op counts don't decide;
  port pressure does.
- **Exactness is the feature.** Every windowed value equals a brute-force
  recompute over raw history — on every early event and every 97th after,
  by `==`, not epsilon — and the AVX2 and scalar runs produce the same
  digest over 200k events. The bias trick
  `(x-m)>>k == ((x-m+2^45)>>k) - (2^45>>k)` is what makes a logical-shift
  ISA (AVX2 has no per-lane 64-bit arithmetic shift) implement arithmetic
  semantics *exactly*, not approximately.

## Why the design is what it is

- **Event windows, running sums.** A window of the last W events is a ring
  plus a running sum: push = one subtract, one add. Integers mean no drift,
  ever — the periodic-recompute resync that float rolling stats need simply
  isn't a thing. Normalization (dividing by W) is deferred to whoever
  consumes the feature; W is a compile-time constant, so the raw sum *is*
  the feature up to scale.
- **Monotonic wedges for extrema.** Rolling min/max can't be a running sum,
  but a wedge (deque of candidate indices whose values are monotone) gives
  amortized O(1): a new sample evicts everything older *and* worse, the
  front is the answer. The front must be retired before the insert — a
  strictly monotone run otherwise overflows the W-slot ring by one (found by
  the 3W-long monotone-run test, which exists for exactly that reason).
- **Shift-only EWMAs.** With alpha = 2^-k, an EWMA is `m += (x - m) >> k` —
  no multiply, no float, deterministic. Eight horizons (k = 2..12) give the
  strategy fast/slow structure; the momentum feature is lattice[fast] -
  lattice[slow]. Eight independent one-shift updates are also the perfect
  SIMD shape, which is why this is where the family's first explicit AVX2
  lives.
- **OFI per Cont–Kukanov–Stoikov.** Quote transitions translated into
  signed depth (bid up/level add = +, bid down/pull = -), summed over the
  window. Each event term needs only the previous top — kept inline, no
  book reconstruction — and the window sum rides the same RollingSum as
  everything else.
- **The score is part of the engine.** Fixed small-integer weights,
  shift-scaled — not because the weights are smart (they aren't; ticklab is
  where weights would be fit) but because folding the score into the digest
  every event is what turns "the backends look similar" into "the backends
  are identical, 200k events deep".

## Build & run

```
cmake --preset vs2026            # or configure from the solution root
cmake --build --preset vs2026-release
build\vs2026\Release\sigcalc_tests.exe
build\vs2026\Release\sigcalc.exe --rt
```

Options: `--events/--tape/--seed/--core/--no-pin/--rt`.

## Lab vs production

| Here | A production signal engine |
|---|---|
| One symbol, one thread | Thousands of symbols sharded across cores; per-symbol state arrays |
| Event-count windows | Time-based windows (dual rings) and session-aware resets |
| Fixed integer weights | Fitted weights, refreshed from research (ticklab's job) |
| Top-of-book OFI | Multi-level OFI, queue-position estimates, cross-venue features |
| Two divides tolerated | Reciprocal tables or rational features where the consumer allows |

## Layout

```
include/sg/types.hpp    Tick, Features, integer conventions
include/sg/windows.hpp  RollingSum, RollingExtrema (monotonic wedges)
include/sg/ewma.hpp     8-horizon shift EWMA lattice, scalar + AVX2
include/sg/engine.hpp   the per-event feature computation + digest
include/sg/sim.hpp      regime-switching market simulator
src/main.cpp            hot path / backend duel / component costs
src/tests.cpp           brute-force references, bit-equality, digests
```
