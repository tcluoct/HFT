# omsd — an order state machine with drop-copy reconciliation

The firm's memory of every order it has in the market — and the machinery
that proves that memory against the venue's. An OMS's difficulty is not
throughput; it is that the strategy's requests and the venue's reports are
two async streams that *cross*: a fill races your cancel, a replace-ack
arrives after the fill that made it moot, a lost ack leaves you pending-new
on an order the venue is happily filling. omsd makes every crossing an
explicit entry in a 7-state x 8-event verdict matrix, keeps exposure
accounting leak-proof through a single choke point, and reconciles the whole
book against the venue's drop-copy feed at end of day — finding exactly the
damage, and nothing else. Ninth project in the family (`tick2trade` = the hot
path, `matchd` = the exchange, `ticklab` = research infra, `seqbus` =
transport, `riskgate` = pre-trade checks, `feedarb` = feed arbitration,
`wirecodec` = the codec, `sesslink` = the session layer).

```
 strategy ──► on_request(new/cancel/replace) ─┐        ┌── venue (primary path,
                                              ▼        ▼    can drop/duplicate)
                                   ┌────────────────────────┐
                                   │ Oms  (ONE thread)      │   on_report:
                                   │  slotmap arena +       │    1 id-map probe
                                   │  id map (backshift) +  │    1 dedup-ring probe
                                   │  exec-id dedup ring    │    1 verdict lookup
                                   │  verdict matrix 7x8:   │    qty arithmetic
                                   │  apply/race/heal/      │    1 reserve retarget
                                   │  stale/venue-err       │    1 digest mix
                                   └───────────┬────────────┘
   venue drop-copy (complete, out-of-band)     │ per-symbol books:
        │                                      ▼ position, open buy/sell
        └──► Recon: fold truth ──► compare ──► phantom / state / cum / qty
                                               findings — exactly the injected
                                               damage, zero false accusations
```

## Measured results

Windows 10 VM on AMD EPYC 7552 (20 vCPUs, no SMT), VS2026 (MSVC 14.51)
Release + LTO, engine pinned at time-critical priority. Order flow from the
venue simulator: 35% canceled, 15% replaced, partial fills, 2% unsolicited
cancels, 0.5% rejects — every crossing arising from timestamps, not scripts.

**Hot path** — 3M orders → 11,962,040 events, grace-period pruning:

```
per event (ns, rdtscp pairs, ~20 ns floor)   count      p50    p90    p99   p99.9
  request (new/cancel/replace)           4,499,690      162    202    448     619
  report  (ack/fill/cxl/rpl/...)         7,462,350       61    110    202     488
engine-busy throughput 9.5M events/s | peak open orders 1,065,904
verdicts: apply 6,725,286 | race 156,751 | heal 0 | stale 580,313 | venue-err 0
invariants: unknown reports 0 | open-interest audit (brute-force recompute) OK
```

**Reconciliation day** — 1M orders, 2,489,075 drop-copy reports, with the
primary path deliberately damaged (0.5% reports dropped, 0.3% duplicated,
100 alien orders in the drop-copy):

```
injected: 12,619 dropped reports across 12,559 orders (4,911 losing fills),
          7,552 duplicates, 100 aliens
recon   : fold 87 ms (28.8M reports/s) + compare 109 ms = 195 ms end-to-end
findings: 6,632 orders flagged (4,627 state, 4,911 cum, 347 qty) + 100 phantom
exactness: false accusations 0 | lost fills missed 0 | phantoms 100/100
           dups absorbed 7,552/7,552 | stuck 0            -> OK
determinism replay: PASS (digest e6254900a05e53d6)
```

Three readings:

- **State keeping costs double-digit nanoseconds.** A venue report is
  ~60 ns at the median (hash probe, dedup probe, matrix lookup, a few adds,
  reserve retarget, digest mix); a request is ~160 ns (it also allocates the
  slot). 156,751 genuine request/report crossings were classified and applied
  without a single misfire — race handling is table lookup, not special-case
  code. (The once-per-run ms-scale max is a single hypervisor descheduling
  event in 12M; p99.9 stays under 650 ns. See the family note on this box.)
- **Recon is exact, both ways.** Of 12,619 dropped reports, about half turn
  out to be immaterial (a lost "too late" rejection changes nothing). The
  reconciler flags 6,632 orders — every one truly damaged (zero false
  accusations), every order that lost a *fill* among them (4,911 cum
  mismatches — position errors cannot hide), every alien found. A 1M-order
  day reconciles in ~0.2 s.
- **Half a million stragglers, zero alarms.** "Terminal" does not mean
  "silent": 546,515 late intents and 580,313 stale answers (mostly
  rejections of cancels that lost their race) landed on finished orders and
  were correctly shrugged off — because terminal orders are pruned on a
  grace period, not eagerly. Prune too early and every straggler becomes a
  spurious unknown-order alarm; the first version of this benchmark proved
  that the hard way with 580k of them.

## Why the design is what it is

- **An exhaustive verdict matrix, not defensive ifs.** All 56 (state, event)
  pairs are classified in one constexpr table — apply / race / heal / stale /
  venue-error — with `static_assert`s on the pairs people argue about in
  design reviews (fill-after-cancel-ack is a venue error; fill-before-ack is
  a heal). A pair the designer never thought about *cannot* slip through,
  because every pair had to be written down. The matrix classifies; small
  per-event arithmetic applies. Both are exercised by construction in tests.
- **Exposure accounting has one choke point.** Every order carries
  `reserved` — the leaves it currently contributes to per-symbol open
  interest. After *every* transition one function recomputes the target
  reserve from (state, qty, pending_qty, cum) and applies the delta. No
  transition-specific exposure code exists, so no transition can leak
  exposure; a replace holds max(old, new) leaves until the venue rules
  (riskgate would size against the worst case). The tests brute-force
  recompute the books from the order store every 100k events and every run
  ends with the audit green.
- **The OMS is a bookkeeper, not a gate.** Late cancel intents are recorded
  and forwarded, not suppressed — the venue is the arbiter and its answer
  lands in the matrix (as a stale). This keeps the OMS's view a pure fold
  over its two input streams, which is what makes determinism (same tape ->
  same digest, proven by replay) and honest reconciliation possible.
- **Drop-copy recon is a simpler machine on purpose.** The drop-copy feed is
  complete and per-order ordered, so none of the crossing cases exist there:
  the fold is a mini state machine over reports alone. Divergence then has
  exactly four shapes — phantom order, state mismatch, cum mismatch, qty
  mismatch — each mapping to a distinct lost-message class (and phantom to
  the scary one: the venue executing orders you never sent).
- **matchd's tricks, reused where they fit.** Slotmap arena + open-addressing
  id map (backshift deletion, no tombstone rot), feedarb's full-value dedup
  ring for exec-ids, FNV digest determinism proof — the family toolbox; the
  new technique is the matrix + choke-point accounting + grace-period prune
  queue (a FIFO of terminal orders recycled only after stragglers can no
  longer arrive, amortized O(1)).

## Build & run

```
cmake --preset vs2026            # or configure from the solution root
cmake --build --preset vs2026-release
build\vs2026\Release\omsd_tests.exe
build\vs2026\Release\omsd.exe --rt
```

Options: `--hot/--recon/--drop-pm/--dup-pm/--aliens/--seed/--core/--no-pin/--rt`.

## Layout

```
include/om/core/types.hpp     Request/ExecReport, states, the verdict matrix
include/om/core/oms.hpp       the engine: arena, id map, dedup, accounting
include/om/core/recon.hpp     drop-copy fold + compare (4 finding classes)
include/om/sim/venue_sim.hpp  lifecycle generator, impairments, ground truth
src/main.cpp                  hot-path + recon-day benchmark, digest replay
src/tests.cpp                 every crossing constructed + sim sweeps
```
