# platd — production deployment guide

`platd.exe` is the production site daemon for Phase 1 of the platform
(commodity futures via the CTP counter, per `docs/platform/ARCHITECTURE.md`).
One process = one venue's instruments on one CTP trading account: market data
in, momentum strategy, 20-check in-line risk gate, OMS/PnL/compliance, full
canonical-stream journal, end-of-day reconciliation against the counter.

What you deploy:

| File | Purpose |
|---|---|
| `platd.exe` | the daemon (live / `--check` / `--replay`) |
| `thosttraderapi_se.dll`, `thostmduserapi_se.dll` | CTP client libraries (must sit next to the exe) |
| `platd.cfg` | your site config (copy `platd.example.cfg`, fill credentials) |
| `plat_tests.exe` | component self-test — run once on the target machine |
| `plat_bench.exe` | hot-path budget check — run pinned on the target to verify the tick floor (expect < 100 ns/tick on real hardware) |
| `platrec.exe` | standalone market-data recorder (record whole market / `--compare` / `--dump`); see §10 |

Build outputs land in `build/vs2026/platform/Release/` (DLLs are copied next
to the exe automatically by the build).

---

## 1. Hardware requirements

### Minimum (SimNow / paper trading / development)

- x64 CPU **with AVX2** (the build uses `/arch:AVX2` — Haswell 2013+ or any Zen)
- 4 cores, 8 GB RAM, any SSD
- Stable outbound TCP to the broker/SimNow fronts

### Recommended production server (broker colo)

China futures HFT runs from your futures broker's machine room (期货公司机房
托管) or the exchange-adjacent data centers they resell (SHFE/INE: Zhangjiang,
Shanghai; DCE: Dalian; CZCE: Zhengzhou; CFFEX: Waigaoqiao). CTP is a TCP
session to the broker's front server, so *network proximity to that front*
dominates everything else you can buy.

| Component | Recommendation | Why |
|---|---|---|
| Chassis | 1U single-socket (e.g. Dell R360/R6615, Supermicro equivalents) | colo racks bill by U |
| CPU | Highest sustained clock, modest core count: AMD EPYC 4584PX (16c, 5.7 GHz boost) or Intel Xeon E-2488 / 6544Y | the engine is one hot thread + CTP api threads; clock > cores |
| RAM | 32–64 GB ECC | whole working set is tens of MB; ECC for a machine that must not flip bits |
| Storage | 2× NVMe SSD; journal on its own volume | journal is sequential 256 B×N appends; keep it off the OS volume |
| NIC | Standard server 10 GbE (Intel X710/E810) | CTP rides broker TCP — kernel-bypass NICs (Solarflare) only pay off for exchange-direct feeds, which is a Phase-2 item |
| BIOS | SMT **off**, C-states off, fixed max clock, NUMA per socket | latency jitter control |
| Time | NTP against the broker's source minimum; PTP if the colo offers it | timestamps in reports/journal; compliance audit |

### OS

- **This build**: Windows 10/11 x64 or Windows Server 2019/2022.
- The CTP SDK and this codebase both support Linux (the CMake file already
  selects `.so` libraries on non-Windows; sources are portable). Linux is the
  common choice in broker colos — build with GCC 13+/Clang 17+ on the target
  distro when you move there.

Windows tuning for the trading host:

- Power plan **High performance**; disable CPU parking.
- Exclude the journal/flow directories from antivirus real-time scanning.
- Pause Windows Update during trading hours.
- Colo tuning triplet in `platd.cfg`: `spin = 1` (busy-poll instead of 200 µs
  sleep quanta), `engine_core = N` + `journal_core = M` (pin the engine and
  journal threads — pick two cores sharing an L3 so the journal ring's cache
  lines transfer cheaply), and `rt = 1` (time-critical scheduling class).
  Leave two further cores free for the two CTP api threads.

---

## 2. SDKs and libraries (internet downloads)

Everything third-party that platd needs, with sources:

| What | Version used | Where to download |
|---|---|---|
| **CTP client SDK** (`thosttraderapi_se`, `thostmduserapi_se`, 4 headers) | **v6.7.11** (verify with `platd --check`) | Official: SFIT 上期技术 <http://www.sfit.com.cn/5_2_DocumentDown.htm> (registration required) · SimNow mirror: <https://www.simnow.com.cn/static/apiDownload.action> · Pinned copy vendored in `platform/third_party/ctp/` (sourced from <https://github.com/vnpy/vnpy_ctp> `vnpy_ctp/api/`, which redistributes the official binaries — win64 DLL/lib + linux64 .so) |
| **Visual Studio 2022/2026 Build Tools** (MSVC v143+, C++ workload) — build machine only | MSVC 14.51 used here | <https://visualstudio.microsoft.com/downloads/> |
| **CMake ≥ 3.21** — build machine only | 4.x (VS-bundled) | bundled with VS, or <https://cmake.org/download/> |
| **VC++ Redistributable x64** — target server runtime | latest | <https://aka.ms/vs/17/release/vc_redist.x64.exe> |
| **SimNow test account** (paper venue) | — | <https://www.simnow.com.cn> — register; you get an InvestorID/password under broker `9999`, plus front addresses |
| **openctp TTS** (optional 7×24 CTP-compatible test venue) | — | <https://github.com/openctp/openctp> — same API surface, useful when SimNow is in maintenance |

No other third-party code is used: the platform itself is self-contained C++23.

To upgrade the CTP SDK later: replace the four headers in
`third_party/ctp/include/`, the import libraries in `third_party/ctp/lib/`,
and the DLLs in `third_party/ctp/bin/` with a matching set, rebuild, and
confirm the version with `platd --check`. Never mix DLL and .lib versions.

**评测版 (CP) SDK.** The 期货评测环境 fronts (`:53205` trade / `:53213` md)
only accept the evaluation build of the API; a production DLL fails the
handshake with `4040 CTP:API Front shake hand err: decode err`. The broker-
supplied v6.7.7_CP (20240524, win64) set is vendored in `third_party/ctp_cp/`
(headers converted GBK → UTF-8, `error.xml` alongside). Select it with
`cmake -S . -B build/vs2026 -DPLAT_CTP_SDK=cp`; `-DPLAT_CTP_SDK=prod` (the
default) switches back to `third_party/ctp/`. The cache variable sticks, so
check the `platform: CTP SDK = ...` configure line before building a
production binary.

---

## 3. Build (on the build machine)

```
cd C:\work\hft
cmake --preset vs2026                                    # or vs2022
cmake --build build\vs2026 --config Release --target platd plat_tests
```

Outputs: `build\vs2026\platform\Release\{platd.exe, plat_tests.exe,
thosttraderapi_se.dll, thostmduserapi_se.dll}`.

Sanity on the build machine:

```
plat_tests.exe                      # expect: "... 0 failures"
platd.exe --check platd.example.cfg # prints CTP api version + config summary
```

---

## 4. Broker provisioning (before first deployment)

Your futures broker (期货公司) must provide, per trading account:

1. **CTP counter access**: trader front + md front addresses
   (`tcp://ip:port`), BrokerID, InvestorID, password.
2. **穿透式监管 terminal registration**: an **AppID** and **AuthCode** for
   your self-built terminal (platd authenticates with these before login —
   this is mandatory since 2019 for direct API access).
3. Colo hosting / seat details and their order-rate ceiling — set
   `risk.max_actions_per_sec` safely below it.

Do the whole runbook against **SimNow first** (broker `9999`, AppID
`simnow_client_test`, AuthCode `0000000000000000`), then swap the six
credential lines for production.

---

## 5. Install on the server

```
C:\trading\platd\
    platd.exe
    platd.pdb              # KEEP WITH THE EXE: crash post-mortems symbolize
                           # to function + file:line only if the pdb is here
    thosttraderapi_se.dll
    thostmduserapi_se.dll
    platd.cfg              # from platd.example.cfg — hold credentials tight
    ctp_td_flow\           # CTP trader session state (create empty)
    ctp_md_flow\           # CTP md session state (create empty)
    journal\               # if you point `journal=` here (its own volume)
```

- Install the VC++ x64 redistributable.
- Firewall: allow **outbound** TCP to the broker's trader and md front
  ports. platd listens on nothing (operator console is stdin).
- Restrict `platd.cfg` read permission to the trading user — it contains the
  account password.
- Run `plat_tests.exe` once on the server (proves the AVX2 build runs on
  that CPU), then `platd.exe --check platd.cfg`.

## 6. Runbook

**Start** (a few minutes before the session, e.g. 08:45 for the day session):

```
cd C:\trading\platd
platd.exe platd.cfg
```

Startup sequence, all logged to stderr: connect → authenticate → login →
settlement confirm → instrument/limit/margin queries → position carry-in →
journal + instrument sidecar (`<journal>.ref`) written → md subscribe →
pre-flight → armed. A red pre-flight halts the site (orders denied) until an
operator fixes the cause and types `disarm` then `arm`.

**Operator console** (type into the running process):

| Command | Effect |
|---|---|
| `status` | envelopes / fills / risk pass–deny / session state |
| `halt` | kill switch: cancel resting orders, deny all new flow |
| `flatten` | halt + flag for manual position close-out |
| `disarm` | re-open the order path (versioned; stale commands lose) |
| `arm` | re-run pre-flight + strategy start |
| `quit` (or Ctrl-C) | cancel resting orders, reconcile, seal journal, exit |

**End of day** happens automatically after the `Closed` phase transition:
open-order and fill-total queries against the counter, OMS/PnL
reconciliation (any break is printed and must be investigated before the
next session), report written to `<journal>.report.txt`, journal sealed,
and **per-strategy position attribution written to `<journal>.positions`**
(human-readable: strategy id, symbol, carried lots, costs).

**Next trading day**: the counter's position query is account-level — it
cannot say which strategy owns which lots. At a fresh start platd reads
yesterday's `.positions` file (validated against the sealed journal's day)
and distributes the counter's carried totals per strategy, so each
strategy's PnL, margin attribution, and close-yesterday validation are
correct from the first order. The counter remains the authority on
*quantity*: any unattributed remainder (manual trades, another terminal, a
lost file) is assigned to the first configured strategy's book with a loud
warning. A strategy stopped via `<name>.enabled = 0` keeps its id and its
carried lots: the EOD write walks the PnL ledger itself, so the `.positions`
chain carries the stopped book forward each day until the strategy is
re-enabled (it picks the book back up) or you flatten it manually. Same-day
`--resume` needs none of this — the journal replay itself restores exact
per-strategy state.

**Recovery drill / audit replay** — run any evening, or after any incident:

```
platd.exe --replay platd.cfg journal\platd_shfe.jnl
```

This rebuilds the whole day from the journal's Source envelopes with fresh
components and verifies the regenerated stream hash equals the live run's —
byte-identical replay is the platform's recovery guarantee (§11). It
requires the same config (strategy/risk/fees) the live session used.

**Crash recovery (mid-session restart)** — automatic. Start the same command
you always start (`platd platd.cfg`, or `platd --fleet fleet.cfg` — each
shard decides for itself); the journal at the configured path tells the
previous run's story and platd picks the right mode:

| Found at the journal path | Decision |
|---|---|
| no file | fresh start |
| sealed journal (clean EOD) | archive as `<jnl>.<day>.sealed`, fresh start |
| **unsealed journal, today's date** | **crashed session → AUTO-RESUME** |
| unsealed journal, older date | archive as `<jnl>.<day>.crashed` + loud warning, fresh start (resume is same-day only) |
| not a journal / no `.ref` sidecar | archive, fresh start |

Journals are never overwritten — every leftover is archived (with its `.ref`
sidecar) before a fresh journal opens, so the audit record survives any
operator mistake. `--resume` remains as a *force* switch (fails rather than
guessing) for drills and unusual situations.

What a resume does:

1. The crashed journal (unsealed — the writer thread flushes every burst, so
   it is intact to within the last instant) is read back **from the
   beginning**: there is no cursor to find, because state is a pure function
   of the full Source prefix and full replay at CTP volumes is milliseconds.
   The instrument sidecar restores the exact table; the old journal is kept
   as `<journal>.crashed`.
2. The whole prefix replays through a fresh engine with the **gateway
   muted** — strategy slots, OMS, PnL, risk counters and the kill switch are
   rebuilt to the exact crash point (state-hash verified in the tests);
   the new journal re-records the prefix, so the resumed day is one
   self-contained record again.
3. The counter is queried account-wide: fills that landed while dead are
   **synthesized as Source exec reports** (journaled — the recovery itself
   replays); orders still working at the venue are **adopted** into the new
   session (cancellable, and their future fills route correctly); orders the
   venue no longer knows are closed out locally; per-instrument positions
   are compared against the counter and any break is printed loudly.
4. The site starts **HALTED**. The operator reviews the reconciliation
   summary and position breaks, then types `disarm` and `arm` (or writes
   them to `shardN.cmd`) to resume trading.

Resume is same-day only — a new trading day starts fresh with carry-in.
Note the safe default: a resumed site never sends an order until a human
has looked at the reconciliation.

**Night sessions**: run a second platd instance with a `phases=` timetable
for the night hours (times past midnight written as 24h+, e.g. `25:00
Closed` = 01:00), a separate journal path, and separate flow directories.

## 7. Sharding (scaling strategies/instruments across cores)

One `platd` site is one core-pinned deterministic engine. To scale beyond
it, run a **fleet**: one command, one process per shard —

```
platd --fleet platd.fleet.cfg
```

`platd.fleet.example.cfg` is the template: shared keys (fronts, credentials,
fees) written once, `shardN.key` overriding per shard. Each shard is a full
platd — own engine/journal cores, own journal + `--replay` drill, own CTP
session — so one shard crashing cannot take down the fleet, and shard order
ids can never collide (OrderRef scope is per session). Per-shard defaults
are derived automatically (journal `shardN.jnl`, flow dirs `ctp_td_shardN/`,
command file `shardN.cmd`).

Assignment rules:

- **An instrument lives in exactly one shard.** Books are single-writer and
  self-trade prevention must see all working orders on an instrument in one
  riskgate. Strategies follow their instruments; the same algorithm may run
  in several shards as independent instances.
- **Coupled instruments stay together** (arb pairs, spread legs): they are
  one atomic assignment unit.
- **Balance by message rate × strategy interest**, using the previous
  session's volumes; rebalance between sessions, never intraday.
- **Slice the risk budget**: `risk.*` keys are per shard — the sum across
  shards must stay inside the account's broker/exchange caps (the example
  file shows a 3-way slice under the 300/s and 20k/day HFT lines).
- **Core map**: shard engines on cores sharing the NIC's NUMA node; each
  journal writer sharing L3 with its engine; leave two cores for the OS and
  CTP api threads.

Operating a fleet: the supervisor console routes commands — `2 halt` halts
shard 2, `quit` stops all shards gracefully (as does Ctrl+C). Any terminal
can command a shard directly: `echo halt > shard2.cmd`. Each shard writes
`shardN.log`, its own journal, report, and EOD reconciliation (scoped to its
own CTP session — other shards' orders and fills are not breaks).

**Why each shard has its own md/td session** (not one shared feed process):
shards subscribe to *disjoint* instrument sets, so nothing is transmitted or
decoded twice — the sessions partition the feed, they don't duplicate it. A
tick lands directly in the process that trades it, with no cross-process
hop, no head-of-line blocking behind other shards' instruments, and no
single feed process whose death blinds the fleet. Trade sessions must be
per-shard regardless (OrderRef scope, independent seat flow control, no
extra hop on the order path). The shared-feed design becomes correct in
Phase 2 with exchange-direct **multicast** feeds — data then arrives once by
nature, and one kernel-bypass feed handler will fan out through the
shared-memory broadcast ring (`plat/bus/ring.hpp` is address-free for
exactly that); the engines don't change, only the ring's source does.

## 8. What to watch

- stderr log lines `[td]`, `[md]`, `[drv]`, `[ops]`, `[platd]`, each prefixed
  with wall-clock `HH:MM:SS.mmm`.
- The same lines persist in a **session log file** (append + unbuffered, so it
  survives a crash): platd writes `<journal>.log` (per-shard `shardN.jnl.log`
  under `--fleet`; override with `log_file =`), platrec writes
  `platrec_<date>_<source>.log`, the fleet supervisor `platd_fleet.log`.
- **After a crash** (any abnormal death — access violation, uncaught
  exception, `abort()`): the log file ends with a `[crash] FATAL: <named
  reason>` block — the faulting address, the thread, and a symbolized stack
  (function + file:line; PDBs are built in Release for exactly this) — and a
  minidump `<app>_crash_<date>_<pid>.dmp` lands beside it. Open the `.dmp` in
  Visual Studio / WinDbg with the matching `.exe` + `.pdb` for full state at
  the faulting instruction. The process exits with the exception code, which
  the fleet supervisor decodes into the same named reason (`shard 2 CRASHED:
  0xC0000005 ACCESS_VIOLATION`), so the fleet log alone answers "how did it
  die". `shardN.log` files append across restarts — crash evidence is never
  overwritten.
- `platd --dump <jnl> [count]` — human-readable journal listing (one line
  per envelope: seq, session time, Source/Derived, schema, key fields, with
  instrument symbols resolved via the `.ref` sidecar). Works on sealed and
  crashed (unsealed) journals alike.
- `status` during the session; `risk.deny.*` counters in the EOD report
  (persistent denials = a strategy/limit mismatch worth investigating).
- EOD report: `EOD recon: order breaks 0 fills OK` is the green line.
  Anything else blocks the next session until explained.
- Journal + `.ref` + report files are the audit set — archive daily.

## 8b. Web ops console (`platops`)

`platops <cfg> [--port N] [--bind ADDR]` — run it **from platd's working
directory** with the SAME config file the site/fleet runs from (it derives
each shard's journal and cmd-file paths from the `shardN.*` sections, or the
standalone keys). Open `http://127.0.0.1:8848/`.

- **Read side (zero trading-path impact):** platops *tails each shard's live
  journal* into a Replay-mode mirror `Site` — the same deterministic
  reconstruction as `platd --replay`, a few milliseconds behind live. The
  page shows per-strategy realized/unrealized/fees, positions (yd/td buckets,
  cost, mark), open orders, recent fills and risk denies, margin utilization
  vs `risk.max_margin_cny`, kill-switch state, md feed age, and an operator
  audit feed. It never touches the platd processes: the journal file is
  opened read-only per poll (never held, so the startup archive rename is
  not blocked).
- **Control side:** buttons write a single operator line into the shard's
  cmd file (`shardN.cmd`), which the platd driver polls at 100 ms. Every
  state-changing verb (`param`, `order`, `cancel`, `transfer`, `halt`,
  `flatten`, `disarm`) enters the engine as a **journaled Source envelope**:
  manual orders pass the full 20-check riskgate, everything replays in the
  recovery drill, and the journal is the regulator-grade audit trail of who
  did what when.
- **Security model:** binds `127.0.0.1` by default and has no auth — treat it
  like the stdin console. For remote access use an SSH tunnel
  (`ssh -L 8848:127.0.0.1:8848 <colo-host>`); do NOT bind `0.0.0.0` on a
  shared network.
- `param` keys are each strategy's whitelisted config keys (`lots`,
  `take_ticks`, `stop_ticks`, `timeout_ms`, `imb_gate_e4`, `max_entries`,
  plus the type-specific gates). Structural keys (universe, ids, EMA shape)
  remain restart-only by design.
- `transfer` is bookkeeping only (moves lots + cost basis between strategy
  books, yd first); to change the account's actual exposure use `order` or
  `flatten`.

## 9. Market-data recorder (`platrec`)

A **standalone** tool — no strategies, no order path, no risk gate — for
capturing the whole market to replay and backtest against, the way an HFT
desk keeps its own tick history.

```
platrec platrec.cfg                          # record until 15:30 / Ctrl+C
platrec --compare A.mdr B.mdr [golden.mdr]   # after-close verify (+ merge)
platrec --dump file.mdr [count]              # human-readable listing
```

**What it records.** At startup it uses the trader front *once* to enumerate
every instrument the counter serves (or the `record =` prefix list), then
subscribes to full-depth L2 on the md front and streams each tick to a
compact `.mdr` file. The format is delta + zigzag-varint per instrument: an
unchanged book level costs one byte, so a real L2 tick lands **~40–60 bytes
against 216 raw (~74% smaller)** — a full commodity-futures day is a few GB,
not tens. The file is integer-exact (no floats), so replay reproduces the
book bit-for-bit.

**Threading / loss.** The CTP network thread only converts and pushes to an
SPSC ring; a pinned writer thread (`writer_core`) drains and encodes to
disk, so a disk stall never blocks the socket. A `dropped` counter (ring
full) prints in the status line and the final summary — it must be zero;
if not, pin the writer and give the box a faster disk.

**Multi-datacenter (the professional discipline).** Deploy one recorder per
datacenter, each with a distinct `source` tag and `outfile`. A crash in one
loses nothing — the others kept recording, and a crashed file stays readable
to its last flush (per-burst `fflush`; unsealed header is the crash signal,
same as the trading journal). Files seal with a payload checksum at close.
Deploy `platrec.pdb` beside `platrec.exe` (same rule as §5): the session log
`platrec_<date>_<source>.log` then carries a symbolized post-mortem — named
reason, function + file:line stack, minidump — for any abnormal death (§8).

**After close: verify and merge.** `platrec --compare dc1.mdr dc2.mdr
golden.mdr` proves the datacenters agree and emits one golden file for
backtest. It aligns per instrument on the venue's own sequence key —
`(exch_ts, cumulative volume, open_interest)`, where cumulative volume is
monotone by construction — which **repairs UDP reordering** (arrival order
is not data order) and drops duplicates. It reports:

- *matched* — both sides agree on content at the same key;
- *A-only / B-only* — one side's gap the other covers (filled in the golden);
- *CONFLICTS* — same venue key, **different** content: a real data problem to
  investigate (nonzero → exit code 2);
- *out-of-order repaired* / *duplicates dropped* — transport artifacts fixed.

The golden file is canonically ordered, gap-filled, sealed, and checksummed
— the input your backtester replays. Run the compare as an automated
after-close job across all datacenter pairs; a nonzero conflict count or a
missing file is the alert.

Recorder config: `platrec.example.cfg` (needs only a read-only md account —
no order rights). The on-disk format, its delta+varint compression, and the
read path are specified in **[MDR_FORMAT.md](MDR_FORMAT.md)** — enough to
write an independent reader. The recording also stores each instrument's
reference data (tick size, contract multiplier, initial margin) captured from
the counter, so an `.mdr` is self-sufficient for backtest — no live counter
needed later.

### Backtesting (`platbt`)

```
platbt backtest.cfg --from 20260701 --to 20260716   # scan bt.dir, select by date
platbt backtest.cfg day1.mdr day2.mdr ...           # or pass explicit files
```

`platbt` replays recorded market data through the **exact same** engine and
strategies platd runs live — `Site<..., SnapMomentum, MeanRevert, Breakout>`,
the 20-check riskgate, the same books, the same PnL/fee library. Only the
venue is modelled (an IOC fill-against-book simulator) instead of connected,
so a backtest can never diverge from live behaviour: it *is* the same code,
and the strategy config uses the identical keys — the same `strategies = ...`
instance lists, per-instance params, and `risk.*`/`fees.*` blocks — copy them
from your `platd.cfg` to test the exact lineup you deploy.

**Selecting days by range.** Accumulate daily recordings in one directory
(`bt.dir`, or `--dir`) and give a `--from`/`--to` window (yyyymmdd, inclusive;
or `bt.from`/`bt.to`). platbt reads each file's header, picks **one file per
trading day** — preferring a `platrec --compare` *golden* merge (canonically
ordered, gap-filled), else the most complete sealed recording — and runs them
in date order. Each `.mdr` is one day, run independently with a fresh Site (as
a fresh platd process would each morning); the intraday strategies flatten by
close, so per-day PnL sums to the total.

**The report.** platbt writes a self-contained HTML performance report
(`bt.out` / `--out`, default `backtest_report.html`) — no external assets, one
file you can email or archive. It contains an **equity curve**, an **underwater
(drawdown) plot**, a **daily-P&L bar chart**, a per-day table, and metric
cards: net P&L, total & annualised return, **Sharpe**, annualised volatility,
**max drawdown**, Calmar, win rate, trade count, fees, and the per-strategy
split. Equity is net of fees (the ledger charges the 平今 surcharge on close).
Set the equity base with `bt.capital_cny` (returns/Sharpe/drawdown are computed
against it) and title it with `bt.report_title`. A console summary prints the
same headline numbers.

Config template: `backtest.example.cfg`. The fill model is documented in
`plat/rec/backtest_gateway.hpp` (optimistic: takes the touch, no queue
/slippage — refine it there for a more conservative test); the report and its
metrics in `plat/rec/backtest_report.hpp`.

## 10. Known Phase-1 limits (by design, see ARCHITECTURE.md)

- One venue, one account, one strategy instance per process; multi-site
  federation (the arb bridge) is Phase 2.
- Stock venues (SSE/SZSE) need the ATP/相关柜台 adapters — the book engine
  and T+1 ledger are already in place behind the same boundaries.
- After a mid-session disconnect the api relogs in automatically, but
  position/order state re-sync is the EOD reconciliation + operator's
  judgment; a full automatic reconcile-on-reconnect is a listed follow-up.
- Kill-switch `flatten` cancels resting orders; position liquidation stays a
  human action in v1.
