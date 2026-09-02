# sord — a smart order router that prices the whole venue, not the quote

Same displayed price, very different trade: one venue charges 30 milli-ticks
of taker fee, another rebates 15; one fills 95% of what you send, another
loses your order to faster traders half the time; one answers in 5 µs,
another in 40 — and adverse selection compounds with every microsecond in
flight. sord turns each venue into one all-in cost number (price + fees −
rebates + latency penalty + fill-risk haircut), ranks the venues through a
**compile-time Batcher sorting network** (branch-free, data-independent
time), and splits the parent across the minimal ranked prefix by the
**largest-remainder method** — integer-exact allocation: shares always sum
to exactly what was routed, no share created or lost, every venue within its
displayed size and in-flight cap. Thirteenth project in the family
(`tick2trade` = the hot path, `matchd` = the exchange, `ticklab` = research
infra, `seqbus` = transport, `riskgate` = pre-trade checks, `feedarb` = feed
arbitration, `wirecodec` = the codec, `sesslink` = the session layer,
`omsd` = order state, `symtab` = the instrument master, `sigcalc` = signals,
`pnld` = positions).

```
 parent (side, qty, limit) ──► per venue v:
    cost_v = px·1000 + fee_mt − rebate + lat_us·2 + (1000−fill_pm)·2
    key_v  = cost_v << 8 | v          (ineligible → ~0: halted, quoteless,
                                       over cap, worse than limit)
    keys ──► SortNet<16>: 63 compare-exchanges, cmp+cmov, no branches
         ──► minimal ranked prefix covering qty
         ──► largest-remainder split, weights = displayed size
         ──► children (venue, qty), in-flight accounting, digest
```

## Measured results

Windows 10 VM on AMD EPYC 7552 (20 vCPUs, no SMT), VS2026 (MSVC 14.51)
Release + LTO, pinned, time-critical priority.

**Hot path** — 2M route() calls over 16 venues, quotes moving:

```
per decision (ns, fenced rdtscp pairs): p50 220  p90 309  p99 418  p99.9 735
bulk (ns/route)   network:   N=4  59   N=8 103   N=16 176
                  std::sort: N=4  77   N=8 133   N=16 217
```

**Routing quality** — 200k parent buys against 8 venues with true
fees/latencies/fill ratios; both routers see identical quotes and identical
per-(parent,venue) luck; unfilled shares chase at mid+3:

```
naive (price only)  slippage 1202.3 mt/share | direct fills 91.3% | chased 8.7%
model (all-in cost) slippage 1142.7 mt/share | direct fills 94.6% | chased 5.5%
model saves 59.6 milli-ticks/share | conservation OK on every route
determinism replay: PASS
```

Three readings:

- **A routing decision is a fifth of a microsecond.** Rank 16 venues, pick
  the prefix, split with exact integers, book the exposure: p50 220 ns,
  p99.9 under 750. The sort is only a third of that — the eligibility/cost
  pass over 16 venues and the split bookkeeping are the rest, a reminder
  that "which sort" arguments are had inside a budget dominated by plainer
  code.
- **The network beats std::sort once reality moves.** With quotes jiggling
  every decision, the fixed 63-comparator network wins at every width (59
  vs 77 ns at N=4, 176 vs 217 at N=16). An earlier version of this bench
  left quotes frozen between calls — identical keys every time — and
  std::sort's branch predictor learned the array, flipping the result. The
  network's deeper point stands either way: its time is data-independent —
  no adversarial ordering, no tail, no timing side-channel about what you
  are routing.
- **Knowing the venue is worth ~6% of the spread.** Both routers cross the
  same spread; the model router just refuses to learn fees and fill ratios
  the hard way. 60 milli-ticks a share, every share, from public
  information about your own fills — on a penny-tick stock that's 6 cents
  per hundred shares, which is precisely the kind of number that funds an
  infrastructure team. Direct fills rise from 91.3% to 94.6% because the
  model pays up (or forgoes a rebate) to avoid venues that lose the race
  half the time.

## Why the design is what it is

- **One u64 key per venue.** Cost in the high bits, venue id in the low
  eight: ranking venues is sorting integers — no structs, no pointers, no
  comparator callbacks — and ineligibility is just `~0`, which sorts every
  dead venue past every live one with zero special cases downstream.
- **A sorting network, generated at compile time.** Batcher odd-even
  mergesort pairs are computed by a `constexpr` function into a static
  table (5/19/63 comparators for N=4/8/16) and `static_assert` proves the
  generator sorts before anything compiles. Each compare-exchange is
  min/max on u64 — cmp+cmov, never a branch — so every route() executes the
  same instruction stream regardless of market data. matchd and riskgate
  keep determinism by construction; sord extends it to *timing*.
- **Minimal prefix, then pro-rata.** The split touches the fewest venues
  that can absorb the order (a better venue is never passed over — this is
  what makes the ranking mean something), then allocates within the prefix
  proportional to displayed size. The first version sprayed all eligible
  venues pro-rata; the quality duel came out a perfect tie — the measured
  proof that allocation policy, not ranking, is where routing quality
  lives.
- **Largest remainder, not floating point.** `floor(R·w_i/W)` then
  leftovers to the largest remainders: allocations sum to exactly R, every
  time — the property tests hammer 50k random routes and demand `==`.
  Percent-based float splits need epsilon reconciliation and still strand
  shares; integer apportionment (the method that allocates congressional
  seats) simply cannot.
- **In-flight credit is part of eligibility.** Every child adds to its
  venue's outstanding; `complete()` returns it. A venue at its cap ranks as
  ineligible, so the router degrades gracefully under slow acks instead of
  piling exposure on the fastest-looking venue — the same choke-point
  discipline omsd applies to order state, applied to venue exposure.

## Build & run

```
cmake --preset vs2026            # or configure from the solution root
cmake --build --preset vs2026-release
build\vs2026\Release\sord_tests.exe
build\vs2026\Release\sord.exe --rt
```

Options: `--routes/--parents/--seed/--core/--no-pin/--rt`.

## Lab vs production

| Here | A production SOR |
|---|---|
| Top-of-book per venue | Full depth; sweep across levels and venues jointly |
| Static fee/latency/fill config | Continuously re-estimated from own executions |
| One symbol, one parent at a time | Portfolio-aware, self-cross prevention across own flow |
| Chase at mid+3 as the miss model | Real re-route logic with queue-position estimates |
| 16 venues max | Dozens, incl. dark pools with indication-of-interest logic |

## Layout

```
include/sr/network.hpp  constexpr Batcher network + compile-time self-test
include/sr/router.hpp   cost model, rank keys, prefix + largest-remainder split
include/sr/sim.hpp      venue truths, deterministic luck, outcome settlement
src/main.cpp            decision latency / sort duel / quality experiment
src/tests.cpp           network==std::sort, conservation property, rank rules
```
