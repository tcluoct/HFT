# Architecture Review 5/5: Implementation Readiness

**Reviewed document:** `ARCHITECTURE.md` v0.1, dated 2026-07-11  
**Review date:** 2026-07-11  
**Scope:** Architecture readiness, phase scope, venue-specific implementation gaps, and operational controls. This review does not independently verify exchange rules, regulatory thresholds, broker/FCM claims, vendor latency claims, or market-data product catalogs.

## Recommendation

**Do not approve the Phase 2 roadmap as written.** The platform direction is coherent, but the document compresses too many first-time systems into Phase 2 and still treats several venue-specific contracts as generic platform details. Approve Phase 0 design/build work only after the missing contracts below are written. Treat Phase 1 live trading as a later operational-readiness decision, not a direct consequence of finishing the backbone.

## Findings

### High: Phase 2 is too large to be the first stock-system delivery

Phase 2 combines a new CFFEX site, a new SSE stock site, metro fiber, `bridged`, the multi-leg arb engine, quota gating, purchased-history backtesting, an SSE order-by-order book, a T+1 inventory ledger, and the auction phase machine in one M6-M12 block ([ARCHITECTURE.md](ARCHITECTURE.md:883), [ARCHITECTURE.md](ARCHITECTURE.md:887)). The document correctly calls the SSE stock stack the largest single engineering item, but it does not break it into promotable milestones.

Split Phase 2 into explicit gates:

1. **2A - Stock data correctness:** SSE feed decode, sequence handling, book build, auction phases, halts, and gap quarantine replayed from purchased and captured data.
2. **2B - Stock trading correctness:** broker UAT, order lifecycle, T+1 sellable ledger, fees, PBU/account mapping, reconciliation, and stock-specific risk checks.
3. **2C - Cross-site arb:** bridge budgets, stale-state policy, CFFEX quota/limits, hedging workflow, and unhedged-exposure operations.

Start 2A in Phase 1 as an offline/sim workstream. Waiting until M6 to discover stock-feed or auction complexity makes the flagship Phase 2 schedule brittle.

### High: The Phase 1 CTP slice needs a concrete order-state contract

The gateway abstraction is `place / cancel / status` behind a single internal order API ([ARCHITECTURE.md](ARCHITECTURE.md:583), [ARCHITECTURE.md](ARCHITECTURE.md:584)). That is too thin for the first live futures slice. CTP-family behavior needs a documented state machine before code or conformance can be judged.

Specify at minimum: client order ID construction; broker/exchange order IDs; insert reject vs exchange reject; cancel-before-ack races; partial fill plus cancel races; reconnect query rules; duplicate callbacks; settlement confirmation; close-today vs close-yesterday flags; hedge/speculation flags; product-level order type support; exchange trading-day handling across night/day sessions; and idempotent journal mapping for every callback. This contract should be a Phase 0 exit artifact, not something discovered during FCM onboarding.

### High: The riskgate battery is a checklist, not yet a safety spec

The riskgate table lists useful checks ([ARCHITECTURE.md](ARCHITECTURE.md:626), [ARCHITECTURE.md](ARCHITECTURE.md:630)), but several required predicates are either implicit or missing: open-order reservations, order type and TIF legality per venue phase, product-level position caps, CFFEX speculative quota consumption, futures close-today fee/position constraints, broker/seat throttle behavior, options exercise/assignment constraints, ETF basket cash-substitute rules, short-sale/sellable-stock edge cases, self-trade scope across accounts/PBUs, and mandatory regulatory tags ([ARCHITECTURE.md](ARCHITECTURE.md:657)).

Turn the table into a risk contract: for each check, define the authoritative input, update event, reservation rule, failure mode, denial code, test fixture, and recovery behavior after reconnect or replay. A check is not complete until it has adversarial tests for stale input, duplicate events, and reject/fill races.

### High: Immutable daily reference data needs an intraday override path

The instrument master is compiled nightly into an immutable per-day snapshot loaded by every process ([ARCHITECTURE.md](ARCHITECTURE.md:617), [ARCHITECTURE.md](ARCHITECTURE.md:618)), and pre-flight validates that snapshot ([ARCHITECTURE.md](ARCHITECTURE.md:702)). That is a good baseline but insufficient as the only model. Trading halts, resumed products, emergency margin/fee changes, broker restrictions, product status changes, and corrected reference files can arrive intraday.

Define a signed intraday override mechanism with versioning, dual approval where appropriate, immediate riskgate visibility, journaled activation, and deterministic replay semantics. If a field cannot be safely changed live, the document should say which instruments/accounts halt until the next clean snapshot.

### High: Local operations during HQ or WAN loss are underspecified

Telemetry exports to Prometheus at HQ and fleet control lives at the HQ console ([ARCHITECTURE.md](ARCHITECTURE.md:808), [ARCHITECTURE.md](ARCHITECTURE.md:815)). Remote actions ride `bridged` ([ARCHITECTURE.md](ARCHITECTURE.md:817)). This leaves a gap for site operation during HQ, WAN, bridge, or bastion failure, especially during commodity night sessions covered by one on-call engineer ([ARCHITECTURE.md](ARCHITECTURE.md:822)).

Each site needs a survivable local-ops mode: local dashboard, local paging path, physical and logical kill controls, local pre-flight/EOD evidence, buffered audit export, local read-only inspection tools, and a procedure for reconnecting to HQ without replaying stale commands. Define which actions remain allowed when HQ is unreachable and which actions fail closed.

### High: Warm-standby automation should be limited at first

The takeover design is operator-initiated or watchdog-proposed, with human confirmation in v1 ([ARCHITECTURE.md](ARCHITECTURE.md:691)). That is the right default. Earlier automation should cover detection, local halt, alerting, evidence capture, and possibly fencing proposal - not automatic trading re-enable.

Do not automate full standby promotion until repeated sim and production-window drills show clean behavior for active host death, journal lag, gateway disconnect, counter ambiguity, split-brain management network, and replay-to-live handoff. Human-confirmed re-enable should remain the v1 rule; automatic halt on hard faults should be built earlier.

### Medium: Compliance evidence is broader than the journal

The document says regulatory artifacts are producible from the journal alone ([ARCHITECTURE.md](ARCHITECTURE.md:91)) and later from journal plus config history ([ARCHITECTURE.md](ARCHITECTURE.md:653), [ARCHITECTURE.md](ARCHITECTURE.md:656)). In practice, the evidence package also includes filings, broker/FCM acknowledgements, strategy classifications, test reports, emergency plans, operator approvals, build provenance, deployment hashes, data-retention proof, and incident records.

Create an evidence model now: artifact type, source of truth, retention period, signer/approver, regeneration command, and regulator-facing export format. This should be part of the compliance engine's data model, not a documentation afterthought.

### Medium: Research-production parity needs tiers and promotion thresholds

The backtester runs the same strategy binaries on vendor history and simulated exchange layers ([ARCHITECTURE.md](ARCHITECTURE.md:776)), with calibrated futures fill models ([ARCHITECTURE.md](ARCHITECTURE.md:781)). That is directionally right, but vendor history, internal capture replay, and live trading have different fidelity.

Define parity tiers:

1. **Replay parity:** internal capture/journal replay reproduces production decisions.
2. **Historical simulation:** vendor data supports research with documented missing fields and timing limitations.
3. **Live validation:** fill probability, slippage, rejects, queue estimates, and latency are compared against production.

Strategy promotion should require explicit error bounds for the tier it depends on. "Same binary" is necessary but not enough.

### Medium: Capture and capacity sizing need per-site acceptance tests

Capacity targets are estimates to validate in Phase 1-2 ([ARCHITECTURE.md](ARCHITECTURE.md:503)), and capture is retained locally and at HQ ([ARCHITECTURE.md](ARCHITECTURE.md:529), [ARCHITECTURE.md](ARCHITECTURE.md:530)). Before procurement and go-live, turn this into a per-site budget: A/B feed rate, counter traffic, pcap write rate, compression ratio, local retention, WAN shipment window, replay restoration time, and packet-loss alarm threshold.

The acceptance test should replay or synthesize at least 2x observed peak for market data plus order bursts while proving no capture loss, no journal backpressure into the hot path, and successful restore from HQ archive.

### Medium: Security controls need operator-level authorization detail

The security section has good primitives - isolated networks, MFA, signed builds, pinned SDKs, and bus-enforced gateway access ([ARCHITECTURE.md](ARCHITECTURE.md:829), [ARCHITECTURE.md](ARCHITECTURE.md:832), [ARCHITECTURE.md](ARCHITECTURE.md:836)). It does not yet define who can do what under normal, degraded, and break-glass conditions.

Add RBAC for deploy, config change, limit change, strategy promotion, halt, flatten, replay, production data access, and vendor SDK upgrade. Define quorum rules, expiry for emergency privileges, key rotation, certificate revocation, and audit review. This matters because many controls are operational, not purely technical.

### Medium: Phase gates omit business and operational acceptance metrics

Phase 1 exits on stable operation, a recovery drill, and 20 clean reconciliations ([ARCHITECTURE.md](ARCHITECTURE.md:880)). Phase 2 exits on live arb and a T0 alpha canary ([ARCHITECTURE.md](ARCHITECTURE.md:891)). These are necessary but not sufficient to decide whether to expand.

Add measured gates: reject rate, fill-model error, slippage vs simulation, unhedged-exposure incidents, manual intervention count, alert quality, on-call load, false kill-switch triggers, data loss, replay reproducibility, and compliance report generation time. Expansion should require both technical correctness and an acceptable operating burden for a five-engineer team.

### Medium: "Budget is not a constraint" hides schedule risk

The cost section lists colocation, fiber, counter seats, market data, history, and hardware but treats them as awareness-only ([ARCHITECTURE.md](ARCHITECTURE.md:909)). For this platform, vendor contracting and physical delivery are schedule-critical architecture dependencies, not just costs.

Add a procurement and contract dependency register with lead time, owner, fallback vendor, required technical evidence, and phase blocked. Cross-connects, CTP seat configuration, ATP counter access, market-data licenses, GNSS antenna approval, and historical L2 licensing can dominate the real timeline.

## Answers to Requested Review Focus

**Phase 2 scope:** Too broad as a single M6-M12 phase. Split stock data, stock trading, and cross-site arb into separate gates; begin offline SSE stock-stack work during Phase 1.

**Riskgate completeness:** The list is a useful starting point but not complete until every venue-specific predicate has an authority, reservation rule, failure mode, denial code, and adversarial test.

**Warm-standby automation:** Automate fault detection, halt, alerting, and evidence capture early. Keep promotion and trading re-enable human-confirmed in v1.

## Positive Elements

- The site-federated shape matches the physical market-access constraint.
- Treating stale remote state as explicit input is the right mental model for cross-site trading.
- Separating ETF creation/redemption from the microsecond path is correct.
- Starting live work with a single SHFE/INE futures slice is sensible if the CTP contract and local operations are specified before go-live.
- The lab-to-platform mapping gives the team a useful implementation starting point.

## Approval Conditions

Before approving Phase 1 live trading, require:

1. A CTP/order-state contract with conformance tests and replay mapping.
2. A riskgate contract with venue-specific checks, reservations, and race tests.
3. Intraday reference-data override semantics and fail-closed rules.
4. Local-ops mode for HQ/WAN loss, including kill controls and audit buffering.
5. Standby drill results covering halt, fence, replay, reconciliation, and human re-enable.
6. Per-site capture/capacity acceptance tests at burst load.
7. A verification/evidence register for regulatory, broker, FCM, market-data, and procurement assumptions.
