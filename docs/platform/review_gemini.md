# Architecture Review: Latency, Determinism, and Concurrency Controls

**Reviewed document:** `ARCHITECTURE.md` v0.1, dated 2026-07-11  
**Reviewer:** Gemini (Antigravity)  
**Scope:** Latency engineering, systems programming constraints, memory models, backtest fidelity, and concurrency safety.

---

## Recommendation

**Approve Phase 0 (Backbone) and Phase 1 Simulation only.** Do not approve Phase 1 live trading or the Phase 2 roadmap. The architecture outlines a solid foundation (leg-local autonomy, sequenced shared-memory bus, and pre-trade risk gates), but it contains critical vulnerabilities in memory ring-buffer boundaries, NUMA-aware memory access, and lock-free concurrency in the risk layer that must be resolved before live capital is at risk.

---

## Findings

### Critical: Memory Ring Buffer Overflow and Backpressure Policy

The `seqbus` design specifies single-producer, lock-free rings in hugepage shared memory ([ARCHITECTURE.md](ARCHITECTURE.md#L425)). During peak bursts (e.g., market open or resumptions exceeding 1M msg/s), queue depth will spike. The document does not define the behavior when a ring buffer fills up:
1. **If it overwrites (ring wrap):** Downstream consumers (strategies, risk, OMS) will silently miss market data updates or execution reports, breaking determinism and corrupting the position ledger.
2. **If it blocks (backpressure):** The producer (e.g., `feedhand`) will busy-wait or block, cascading latency delays up to the network interface card (NIC) ring buffers, causing packet drops at the OS/NIC level.

**Mitigation:** 
- Explicitly partition topics into **loss-tolerant** (e.g., raw depth snapshots) and **loss-intolerant** (e.g., order intents, execution reports, risk controls).
- Define a fail-closed policy for loss-intolerant rings: if a ring fills, the producer must trigger an emergency local halt (kill switch) and refuse new transactions.
- Size all rings based on a worst-case 10-second burst at 2x peak capacity (minimum 16M slots for market data).

---

### Critical: NUMA Locality and Core-to-Core TSC Skew

The physical server specification chooses high-clock AMD EPYC / Intel Xeon processors ([ARCHITECTURE.md](ARCHITECTURE.md#L383)) and maps processes to different cores (e.g., Core 2 for feed RX, Core 3 for book+strategies, Core 5 for riskgate/gateway) ([ARCHITECTURE.md](ARCHITECTURE.md#L752)).
1. **NUMA Hop Penalty:** If Core 2 and Core 3 reside on different NUMA nodes (or different L3 cache domains on AMD Zen architectures), reading shared memory across the socket or CCX boundary will incur a 50–150 ns penalty per message. This invalidates the ≤ 150 ns riskgate budget and the ≤ 1.1 µs tick-to-trade target.
2. **TSC Drift:** Although modern CPUs support invariant TSC, multi-socket configurations (or deep power management states if misconfigured) can experience core-to-core TSC drift. This breaks the precision of `hft::clock` timestamps used for latency attribution.

**Mitigation:**
- Enforce strict NUMA-node pinning. The entire hot path (feed handler, book engine, strategy inline, riskgate, and gateway) must run on cores sharing the same physical NUMA node and L3 cache domain.
- Restrict hardware selection to single-socket bare-metal servers for trading.
- Implement a boot-time calibration utility that measures TSC offsets across all pinned trading cores and halts startup if any core-to-core skew exceeds 10 nanoseconds.

---

### High: Multi-Producer Concurrency in Risk Gate Reservations

The `riskgate` reads position and buying power mirrors in shared memory that are updated off-path by `omsd`/`pnld` ([ARCHITECTURE.md](ARCHITECTURE.md#L606)). When multiple strategy pods run concurrently on separate cores ([ARCHITECTURE.md#L330]), they may simultaneously submit orders that require the same risk limits (e.g., max notional or sellable inventory).
- If two strategies check the same shm position mirror concurrently, both might get approved, resulting in a limit breach (double allocation).
- If `riskgate` uses mutex locks to serialize checks, the hot-path latency will degrade significantly under load.

**Mitigation:**
- riskgate must be the **sole author** of resource reservations. It must use lock-free atomic reservations (e.g., `std::atomic::compare_exchange_strong`) on a dedicated reservation table in shared memory.
- Alternatively, run `riskgate` as a single-threaded executor on a dedicated core that processes all order intents sequentially.

---

### High: Futures Queue Position and Adverse Selection in Backtesting

The backtester relies on "calibrated fill models" for futures because exchanges only publish snapshots every 250–500 ms ([ARCHITECTURE.md](ARCHITECTURE.md#L181), [ARCHITECTURE.md](ARCHITECTURE.md#L781)). 
- If a backtest assumes FIFO queue priority based on snapshot status, it will suffer from massive adverse selection. In reality, we are filled when the market moves against us, and queue priority decays dynamically.

**Mitigation:**
- The simulator (`matchd`) must model queue-priority decay and fill probability as a function of the traded volume delta between snapshots, queue size, and estimated network latency.
- Backtest reports must output a "fill sensitivity" metric, showing how strategy performance degrades if fills are delayed by 1 to 5 ticks.

---

### High: SSE Merged 逐笔 Feed Book Reconstruction Gaps

SSE provides a merged 逐笔 feed containing both orders and trades, but unlike SZSE, it does not explicitly publish all cancel messages.
- Reconstructing the SSE order book requires inferring cancels from order modifications and execution volumes. If the reconstruction logic is incorrect, the book state will diverge from the exchange matching engine, causing strategies to trade on stale/corrupted depth.

**Mitigation:**
- Specify a formal state machine for SSE book reconstruction.
- Implement a real-time sanity check: cross-reference the reconstructed L2 top-of-book against the 10-level snapshots. If a divergence persists for more than 1 snapshot interval, quarantine the book and halt trading on that instrument.

---

### Medium: Night-to-Day Session Transitions and Trading Day State

Commodity night sessions cross midnight and belong to the next exchange trading day ([ARCHITECTURE.md](ARCHITECTURE.md#L192)). Clearing reports (对账) are received from brokers after the day session ends (15:00) but before the night session starts (21:00).
- The system must explicitly separate "Calendar Day" from "Exchange Trading Day" in its internal state. A failure to reset sequence numbers or clear daily risk limits during the evening transition will lead to rejected orders or stuck strategies.

**Mitigation:**
- Define a formal transition protocol for the 20:30 pre-flight sequence that archives the day session's journals, resets the compliance rate token buckets, and loads the new trading day's reference data.

---

## Approval Conditions

Before approving Phase 1 live trading, the following artifacts must be completed and verified:

1. **Queue Boundary Specification:** Define sizing, wrap-around protection, and fail-closed policies for all `seqbus` rings.
2. **NUMA Core Mapping:** A documented core-affinity mapping ensuring all hot-path components reside within a single NUMA node.
3. **SSE Book Engine Spec:** A formal specification of the SSE 逐笔 book-building algorithm.
4. **Local Survivability Runbook:** A plan for running and shutting down a site locally during a complete WAN partition from HQ.
