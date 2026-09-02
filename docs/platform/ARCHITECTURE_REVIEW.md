# Architecture Review: China Multi-Venue HFT Platform

**Reviewed document:** ARCHITECTURE.md v0.1, dated 2026-07-11  
**Scope:** Architecture, operational safety, and implementation readiness. This review does not independently verify exchange rules, regulation, or vendor claims.

## Recommendation

**Do not approve Phase 1 live trading yet.** The direction is good: site autonomy, an in-line gate, raw capture, and reconciliation are the right foundations. The design needs a precise order-authority, recovery, and data-integrity specification before it can support the claimed safety properties.

## Findings

### Critical: Risk approvals can use stale state

omsd and pnld are off-path bus consumers, while riskgate reads their shared-memory mirrors ([ARCHITECTURE.md](ARCHITECTURE.md:606)). Yet riskgate must enforce position, sellable inventory, margin, cancel-ratio, and self-trade limits for every order ([ARCHITECTURE.md](ARCHITECTURE.md:626)). A consumer can lag accepted orders, fills, cancels, or remote updates. Concurrent strategies can therefore be approved against the same old state and breach a hard limit.

Define a single writer for every state that affects approval. riskgate needs synchronous, linearizable reservations for quantity/notional, cash or margin, sellable inventory, rate capacity, and resting-order ownership. Execution reports must release or amend reservations idempotently. Projections may remain off-path, but cannot be the authority used to approve an order. Define a fail-closed overload or inconsistency policy.

### Critical: A local risk gate cannot enforce a global cross-site cap

The design states that remote inputs are stale by construction ([ARCHITECTURE.md](ARCHITECTURE.md:283)) while requiring each local riskgate to enforce a global unhedged-exposure limit ([ARCHITECTURE.md](ARCHITECTURE.md:573)). During a partition or simultaneous fills, no site can know the current firm-wide exposure from asynchronous replicas.

Use pre-allocated, expiring local risk budgets by pair, account, and site. A site must trade only within its durable allocation; a coordinator may rebalance allocation but must not be in the approval path. Specify lease expiry, idempotency, partition behavior, and treatment of existing exposure on bridge loss. Design this in Phase 1 even though the first cross-site strategy is Phase 2.

### Critical: Journal semantics do not yet support deterministic recovery

The document claims a deterministic canonical stream ([ARCHITECTURE.md](ARCHITECTURE.md:432)) and recovery through snapshot plus journal replay ([ARCHITECTURE.md](ARCHITECTURE.md:683)), but journal persistence is an asynchronous drain ([ARCHITECTURE.md](ARCHITECTURE.md:441)). It does not define the boundary for an order accepted by the gateway/counter but absent from durable journal state, snapshot races, or the gap between replay completion and live subscription.

Specify durable journal watermarks; snapshot sequence/checksum; atomic replay-to-live handoff; persisted outbound intent before SDK submission; inbound execution deduplication; and exact behavior on journal-writer or host failure. Reconciliation with the counter must be a trading barrier. Fault-injection must cover every boundary, including failure between SDK handoff and journal durability.

### High: Snapshot recovery cannot repair an order-by-order gap

The feed design says to use A/B arbitration and request/rebuild via snapshot recovery ([ARCHITECTURE.md](ARCHITECTURE.md:486)). A periodic stock snapshot cannot reconstruct omitted orders, queue state, or trades. It may only establish a fresh baseline if the venue protocol permits it.

Define per feed: sequence domain, A/B deduplication, retransmission or snapshot source, checksum policy, and blocked trading scope. On an unrecoverable order-by-order gap, discard affected book and queue state, obtain an authoritative baseline, then re-enable after a specified synchronization condition. Treat capture loss and decoder errors as separate states. Make this conformance evidence a Phase-1 exit gate.

### High: Latency targets are premature and omit real boundaries

The p99 table assigns 200 ns to the full risk battery and 800 ns to order encoding and SDK handoff ([ARCHITECTURE.md](ARCHITECTURE.md:724)). The gateway owns vendor SDK outbound calls on a pinned session thread ([ARCHITECTURE.md](ARCHITECTURE.md:588)), and the path crosses several processes/rings. Cache misses, queueing, SDK behavior, and reservation/self-trade work are not represented.

Treat 1.5/5 microseconds as a measured Phase-1 target, not a commitment. Publish exact interval endpoints, CPU topology, payload, load, and whether SDK invocation is included. Establish separate SLOs for decision, gateway enqueue, and counter acknowledgement, and gate rollout on reproducible benchmarks with the selected counter SDK and bare-metal hardware.

### High: Kill/control state is neither durable nor partition-safe

Kill state is a shared-memory flag ([ARCHITECTURE.md](ARCHITECTURE.md:643)), while remote operator actions ride bridged ([ARCHITECTURE.md](ARCHITECTURE.md:817)). The design does not define process restart, stale or replayed commands, bridge partition, or conflicting HQ/site commands. A firm-global switch cannot be an instantaneous guarantee across disconnected sites.

Define a signed control command with command ID, issuer, scope, monotonic version, expiry, acknowledgement, and durable audit. Persist the latest restrictive local state before activation; a restarted riskgate must load it before opening the order path. State that global halt is eventually propagated across partitions, and retain independently operable local/site controls. Treat flatten as a constrained execution workflow, not a flag write.

### High: Capture retention and capacity are ambiguous

The design requires hardware-timestamped capture on every relevant port, retained at HQ for five years ([ARCHITECTURE.md](ARCHITECTURE.md:530)). It estimates 100-200 GB/day uncompressed ([ARCHITECTURE.md](ARCHITECTURE.md:511)) and specifies 60 TB per capture server ([ARCHITECTURE.md](ARCHITECTURE.md:387)), without identifying whether volume is per feed, site, or firm or whether five-year retention is raw pcap, journal, or derived audit records.

Define retention classes: immutable audit journal for the required period, raw pcap for a defined forensic period, and incident-held segments. Size storage and WAN transfer per site using measured A/B feed and counter traffic. Define archival encryption, integrity chains, access logging, and restoration tests.

### Medium: Research-production parity is overstated

The determinism contract excludes clocks, allocation, syscalls, and global state, but omits PRNG source, floating-point mode, timer ordering, configuration identity, external features, and topic ordering. The backtester promises the same binary on vendor history ([ARCHITECTURE.md](ARCHITECTURE.md:776)), which does not necessarily contain production timing or complete sequencing.

Specify a deterministic execution ABI: event-ordering rule, timer source, PRNG seed, compiler/floating-point settings, and config/reference-data hashes. Report parity in tiers: deterministic replay of internal capture, historical simulation with stated limitations, and live-fill validation. Do not treat vendor history as byte-equivalent production input.

### Medium: Regulatory and vendor assumptions need ownership and release gates

The document leaves material matters marked [verify], including data products, FCM throughput, HFT thresholds, and CFFEX quota ([ARCHITECTURE.md](ARCHITECTURE.md:235)). The onboarding list has no owner, evidence standard, deadline, or go/no-go gate, even though several assumptions change the architecture.

Create a verification register with owner, authoritative source/counterparty, expiry date, affected components, and phase gate. Before Phase 1, obtain written confirmation of selected FCM API/session semantics, flow control, market-data recovery, legal retention, test requirements, and emergency procedures.

### Medium: Five-engineer scope and Phase-1 readiness are optimistic

Five people are assigned the bus, journal, feeds, gateway, risk, OMS/PnL, control plane, capture/archive, research, backtester, compliance, and night-session operations ([ARCHITECTURE.md](ARCHITECTURE.md:858)). The Phase-1 exits omit an independent operational readiness review, vendor incident drill, and measured capacity/data-loss test.

Limit Phase 1 to one venue, account, gateway, feed type, strategy, and safe control plane. Buy or defer non-differentiating systems where practical. Add ownership, on-call coverage, and phase exits for fault injection, reconciliation, recovery, feed gaps, capacity, and operator drills before a second venue.

### Medium: The source document has character-encoding corruption

ARCHITECTURE.md contains pervasive mojibake in prose, diagrams, Chinese terms, and units from the opening line. This makes operational and regulatory language ambiguous and prevents reliable rendering and search.

Restore the authoritative source in UTF-8, add an encoding check and rendered-document check in CI, and correct it before treating the document as a controlled design or compliance artifact.

## Approval Conditions

Approve Phase 0 simulation only after the risk-authority and journal-recovery designs are written and reviewed. Approve Phase 1 live trading when evidence exists for:

1. Tested linearizable local risk reservations, including fill/cancel recovery.
2. Fault-injection results for gateway, journal, process, and host failures, with reconciliation before re-enable.
3. Per-feed gap-recovery conformance and explicit trade-quarantine rules.
4. Measured end-to-end latency and capacity on production-intent hardware and selected SDK.
5. Signed control-plane/kill-state design plus successful site kill/restart drill.
6. Closed onboarding verification register with retained vendor/regulatory evidence.
7. Corrected UTF-8 source and operational runbook review.

## Positive Elements

- Per-site autonomy and explicit staleness suit the physically federated topology.
- Capture, reconciliation, and compliance are correctly treated as platform primitives.
- ETF primary-market work is correctly separated from the microsecond execution path.
- Starting with SHFE/INE is sensible if the safety primitives are completed rather than deferred.

