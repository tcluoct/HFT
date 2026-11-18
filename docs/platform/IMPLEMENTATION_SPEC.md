# Shared-account trading platform implementation spec

Status **Draft for review**

## 1. Scope and acceptance target

Several PM strategy instances trade concurrently on one broker account from one Linux server near the exchange. One upstream market-data connection serves the configured instrument universe. One account engine owns automated order entry and evaluates risk across all PMs on the account. Operators intervene through the broker terminal. PMs see only their own trades and positions in the web UI.

**Tick-to-trade must be below 50 µs at p99 on the production Linux host using the production counter SDK.** Measure from entry into the market-data receive callback for the triggering tick to successful return from the local order-submission SDK call. The interval includes strategy calculation, queueing, account risk, and local SDK time. Report counter acknowledgment latency separately. Use CTP for the initial functional integration and Windows for functional and failure-isolation testing.

The new topology replaces inline strategy execution and per-shard risk budgets for a shared account. Retain the static strategy libraries, market-data codecs and books, OMS, PnL, risk rules, session controls, journal, reconciliation, and backtest harness where their semantics carry over.

## 2. Process layout and ownership

```mermaid
flowchart LR
    Data[Market-data server] -->|One connection| Feed[Feed process]
    Feed -->|Read-only feed| A[Strategy runner A]microsecond-leve
    Feed -->|Read-only feed| B[Strategy runner B]
    Feed -->|Market state| Engine[Account engine]
    A -->|Order and cancel intents| Engine
    B -->|Order and cancel intents| Engine
    Engine -->|Private reports| A
    Engine -->|Private reports| B
    Engine <-->|Orders and account reports| Counter[Counter adapter]microsecond-leve
    Manual[Broker terminal] -->|Manual orders| Counter
    Engine -->|Sequenced audit and read model| Ops[Journal and web services]
```

| Component | Owns |
| --- | --- |
| Feed process | Market-data credentials, upstream session, subscription union, feed sequence, and book baseline publication. |
| Strategy runner | One configured strategy instance, its callback state, local market view, and attributed order and position state. Several instances may use the same strategy type. |
| Account engine | The automated trading session, canonical account event sequence, OMS, risk reservations, counter ID mapping, position attribution, and account controls. A single engine thread mutates this state. |
| Supervisor | Startup order, process and callback progress, alerts, and runner generations. |
| Journal and web services | Audit storage and authenticated account or PM read models. Trading processes do not wait for web requests. |

Deploy one runner executable linked against the existing strategy static libraries. Launch one process per configured instance and select its strategy type and ID at startup. Only the feed process and account engine hold their respective SDK credentials. Enforce one active automated execution owner for the account, including during restart.

## 3. IPC and the strategy SDK

Use process-shared, fixed-capacity IPC on Linux and Windows. Verify the existing ring's memory layout and publication semantics before reusing it across processes. Keep mapped messages versioned, fixed-width, and free of process-local pointers. Validate version, capacity, and publisher or runner generation on attach. Give each producer ownership of its write cursor and each consumer ownership of its read cursor.

| Channel | Contract |
| --- | --- |
| Feed to runners and account engine | One sequenced broadcast stream with independent reader cursors. Each market event carries the ingress timestamp and feed sequence. Publish a snapshot with its sequence so consumers can attach and resume consistently. |
| Runner to account engine | One bounded single-producer, single-consumer queue per instance. Each intent carries a request sequence, runner generation, action, order fields, and triggering feed sequence when applicable. Resolve the strategy and account from the authenticated runner binding. Poll queues in bounded batches so one runner cannot starve others or delay counter reports indefinitely. |
| Account engine to runner | One bounded queue per instance with request outcomes, execution reports, and attributed position updates. Include the canonical account sequence and support resync from an account snapshot plus subsequent events. |

Keep the CRTP strategy callbacks and replace their live `Site` context with a runner-local context. Book and position reads use local state. `place()` returns a worker-local correlation handle when enqueue succeeds, or an explicit enqueue failure. The engine allocates the canonical order ID and returns the mapping with the first outcome. A runner can issue a cancel using its handle before the first report arrives. The engine resolves it in request order or returns an explicit failure if the original intent was rejected. Each callback runs serially within its runner. Execution reports and risk denials arrive between callbacks.

For a market-triggered order, the account engine must have applied market state through the trigger sequence before checking risk. Bound that wait and reject an intent if its trigger or required risk data is stale. Size queues, CPU assignments, and staleness thresholds using the target workload.

## 4. Account risk and attribution

For each new intent, validate the runner identity and generation, session, instrument, and order fields. Evaluate venue rules and shared account limits using current positions, outstanding reservations, and the proposed order. On approval, reserve exposure and margin before handing off to the counter. Keep applicable order and cancel rate limits in the same engine.

- Aggregate account risk across all strategies and observed manual orders. Cover exposure and margin, instrument limits, action rates, account loss, self-trade prevention, and closeable inventory according to each rule's scope. Strategy-level limits may further restrict a PM. Keep existing inventory semantics such as today versus yesterday and long versus short, with checks for both account availability and the PM's attribution.
- Keep a reservation through an accepted submission and any pending cancellation. Reduce it as fills arrive or release it on a confirmed terminal result. Release immediately on a definite local submission failure. Reconcile an ambiguous outcome before releasing its reservation or considering a replacement.
- Restrict strategy cancels to orders owned by that strategy. Apply broker-terminal fills and cancels to the matched order even when the action originated outside the platform. Attribute unmatched external activity to an operator book pending reconciliation. If a manual close reduces inventory attributed to a PM, keep the account total current, record the attribution discrepancy, and resolve it through an audited operator assignment.

**Manual terminal integration is a release gate.** Verify with the broker environment that the automated session receives external working-order, cancel, and fill events, and can reconcile them from account-wide queries. The current CTP handlers filter reports to known local or adopted orders, so the adapter needs an explicit external-order ingestion path. Map local orders by session identity and counter reference, then use venue order and trade identifiers to correlate and deduplicate broker reports. A raw order reference can collide across sessions.

An external order can consume capacity before the engine sees it. A hard pre-trade cap across both entry paths requires the broker to enforce it across both sessions. If account visibility is incomplete, hold automated new orders during manual intervention until account-wide reconciliation completes. Confirm this operating mode with the broker before production.

## 5. Counter adapter contract

Build the account engine as `AccountEngine<Counter>`. Extend the existing `OrderGateway` concept or adapt it behind this boundary. Choose the counter at startup from the implementations built with the executable. Keep market-data and trading adapters independently selectable, and keep SDK types and libraries behind their adapters.

- **Submit and cancel.** A direct call from the engine returns a definite local acceptance, definite local failure, or ambiguous result. Normalize asynchronous counter responses onto the account event queue. Local acceptance and counter acknowledgment are separate states.
- **Session and reconciliation.** Normalize session status, order and trade reports, and query results. Provide account positions, working orders, and trades required for startup and reconnect, with the identity and sequencing needed to merge streaming events with query results.
- **Capabilities.** Declare supported order instructions, account-wide report visibility, reconciliation support, and identity semantics. Validate the enabled strategy features against those capabilities at startup.
- **Implementation order.** Use a scripted test counter to validate behavior, then CTP for end-to-end integration. Prove build isolation by compiling and exercising the same engine with both. Implement 盛立, 天策, or another production counter against its broker-supplied SDK and qualify that build on the same correctness and latency tests.

## 6. Failure handling, audit, and replay

| Condition | Required behavior |
| --- | --- |
| Strategy hangs or exits | Fence the runner and halt its strategy. The account engine cancels that strategy’s resting orders, monitors them until terminal, and continues processing fills and serving healthy runners. Leave positions for manual handling; do not automatically flatten. A restarted runner loads a fresh baseline before it can submit. |
| A runner falls behind or fills its queue | Surface the overflow to the affected runner and operations. After a feed or execution-report gap, suspend new intents from that runner until it resynchronizes. Leave the feed publisher and account engine independent of the runner's progress. |
| Account engine loses market data, counter reports, or journal integrity | Suspend new order entry when the affected risk state is untrustworthy. Rebuild the feed baseline or reconcile account state as applicable. Restore order entry only after the failed dependency is healthy and state is verified. |
| Account engine or counter session restarts | Start with order entry disabled. Restore the journal and snapshot, query current broker state, merge and deduplicate reports, reconcile outstanding submissions and reservations, then reopen. An ambiguous submission is never automatically resent. |

The account journal records the exact sequence of consumed intents, risk decisions, submissions and local outcomes, counter events, external orders, and operator controls. Retain the market state used by risk or a durable reference to the exact version in the feed capture. Replay account state from this sequence. Use the same callbacks and order/report semantics in the backtest harness. Capture per-runner inputs and parameters where strategy decisions need to be reproduced.

Record the outbound intent in canonical order before invoking the SDK. The current async journal separates enqueue, OS write, and stable-storage synchronization. **Review decision:** specify which durability watermark must be reached before the SDK call and which failure modes the guarantee covers. If the approved policy requires waiting on durability, include that wait in tick-to-trade. Exercise crashes immediately before and after the SDK call and prove reconciliation can recover each recorded submission state.

## 7. Performance and release tests

Use a common monotonic timebase across processes. Carry the receive timestamp through the intent and take the end timestamp at the return from the SDK before account bookkeeping. Report end-to-end p50, p95, p99, and sample count overall and per active strategy. Break out feed normalization and publication, runner scheduling and calculation, intent queue wait, account risk and journal wait, and SDK call time. Count local failures, rejected or stale intents, feed gaps, and queue overflows alongside latency.

Preallocate and warm shared-memory queues and hot state. Pin feed and execution work and assign strategy cores for the production workload. Keep disk I/O, web reads, and log formatting off the normal trading path. Use the agreed durability policy in the timed build.

| Test | Pass condition |
| --- | --- |
| Isolation | Deliberately hang one of two live runners. The other keeps receiving ticks, submitting orders, and receiving fills. Operations sees the stall and verifies cancellation of the hung runner’s resting orders at the broker. |
| Shared limit | Two runners submit orders competing for the last available capacity. Only capacity permitted by account rules is reserved and sent. Working orders, pending cancels, partial fills, and rejects update capacity correctly. |
| Manual intervention | Submit, fill, and cancel through the broker terminal, including a manual cancel of a strategy order. Verify working exposure and attribution, external ID collisions, duplicate reports, and account-wide reconciliation. |
| Recovery and access | Force feed gaps, runner overflow, disconnects, and crashes around submission. Verify replay and recovery. A PM cannot retrieve another PM's records via UI, API, or streams. |
| Production latency | On the broker Linux host with the production SDK, use the agreed PM count, strategy mix, tick bursts, and competing order flows, with audit enabled. Achieve **p99 < 50 µs overall and for every measured active strategy** with a representative sample. Publish failure and overflow counts beside the percentile. |

## 8. Delivery order and review decisions

1. **Extract and test.** Use the existing books, risk, OMS, and PnL as the account-engine core. Prove request lifecycle, account reservations, external activity, and recovery with a scripted counter.
2. **Separate processes.** Implement shared feed and runner IPC. Port the static-linked strategy context and demonstrate failure isolation and backtest parity on Windows and Linux.
3. **Integrate CTP and PM access.** Exercise CTP session behavior and manual-terminal visibility. Implement backend PM authorization and validate the operator view.
4. **Qualify production.** Integrate the chosen production SDK when available. Run the Linux fault, replay, and latency tests. Cut over one test account at a time, with the prior automated session stopped before the new engine takes ownership.

Review the outbound durability policy, the operating mode if CTP cannot provide complete manual-order visibility, and the representative PM count and burst workload for the latency gate. These inputs determine whether the production build is ready for release.

### Repository references

- [Existing strategy routing and callback execution](../../platform/include/plat/engine/site_dispatch.hpp)
- [Existing strategy context](../../platform/include/plat/engine/site.hpp)
- [Risk gate and current scope of checks](../../platform/include/plat/risk/riskgate.hpp)
- [Existing gateway concept](../../platform/include/plat/gw/gateway.hpp) and [CTP order and trade handlers](../../platform/include/plat/ctp/trader_client_orders.hpp)
- [Existing journal design](ARCHITECTURE.md) and [asynchronous writer](../../platform/include/plat/bus/async_journal.hpp)
