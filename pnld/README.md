# pnld — a position & P&L keeper in exact integer money

The firm's answer to "what do we own and what have we made" — continuously,
per fill, not at end of day. omsd tracks *order* state; pnld tracks what
executed orders did to the *book*: per-symbol positions carried as FIFO tax
lots in a ring, realized/unrealized P&L split under two attribution rules at
once (FIFO and weighted-average-cost), fees, marks, and a firm-level rollup
maintained by O(1) incremental deltas. Money is integers end to end, which
turns accounting folklore into provable theorems: the two attributions
disagree on the realized/unrealized *split* — and agree on equity, exactly,
after every one of ten million fills, because

```
equity := realized - fees + (pos * mark - open_cost)
       == signed trade cash - fees + pos * mark          (any cost rule)
```

Rounding in a cost-removal rule (WAC's truncated division) moves P&L between
realized and unrealized, never in or out of equity. Twelfth project in the
family (`tick2trade` = the hot path, `matchd` = the exchange, `ticklab` =
research infra, `seqbus` = transport, `riskgate` = pre-trade checks,
`feedarb` = feed arbitration, `wirecodec` = the codec, `sesslink` = the
session layer, `omsd` = order state, `symtab` = the instrument master,
`sigcalc` = signals).

```
 fills ──► PositionBook[sym]                       Keeper (firm)
            FIFO lot ring: consume oldest first     Δpos*mark, Δcost,
            + WAC single-cost, pro-rata slice   ──► Δrealized, Δfees
            realized/unrealized under BOTH          O(1) firm equity,
 marks ──►  pos * mark - open_cost                  audited vs recompute
```

## Measured results

Windows 10 VM on AMD EPYC 7552 (20 vCPUs, no SMT), VS2026 (MSVC 14.51)
Release + LTO, pinned, time-critical priority. Flow: 10M fills across 1,024
symbols, inventory-mean-reverting sides (a market maker's flow), qty 1–1000,
a mark every 8 fills.

```
hot path (ns, rdtscp pairs, ~20 ns floor)    count      p50   p90   p99  p99.9
  fill (both attributions + rollup)     10,000,000      61   101   140    244
  mark (O(1) firm delta)                 1,250,000      20    20    30     30
engine-busy throughput 16.8M events/s | peak lots on one book 42

firm equity: fifo -541,920,153 == wac -541,920,153 == cash+posval -541,920,153
realized split: fifo -35,526,285 / wac -36,240,653   (split differs, equity identical)
invariants: rollup+lots audit OK | equity identity OK

worst cases:
  flip churn (close + flip + open per fill)   8.8 ns/fill
  100k-lot drain in one fill                  0.36 ms total, 3.6 ns/lot
determinism replay: PASS (digest 0db725b1e220c8c0)
```

Three readings:

- **Keeping the books costs less than reading a clock.** A fill — two
  attribution updates, lot-ring work, six firm-level deltas, digest fold —
  is ~60 ns at the median including the ~20 ns measurement floor; a mark is
  indistinguishable *from* the floor. Position keeping is never the
  bottleneck; the reason to make it this cheap is that risk (riskgate) and
  quoting (quotd) want to read positions synchronously.
- **The identity is the regression test that can't lie.** After 10M fills
  the FIFO equity, the WAC equity, and a cold `cash + pos*mark` ground truth
  agree to the last tick-share — while the realized numbers differ by
  ~714k. Any leak (a lot mis-consumed, a flip mishandled, rounding dust
  escaping) breaks a `==` somewhere. Compare omsd's approach: same
  philosophy — one brute-force-checkable invariant beats a hundred
  spot checks.
- **Worst cases are boring, by construction.** Every-fill zero-crossing
  churn runs at 8.8 ns/fill (close + flip + open is still just arithmetic);
  a pathological 100k-lot position drains in one fill at 3.6 ns/lot,
  linear and cache-friendly, because lots live in a contiguous ring, not a
  linked structure. Each lot is created once and consumed once — amortized
  O(1) however the flow slices it. (The once-per-run ms-scale fill max is
  the usual hypervisor descheduling on this box.)

## Why the design is what it is

- **Lots in a ring, direction in the sign.** A position's lots always share
  its direction, so lots store only (qty, px); the sign lives once, in
  `pos`. FIFO consumption is a head cursor walking forward — no allocation,
  no tombstones. If flow ever out-runs the ring (a configuration error, not
  a runtime condition), an `overflow_` counter flags it and every audit
  fails loudly.
- **Two attributions, one book, priced identically.** FIFO and WAC share
  the same fills, fees, and marks; only the cost-removal rule differs. Both
  define realized as `cash-in - cost-removed`, which is what makes the
  equity identity rule-independent — and the pair is cheap enough (~one
  extra multiply and divide per reducing fill) that carrying both beats
  arguing about which one is "right". Traders read FIFO; the identity says
  it doesn't matter for the bottom line.
- **Integer money with a magnitude contract, not 128-bit.** riskgate takes
  the `_umul128` route for notional; pnld instead states its bounds
  (|px| < 2^21, |pos| < 2^22, qty < 2^12 → worst intermediate ~2^55) and
  lets i64 be exact inside them. The sim enforces the contract; a
  production keeper would assert it at ingest. Floats appear nowhere — a
  P&L that drifts is a P&L that gets argued with.
- **The firm rollup is deltas, not walks.** Each fill/mark applies its
  exact change to six firm aggregates, so firm equity is always current at
  O(1). The `audit()` recomputes everything from the books and demands
  equality — run every 5k events in tests, and at end of every bench run.
  Incremental-vs-recompute is the same discipline omsd applies to open
  interest; here it covers money.
- **Marks are events, not queries.** `pos * mark` is maintained as marks
  arrive, so unrealized P&L never needs a mid-day loop over instruments.
  The identity is checked against whatever marks are current — it holds at
  mark 0 too; marks change the *number*, never the *identity*.

## Build & run

```
cmake --preset vs2026            # or configure from the solution root
cmake --build --preset vs2026-release
build\vs2026\Release\pnld_tests.exe
build\vs2026\Release\pnld.exe --rt
```

Options: `--fills/--seed/--core/--no-pin/--rt`.

## Lab vs production

| Here | A production P&L keeper |
|---|---|
| One account | Account/strategy/trader hierarchy with rollups at each node |
| Ticks and shares | Per-instrument multipliers, currencies, FX conversion at report |
| FIFO + WAC | Plus tax-lot elections (LIFO, spec-ID), wash-sale handling |
| Marks from the fill stream | Independent mark source with staleness policy |
| Fees as one integer | Full fee schedules: maker/taker, tiers, regulatory, clearing |
| In-memory, rebuilt from tape | Journaled and recoverable (sesslink/omsd patterns) |

## Layout

```
include/pl/types.hpp   Fill/Mark, integer-money conventions, magnitude contract
include/pl/book.hpp    PositionBook: FIFO lot ring + WAC, the equity identity
include/pl/keeper.hpp  firm rollup by O(1) deltas, audits, digest
include/pl/sim.hpp     inventory-mean-reverting flow generator
src/main.cpp           hot path / worst cases / identity proof / replay
src/tests.cpp          hand-built accounting cases + 200k-fill audited tape
```
