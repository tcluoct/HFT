# China Multi-Venue HFT Platform — Architecture Design

| | |
|---|---|
| **Version** | 0.2 (review-incorporated draft) |
| **Date** | 2026-07-11 |
| **Status** | Draft — v0.1 review findings incorporated |
| **Deliverable** | Requirements + architecture; drives phased implementation in `C:\work\hft` |
| **Audience** | Founding engineering team (≤5), principals |

> Facts about exchange rules, fees, and vendor latency claims are stated to the best
> current knowledge and marked **[verify]** where they must be re-confirmed with the
> broker/FCM/exchange during onboarding. Regulations cited are those in force as of
> July 2026.

> **v0.2 provenance.** Three independent reviews of v0.1 are incorporated. Changes are
> tagged inline with their source:
> **[R1]** = `ARCHITECTURE_REVIEW.md` (order authority / recovery / data integrity),
> **[R2]** = `review_5_5.md` (implementation readiness / phasing / venue contracts),
> **[R3]** = `review_gemini.md` (latency / determinism / concurrency).
> §23 lists what was adopted, what was rejected, and why.

---

## Table of contents

1. [Executive summary](#1-executive-summary)
2. [Goals and non-goals](#2-goals-and-non-goals)
3. [Requirements snapshot](#3-requirements-snapshot)
4. [Market and regulatory context](#4-market-and-regulatory-context)
5. [System overview](#5-system-overview)
6. [Physical infrastructure](#6-physical-infrastructure)
7. [Messaging and IPC backbone](#7-messaging-and-ipc-backbone)
8. [Market data platform](#8-market-data-platform)
9. [Trading core](#9-trading-core)
10. [Risk and compliance](#10-risk-and-compliance)
11. [State, recovery, and availability](#11-state-recovery-and-availability)
12. [Latency engineering](#12-latency-engineering)
13. [Research and backtest platform](#13-research-and-backtest-platform)
14. [Observability and operations](#14-observability-and-operations)
15. [Security](#15-security)
16. [Testing strategy](#16-testing-strategy)
17. [Build plan and phasing](#17-build-plan-and-phasing)
18. [Cost lines (informational)](#18-cost-lines-informational)
19. [Risks and mitigations](#19-risks-and-mitigations)
20. [Lab-to-platform component mapping](#20-lab-to-platform-component-mapping)
21. [Open items to verify during onboarding](#21-open-items-to-verify-during-onboarding)
22. [Glossary](#22-glossary)
23. [Review changelog (v0.1 → v0.2)](#23-review-changelog-v01--v02)

---

## 1. Executive summary

A proprietary trading firm (own capital, ≤5 engineers) is building a latency-sensitive
trading platform for the China market covering **A-shares (SSE/SZSE), CFFEX index
futures, commodity futures (SHFE/INE, DCE, CZCE, GFEX), and ETF/index options**, running
**cross-venue arbitrage** and **short-horizon L2 alpha / T+0-on-inventory** strategies.

The defining structural fact: China's venues live in **physically separate data centers**
and are reachable only **through broker/FCM counter systems** — never directly. The
platform is therefore designed as a **federation of identical per-site trading stacks**,
each colocated at one venue and autonomous on its hot path, coordinated asynchronously
over low-latency private lines. Cross-site round trips are milliseconds; they never sit
inside a single order decision.

Core architectural commitments:

- **Software µs-class hot path** — kernel-bypass NICs, pinned cores, single-writer
  lock-free pipelines; internal tick-to-trade target **≤ 1.5 µs p50 / ≤ 5 µs p99**.
  No FPGA in v1.
- **Sequenced shared-memory bus + total-order journal** at each site — every component
  is a deterministic function of its journaled input stream, which yields warm-standby
  recovery, byte-accurate replay, and a backtester that shares production semantics.
- **Full-market L2 data platform** — both stock exchanges' order-by-order feeds
  (逐笔委托/逐笔成交) plus all futures/options depth, engineered for >1M msg/s bursts,
  captured losslessly from day one.
- **Risk and compliance in-line, not alongside** — pre-trade checks inside the order
  path, with riskgate as the **sole, linearizable authority for risk reservations**
  (position, margin, sellable inventory, rate capacity) **[R1]**, plus a compliance
  engine implementing the 2024–2025 programmatic-trading regime (reporting,
  HFT-designation thresholds, cancel-ratio and abnormal-trading monitors, kill switch,
  immutable audit).
- **Phased rollout**: Phase 1 proves the entire vertical slice on **SHFE/INE commodity
  futures via CTP**; Phase 2 adds CFFEX + SSE for futures–spot arb, split into gated
  sub-phases (2A stock-data correctness → 2B stock-trading correctness → 2C cross-site
  arb) with 2A starting as an offline workstream during Phase 1 **[R2]**; Phase 3 adds
  SZSE, the Shanghai–Shenzhen line, ETF arb, and remaining futures venues.

---

## 2. Goals and non-goals

### Goals

- G1. Trade all four instrument classes through colocated low-latency counters, with a
  single strategy framework and one operational model across venues.
- G2. Internal tick-to-trade ≤ 1.5 µs p50 / ≤ 5 µs p99 (NIC wire in → counter API out).
- G3. Full-market L2 ingest and lossless capture; no dropped packets at exchange burst
  peaks (design point ≥ 2× observed peak).
- G4. Deterministic recovery: any component restart or standby takeover completes in
  seconds with zero state corruption and full order-state reconciliation.
- G5. Compliance by construction: the platform cannot emit an order that violates
  configured pre-trade limits; all regulatory reporting artifacts are producible from
  the journal alone.
- G6. Research–production parity: the same strategy binary runs in backtest, replay,
  simulation, and live, differing only in the event source and gateway binding.
- G7. Operable by ≤5 engineers: one-command session orchestration, automated pre-flight
  checks, deploy-as-code, actionable alerting.

### Non-goals (v1)

- N1. Market making and quoting obligations (options MM is a future phase; `quotd` lab
  is the seed when it comes).
- N2. FPGA on the critical path (revisit after Phase 3 with measured data).
- N3. Execution-algo products (VWAP/TWAP for external parent flow).
- N4. Hot-hot dual-live redundancy with sender arbitration (warm standby chosen instead).
- N5. Offshore markets (HK Stock Connect, SGX A50) — data ingestion may come earlier,
  trading is out of scope.
- N6. Sub-account/multi-tenant support for external clients — this is a prop platform.

---

## 3. Requirements snapshot

Agreed 2026-07-11:

| Dimension | Decision |
|---|---|
| Operator | Proprietary firm, own capital, ≤5 engineers |
| Instruments | A-shares, CFFEX futures, commodity futures, ETF/index options |
| Strategies | Cross-venue arbitrage; short-horizon L2 alpha / T+0 on inventory |
| Access | Colocated; 华锐 ATP-class stock counter, CTP二席/飞马/易盛 futures counters (default: one full-service broker + one FCM) |
| Topology | Multi-site arb-aware federation; legs fire locally |
| Latency class | Software µs-class; kernel bypass; no FPGA v1 |
| Data universe | Full market, both exchanges + all liquid derivatives |
| Scope | Market data, trading core, risk & compliance, research & backtest — all in-house |
| Availability | Warm standby + journaled deterministic recovery |
| Phase 1 | Commodity futures via CTP (SHFE/INE site) |
| Historical data | Purchased vendor L2 history (3–5 years) to bootstrap research |
| Budget | Not a constraint worth modeling; cost lines listed for awareness only |

---

## 4. Market and regulatory context

Everything in this section is a design input; the architecture references it constantly.

### 4.1 Venues and instruments

| Venue | Location (metro) | Instruments traded by us | Access path |
|---|---|---|---|
| SSE 上交所 | Shanghai | A-shares, ETFs, SSE ETF options | Broker counter (ATP-class) in SSE hosting |
| SZSE 深交所 | Shenzhen | A-shares, ETFs, SZSE ETF options | Broker counter (ATP-class) in SZSE hosting |
| CFFEX 中金所 | Shanghai | IF/IH/IC/IM index futures; IO/MO/HO index options | FCM counter (飞马 or CTP) in CFFEX hosting |
| SHFE/INE 上期所/能源中心 | Shanghai | Metals, energy, crude | FCM CTP二席 in SHFE hosting |
| DCE 大商所 | Dalian | Ags, chemicals, iron ore | FCM CTP/exchange feed in DCE hosting |
| CZCE 郑商所 | Zhengzhou | Ags, chemicals | FCM CTP/易盛 in CZCE hosting |
| GFEX 广期所 | Guangzhou | Industrial silicon, lithium carbonate, polysilicon | FCM CTP (later phase) |

### 4.2 Microstructure essentials that shape the design

**Stocks (SSE/SZSE)**

- **T+1**: shares bought today are sellable tomorrow. T+0 strategies trade around a
  held inventory (底仓). The position ledger must track *sellable* vs *total* quantity
  per stock per account; a pre-trade check enforces it. Securities lending (融券) is
  not assumed available (the 转融券 channel remains effectively shut).
- **Price limits**: ±10% main boards, ±20% STAR (科创板) and ChiNext (创业板), ±5% ST
  names, ±30% BSE (not traded v1). Limit state is a first-class book attribute —
  strategies and risk both consume it (queue-at-limit dynamics are an alpha regime of
  their own).
- **Sessions and auctions**: 09:15–09:25 opening call auction (cancels prohibited
  09:20–09:25), 09:30–11:30 and 13:00–14:57 continuous, 14:57–15:00 closing call
  auction (SZSE prohibits cancels; SSE rules equivalent) **[verify exact per-venue
  cancel windows]**. STAR/ChiNext after-hours fixed-price session 15:05–15:30. The
  platform models venue phase as an explicit state machine; order types legal per
  phase differ.
- **L2 data**: 10-level snapshots every ~3 s, plus **order-by-order** feeds — SZSE 逐笔委托
  (includes cancels) and 逐笔成交; SSE merged 逐笔 feed (order + trade) available to
  hosting users on the LDDS distribution **[verify current SSE feed catalog]**. Books
  must be *built* from 逐笔; the 3 s snapshot is only a checksum/recovery aid.
  **[verify]** exact SSE cancel-message semantics in the merged feed — one review
  **[R3]** asserts cancels must be inferred rather than received explicitly; whether
  or not that holds for the current feed version, the SSE book builder gets a formal
  reconstruction state machine and a continuous top-of-book cross-check against the
  ~3 s snapshots, with per-instrument quarantine on persistent divergence (§8.2).
- **Tick sizes/lots**: 0.01 CNY ticks, 100-share lots (STAR 200+ single-share
  increments above). Odd-lot rules differ per board.
- **Fees**: sell-side stamp duty 0.05%, transfer fee, broker commission. Fee model
  lives in one library shared by backtest and live PnL.

**Futures (CFFEX/SHFE/INE/DCE/CZCE/GFEX)**

- **T+0**, margin-based, both directions. Intraday close-out (平今) fees can be
  punitive on some products; the fee/margin table is per-product, per-date, and
  data-driven.
- **Market data is snapshot-based, not order-by-order**: exchanges publish snapshots
  every 250–500 ms depending on venue (1- or 5-level depth; five-level products
  available to hosting users on several venues) **[verify per-venue feed products and
  cadence]**. Consequence: futures "HFT" is a race to react to snapshot deltas and to
  queue well between them — the book engine for futures is a *snapshot-delta* engine,
  not an order-book replayer, and the backtest fill model must be calibrated, not
  assumed.
- **CFFEX specifics**: elevated intraday close-out fees and a per-product daily
  open-position cap for speculative accounts (on the order of hundreds of lots);
  arbitrage/hedge quota applications lift these for our arb book **[verify current
  caps and quota process — this gates Phase 2 sizing]**.
- **Night sessions** on commodity venues (21:00 to 23:00/01:00/02:30 depending on
  product) — the ops model is two trading shifts per day, and "start of day" logic
  must handle the night+day composite trading day.

**ETFs / options**

- **ETF arbitrage**: primary-market creation/redemption versus secondary-market price.
  Single-market ETFs support same-day loops (buy basket → create → sell ETF, or buy
  ETF → redeem → sell constituents). Cross-market ETFs have different settlement
  mechanics that constrain same-day loops **[verify per-fund rules]**. Creation/
  redemption goes through a broker's participation-dealer channel — a *slow path*
  (seconds+) that the architecture treats as a distinct gateway class.
- **Options**: SSE/SZSE ETF options and CFFEX index options are consumed as data and
  used as hedge legs in v1 (no MM). Options chains inflate the instrument universe
  (thousands of series) — the instrument master and feed handlers size for it.

### 4.3 Access model

No direct exchange access exists for a prop firm. The chain is:

```
strategy → our gateway → broker/FCM counter (same DC) → exchange gateway → matching engine
```

- **Stock**: broker deploys a low-latency counter (华锐 ATP class; alternatives 顶点,
  金证, 恒生 UST) in exchange hosting; we cross-connect to it. Counter-internal latency
  is single-digit µs class **[verify broker SLA]**. Orders are attributed to exchange
  trading units (交易单元/PBU) owned by the broker.
- **Futures**: FCM deploys CTP (dedicated low-latency seat, "CTP二席"), 飞马 (CFFEX
  lineage), or 易盛 (CZCE lineage) in exchange hosting. CTP trade-side latency is tens
  of µs class **[verify per-seat config]**. Seat-level flow-control limits (orders/sec
  per seat) are configured by the FCM — a hard input to our rate-limit design **[verify]**.
- Counter APIs are vendor C++ SDKs with callback threading models we do not control;
  §9.4 isolates them behind our session layer.

### 4.4 Regulatory regime (design inputs)

In force as of mid-2026:

- **《证券市场程序化交易管理规定(试行)》** (CSRC, effective 2024-10) and the exchanges'
  **程序化交易管理实施细则** (effective 2025-07): programmatic traders must file via
  their broker before trading (account, capital source, strategy classification,
  system and server-location info) and re-file on material change.
- **HFT designation**: ≥ 300 order actions (submissions + cancels) per second on a
  single account, or ≥ 20,000 per day, classifies the account as high-frequency —
  bringing extra filing (system test report, emergency plan) and differential fee
  exposure **[verify current fee schedule status]**. We assume we **will** be
  designated HFT and build the obligations in, while the risk layer enforces
  configurable per-account rate ceilings so designation is a choice, not an accident.
- **Abnormal-trading monitoring** (exchange rules enumerate patterns: instantaneous
  order-rate spikes, frequent instant cancels, momentum ignition
  (拉抬打压), large concentrated executions, self-trades): the compliance engine
  mirrors these patterns as internal monitors with thresholds tighter than the
  exchanges' **[verify current numeric thresholds where published]**.
- **《期货市场程序化交易管理规定(试行)》** (effective 2025-10): equivalent regime on the
  futures side — filing through FCMs, system requirements, monitoring.
- **看穿式监管 (see-through supervision, futures)**: client-system information is
  collected at the API layer (AppID/AuthCode authentication in CTP-family APIs);
  our gateways carry the firm's registered identifiers.
- **Audit**: order-level records reconstructable for years; our journal (§7.2) is the
  primary artifact, with daily immutable archival.

Design consequence: **compliance functions are platform primitives** — rate
accounting, cancel-ratio tracking, self-trade prevention, order tagging, report
generation — not bolt-ons.

---

## 5. System overview

### 5.1 Federation of sites

```
                        ┌────────────────────────────────────────────┐
                        │                 HQ / Research               │
                        │  tick warehouse · backtest grid · research  │
                        │  fleet control · dashboards · cold storage  │
                        └───────▲───────────────▲───────────────▲────┘
                                │ (non-latency-critical WAN)     │
        ┌───────────────────────┼───────────────┼────────────────┼──────────────┐
        │ Shanghai metro        │               │                │              │
        │  ┌──────────┐   ┌──────────┐   ┌──────────┐            │              │
        │  │ SH-EQ    │◄─►│ SH-CFFEX │◄─►│ SH-SHFE  │            │              │
        │  │ SSE colo │   │ CFFEX    │   │ SHFE/INE │            │              │
        │  └────▲─────┘   └──────────┘   └──────────┘            │              │
        │       │  sub-ms metro dark fiber                       │              │
        └───────┼────────────────────────────────────────────────┼──────────────┘
                │ premium long-haul (~15 ms one-way class)       │
        ┌───────▼─────┐        ┌───────────┐   ┌───────────┐  ┌──▼────────┐
        │ SZ-EQ       │        │ DL-FUT    │   │ ZZ-FUT    │  │ GZ-FUT    │
        │ SZSE colo   │        │ DCE colo  │   │ CZCE colo │  │ GFEX (P4) │
        └─────────────┘        └───────────┘   └───────────┘  └───────────┘
```

Principles:

- **Leg-local autonomy.** Every order decision completes within one site using that
  site's local book. Cross-site information (remote books, hedge intents, position
  state) arrives asynchronously and is treated as *stale by construction* — the
  strategy framework exposes its age explicitly.
- **Identical stacks.** Every site runs the same software stack with different
  venue plugins (feed handler, gateway) and config. One build, one deployment
  system, one mental model.
- **HQ is never on the hot path.** It aggregates, stores, supervises, and can command
  (kill switch, limit changes), but no order waits on HQ.

### 5.2 Site roles

| Site | Venue(s) | Phase | Notes |
|---|---|---|---|
| SH-SHFE | SHFE + INE | **1** | First live site; proves the vertical slice |
| SH-CFFEX | CFFEX | 2 | Index futures + index options data |
| SH-EQ | SSE | 2 | Stock/ETF leg for futures–spot arb; T+0 alpha pilot |
| SZ-EQ | SZSE | 3 | Second stock venue; cross-market ETF arb |
| DL-FUT / ZZ-FUT | DCE / CZCE | 3 | Commodity expansion |
| GZ-FUT | GFEX | 4 | Optional; liquidity-driven |
| HQ | — | 1 | Research, warehouse, fleet control from day one |

### 5.3 Per-site process model

Each site is 2–4 servers (§6.2). The trading server runs a fixed set of pinned
processes communicating over the shared-memory bus (§7):

```
                       ┌──────────────────────────── trading server ───────────────────────────┐
 exchange multicast ──►│ feedhand ──► bookeng ──► [ strategy pods ] ──► riskgate ──► gateway ───┼──► counter
 (A + B feeds)         │    │            │              │    ▲              │           │       │
                       │    ▼            ▼              ▼    │              ▼           ▼       │
                       │  ┌───────────────── seqbus: sequenced shm bus + journal ────────────┐  │
                       │  └──▲───────────▲──────────────▲───────────────▲───────────▲────────┘  │
                       │     │           │              │               │           │           │
                       │   omsd        pnld          compliance      telemetry   bridged        │
                       │ (order state) (positions)   engine          exporter    (cross-site)   │
                       └────────────────────────────────────────────────────────────────────────┘
```

- **feedhand** — venue feed handler: A/B arbitration, decode, normalize (§8).
- **bookeng** — order-book / snapshot-delta engine; publishes book events.
- **strategy pods** — strategy container processes; the hottest strategies can be
  compiled *into* the bookeng process ("inline mode", §9.1) to skip one hop.
- **riskgate** — in-line pre-trade risk; the only component allowed to hand orders
  to a gateway.
- **gateway** — counter session adapter (CTP/飞马/易盛/ATP) (§9.4).
- **omsd / pnld** — order-state and position/PnL keepers; consumers of the bus, never
  in the order path.
- **compliance engine** — rate/cancel/abnormal-pattern accounting; reads the same bus.
- **bridged** — cross-site bridge: compresses and ships selected bus topics over the
  private lines; injects remote topics into the local bus.
- **telemetry exporter** — scrapes shm counters into Prometheus off-path (§14).

Cold-path services (instrument master sync, config service, session orchestrator,
capture-to-disk) run on the site's second server, off the trading cores.

### 5.4 Canonical data flows

**Hot path (single-venue decision, e.g., SHFE momentum/arb leg):**
wire → feedhand → bookeng → strategy (inline) → riskgate → gateway → counter.
Budget: §12.1.

**Cross-site arb (e.g., IF vs SSE ETF basket):**
each site trades its own leg on local triggers; `bridged` carries basis state and
hedge intents; the arb coordinator (§9.3) enforces max-unhedged-exposure bounds; the
slower leg is managed as an execution problem (aggressive-hedge policy) rather than a
synchronized transaction.

**Recovery path:** journal (+ counter query APIs) → deterministic replay →
component state rebuilt → reconciliation gates trading re-enable (§11).

---

## 6. Physical infrastructure

### 6.1 Data centers and connectivity

- One colocation footprint per venue in the exchange's designated hosting environment,
  contracted through the broker (stock) / FCM (futures). Two cabinets per site at
  build-out (trading + capture/spares); Phase 1 sites can start with one.
- **Shanghai metro**: SSE, CFFEX, SHFE hosting sites interconnected with dual dark
  fiber (diverse routes), sub-millisecond one-way **[verify achievable routes at
  contract time]**. This makes Shanghai-internal futures–spot arb (IF/IH vs SSE ETFs)
  the tightest cross-venue loop we own — it is the Phase 2 flagship.
- **Shanghai ↔ Shenzhen**: premium low-latency fiber, ~15 ms one-way class, dual
  providers with diverse routes **[verify best commercial one-way at contract time]**.
  Used for basis distribution, hedge intents, and fleet control — never intra-order.
- **Dalian / Zhengzhou / Guangzhou**: premium carrier links to HQ + Shanghai; same
  asynchronous treatment.
- **HQ**: standard enterprise DC/cloud hybrid; receives compressed capture streams
  end-of-day (and trickle intraday); hosts research grid.

### 6.2 Server hardware (per trading site)

| Role | Spec (indicative) | Notes |
|---|---|---|
| Trading server ×2 (active + warm standby) | Latest high-clock x86 (e.g., AMD EPYC F-series / Intel high-frequency SKU), 1 socket preferred, 256 GB, NVMe | Single-socket avoids cross-socket NUMA traps; the lab's EPYC VM quirks (no-SMT, hypervisor outliers) argue for **bare metal, SMT off, fixed all-core turbo** |
| Kernel-bypass NICs | Solarflare/AMD X2522+ (ef_vi/Onload/TCPDirect) or NVIDIA ConnectX-6/7 (VMA/DPDK) — pick per counter compatibility **[verify counter NIC guidance]** | One NIC for market data (multicast), one for order/counter traffic |
| Capture server ×1 | High-core-count + large NVMe array (≥ 60 TB usable) | Passive optical taps on MD and order links; hardware-timestamped pcap of everything |
| Support server ×1 | Modest | Cold services, telemetry, jump host |
| Switching | Ultra-low-latency L2/L3 switch with hardware timestamping; optical taps | Vendor availability in China constrains model choice **[verify]** |
| Time | GNSS (GPS/北斗) antenna + PTP grandmaster per site; boundary-clock switches | See 6.4 |

### 6.3 Network design

- Market data NIC: exchange/counter multicast A and B feeds into ef_vi/DPDK zero-copy
  rings; strict receive-side CPU affinity; no kernel IP stack on these ports.
- Order NIC: counter connectivity per vendor requirements (some counters require
  Onload-accelerated TCP rather than raw ef_vi **[verify per counter]**).
- Every packet in and out is **tapped and hardware-timestamped** at the capture server:
  this is the wire truth for latency attribution (§12.2), dispute resolution, and
  replay.
- Management traffic on a physically separate NIC/VLAN; trading servers have no
  internet route.

### 6.4 Time synchronization

- GNSS-disciplined PTP grandmaster per site; PTP (hardware timestamping) to all
  servers; NIC-timestamped RX/TX where supported.
- Target: ≤ 1 µs absolute error site-to-site **[verify achievable]**, sufficient to
  order cross-site events for research and arb attribution (cross-site links are ms —
  µs sync error is negligible there, but intra-site attribution wants ns-class, which
  NIC hardware timestamps provide).
- `rdtsc`/invariant-TSC used on-core for interval measurement, disciplined against
  PTP; the lab's MSVC `rdtscp`-ordering pitfall generalizes: **all timestamping code
  goes through one audited `hft::clock` facade** with documented serialization
  semantics per compiler.

---

## 7. Messaging and IPC backbone

The backbone generalizes the `seqbus` lab: a **per-site, sequenced, shared-memory
message bus with a total-order journal**. It is the platform's spine; everything else
is a plugin on it.

### 7.1 Sequenced shared-memory bus

- Single-producer-per-topic, multi-consumer lock-free rings in hugepage shm; fixed
  power-of-two slots; cache-line-aligned headers; no allocation, no syscalls, no
  locks on publish or consume.
- A lightweight **site sequencer** assigns a monotonically increasing sequence number
  to every message accepted onto the "canonical" topics (orders, executions, risk
  decisions, book deltas that trigger actions). The canonical stream defines the
  site's logical time: *the state of any component is a pure function of the
  canonical stream prefix it has consumed*.
- Hot-path publishes are wait-free for the producer; consumers busy-poll with bounded
  spin, pinned per §12.3.
- Message envelope: `seq | ns-timestamp | topic | schema-id | payload` — schemas
  from the codec layer (§7.4).
- **Overflow policy is explicit per topic class [R3]:**
  - **Loss-tolerant** topics (raw depth, telemetry): a lapped consumer resyncs to the
    live edge, the loss is counted and alarmed, and the affected books enter
    gap-quarantine handling (§8.1) — the consumer never blocks the producer.
  - **Loss-intolerant** topics (order intents, execution reports, risk decisions,
    control commands): a lapped consumer is a **fail-closed fault** — the consumer
    latches a sticky fault state, the site halts new orders (kill-switch path), and
    trading re-enables only after reconciliation (§11). The producer is never
    back-pressured into the NIC path.
  - Ring sizing is derived from measured venue bursts: hold ≥ 10 s of traffic at 2×
    the observed peak (§8.3), validated by the per-site capacity acceptance test
    (§16). (R3's "minimum 16M slots" is treated as illustrative, not a requirement —
    sizing follows measurement.)

### 7.2 Journal

- A dedicated journal writer drains the canonical stream to NVMe via `io_uring`
  (O_DIRECT, preallocated extents), double-buffered; sustained write sized for full
  burst rate (§8.6 numbers).
- Journal files are the **legal audit record**, the **recovery substrate** (§11), and
  the **replay/backtest input**. Daily segments are sealed, checksummed, and shipped
  to HQ cold storage (WORM bucket) — retention ≥ 5 years.
- A second-priority journal captures non-canonical bulk topics (full book deltas,
  telemetry) with relaxed durability.
- **Durability semantics [R1]** — the journal is an asynchronous drain, so the
  boundary between "on the bus" and "durable" is specified, not implied:
  - The journal writer publishes a **durable watermark** (highest sequence known
    fsync'd). Components that must not outrun durability (snapshotting, EOD seal,
    standby freshness) key off the watermark, not the live head.
  - **Order-path rule**: an outbound order intent is journaled *before* the gateway
    hands it to the counter SDK; inbound execution reports are journaled on receipt
    and **deduplicated idempotently** on replay/reconnect (counter APIs can redeliver).
    An order the counter accepted but the journal lost is therefore impossible in one
    direction and detectable in the other — the failure window is closed by counter
    query reconciliation, which is a **trading barrier**: no re-enable until order and
    position state match the counter (§11.2).
  - Component snapshots carry the canonical sequence number and a checksum; replay-to-
    live handoff is atomic (a component subscribes at the exact next sequence after
    its replayed prefix — no gap, no overlap).
  - Journal-writer or journal-volume failure is a fail-closed site fault: halt new
    orders, keep cancels available, alarm. Fault injection covers every one of these
    boundaries, including failure between SDK handoff and journal durability (§16).

### 7.3 Cross-site bridge (`bridged`)

- Publishes selected topics to remote sites over TCP (or reliable-UDP) on the private
  lines: basis/board summaries (compact, high-frequency), hedge intents, position
  deltas, fleet-control commands.
- Remote messages are injected into the local bus under remote-origin topics carrying
  both origin timestamp and local arrival timestamp — consumers always see staleness
  explicitly.
- Bandwidth discipline: raw L2 never crosses sites in real time (a full SZSE feed
  would saturate the line and be stale anyway); summaries and features do. Full raw
  data travels to HQ after hours.

### 7.4 Codec layer

- All schemas defined in a single IDL (evolving the `wirecodec` lab): fixed-layout,
  little-endian, versioned, zero-parse-on-read (SBE-style). Codegen emits C++23
  views + Python bindings (research reads the same bytes).
- Venue-native messages are decoded once at the feed handler into normalized schemas;
  the original raw bytes are preserved in capture (§8.6) so decoding bugs are always
  recoverable after the fact.

---

## 8. Market data platform

### 8.1 Feed handlers (per venue plugin)

| Venue | Feed | Handler behavior |
|---|---|---|
| SSE | LDDS-distributed L2: merged 逐笔 (orders+trades), 10-level snapshots, indices, options **[verify catalog]** | Build books from 逐笔; snapshots as integrity check |
| SZSE | Binary multicast: 逐笔委托 (incl. cancels), 逐笔成交, snapshots, options | Same; SZSE order feed includes cancels → exact books |
| CFFEX / SHFE / INE / DCE / CZCE / GFEX | Exchange/counter snapshot feeds, 250–500 ms cadence, 1–5 levels; five-level products for hosting users where offered **[verify per venue]** | Snapshot-delta engine; derive trade/flow estimates between snapshots |

Common handler core: ef_vi/DPDK RX → decode → normalize → publish, with per-feed
**A/B arbitration** (evolving `feedarb`): accept-first-valid by sequence, detect gaps,
and *quarantine trading* (stale-data interlock, §10.1) on unrecovered gaps.

**Gap recovery is specified per feed, and snapshots cannot repair order-by-order
gaps [R1].** A periodic snapshot cannot reconstruct omitted 逐笔 orders, queue state,
or trades — it can only establish a fresh baseline where the venue protocol permits.
Each feed handler therefore documents: its sequence domain, A/B deduplication rule,
retransmission or baseline source, checksum policy, and the **blocked trading scope**
while degraded. On an unrecoverable order-by-order gap: discard the affected book and
own-queue state, publish the gap as a journaled event, obtain an authoritative
baseline (venue snapshot/retransmission), and re-enable only after a stated
synchronization condition holds (e.g., top-of-book consistent with the next N
snapshots). Capture loss and decoder errors are tracked as **separate states** from
feed gaps. Per-feed gap-recovery conformance evidence is a Phase-1 exit gate (§17).

### 8.2 Book engine

- **Stocks**: full order-by-order book (price-time), per-instrument; maintains queue
  position estimates for our own orders (from 逐笔 stream + our order events);
  auction-phase books (indicative match price/volume during call auctions); limit-state
  tracking. The SSE builder is specified as a **formal reconstruction state machine**
  (per message type, including whatever cancel representation the current feed uses),
  with a real-time sanity check: reconstructed top-of-book is cross-referenced against
  the periodic 10-level snapshots, and an instrument whose divergence persists beyond
  one snapshot interval is quarantined (riskgate #11) until re-baselined **[R3]**.
- **Futures/options**: snapshot-delta books with derived micro-features (imbalance
  deltas, implied queue turnover between snapshots).
- Output: normalized book events on the bus; top-N mirrors in shm for random-access
  readers (risk collar checks read best-bid/ask lock-free).
- Sizing: full market ≈ ~5,500 stocks + ~1,000 ETFs + thousands of option series +
  hundreds of futures contracts. Books are sharded across cores by instrument hash;
  the alpha universe's books are pinned to the strategy cores' NUMA node.

### 8.3 Capacity targets (estimates to validate in Phase 1–2)

| Metric | Design point |
|---|---|
| Stock L2 events, both exchanges, active day | several ×10⁸ messages/day |
| Sustained mid-session rate | ~10⁵ msg/s |
| Burst peak (09:25/09:30, resumptions) | > 10⁶ msg/s; design ≥ 2× |
| Futures snapshots, all venues | ~10³–10⁴ msg/s steady |
| Raw capture volume | ~100–200 GB/day uncompressed; 30–60 GB/day compressed |

### 8.4 Derived data services

- Basis engine: continuous fair-basis for each arb pair (futures vs index/ETF/basket),
  published as compact topics — the input to arb strategies and the main cross-site
  export.
- Bar/feature builders (cold path): consume the bus, emit research features intraday
  to HQ; identical feature code is importable by the backtester.

### 8.5 Replay service

- Any journal/capture segment can be replayed onto a private bus instance at wall-clock,
  accelerated, or step mode — used by the backtester (§13.2), regression tests (§16),
  and post-mortems. Replay fidelity is a supported product feature, not a debug hack.

### 8.6 Capture

- Optical-tap pcap with hardware timestamps on every exchange-facing and
  counter-facing port, 24×7.
- **Retention classes [R1]** (v0.1's blanket "5 years of everything" was ambiguous
  about what is retained where):
  | Class | Content | Retention | Location |
  |---|---|---|---|
  | Audit journal | Canonical stream (sealed daily segments) | ≥ 5 years (regulatory) | HQ WORM + site |
  | Raw pcap | Full tapped traffic | ≥ 10 trading days local; ~6 months at HQ (forensic window) **[verify legal minimum with broker/FCM]** | Site → HQ |
  | Incident holds | Segments referenced by an incident/dispute | Life of the matter + retention | HQ WORM |
  - Volumes in §8.3 are **per site**; storage and WAN shipment windows are sized per
    site from measured A/B feed + counter traffic, with archival encryption,
    integrity chains, access logging, and periodic restoration tests.
- **Per-site capture/capacity acceptance test [R2]**: before go-live, replay or
  synthesize ≥ 2× observed peak market data plus order bursts and prove no capture
  loss, no journal backpressure into the hot path, and a successful restore from the
  HQ archive. Re-run on hardware or venue-format changes.
- Capture is the ground truth that decoders and the journal are audited against
  (weekly automated cross-check job).

---

## 9. Trading core

### 9.1 Strategy container and framework API

- C++23 framework; strategies are event handlers over normalized events:
  `on_book`, `on_tick`, `on_snapshot`, `on_signal`, `on_timer`, `on_order_update`,
  `on_fill`, `on_phase_change`, `on_remote(topic)`.
- **Determinism contract**: no syscalls, no allocation, no clocks other than injected
  event time, no global state outside the framework — enforcing that a strategy is a
  pure function of its event stream (this is what makes backtest = live).
- Two deployment modes per strategy, chosen by config:
  - **inline** — compiled into the bookeng/strategy process, called synchronously on
    book update (hottest path, ~zero hop cost);
  - **pod** — separate pinned process on the bus (isolation for heavier or
    experimental strategies).
- Strategy pods are versioned artifacts deployed through the promotion pipeline
  (§13.4); config (universe, limits, parameters) is data, versioned and dual-approved.

### 9.2 Signal pipeline

- `sigcalc` heritage: a signal graph library (stateless nodes over event streams,
  fixed-point where possible) used both inline (hot signals) and in feature builders
  (research parity). Signals declare their input topics; the framework schedules them
  without virtual dispatch on the hot path (CRTP/静态 graph codegen).

### 9.3 Execution: multi-leg arbitrage engine

The arb engine (evolving `sord`) runs per pair/complex:

- **Opportunity ledger**: tracks each detected edge (basis deviation beyond costs),
  its lifecycle (detected → legged → hedged → closed), and realized slippage —
  feeding both risk (unhedged exposure) and research (edge decay curves).
- **Leg policy**: the liquid/passive leg works locally (join/improve at its venue);
  on fill, a **hedge intent** fires to the other leg's site, where the hedge executes
  aggressively within a bounded slippage budget; if the budget is exceeded, the
  engine unwinds or escalates per config.
- **Exposure bounds**: hard cap on net unhedged notional per pair and globally,
  enforced by riskgate on every order — the arb engine *requests*, riskgate *permits*.
- **Global caps are enforced through pre-allocated local budgets, not shared state
  [R1]**: a local riskgate cannot know firm-wide exposure from asynchronous replicas
  during a partition or simultaneous fills. Each site holds a **durable, expiring
  risk-budget lease** per pair/account/site, allocated by an HQ coordinator that
  rebalances allocations but is **never in the approval path**. A site trades only
  within its lease; allocation messages are idempotent and versioned. On bridge loss:
  new legging halts when the lease expires, existing exposure is managed with local
  hedges (degraded mode, §19), and leases re-sync on reconnect. Designed and built in
  Phase 1 even though the first cross-site strategy is Phase 2.
- **Same-metro fast pairs** (CFFEX vs SSE within Shanghai) get a tighter variant:
  sub-ms intent delivery makes near-simultaneous legging viable; the engine is the
  same, only the staleness/budget parameters differ.
- ETF primary-market loop (create/redeem) is a slow-path executor: basket/ETF legs
  trade on the fast path, the creation/redemption instruction goes through the broker
  PD gateway with its own state machine and end-of-day reconciliation.

### 9.4 Order gateways and session layer

- One gateway process per counter session, wrapping vendor SDKs (CTP, 飞马, 易盛,
  ATP-class) behind a single internal order API: `place / cancel / status`, normalized
  order-state machine, translated error taxonomy.
- Vendor callback threads are confined at the boundary: SDK threads write into a
  lock-free inbound ring; our pinned session thread owns all SDK calls outbound
  (`sesslink` heritage). No vendor thread ever touches the bus directly.
- Session lifecycle: authentication (incl. 看穿式 identifiers), settlement
  confirmation (futures), flow-control tracking against seat limits, heartbeat and
  reconnect with **re-entrancy protection** — after reconnect, order state is
  re-queried before any new order (no blind resend, ever).
- Rate governance lives in riskgate (token buckets per account/seat), not in the
  gateway, so it is enforced even if a gateway misbehaves.
- **Per-counter order-state contract, written before code [R2]**: `place / cancel /
  status` is the internal API, not the specification. For each counter family
  (starting with CTP as a **Phase 0 exit artifact**) a documented state machine
  covers: client order ID construction; broker vs exchange order IDs; insert-reject
  vs exchange-reject; cancel-before-ack races; partial-fill + cancel races; reconnect
  query rules; duplicate callbacks; settlement confirmation; 平今/平昨 flags;
  hedge/speculation flags; per-product order-type support; exchange trading-day
  handling across night/day sessions; and the idempotent journal mapping for every
  callback. Conformance tests are generated from this contract and re-run on every
  SDK upgrade (§16).

### 9.5 OMS, positions, PnL

- `omsd`: the order-state keeper — consumes canonical order/exec topics, maintains the
  authoritative in-memory order table (mirrored in shm for read-only consumers),
  serves queries, and drives end-of-day order reconciliation against counter records.
- `pnld`: positions and PnL — per instrument/strategy/account/site; futures margin and
  平今/平昨 lot accounting; stock **sellable-vs-total** ledger implementing T+1; ETF
  creation/redemption inventory in transit; marks from book engine; realized/unrealized
  split; end-of-day reconciliation against broker/FCM statements (对账) with breaks
  alarmed, and trading blocked next session until cleared.
- Both are bus consumers (never in the order hot path) but their shm mirrors are
  readable by riskgate at nanosecond cost.

### 9.6 Instrument master

- `symtab` heritage: the reference-data service. Sources: exchange files via
  broker/FCM + purchased reference feeds. Contents: instrument static data, tick
  sizes/lots, price-limit rules per board, fee/margin tables (per product per date),
  trading-phase calendars (incl. night sessions and holiday bridging), option series,
  index/ETF constituents and weights (arb needs exact creation baskets daily —
  申赎清单 ingested each morning), futures roll calendars.
- Compiled nightly into an immutable per-day binary snapshot loaded by every process
  at start — no live reference-data lookups on the hot path.
- **Intraday override path [R2]**: halts/resumptions, emergency margin or fee
  changes, broker restrictions, and corrected reference files can arrive mid-session,
  so the immutable snapshot is the baseline, not the only mechanism. Overrides are
  signed, versioned deltas — dual-approved where not time-critical — journaled as
  canonical events (deterministic in replay), applied atomically, and visible to
  riskgate immediately. Fields that cannot be safely changed live (e.g., multipliers,
  tick size) instead **halt the affected instruments** until the next clean snapshot;
  the field classification (live-safe vs halt-required) is part of the schema.

---

## 10. Risk and compliance

### 10.1 In-line pre-trade risk (`riskgate`)

Single process (per site) through which every order must pass; total budget
**≤ 150 ns p50** on the approve path (a Phase-1 measured target, §12.1). Checks:

**Authority model [R1][R3].** v0.1 had riskgate approving against omsd/pnld
projections that lag the bus — concurrent strategies could both pass against the same
stale state. v0.2 makes riskgate the **single, linearizable authority** for every
resource that gates approval:

- riskgate runs as a **single-threaded executor on a dedicated core**; all order
  intents from all strategy pods serialize through it (this is also what the
  sequenced canonical stream gives the in-process composition — the lab
  implementation already has this shape).
- It is the **sole writer of a reservation ledger**: exposure/notional, margin,
  sellable inventory, and rate capacity are *reserved* at approval time — an approved
  order's worst case is counted before the next order is judged, so an order burst
  can never pass a cap before its first fill lands. Execution reports (fills,
  cancels, rejects) **release or convert reservations idempotently**.
- omsd/pnld shm mirrors remain read-optimized projections for humans, strategies,
  and monitors — they are **never the approval authority**.
- Inconsistency (reservation ledger vs reconciled counter state) and overload are
  **fail-closed**: halt new orders on the affected scope, keep cancels flowing.

**Risk contract, not checklist [R2].** Each check in the table below is specified
with: authoritative input, updating event, reservation rule (where stateful), failure
mode, machine-readable denial code, test fixture, and post-reconnect/replay recovery
behavior. A check is complete only when it has adversarial tests for stale input,
duplicate events, and reject/fill races. Venue-specific predicates called out by
review — per-product position caps, CFFEX speculative quota consumption, 平今
constraints, options exercise/assignment limits, ETF cash-substitute rules, order
type/TIF legality per phase, self-trade scope across accounts/PBUs, mandatory
regulatory tags — are rows in that contract, not afterthoughts.

| # | Check | Notes |
|---|---|---|
| 1 | Session/phase gate | Venue phase machine: order type legal now? (auction cancel bans, night session, halt) |
| 2 | Price collar | vs limit-up/down and vs reference price band (fat-finger) |
| 3 | Size/notional caps | Per order, per instrument class |
| 4 | Position/exposure caps | Per instrument, strategy, account, firm; arb unhedged-exposure caps |
| 5 | Sellable inventory (stocks) | T+1 ledger check |
| 6 | Buying power / margin (futures) | Against pnld margin mirror + FCM headroom |
| 7 | Rate token buckets | Per account and per seat (below seat flow-control and, if configured, below HFT thresholds) |
| 8 | Cancel-ratio guard | Rolling windows per account/instrument |
| 9 | Self-trade prevention | Against firm's own resting orders across all strategies on that venue (cancel-newest policy default) |
| 10 | Duplicate/burst breaker | Identical-order suppression; per-strategy circuit breaker on repeat-fire |
| 11 | Stale-data interlock | Refuse to trade instruments whose book is gap-quarantined or older than a bound |
| 12 | Kill-switch state | Global/account/strategy arm state |

Denials are messages on the bus (audit + strategy feedback), never silent.

### 10.2 Compliance engine (cold, same data)

- Reconstructs, from the canonical stream, the exact per-account counters the
  exchanges compute: order actions/sec and /day, cancel ratios, self-trade incidence,
  pattern monitors mirroring the 异常交易 catalog — alerting *before* exchange
  thresholds are approached.
- Generates filing artifacts: initial and change filings for 程序化交易报备 (via
  broker/FCM), HFT supplementary materials (system test reports from §16 outputs,
  emergency-response plan document), and ad-hoc regulator inquiries.
- **Evidence model [R2]**: the regulatory evidence package is broader than the
  journal — it includes filings and broker/FCM acknowledgements, strategy
  classifications, system test reports, emergency plans, operator approvals, build
  provenance and deployment hashes, data-retention proof, and incident records. The
  compliance engine's data model enumerates each artifact type with its source of
  truth, retention period, signer/approver, regeneration command, and
  regulator-facing export format. "Producible from the journal alone" (v0.1 §1) is
  narrowed to *order-level* artifacts.
- Maintains the order-tagging requirements per venue rulebook **[verify current
  tagging mechanics per venue — broker-side vs API field]**.

### 10.3 Kill switch and flatten

- Three scopes (strategy / account / site) plus firm-global; armed states are shm
  flags checked by riskgate (#12) — engage latency is one flag write.
- **Control commands are signed, versioned, and durable [R1]**: every kill/limit/
  fleet command carries a command ID, issuer, scope, **monotonic version**, expiry,
  and requires acknowledgement; stale or replayed commands are ignored by version
  comparison; all commands and acks are journaled. The latest restrictive local
  state is **persisted before activation**, and a restarted riskgate loads it before
  opening the order path — a restart can never silently disarm a halt.
- **Partition semantics [R1]**: a firm-global halt is *eventually propagated* — it
  cannot be an instantaneous guarantee across disconnected sites. Each site retains
  independently operable local/site controls (physical and logical) that work with
  HQ unreachable (§14 local-ops mode).
- Two actions: **halt** (block new orders, cancel resting) and **flatten**. Flatten
  is a **constrained execution workflow** (bounded-aggression liquidation with its
  own limits, progress reporting, and abort), not a flag write **[R1]**. Triggerable
  by: any engineer (2-person confirm for firm-global flatten), automated monitors
  (position breaks, loss limits, connectivity loss, data-integrity alarms, ring/
  journal fail-closed faults), and physically from HQ and each site.
- Drilled monthly (§16).

### 10.4 Audit

- The sealed journal (§7.2) + capture (§8.6) + config/version history constitute the
  audit stack. A one-command tool reconstructs any order's full lifecycle with wire
  timestamps — built early because it is also the debugging tool.

---

## 11. State, recovery, and availability

### 11.1 Determinism as the recovery primitive

Every stateful component = `snapshot + replay(canonical stream since snapshot)`.
Components snapshot to shm/NVMe every N seconds. Restart: load snapshot, replay from
journal, rejoin live stream — target **< 5 s** to live for any single component.

### 11.2 Warm standby

- The standby trading server consumes the same multicast feeds and mirrors the
  journal stream (tail-follow over the site LAN), holding warm state minutes-fresh.
- **Takeover procedure** (operator-initiated or watchdog-proposed, human-confirmed in
  v1): fence the active (management-network power/port fence), standby replays to
  journal head, **reconciles order/position state against the counter's query APIs**
  (the counter is the source of truth for what got through in the failure window),
  arms riskgate in query-only mode, then re-enables trading per strategy. Target:
  < 60 s to first re-enabled strategy.
- Order-ID space is partitioned (epoch prefix per activation) so a takeover can never
  reuse in-flight IDs.
- **Automation boundary [R2]** (resolves v0.1 open question (c)): automate
  *detection, local halt, alerting, evidence capture, and fencing proposal* from the
  start — these are safe when wrong. Keep *promotion and trading re-enable*
  human-confirmed in v1, and revisit only after repeated sim and production-window
  drills cover: active-host death, journal lag, gateway disconnect, counter
  ambiguity, split-brain management network, and replay-to-live handoff.

### 11.3 Session lifecycle (daily)

- **Pre-flight** (automated, gate to session start): time-sync health, reference-data
  snapshot loaded and cross-checked, feeds alive on A and B, gateway auth + settlement
  confirm, limits loaded and hash-verified against approved config, kill-switch test
  (arm/disarm on test account), capture rolling, journal writable, standby fresh.
  Any red item blocks trading; the checklist result is itself journaled.
- **EOD**: order/position reconciliation vs counter and broker/FCM statements; journal
  seal + ship; capture ship; PnL sign-off; compliance counters archived; next-day
  reference build. Night-session venues run a compressed variant at 20:30.
- **Trading-day vs calendar-day state is explicit [R3]**: night sessions belong to
  the *next* exchange trading day, and broker statements arrive between the day close
  and the night open. The 20:30 pre-flight is a formal transition protocol: archive
  the completed trading day's journal segments, reset per-trading-day compliance
  counters and rate buckets, load the new trading day's reference snapshot, and
  re-confirm settlement — no daily state may key off the calendar date.

---

## 12. Latency engineering

### 12.1 Budgets

**Status: engineering targets to be validated in Phase 1, not commitments [R1].**
The table below is the design allocation; the numbers become SLOs only after
reproducible measurement on production-intent bare metal with the selected counter
SDK. Interval endpoints are exact: "internal tick-to-trade" is **first byte of the
triggering packet at the MD NIC → order API call enqueued to the counter SDK**
(SDK-internal time excluded, measured separately). Three SLOs are tracked
independently: decision (wire→riskgate verdict), gateway enqueue (verdict→SDK call),
and counter acknowledgement (SDK call→counter ack, external). Reservation and
self-trade work is inside the riskgate line.

Internal hot path (NIC wire in → counter API handoff out), software µs-class:

| Segment | p50 target | p99 target |
|---|---|---|
| NIC RX → decoded event (ef_vi, zero-copy) | 300 ns | 600 ns |
| Normalize + book update | 150 ns | 350 ns |
| Signal + strategy decision (inline mode) | 250 ns | 700 ns |
| riskgate (full check battery) | 100 ns | 200 ns |
| Order encode + session handoff (API call enqueue) | 300 ns | 800 ns |
| **Total internal tick-to-trade** | **~1.1 µs** | **~2.7 µs** |
| Contingency to stated SLO | ≤ 1.5 µs | ≤ 5 µs |

External segments (measured, not controlled): counter processing (ATP-class:
single-digit µs **[verify]**; CTP-class: tens of µs **[verify]**), exchange gateway +
matching, and — dominant on futures — the 250–500 ms MD snapshot cadence. The
per-venue *edge model* (where speed actually pays) is a research deliverable that
these measurements feed.

### 12.2 Measurement methodology

- **Wire truth**: hardware-timestamped taps (capture server) measure NIC-to-NIC
  reality; internal shm timestamps decompose it per segment. Every production order
  carries its trigger-event timestamp end-to-end, so tick-to-trade is measured on
  every order, not sampled.
- HDR histograms per segment per instrument-shard, exported off-path; p50/p99/p99.9
  regression-tracked daily against golden baselines (§16).
- Clock discipline per §6.4; all measurement code through `hft::clock` (the lab's
  MSVC `rdtscp` fence lesson is institutionalized there).

### 12.3 Tuning standards (per trading server)

- Bare metal, SMT off, fixed frequency (no turbo variance), C-states capped at C1,
  NUMA-local everything; `isolcpus` + `nohz_full` + `rcu_nocbs` for trading cores;
  IRQ affinity away from trading cores; hugepages for all shm; no page-cache writes
  on trading cores (journal writer owns its core + NVMe queue).
- **Single-socket is mandatory for trading servers, and the entire hot path (feed
  RX → book → inline strategy → riskgate → gateway TX) is pinned within one NUMA
  node — on AMD Zen parts, within one CCD/L3 complex [R3]**: a cross-CCX or
  cross-socket shm hop costs 50–150 ns per message and would erase the riskgate and
  tick-to-trade budgets. The core map is chosen against the CPU's cache topology,
  not just its core count.
- A published **core map** per site (documented in config, asserted at startup):
  e.g., cores 0–1 OS/housekeeping; 2 feed RX; 3 book+inline strategies (hot shard);
  4 second book shard; 5 riskgate+gateway TX; 6 journal; 7 telemetry/spare.
- Startup self-test asserts the environment (frequencies, isolation, hugepages, NIC
  modes) and refuses to trade on mismatch — environment drift is a first-class
  defect. It includes a **TSC cross-core calibration step [R3]**: measure TSC offsets
  across all trading cores and refuse to trade if skew exceeds the configured bound
  (tens of ns; exact threshold set from measured hardware, not folklore) — protecting
  `hft::clock`-based latency attribution.

---

## 13. Research and backtest platform

### 13.1 Tick warehouse

- **Bootstrap**: purchase 3–5 years of full-market L2 history (stock 逐笔 + snapshots,
  futures snapshots, options) from a data vendor (通联/RQData/Wind-class **[select
  during Phase 1]**), normalized into our event schemas so vendor data and our own
  capture are interchangeable to research code.
- **Forward**: our own journal/capture becomes the primary source from each site's
  go-live (higher fidelity: wire timestamps, our order interleaving).
- Storage: normalized binary event files (replay-grade, memory-mappable) as ground
  truth + columnar projections (Parquet/ClickHouse) for interactive research; HQ
  cluster, ~30–60 TB/year growth, cold tier to object storage.

### 13.2 Backtester

- The replay service (§8.5) drives the **same strategy binaries** and the same signal
  graph code through a simulated exchange layer (evolving `matchd`):
  - **Stocks**: venue-accurate price-time matching reconstructed from 逐笔 — our
    simulated orders are merged into the historical stream with queue-position
    modeling and auction matching (opening/closing calls, price limits, halt rules).
  - **Futures**: snapshot-cadence simulation with **calibrated fill models**
    (fill probability vs queue/imbalance/latency, fitted continuously from our own
    live fills — a standing Phase-1 data product).
  - Costs: the shared fee/tax/margin library (§4.2) — one implementation for
    backtest and live PnL.
- Latency modeling: per-venue distributions measured in production (§12.2) injected
  into simulation; no zero-latency fantasies.
- Determinism: a backtest is a replay with a seed — bit-reproducible, diffable. The
  determinism contract (§9.1) is extended to a full **execution ABI [R1]**: event-
  ordering rule, timer source, PRNG seeding, compiler and floating-point settings,
  and config/reference-data hashes are all pinned and recorded with each run.
- **Parity is tiered, with promotion thresholds per tier [R1][R2]** — "same binary"
  is necessary but not sufficient, and vendor history is not byte-equivalent
  production input:
  1. **Replay parity** — internal capture/journal replay reproduces production
     decisions byte-identically (hard guarantee, tested nightly).
  2. **Historical simulation** — vendor-data research with documented missing fields
     and timing limitations (soft tier; stated error bounds).
  3. **Live validation** — fill probability, slippage, reject rates, queue estimates,
     and latency compared against production continuously.
  Strategy promotion (§13.4) states which tier its evidence comes from and the error
  bounds required at that tier.
- **Fill-model honesty on futures [R3]**: naive FIFO-from-snapshot assumptions
  systematically flatter passive fills (we get filled when the market moves against
  us). `matchd` models queue-priority decay and fill probability as functions of
  traded-volume delta between snapshots, queue size, and measured latency; every
  backtest report includes a **fill-sensitivity table** showing PnL degradation when
  fills are delayed by 1–5 ticks. A strategy whose edge dies at +1 tick of fill
  delay is not promotable on backtest evidence alone.

### 13.3 Simulation and paper environments

- **Sim**: full site stack against `matchd` in real time — the staging environment
  for strategy promotion and ops drills.
- **Counter/venue test environments**: CTP SimNow, broker/FCM UAT, and exchange test
  sessions for gateway conformance (mandatory before go-live; §16).

### 13.4 Strategy lifecycle

`research (Python/C++ over warehouse) → backtest (registered, reproducible) → sim
(≥ N sessions clean) → live canary (min size, tight limits) → scaled` — each promotion
is a recorded, dual-approved config change; demotion is one command. The framework's
determinism contract (§9.1) is what makes each stage's behavior predictive of the next.

---

## 14. Observability and operations

- **Telemetry**: every process exposes shm counter/gauge/histogram pages; an off-path
  exporter publishes to Prometheus at HQ; Grafana dashboards per site (feed health,
  gap counts, book staleness, order rates vs thresholds, riskgate denials by reason,
  latency percentiles, journal lag, standby freshness, PnL/positions, line health).
- **Alerting tiers**: page (trading-impacting: feed gap unrecovered, gateway
  disconnect, position break, loss limit, time-sync fault) vs ticket (degradation).
  Alert rules live in the repo; every page has a runbook.
- **Fleet control**: HQ console for session orchestration (start/stop/pre-flight per
  site), config rollout with dual approval, kill-switch, and journal-backed audit of
  every operator action. All remote actions ride the authenticated control topic on
  `bridged`.
- **Local-ops mode — every site is operable with HQ unreachable [R2]**: HQ/WAN/
  bridge/bastion failure must not leave a site blind or uncontrollable, especially
  during single-engineer night sessions. Each site keeps: a local dashboard (same
  Grafana, site-local Prometheus), a local paging path, physical and logical kill
  controls, locally journaled pre-flight/EOD evidence, buffered audit export
  (shipped on reconnect), and read-only inspection tools. The action policy under
  partition is explicit: **allowed locally** — halt, cancel, flatten, takeover with
  on-site confirmation, session shutdown; **fail closed** — limit increases, config
  rollout, strategy promotion. Reconnection to HQ replays no stale commands
  (control-command versioning, §10.3).
- **Runbooks**: takeover drill, gap-storm response, counter disconnect, position
  break, regulator inquiry, DC evacuation, **HQ/WAN partition (local-ops entry and
  exit) [R2]**. Written as executable checklists; drilled (§16).
- On-call: two-person rotation across the team; night sessions covered by the
  on-call engineer with automated pre-flight and reduced-universe defaults.

---

## 15. Security

- Trading networks physically isolated from internet; management access via HQ
  bastion, hardware-key MFA, per-engineer certificates; break-glass local console
  procedure per site.
- Signed, reproducible builds; deploy artifacts hashed and journaled; config changes
  dual-approved and journaled (risk-limit changes are compliance records).
- Vendor SDK binaries pinned and checksummed; counters' required client-info
  (看穿式) identifiers managed as sealed secrets.
- Least privilege: strategies cannot reach gateways except through riskgate (enforced
  by bus topology, not convention).
- **Operator RBAC [R2]**: an explicit role matrix for deploy, config change, limit
  change, strategy promotion, halt, flatten, replay, production-data access, and
  vendor SDK upgrade — under normal, degraded (local-ops), and break-glass
  conditions. Quorum rules (which actions need two people), expiry for emergency
  privileges, key rotation, certificate revocation, and periodic audit review are
  specified alongside; many §10 controls are operational, and this is where their
  authorization lives.

---

## 16. Testing strategy

| Layer | Practice |
|---|---|
| Unit | Standard C++23 unit tests; codecs fuzzed; book engines property-tested against reference implementations |
| Golden replay | Nightly: replay canonical days through the full stack; diff every derived artifact (books, signals, orders, PnL) against goldens byte-for-byte — the determinism contract makes this possible and cheap |
| Performance regression | Nightly bench suite (lab `bench.hpp` heritage) + weekly wire-tap latency report; p99 regressions block release |
| Simulation soak | New builds run full sim sessions (incl. auction transitions, halts, night sessions) before eligibility for prod |
| Chaos/failover | Monthly: kill -9 each component live-in-sim; quarterly full standby takeover drill at each site; kill-switch drill monthly on test account |
| Fault injection **[R1]** | Every specified recovery boundary has an injected-failure test: journal-writer death, failure between SDK handoff and journal durability, duplicate/replayed exec reports, snapshot races, replay-to-live handoff, loss-intolerant ring lap, riskgate restart with armed kill state, reference-override mid-replay |
| Capacity acceptance **[R2]** | Per-site, pre-go-live and on change: ≥ 2× observed-peak MD + order-burst replay proving no capture loss and no hot-path backpressure (§8.6) |
| Conformance | Counter/exchange test environments for every gateway before go-live and after every SDK upgrade; conformance evidence archived (feeds HFT filing materials) |
| Reconciliation | Daily automated 对账 as a test with alarms, not a manual chore |

---

## 17. Build plan and phasing

Team assumption: 5 engineers — E1 data platform, E2 trading core/gateways, E3
risk/OMS/ops, E4 research platform, E5 quant/strategy; everyone shares on-call.

### Phase 0 — Backbone (M0–M3)

- seqbus-derived bus + journal + codecs + telemetry skeleton; `hft::clock`;
  replay harness; CI with golden-replay from day one.
- CTP gateway + session layer against SimNow; SHFE feed handler against recorded
  data; riskgate v1 (checks 1–4, 7, 11, 12) with the reservation ledger (§10.1);
  minimal omsd/pnld.
- Port/build: Linux (GCC/Clang) becomes the primary target; Windows/MSVC retained as
  dev convenience where cheap.
- HQ: warehouse schema, vendor-data purchase + normalization pipeline started.
- **Written design artifacts gate this phase [R1][R2]**: the CTP order-state
  contract (§9.4), the riskgate authority/reservation design (§10.1), and the
  journal durability/recovery semantics (§7.2) are reviewed documents before the
  corresponding code is considered done.
- **Exit**: paper-trading a toy strategy on SimNow through the full stack, replayable
  and byte-deterministic; CTP contract conformance tests green against SimNow.

### Phase 1 — First blood: SHFE/INE live (M3–M6)

Deliberately narrow **[R1][R2]**: one venue, one account, one gateway, one feed
type, one conservative strategy, and the safe control plane — breadth waits.

- Colo contract + hardware install (trading, capture, support, time); FCM onboarding;
  程序化 filing; conformance tests.
- Full riskgate battery; compliance engine v1; pre-flight/EOD automation; capture;
  night-session ops; local-ops mode (§14); cross-site risk-budget lease design
  implemented against sim (§9.3) even though no second site exists yet.
- **Parallel offline workstream — Phase 2A (stock data) starts here [R2]**: SSE feed
  decode, sequence handling, book build, auction phases, halts, and gap quarantine
  against purchased/recorded data. No stock trading; discovering stock-feed
  complexity at M6 would make the flagship schedule brittle.
- Go-live with one conservative strategy at minimum size; run weeks; harvest fill
  data → fill-model calibration; latency baseline published.
- **Exit** (expanded **[R1][R2]**): stable live operation; recovery drill passed in a
  production window; reconciliation clean 20 consecutive sessions; fault-injection
  suite green (gateway, journal, process, host failures with reconciliation before
  re-enable); per-feed gap-recovery conformance evidence; capacity acceptance test at
  ≥ 2× peak; site kill/restart drill including riskgate restart with persisted kill
  state; measured latency baseline on production hardware + selected SDK; closed
  verification register for Phase-1 assumptions (§21); an independent operational
  readiness review (the engineer *not* on the build reviews the runbooks cold); and
  operating-burden metrics inside agreed bounds (manual interventions/week, alert
  noise, on-call load) **[R2]**.

### Phase 2 — The flagship: futures–spot arb (M6–M12), in three gates [R2]

v0.1 compressed too many first-time systems into one block; v0.2 splits it into
promotable sub-phases, each with its own exit. 2A begins during Phase 1.

**2A — Stock data correctness** (offline/sim; starts in Phase 1)
- SSE 逐笔 feed handler, sequence handling, order-book engine, auction phase machine,
  halts, gap quarantine — replayed from purchased and captured data; book-vs-snapshot
  divergence checks (§8.2) proven on real data.
- **Exit**: SSE books rebuilt from vendor + recorded data with divergence rate below
  threshold across N full days, including auction transitions and halt days.

**2B — Stock trading correctness**
- SH-EQ site (broker + ATP counter); broker UAT; ATP order-state contract (§9.4);
  order lifecycle; T+1 sellable ledger; stock fees; PBU/account mapping;
  reconciliation; stock-specific risk checks.
- **Exit**: T+0 alpha pilot on SSE inventory at small size, clean reconciliation and
  clean risk-contract adversarial tests.

**2C — Cross-site arb**
- SH-CFFEX site (飞马/CTP) + Shanghai metro dark fiber; `bridged`; arb engine;
  risk-budget leases live (§9.3); CFFEX arb-quota application **[gates sizing]**;
  stale-state policy and hedge workflow under injected bridge loss.
- **Exit**: IF/IH/IC/IM vs SSE ETF/basket arb live within exposure bounds; unhedged-
  exposure incidents = 0 over the canary window; backtest-vs-live slippage within
  stated tier-3 bounds (§13.2); research loop (warehouse → backtest → promote)
  exercised end-to-end.

Backtester v1 on purchased history (stock queue-model + futures fill-model with
sensitivity reporting) lands across 2A–2B.

**Expansion gates carry operating metrics as well as correctness [R2]**: reject
rate, fill-model error, slippage vs simulation, unhedged-exposure incidents, manual
intervention count, alert quality, on-call load, false kill-switch triggers, data
loss, replay reproducibility, compliance report generation time — a phase does not
expand the footprint while the team is saturated operating the current one.

### Phase 3 — Breadth (M12–M18)

- SZ-EQ site + SH–SZ premium line; cross-market ETF arb incl. PD creation/redemption
  slow path; DCE + CZCE sites; options data ingestion at full scale; warm standby at
  every site; compliance engine to full 异常交易 monitor coverage.

### Phase 4 — Scale and optionality (M18–24)

- GFEX; strategy breadth; FPGA evaluation with real measured budgets; options
  strategies groundwork (`quotd` revival) if MM becomes a business goal.

---

## 18. Cost lines (informational)

Budget is not a constraint, but these are the recurring lines to expect: colocation
cabinets ×6–7 venues; premium long-haul + metro fiber; counter seat/license fees
(ATP-class stock counters and dedicated CTP seats are significant); market-data
license fees (SSE/SZSE L2 redistribution-grade); vendor historical data (one-time +
updates); hardware refresh cycle; broker/FCM commitments. A live cost sheet is kept
alongside vendor contracts, outside this document.

**"Budget is not a constraint" does not mean procurement is not a dependency [R2].**
Vendor contracting and physical delivery are schedule-critical: cross-connects, CTP
seat configuration, ATP counter access, market-data licenses, GNSS antenna approval,
and historical-L2 licensing can dominate the real timeline. A **procurement and
contract dependency register** is kept with the cost sheet: item, lead time, owner,
fallback vendor, required technical evidence, and which phase it blocks. Long-lead
items are ordered against the phase plan, not when the code is ready.

---

## 19. Risks and mitigations

| Risk | Mitigation |
|---|---|
| **Scope vs team**: 4 asset classes / 7 sites vs 5 engineers | Strict phasing (§17); identical per-site stacks; buy non-core (monitoring, data); phase gates are real — Phase N+1 does not start until exit criteria pass |
| Counter throughput/flow-control ceilings (seat orders/sec) | Measured in conformance; riskgate token buckets enforce below ceiling; multiple seats where justified **[verify FCM options]** |
| Regulatory drift (HFT differential fees, cancel fees, new thresholds) | Compliance engine thresholds are config, not code; counters mirror exchange accounting so impact is quantifiable same-day |
| SSE/SZSE feed evolution (formats, catalogs change) | Feed handlers are versioned plugins; raw capture guarantees re-decode; conformance re-run on venue notices |
| Cross-site line failure | Dual diverse routes; arb engine degrades to local-only mode (halts new legging, manages existing exposure with local hedges) |
| Fill-model error (futures snapshot regime) | Continuous calibration from live fills; backtest-vs-live slippage tracked per strategy with automatic demotion thresholds |
| Position/order state divergence | Counter is source of truth; continuous reconciliation; breaks halt the affected account automatically |
| Key-person risk (5 people) | Determinism + runbooks + golden replays make systems inspectable; no component owned by memory alone; docs-as-code in repo |
| Vendor SDK defects (closed-source counters) | Boundary confinement (§9.4); capture-based dispute evidence; conformance suite re-run on every SDK update |

---

## 20. Lab-to-platform component mapping

The `C:\work\hft` labs are deliberate seeds; the platform is their graduation:

| Lab | Platform component (§) |
|---|---|
| `seqbus` | Sequenced shm bus + journal (§7.1–7.2) |
| `wirecodec` | Codec/IDL layer (§7.4) |
| `feedarb` | A/B feed arbitration (§8.1) |
| `ticklab` | Book engines + derived data (§8.2, §8.4) |
| `tick2trade` | Hot-path skeleton and latency budget methodology (§12) |
| `sigcalc` | Signal graph (§9.2) |
| `sord` | Multi-leg arb/execution engine (§9.3) |
| `sesslink` | Session layer under gateways (§9.4) |
| `omsd` | Order-state keeper (§9.5) |
| `pnld` | Positions/PnL (§9.5) |
| `symtab` | Instrument master (§9.6) |
| `riskgate` | In-line pre-trade risk (§10.1) |
| `matchd` | Backtest/sim exchange (§13.2–13.3) |
| `quotd` | Future options-MM quoting (non-goal N1) |
| `hft::common` (incl. `bench.hpp`) | `hft::clock`, bench/regression suite (§12.2, §16) |

---

## 21. Open items to verify during onboarding

**These items live in a verification register, not a list [R1][R2]**: each entry
carries an owner, the authoritative counterparty/source, the evidence standard
(written confirmation where it changes the architecture), an expiry/re-verify date,
the components it affects, and the phase gate it blocks. Before Phase 1 live, the
register must hold written FCM confirmation of API/session semantics, seat flow
control, market-data recovery mechanics, legal retention requirements, mandated test
procedures, and emergency procedures. The register is closed (no open red items for
the phase) at each phase exit.

1. Broker selection: ATP counter latency SLA, hosting availability at SSE/SZSE,
   trading-unit arrangements, PD channel for ETF creation/redemption.
2. FCM selection: CTP seat configuration and flow-control settings, 飞马/易盛
   availability, hosting racks at each futures venue.
3. Current market-data product catalogs and licensing per venue (SSE merged 逐笔
   status; futures five-level feed products and cadences).
4. CFFEX speculative intraday caps, 平今 fee schedule, arb/hedge quota process.
5. HFT differential-fee schedule status and exact order-tagging mechanics per venue.
6. Cross-market ETF same-day mechanics per target fund.
7. Achievable one-way latencies: Shanghai metro dark fiber; SH–SZ premium routes;
   provider selection.
8. Time-sync: GNSS antenna feasibility per colo site; PTP support in venue switches.
9. Low-latency switch and NIC procurement availability in-country.
10. Historical L2 vendor selection (coverage, 逐笔 completeness, licensing for
    internal derived storage).

---

## 22. Glossary

| Term | Meaning |
|---|---|
| 逐笔委托 / 逐笔成交 | Order-by-order / trade-by-trade L2 feed messages |
| 底仓 | Base inventory held overnight enabling intraday T+0 round trips under T+1 |
| 平今 / 平昨 | Closing today's vs prior days' futures positions (different fees) |
| 看穿式监管 | See-through supervision: client-system identification at the API layer |
| 程序化交易报备 | Programmatic-trading filing via broker/FCM before trading |
| 异常交易 | Exchange-defined abnormal trading behaviors catalog |
| 交易单元 / PBU | Exchange participant business unit through which orders are attributed |
| CTP二席 | Dedicated low-latency secondary CTP seat at an FCM |
| 申赎清单 | Daily ETF creation/redemption basket file |
| 对账 | End-of-day reconciliation against broker/FCM statements |
| 一级/二级市场 | Primary (creation/redemption) / secondary (exchange) market |
| SimNow | CTP public simulation environment |

---

## 23. Review changelog (v0.1 → v0.2)

Three independent model reviews of v0.1, all in this directory. v0.1's requested
focus questions were answered: (a) Phase-2 scope → split into 2A/2B/2C with 2A
pulled into Phase 1 [R2]; (b) riskgate completeness → reframed from checklist to
risk contract with reservations [R1][R2][R3]; (c) standby automation → automate
detection/halt/evidence early, keep re-enable human-confirmed [R2].

### Adopted

| Tag | Source | Findings incorporated (section) |
|---|---|---|
| **[R1]** | `ARCHITECTURE_REVIEW.md` | Riskgate as linearizable reservation authority (§10.1); cross-site caps via expiring local budget leases (§9.3); journal durability watermarks, outbound-intent-before-SDK, exec dedup, reconciliation as trading barrier (§7.2); snapshots cannot repair 逐笔 gaps — per-feed recovery spec (§8.1); latency table reframed as measured Phase-1 targets with exact endpoints (§12.1); signed/versioned/durable control commands, persisted kill state, partition semantics, flatten as workflow (§10.3); capture retention classes (§8.6); deterministic execution ABI + parity tiers (§13.2); verification register (§21); Phase-1 exit hardening incl. fault-injection evidence and ops readiness review (§16, §17) |
| **[R2]** | `review_5_5.md` | Phase 2 split into 2A/2B/2C, 2A starts during Phase 1 (§17); CTP order-state contract as Phase 0 exit artifact (§9.4, §17); risk contract per check with adversarial tests (§10.1); intraday reference-data override path (§9.6); local-ops mode under HQ/WAN loss (§14); standby automation boundary (§11.2); compliance evidence model beyond the journal (§10.2); parity tiers with promotion thresholds (§13.2); per-site capture/capacity acceptance tests (§8.6, §16); operator RBAC (§15); operational metrics in phase gates (§17); procurement dependency register (§18) |
| **[R3]** | `review_gemini.md` | Explicit ring overflow policy — loss-tolerant vs loss-intolerant topics, fail-closed halt (§7.1); NUMA/CCX single-domain hot-path pinning, single-socket mandatory, boot-time TSC skew gate (§12.3); riskgate single-writer concurrency (§10.1, jointly with R1); futures fill-model queue-decay + fill-sensitivity reporting (§13.2); SSE book-reconstruction state machine + snapshot cross-check quarantine (§4.2, §8.2); trading-day vs calendar-day transition protocol (§11.3) |

### Rejected or amended

- **[R1] "pervasive character-encoding corruption / mojibake"** — does not
  reproduce: the source file is valid UTF-8 (byte-level check: no BOM issues, zero
  invalid sequences); the reviewer's toolchain likely decoded it with a legacy
  codepage. Two genuine one-word slips existed and are fixed in v0.2 ("книги" →
  "books" §8.2; "路由" → "routes" §6.1). A CI check asserting UTF-8 cleanliness +
  no non-CJK/non-ASCII scripts is still worth adding and is adopted.
- **[R3] "SSE merged 逐笔 does not publish cancels"** — stated as fact, but this
  depends on the current SSE feed version; kept as **[verify]** (§4.2). The
  mitigation (formal reconstruction state machine + snapshot cross-check) is adopted
  regardless of the answer.
- **[R3] "minimum 16M slots" and "10 ns TSC skew"** — the principles (size rings
  from burst math; gate startup on TSC skew) are adopted; the specific numbers are
  replaced by measurement-derived configuration (§7.1, §12.3).

*End of v0.2.*
