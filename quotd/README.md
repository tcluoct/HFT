# quotd — a market-making quote engine that spends operations like money

The other half of HFT: not taking liquidity, *standing* in it. A quoting
engine's real outputs are venue operations — new, cancel — and its real
assets are queue positions; every unnecessary requote burns both. quotd
never "sends its quotes": each tick it computes the DESIRED ladder (fair
value, half-spread, inventory skew from a clamped LUT) and diffs it against
the WORKING ladder, emitting the minimal ops that venue priority rules
allow. A hysteresis band trades price staleness for queue position; an
inventory skew shifts the ladder away from the loaded side until aggressors
stop reaching it; one epoch bump mass-pulls the whole book O(1) before a
single cancel reaches the wire. Fourteenth project in the family
(`tick2trade` = the taking hot path, `matchd` = the exchange, `ticklab` =
research infra, `seqbus` = transport, `riskgate` = pre-trade checks,
`feedarb` = feed arbitration, `wirecodec` = the codec, `sesslink` = the
session layer, `omsd` = order state, `symtab` = the instrument master,
`sigcalc` = signals, `pnld` = positions, `sord` = routing).

```
 fair value ──► desired ladder                 working ladder (at venue)
 inventory ──►  center = fv − skewLUT[inv]      {px, qty, epoch, live} x 2x4
                bid_i = center − hs − i          │
                ask_i = center + hs + i          ▼
                        └────────► DIFF ─────────┘
   same within band → NO OP (queue spot kept)     eaten < half → cancel+new
   fresh/vacated → new           drifted > band → cancel+new
   mass pull: ++epoch = whole book logically dead, O(1); cancels drain after;
              racing fills recognized as stale on arrival (inventory still real)
```

## Measured results

Windows 10 VM on AMD EPYC 7552 (20 vCPUs, no SMT), VS2026 (MSVC 14.51)
Release + LTO, pinned, time-critical priority. Sim: momentum fair value, 30%
of steps move; aggressors on 30% of steps, 60% informed (they trade the
direction of the imminent move); fills only when a quote is within the
aggressor's reach (2–4 ticks of fv). 4 levels/side, size 100, half-spread 2.

**Hot path** — 5M ticks, fills landing between them (band 1):

```
on_tick (desired + diff, ns): p50 40  p90 81  p99 101  p99.9 110
venue ops 2075/1k ticks | zero-op ticks 65.8% | 1.29M fills
epoch pull: pull + resume + full requote cycle 67 ns (8 cancels + 8 news)
```

**The skew experiment** — 2M steps, identical informed flow, same seed:

```
skew off : inventory std 1191, worst 2099 | equity 69,177,878 | fills 547k
skew on  : inventory std  260, worst  859 | equity 59,800,875 | fills 515k
```

**The band experiment** — hysteresis 0–4 ticks, same seed:

```
band 0 : 5733 ops/1k ticks | equity 66,040,975
band 1 : 2071 ops/1k ticks | equity 59,799,443
band 2 : 1186 ops/1k ticks | equity 53,183,267
band 4 :  620 ops/1k ticks | equity 40,727,259
```

Three readings:

- **Skew buys risk down 4.6x for a 13% revenue toll.** With the ladder
  pinned to fair value, inventory random-walks to ±2099 and its stddev is
  1191 shares; with a 6-tick clamped skew, the loaded side drifts out of
  most aggressors' reach and stddev collapses to 260. Equity drops 13.5% —
  skewed quotes buy lower and sell lower when long. Per unit of inventory
  risk, the skewed book earns ~5x more: this sim charges nothing for
  carrying inventory overnight, real desks charge a lot, and that's the
  whole case for skew.
- **The band is an op budget, priced in equity.** Dropping hysteresis from
  1 tick to 0 earns 10% more equity — and nearly triples the message rate
  (5733 vs 2071 ops per 1000 ticks). The sim prices operations at zero;
  venues do not (order-to-trade ratios, messaging fees, gateway capacity),
  which is why every real quoting engine runs a band and why it is a
  config knob here, not a constant. Two-thirds of ticks produce *zero*
  ops at band 1 — the diff engine's whole point.
- **The pull is free; the cancels are not, and don't need to be.** A full
  pull + resume + requote cycle is 67 ns, but the load-bearing part is the
  epoch bump: from that instant every resting quote is logically dead, and
  a fill that races a cancel to the venue arrives already classified as
  stale (inventory moves — exposure is real — but the strategy knows it
  came from a dead regime). Safety is a state change, not a round trip:
  the same lesson sesslink's journal-cursor replay teaches for sessions,
  applied to quotes.

## Why the design is what it is

- **Desired-vs-working, not imperative requoting.** The strategy layer only
  ever states what the ladder *should* be; the diff engine owns what to do
  about it. That split makes minimality checkable (same input twice → zero
  ops, asserted in tests), makes op accounting exact, and means new quote
  logic (a different skew curve, a different spread model) cannot introduce
  op-storm bugs — the diff is the only thing that talks to the venue.
- **Queue priority drives the rules.** Untouched quotes keep their place in
  line, so "keep" is the default and every other verdict must earn its
  cost: price drifted beyond the band, quote eaten below half size
  (replenish), side suppressed by the inventory hard stop. A quote engine
  that requotes eagerly is donating queue position to everyone behind it.
- **Skew as a clamped LUT.** Inventory divides into buckets, buckets index
  a precomputed table (linear, clamped at ±max_skew) — no float curve
  evaluation on the tick path, one clamp and one load. The *mechanism* by
  which skew works is in the sim on purpose: it does not reprice fills,
  it changes which fills happen, and only a fill model with finite
  aggressor reach can show that.
- **Epochs, not flags-per-order, for the pull.** riskgate hot-reloads
  limits under a seqlock, symtab swaps snapshots under reader epochs; quotd
  applies the epoch idea to its own orders. One counter classifies every
  in-flight event as live or stale with no per-order bookkeeping — and the
  scripted race in the tests (fill lands after the pull) proves the stale
  path counts exposure without reviving the regime.
- **Self-match prevention by construction.** Bid and ask ladders derive
  from one center with ±half-spread: they cannot cross for any inventory or
  fair value, which the 10k-step sweep asserts. Cheaper than checking at
  the boundary, impossible to forget in a refactor.

## Build & run

```
cmake --preset vs2026            # or configure from the solution root
cmake --build --preset vs2026-release
build\vs2026\Release\quotd_tests.exe
build\vs2026\Release\quotd.exe --rt
```

Options: `--steps/--seed/--core/--no-pin/--rt`.

## Lab vs production

| Here | A production quote engine |
|---|---|
| One symbol, one venue | Thousands of symbols, quotes coordinated across venues |
| Linear clamped skew | Fitted curves (Avellaneda–Stoikov and successors), vol-scaled |
| Fair value handed in | sigcalc-style microprice/signal stack feeding it |
| Instant venue (ops apply now) | Real acks/rejects via sesslink, in-flight state via omsd |
| Pull on demand | Auto-pull triggers: vol spikes, feed staleness, position/loss limits |
| Amend modeled as keep/replace | Venue-specific amend semantics exploited per destination |

## Layout

```
include/qt/engine.hpp  desired ladder, diff engine, skew LUT, epoch pull
include/qt/sim.hpp     momentum fv, informed aggressors with finite reach
src/main.cpp           hot path / skew & band experiments / pull cycle
src/tests.cpp          ladder, band, replenish, hard stop, pull race, capture
```
