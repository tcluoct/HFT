# riskgate — a branchless pre-trade risk gate

The one component a trading firm is *legally required* to run at wire speed:
SEC rule 15c3-5 (and MiFID II RTS 6) says every order must pass financial and
regulatory checks *before* it reaches the exchange — no post-hoc monitoring,
no sampling. It sits inside the firm's most latency-critical path (strategy →
**riskgate** → gateway), so every nanosecond it spends is paid on every order
the firm ever sends. Fifth project in the family (`tick2trade` = the trading
hot path, `matchd` = the exchange, `ticklab` = research infra, `seqbus` = the
transport tier).

```
 strategy ──► check(order, now) ────────────────────► 0 = send to gateway
                │                                     else = reject bitmask
                │  branchless rule pipeline (one pass, no rule branches):
                │    kill switch · symbol enabled · qty>0 · max qty
                │    max notional (128-bit) · price collar lo/hi
                │    max long/short position · token-bucket rate
                ▼
   per-symbol limits ◄── seqlock'd table ◄── control thread (ops desk)
   (one cache line each)    hot-reload while checking, no lock, no stall
```

## Measured results

Windows 10 VM on AMD EPYC 7552 (20 vCPUs, no SMT), VS2026 (MSVC 14.51)
Release + LTO, gate thread pinned at time-critical priority. 20M checks (2M
warmup), 256 symbols, 5% of the flow priced out of band, `rdtscp` pairs
around each `check()` (~20 ns measurement floor on this box).

```
                 p50    p90    p99    p99.9   (ns per check, floor included)
accept path       30     40     50      61
reject path       30     40     50      61
```

**Hot reload while checking** (`--reload-us 100`: a control thread
republished a random symbol's limits every 100 µs, 13,641 times during the
measured window):

```
                 p50    p90    p99    p99.9   seqlock retries
reload off        30     40     50      61          0
reload on         30     40     50      61          1
```

The headline: **live limit updates are latency-free.** 13,641 mid-flight
reloads produced exactly one reader retry and moved no percentile. The
occasional multi-µs max in any run is hypervisor scheduling (see the family
note on this box), not the gate.

## Why a branchless bitmask pipeline is the design

A risk gate's rules are *all* evaluated on *every* order, and violations are
measured in parts per thousand. That shape is hostile to branches (nine
almost-never-taken branches = nine BTB entries that occasionally blow up into
mispredicts) and friendly to straight-line ALU work. So `check()` computes
every rule as a bool, multiplies each into its reject bit, and ORs the bits:

- **One pass, all answers.** A rejected order reports *every* rule it broke
  (`kQtyTooLarge | kPriceBandHigh`), which is what the ops desk actually
  wants to see — for free, because accumulation never short-circuits.
- **The only data-dependent branch is the final accept**, and order flow is
  overwhelmingly accepts, so it predicts essentially perfectly.
- **Rejects must be as fast as accepts** — the reject path is your defense
  against a runaway algo spraying garbage; measured above: identical.

The limits table is the other half of the design. Limits change (ops tightens
a band, enables a symbol) while orders flow; a lock here would let the
control plane stall the order path. Each symbol's limits live in their own
cache line behind a **write-rare seqlock**: readers never write shared state
(scales to any number of gate threads), writers never wait, and a reader that
races a write simply retries — measured: once in 19 million.

## The technique inventory (what's new vs the other four projects)

| Technique | Where | Why |
|---|---|---|
| **Branchless rule evaluation into a reject bitmask** | `core/gate.hpp` | `setcc`/`shift`/`or` instead of nine unpredictable branches; all violations reported in one pass; reject path provably as fast as accept. |
| **Write-rare seqlock (config-publication flavor)** | `core/seqlock.hpp` | seqbus stamps every slot of a data firehose; here the same primitive is inverted into control-plane→data-plane config delivery: readers retry ~never (1 in 19M measured), writers can't stall the hot path. Relaxed-atomic payload words keep it formally data-race-free (Boehm, MSPC'12) and free on x86. |
| **128-bit notional check via `_umul128`** | `core/types.hpp` | `price * qty` overflows 64 bits exactly when the order is most dangerous. One `MULX`, compare, no division, no branch — a fat-finger check that can't be fooled by wraparound (there's a test that proves it). |
| **Integer token bucket clocked by the TSC** | `core/throttle.hpp` | 32.32 fixed-point tokens, refill = one multiply-add on the cycle delta. No float, no timer thread. Rejected-elsewhere orders don't spend tokens (they never reach the wire). |
| **Worst-case position reservation at entry** | `core/gate.hpp` | Position is committed as if the order fully fills, `unwind()` hands it back on exchange reject/cancel — the conservative model 15c3-5 auditors expect. |
| **Caller-stamped time** | `check(o, now)` | The gate never reads the clock; the caller (who already has a timestamp) passes it in. Tests pin time and prove throttle refill to the exact cycle. |
| Flat per-symbol state array, one-cache-line limit records, `/O2 /Ob3 /GR-` + LTO | throughout | Carried over from the family — see tick2trade `docs/DESIGN.md`. |

## Build & run

```pwsh
cmake --preset vs2026            # standalone; or build the whole hft solution from ..
cmake --build --preset vs2026-release

build\vs2026\Release\riskgate_tests.exe   # 11 scenarios incl. 2s seqlock torture
build\vs2026\Release\riskgate.exe --rt    # 20M checks, pinned, report
build\vs2026\Release\riskgate.exe --rt --reload-us 100   # + live limit reloads
```

```
--orders N / --warmup N / --seed N           --symbols N     (default 256)
--reject-pct N   % of flow priced out of band (default 5)
--reload-us N    control thread republishes limits every N us
--core N / --no-pin / --rt
```

## Lab vs production

| Here | Production (15c3-5 gate) |
|---|---|
| 9 rules: collar, qty, notional, position, throttle, kill | Dozens: duplicate-order, self-trade prevention, restricted lists, locate checks for shorts, per-account × per-symbol × firm-wide aggregation |
| Position = worst-case reservation at order entry | Open-order exposure + fills stream reconciled in real time; risk-reducing orders exempted from position rejects |
| One gate thread (serialized order path) | Sharded by symbol/session; firm-wide aggregates via lock-free accumulators |
| Token bucket per gate | Per-session, per-destination, and message-type-weighted throttles matching each venue's published limits |
| Kill switch = one atomic flag | Hardware-enforced (switch/NIC-level) kill paths with independent monitoring, as regulators expect |
| EPYC VM, ~20 ns rdtscp floor, hypervisor outliers | Bare metal or FPGA pre-trade checks at single-digit ns |

## Layout

```
../common/include/hft/common/   platform, tsc_clock, histogram (solution-wide toolbox)
include/rg/
  core/     types.hpp     ids, order, reject bits, 128-bit notional helper
            seqlock.hpp   write-rare single-record seqlock
            limits.hpp    SymbolLimits + per-symbol seqlock'd table
            throttle.hpp  32.32 fixed-point TSC token bucket
            gate.hpp      the branchless check() (the core)
src/main.cpp   bench: pinned check loop, reject mix, optional reload thread
src/tests.cpp  11 scenarios: every rule bit, boundaries, overflow, throttle
               accounting, hot-reload visibility, seqlock torture
```
