# platform — the China multi-venue HFT platform (production, Phase 1)

The implementation of [docs/platform/ARCHITECTURE.md](../docs/platform/ARCHITECTURE.md):
the fourteen labs graduated into one composed system, now with **real venue
connectivity** — Phase 1 trades commodity futures through the SFIT CTP
counter. All simulation code has been removed; the venue boundary is the
production CTP SDK (`third_party/ctp`, v6.7.11).

Deployment (hardware, SDK downloads, broker provisioning, runbook):
**[DEPLOYMENT.md](DEPLOYMENT.md)**. Config template: `platd.example.cfg`.

## What runs

| Binary | What it does |
|---|---|
| `platd` | The production site daemon. `platd <cfg>` runs a live session: CTP login → instrument/margin/limit queries → position carry-in → md subscription → **a config-chosen lineup of concurrent multi-instrument strategy instances** (`strategies = ...`: any subset of momentum/mean-reversion/session-breakout, several instances of one type with their own params/universes, each with its own durable StrategyId for risk scopes/PnL/kill targeting; the classic trio when the key is absent) through the 12-check riskgate → asynchronously journaled canonical stream → EOD reconciliation against the counter. Stop one strategy for a while with `<name>.enabled = 0` (its id stays reserved and its carried positions keep flowing through the `.positions` chain) or intraday via the console's `halt <id>` / `disarm <id>`. `platd --check <cfg>` validates config + strategy lineup + CTP linkage offline; `platd --replay <cfg> <jnl>` is the recovery drill (see below). |
| `plat_tests` | Component tests (books, OMS, riskgate battery, PnL 平今/平昨, journal, ring, config) plus the CTP mapping layer's pure conversions and a full engine-loop test driven through a fake gateway at the production `OrderGateway` boundary. No counter connection needed. |
| `plat_bench` | §12.1 budget lines through the *production* dispatch chain (no simulation — synthetic snapshots, capture gateway): tick floor, full order path, adapter-ring one-way. |
| `platrec` | Standalone market-data recorder: enumerates the whole market, records L1/L2 depth to a compact delta-coded `.mdr` file (~24 B/tick L1, ~56 B/tick L2 vs 216 raw); `--compare` cross-datacenter verify + golden merge (repairs UDP reordering on the venue's cumulative-volume sequence key); `--dump`. Stores instrument specs for self-sufficient backtest. See DEPLOYMENT.md §9; format spec in [MDR_FORMAT.md](MDR_FORMAT.md). |
| `platbt` | Strategy backtester: replays recorded `.mdr` days through the **same** `Site` + strategies + riskgate + PnL platd runs live (only the venue is a fill simulator), with the identical config keys — so backtest results can't diverge from live. Multi-day: `platbt cfg d1.mdr d2.mdr …`. |
| `platops` | Web ops console: `platops <cfg>` (same file the fleet runs from) tails every shard's live journal into a Replay-mode **mirror Site** — per-strategy PnL, positions, open orders, fills, risk-limit utilization and alerts reconstructed deterministically a few ms behind live, with zero contact with the trading processes — and drives operator actions (halt/flatten/arm, per-strategy stop, live `param` re-tune, risk-checked manual orders, order cancel, position `transfer` between books) through the per-shard cmd files. Every action enters the engine as a journaled Source envelope: risk-checked, replayable, audited. Loopback-only by default (`ops.port`/`ops.bind`); SSH-tunnel for remote. |

Adding a strategy type = one new header (the strategy + its `Def`
registration traits, see `plat/engine/pack.hpp`) plus one line appending the
Def to the `Pack` list in `src/strat_params.hpp`, then rebuilding — parsing,
bank building and the platd/platbt/--replay compositions are all derived
from that list. The rebuild is deliberate: every strategy inlines into the
engine and the tick path carries zero indirection for extensibility's sake —
a runtime plugin mechanism was evaluated and rejected because its per-event
virtual hop is a latency tax on exactly the path that matters.

## Measured results (dev VM, Release; see lab notes for the ~200 µs hypervisor outliers)

```
md snapshot -> book -> strategy (no order):     62.3 ns/tick
same tick + async journal attached:            264.4 ns/tick   (see note)
md snapshot -> ... -> riskgate -> gateway:     389.3 ns/event (incl. fill dispatch)
adapter ring publish+poll (same core):          24.9 ns/msg
```

Optimization history, each measured before/after: the tick floor was 234 ns
until the per-envelope stream hash (byte-wise FNV-1a: 208 serial multiplies
per snapshot payload) went word-at-a-time (→ ~70 ns); the FutBook
double-copy per snapshot became a buffer flip and the histogram record's
shift loop became one `bit_width` (→ ~62 ns). Journal note: the async
journal's engine-side cost is one wait-free ring publish, but on this VM the
continuously-draining journal thread turns each publish into a cross-CCX
cache-line transfer (~200 ns — the seqbus lab measured the same shape). At
real snapshot cadence the consumer is asleep between events and the
contention vanishes; what the async design removes is the CRT flush syscall
(multi-µs, unpredictable) that the old synchronous `fwrite` put directly on
the tick path. Pin `journal_core` to a core sharing L3 with `engine_core`.
Measured with `hft::bench_ns_per_op` (signal-fence discipline; MSVC's
`__rdtscp` is not a compiler barrier).

## Architecture ↔ code map (§20)

| ARCHITECTURE.md | Code | Heritage lab |
|---|---|---|
| §7.1 sequenced bus | `plat/bus/ring.hpp` (adapter→engine transport), `plat/bus/stream.hpp` (canonical stream) | seqbus |
| §7.2 journal | `plat/bus/journal.hpp` | — |
| §7.4 codec/IDL | `plat/codec/msgs.hpp` | wirecodec |
| §8.1 feed handlers | `plat/ctp/md_client.hpp` (CTP depth → snapshots), `plat/md/feed.hpp` (A/B arbiter, for Phase-2 stock feeds) | feedarb |
| §8.2 book engines | `plat/md/fut_book.hpp` (snapshot-delta), `plat/md/stock_book.hpp` (order-by-order, Phase 2) | ticklab |
| §9.1 strategy container | `plat/strat/framework.hpp` | tick2trade |
| §9.2 signals | `plat/strat/signals.hpp` (tick-path primitives), `plat/strat/kbar.hpp` (k-bar aggregation from snapshot/trade feeds), `plat/strat/indicators.hpp` (bar-cadence ATR/RSI/MACD/rolling stats/VWAP) | sigcalc |
| §9.3 multi-leg arb | `plat/strat/basisarb.hpp` (Phase-2 federation legs) | sord |
| §9.4 gateway/session | `plat/gw/gateway.hpp` (boundary), `plat/ctp/trader_client.hpp` (CTP implementation: auth → login → settlement confirm, order entry, reports, queries) | sesslink |
| §9.5 OMS / PnL | `plat/oms/oms.hpp`, `plat/pnl/pnl.hpp` (T+1 sellable, 平今/平昨 buckets) | omsd, pnld |
| §9.6 instrument master | `plat/ref/instrument.hpp` — built at startup from counter queries (tick/multiplier/margin/limit band), persisted as the journal's `.ref` sidecar | symtab |
| §4.2 fees, one library | `plat/ref/fees.hpp` | — |
| §10.1 riskgate battery | `plat/risk/riskgate.hpp` (all 12 checks) | riskgate |
| §10.2 compliance | `plat/comp/compliance.hpp` (300/s & 20k/day thresholds, alerts) | — |
| §10.3 kill switch | `KillSwitch` in riskgate + operator console (`halt`/`flatten`/`disarm`) | — |
| §11 recovery | component `save/load/state_hash`, `plat/replay/replayer.hpp`, `platd --replay` | — |
| §13.2 production driver | `plat/engine/live.hpp` (wall clock, rings, timers, phases, ops console) | matchd (retired venue sim) |
| §14 telemetry | `plat/tel/telemetry.hpp`, `plat/engine/report.hpp` | common bench |
| §5.1 site engine | `plat/engine/site.hpp` | — |

## The determinism contract (why replay = recovery)

Every envelope on the site's canonical stream is either **Source** (crossed
the deterministic boundary: CTP depth snapshots, exec reports, session state,
phase/timer ticks, operator commands) or **Derived** (regenerated by pure
components: book tops, order requests, denials). The live driver
(`plat/engine/live.hpp`) is the only place a wall clock exists; it stamps
Source envelopes as they cross. The journal stores both kinds; `platd
--replay` injects only Source and requires the Derived stream to regenerate
**byte-identically** — verified by hashing the whole stream. Warm-standby
recovery is the same mechanism starting from a snapshot.

Threading is confined by construction — the process runs five threads, each
with one job:

| Thread | Work | Why it exists |
|---|---|---|
| engine (pin: `engine_core`, `rt=1`) | drain rings → seal → dispatch → strategies → riskgate → gateway send | the single sequencing point: total order, race-free risk, replayable journal |
| journal writer (pin: `journal_core`) | drain the journal ring → file I/O | takes the CRT flush syscall off the tick path (§7.2); fail-closed if lapped |
| CTP md api | socket → convert → publish to md ring | vendor-owned |
| CTP trader api | socket → reports → publish to trade ring | vendor-owned |
| ops console | blocking stdin → command queue | operator interface |

The deterministic core is deliberately **one** thread: parallelizing
strategy dispatch would let strategies observe different event orders,
un-serialize the risk gate, and break byte-identical replay. Concurrency
lives at the transport instead: the broadcast rings (`plat/bus/ring.hpp`,
the seqbus lab's single-writer/multi-reader stamp-seqlock) let any number of
readers — journal thread, capture, a warm standby over shared memory —
consume the same stream with independent cursors while the wait-free writer
never knows they exist. Strategy fan-out happens *after* the sequencing
point: every strategy in the tuple sees every event in-process, statically
dispatched. The market-data ring is loss-**Tolerant** (a lapped reader
resyncs; the staleness interlock covers the hole); the trade and journal
rings are loss-**Intolerant** (losing an exec report or an audit record
halts the site).

Scaling across cores is **sharding, not intra-engine threading**: `platd
--fleet fleet.cfg` spawns one shard process per `shardN.*` config section —
each a full site (core-pinned engine, own journal/replay, own CTP session,
session-scoped EOD reconciliation) supervised from one console. An
instrument lives in exactly one shard; strategies follow their instruments;
per-shard `risk.*` keys slice the account budget. See DEPLOYMENT.md
§Sharding and `platd.fleet.example.cfg`.

## Hot-path discipline (the labs' tricks, applied)

Compile-time polymorphism end to end — there is **no virtual call, no
`std::function`, and no map lookup** between a tick entering and an order
leaving (`static_assert(!std::is_polymorphic_v<...>)` in the tests):

- **Strategies are CRTP** (`Strategy<Derived>`, §9.1 inline mode): handlers
  are templates on the site type; `Site<Gateway, Strats...>` walks a
  `std::tuple` with a fold — tick2trade's inline hot path.
- **One instance per algorithm, slots per instrument**: every strategy holds
  its per-contract state in a dense-id slot table (momentum/T0) or per-pair
  slots (arb legs) — the universe is config data, an event costs one indexed
  load whatever its size, and no O(N) "is it mine?" fan-out exists.
- **Gateway is a concept** (`OrderGateway`), not an interface: the CTP
  client and the tests' fake both bind statically — sesslink's
  ByteTransport discipline.
- **Stream dispatch is one switch** on the envelope schema with a fixed,
  inlined consumer order; `StreamCore` keeps only sequencing/hash/journal.
- **Dense-id flat arrays** where the tick path used to walk maps: books in
  the site, book mirrors in riskgate, PnL marks — symtab's stable-dense-id
  discipline. The md adapter matches symbols as zero-padded u64 wire words.
- **Allocation-free order path**: riskgate's rate window is a circular
  buffer whose capacity *is* the per-second cap; the duplicate breaker is a
  fixed 16-slot ring; the strategy re-entrancy queue is a fixed 64-slot
  ring (deques and node maps evicted from every per-action structure).
- **Wire before bookkeeping**: the gateway send happens before compliance
  accounting and telemetry on the risk-pass path.
- Word-at-a-time FNV for the stream/state hashes (see measured results);
  `hft::cpu_relax()` busy-poll in the driver's colo mode.

## Production hardening carried over from the lab reviews

- No strings/allocation on the hot path; preregistered telemetry slots;
  riskgate walks the OMS per-instrument open-order index via visitors.
- Exposure and margin checks count *working* orders at reference/order
  prices — an order burst cannot pass a cap before its first fill lands.
- Kill state is recovery state: the switch snapshots/loads/hashes with the
  gate, and commands carry a monotonic `cmd_seq` so a stale or replayed
  command can never override a newer one.
- Un-forwardable orders/cancels are bounced as Reject/CancelReject — never
  swallowed; reports are session-FIFO through one serialized ring writer.
- Riskgate rate/day caps default below the 300/s / 20k-day HFT designation
  thresholds; compliance counters alert before the venue does.

Build: part of the `hft` solution — `cmake --preset vs2026` at the repo
root, then `cmake --build build\vs2026 --config Release --target platd
plat_tests plat_bench`. `/W4` warning-clean; `plat_tests` green (119 checks).
