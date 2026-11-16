# System latency map

Run `status` in the live daemon, or read its end-of-session report. Both use the same grouped report and show cumulative count, P50, P99, P99.9 and maximum in nanoseconds. An empty stage is shown as `n=0 (no samples)`, not as zero latency. Percentiles are approximate histogram bucket bounds.

## Pipeline and measurements

```text
CTP market-data callback
  -> md.queue_ns
Engine source entry
  -> book.update_ns
Strategy callback
  -> strategy.<id>.calc_ns (pauses during context actions)
  -> order.prepare_ns / cancel.prepare_ns
  -> risk.new_order_ns / risk.cancel_ns
  -> gateway.place_ns / gateway.cancel_ns
CTP API
  -> order_roundtrip_ns (starts before place API call)
Acknowledgment report timestamp
  -> response.queue_ns
Engine source entry
  -> response.update_ns
Strategy execution-report callback

Engine journal publish -> journal.enqueue_ns
  -> journal.queue_ns -> journal.write_batch_ns -> journal.flush_ns
```

| Metric | Exact boundaries |
| --- | --- |
| `md.queue_ns` | Adapter's local receive timestamp to `Site::source` entry. Includes adapter conversion/publication, queue waiting and dequeue overhead. |
| `book.update_ns` | Market-data mark/book processing, before strategy dispatch or derived book-top publication. |
| `strategy.<id>.calc_ns` | Sum of callback calculation segments, explicitly paused during `place`, `cancel`, `set_timer` and cross-site send actions. Includes context reads and measurement overhead. |
| `strategy.<id>.callback_ns` | Whole delivered callback, including synchronous engine/risk/gateway actions. |
| `order.prepare_ns` | `place` request construction, canonical seal/journal enqueue, dispatch and OMS registration, ending before risk. Direct source order requests measure the OMS portion only. |
| `cancel.prepare_ns` | Order lookup, cancel construction and canonical seal/journal enqueue, before dispatch/risk. Unknown order IDs produce no preparation sample. |
| `risk.new_order_ns`, `risk.cancel_ns` | Immediately around `check_new` / `check_cancel`, including passing and failing checks. |
| `gateway.place_ns`, `gateway.cancel_ns` | Immediately around the gateway call; includes encoding, internal locking and CTP API submission, including local failures. |
| `response.queue_ns` | Report/session-event timestamp assigned by adapter to `Site::source` entry. Includes ring-publication lock wait and engine scheduling. |
| `response.update_ns` | OMS, PnL, risk, compliance and own-order bookkeeping, before strategy delivery. |
| `engine.event_ns` | A source event's full seal/dispatch cycle, including nested derived events and strategy fan-out. |
| `tick_to_trade_ns` | Trigger timestamp to return from gateway `place`. For CTP snapshot-triggered orders, includes the MD queue delay. |
| `order_roundtrip_ns` | Immediately before gateway `place` to the matched acknowledgment's adapter timestamp. Includes API submission; excludes response-ring waiting. |
| `tick_to_ack_ns` | Trigger timestamp to the same acknowledgment. |
| `journal.enqueue_ns` | Engine's journal ring publication duration. |
| `journal.queue_ns` | Journal publication start to worker consumption; includes publication and poll overhead. |
| `journal.write_batch_ns` | First timed record write start to last timed record write end in a drain burst. Includes intervening ring polls and worker telemetry; excludes the explicit flush. |
| `journal.flush_ns` | `fflush` duration per timed drain burst. Measures handing buffered writes to the OS, not physical disk durability. |

Stages describe operation costs; they are not an additive accounting ledger. Callback, engine-event, end-to-end and order-preparation measurements overlap smaller stages. Different stages can have different sample counts. **Never sum stage P99 values to get end-to-end P99**; the end-to-end intervals are measured directly.

## Explicit timing in code

`plat/tel/latency.hpp` owns metric names, histogram registration, bounded ACK correlation and report grouping. Stage call sites use local start/end readings:

```cpp
const auto begin = latency_.now();
const auto verdict = risk_.check_new(order, event_time);
const auto end = latency_.now();
latency_.record(LatencyStage::RiskNew, begin, end);
```

Strategy delivery calls `strategy_timer_.begin()` and `finish()`. Context actions explicitly call `pause()` and `resume()`, including early returns. Multiple actions accumulate calculation segments. Nested pauses cannot restart an outer paused segment. There is no `ActionTiming` destructor and no `strategy_excluded_` pointer.

Per-strategy histograms aggregate delivered event callbacks, including no-op handlers, but exclude startup. Targeted exec/deny/timer/parameter events count only for the addressed instance. Strategy IDs are unique within a site.

## Threading and timestamps

A ring slot carries one optional receive timestamp protected by the same version stamp as its payload. It uses existing slot padding: `Envelope` remains 256 bytes, ring slots remain 320 bytes, and the journal format is unchanged. Untimed publishers write zero; consumers skip queue measurements when metadata is absent.

The live driver supplies the adapters' monotonic session clock to the engine. Custom drivers must call `set_latency_clock()` and pass local receive metadata to `source()` for cross-boundary measurements. Local stage durations otherwise use `steady_clock`. Trigger timestamps from remote sources must share the clock domain; venue-time or historical timestamps must not be mixed with live clock readings. CTP timestamps are application callback/report times, not NIC receive/transmit timestamps.

Only the engine writes engine histograms. The journal worker maintains its own histograms and periodically publishes a mutex-protected copy using `try_lock`, so a report never blocks the worker. `refresh_latency()` reads this copy on the engine/report thread; live worker statistics can lag by a batch, and after join the snapshot is final. No per-order trace or timestamp array is exported.

RTT uses 65,536 fixed correlation slots. Replacement increments the displayed `latency.correlation_overwrite` counter and drops the older sample instead of matching the wrong order. Duplicate ACKs do not count; terminal responses clear pending slots. Recovered/foreign orders without a local send timestamp have no RTT sample.

Telemetry does not enter strategy decisions, envelopes, snapshots or deterministic hashes. Replay/recovery suppress samples; a mixed recovery/live journal drain burst can contain both, so batch timing begins at its first timed record. Histograms reset with the process and cannot be reconstructed from the journal.

Clock reads and histogram updates add overhead. `plat_bench` remains a synthetic fastest-round average workload, not a live P99 or a measurement of network latency.
