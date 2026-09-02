# tick2trade — Design & Low-Latency Techniques, Explained

This document walks through the code logic of every component and explains each
low-latency technique in depth: what the mechanism is, what cost it avoids, and
where it lives in the source. It is written to be read next to the code.

Companion to `README.md` (which has build instructions and measured results).

---

## 1. System overview

### 1.1 The shape of the problem

A trading system's hot path is a *pipeline with a loop in it*:

```
  market data in ──▶ decode ──▶ book ──▶ signal ──▶ risk ──▶ order out
       ▲                                                        │
       └──────────────────── the exchange ◀────────────────────┘
```

The business metric is **tick-to-trade latency**: the time from a market-data
packet hitting "the wire" to the resulting order leaving for the exchange.
Competitive software systems run this in ~1–10 µs; FPGA systems in hundreds of
nanoseconds. Everything in this codebase exists to make that number small and
— just as important — to *measure it honestly*.

### 1.2 Threads and queues

Five threads, each owning exactly one stage, connected by five single-producer/
single-consumer rings:

| Thread | Core (default) | Consumes | Produces |
|---|---|---|---|
| `ExchangeSim` | 2 | `OrderRing` (wire) | `MdRing`, `ExecRing` |
| `FeedHandler` | 4 | `MdRing` | `BookRing` |
| `ImbalanceTaker` | 6 | `BookRing`, `ExecRing` | `OrderRing` |
| `OrderGateway` | 8 | `OrderRing` | `OrderRing` (wire) |
| `AsyncLogger` worker | unpinned | 4 × log queues | file I/O |

Two consequences of this design are load-bearing:

1. **Every ring has exactly one producer and one consumer.** That is a
   *structural* property of the pipeline, decided at design time. It lets every
   queue be the cheapest possible kind (wait-free SPSC, §3) instead of a
   multi-producer queue with CAS loops. In real firms this is a deliberate
   architectural choice, not an accident.
2. **Each stage owns its data exclusively.** The order book belongs to the feed
   handler thread; position belongs to the strategy thread; histograms belong
   to the thread that records into them. Nothing is shared mutable state, so
   nothing needs a lock. The only synchronization in the whole hot path is the
   acquire/release pair inside each ring.

### 1.3 Timestamps

Latency is *attributed*, not just totaled. Five TSC stamps ride inside the
messages themselves (`wire.hpp`):

```
t0  ExchangeSim   stamps MdMsg.ts_origin as the last action before pushing (the "wire")
t1  FeedHandler   after the book is updated
t2  Strategy      when the order is created (decision made)
t3  OrderGateway  after risk checks pass, before pushing to the wire ring
t4  ExchangeSim   on popping the order (order "arrives" at the venue)
```

Each hop's histogram is recorded *by the thread where that interval completes*
— feed records t1−t0, strategy records t2−t1, gateway t3−t2, exchange t4−t0 —
so no histogram is ever written by two threads and none needs a lock.

### 1.4 Run phases

`RunControl` holds one `std::atomic<int> phase`: Warmup → Measure → Stop, set
by `main`. Hot loops sample it **once every 4096 iterations** with a relaxed
load (§10.2). On the Warmup→Measure edge each thread resets its own histograms
and counters — so cold caches, cold branch predictors, page faults, and the
first-touch costs of the big arrays are excluded from the report, which is what
"warmup" means in every serious benchmark.

---

## 2. Life of a tick (end-to-end walkthrough)

Follow one message through the measured 2.1 µs median:

1. **ExchangeSim.emit_one()** rolls its RNG, decides "add order, sell side,
   120 ticks below the ask, 300 shares", applies that message to its own mirror
   book (so its fill engine later agrees with what the strategy saw), stamps
   `ts_origin = rdtscp()` and pushes the 32-byte `MdMsg` into `MdRing`.
   The stamp is the *last* action so queue time is charged to the consumer's
   latency, exactly like a NIC hardware timestamp would be.

2. **FeedHandler.run()** pops the message (its poll loop was spinning on the
   ring, so the pop happens within nanoseconds of the push becoming visible),
   switches on `m.type`, and calls `OrderBook::apply`. For an add: one hash
   insert + one array index into the price ladder + best-bid/ask maintenance.
   It stamps `t1 = rdtscp()`, records `t1 − t0` (measured p50: **517 ns**,
   including the ring transit), and — only if the top of book changed — pushes
   a normalized `BookUpdate` to the strategy. That filter is why 10M md
   messages produced only 213k strategy events: most book changes don't move
   the touch, and the strategy only cares about the touch.

3. **ImbalanceTaker.on_book()** (statically dispatched through CRTP, inlined
   into the poll loop) checks: spread within 4 ticks? cooldown elapsed? one
   side ≥ 1.5× heavier? position limit respected? If all pass, it builds a
   64-byte `NewOrder`, stamps `t2 = rdtscp()` inside `send()`, and pushes to
   the gateway. Decision latency `t2 − t1` measured p50: **151 ns** — the cost
   of a ring pop, a handful of integer compares, and a ring push.

4. **OrderGateway.run()** pops the order and runs pre-trade risk as four
   integer compares arranged so the passing path is straight-line: qty bounds,
   price collar, kill switch, rate throttle. It stamps `t3`, records `t3 − t2`
   (p50: **648 ns**, mostly ring transit + poll pickup), and forwards to the
   exchange wire ring.

5. **ExchangeSim** pops the order, stamps `t4`, records the headline
   `t4 − t0` **tick-to-trade** number, matches the IOC against its mirror
   book's touch, and pushes an `ExecReport` back to the strategy, which updates
   its position on fill.

Total: five threads, five rings, one full book update, a strategy decision and
a risk check — **p50 2.1 µs, p99 4.6 µs** wire-to-wire.

---

## 3. `common/spsc_ring.hpp` — the wait-free SPSC ring

This is the most important 60 lines in the project; every byte that moves
between threads moves through it.

### 3.1 Core logic

A fixed, power-of-two array of slots plus two indices: `head_` (next slot the
producer writes) and `tail_` (next slot the consumer reads). Empty when
`head == tail`; full when advancing head would collide with tail (one slot is
deliberately sacrificed so full ≠ empty without a separate counter).

```cpp
bool try_push(const T& v) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);  // (a)
    const std::size_t next = (head + 1) & kMask;
    if (next == cached_tail_) [[unlikely]] {                          // (b)
        cached_tail_ = tail_.load(std::memory_order_acquire);
        if (next == cached_tail_) return false;                       // full
    }
    slots_[head] = v;                                                 // (c)
    head_.store(next, std::memory_order_release);                     // (d)
    return true;
}
```

### 3.2 The memory-model argument (why this is correct)

- (a) The producer loads **its own** `head_` relaxed: no other thread ever
  writes it, so there is nothing to synchronize with.
- (d)→consumer: the release store to `head_` *publishes* the slot write (c).
  The consumer's acquire load of `head_` that observes the new value therefore
  also observes the fully-written slot. This is the textbook release/acquire
  message-passing pattern — the flag is `head_`, the payload is the slot.
- Consumer's release store to `tail_` ↔ producer's acquire load (b): the same
  pattern in the other direction. It guarantees the producer only reuses a slot
  after it has *seen* that the consumer finished copying out of it.
- Nothing needs `seq_cst`: there is no algorithm here that depends on a single
  total order across independent atomics — only two point-to-point
  publish/observe relationships. On x86 the release/acquire pairs compile to
  plain loads and stores (the hardware model is already that strong); on ARM
  they become cheap `ldar/stlr` instead of full fences. Writing `seq_cst`
  "to be safe" would insert real fences for zero benefit.
- Making them `relaxed` would be a genuine bug that x86 hides and ARM exposes:
  the consumer could observe the new `head_` but stale slot bytes.

### 3.3 Trick: the cached counterpart index

The naive ring loads the *other* thread's atomic on **every** operation. That
load pulls a cache line that the other core is constantly writing — so every
push/pop pays a cache-coherence round trip (~40–100 ns cross-core), even when
the ring is nearly empty and the answer is obviously "there's room."

The fix (`cached_tail_` / `cached_head_`, popularized by Erik Rigtorp's
`SPSCQueue`): keep a *private, possibly stale* copy of the counterpart index.
Staleness is conservative in exactly the safe direction — a stale tail can only
make the ring look *fuller* than it is, a stale head only *emptier*. So the
fast path uses the cached value and only re-reads the real atomic when the
cached one says full/empty. Result: in steady state the producer touches only
lines it owns, and cross-core traffic happens once per "the ring filled up,"
not once per message.

### 3.4 Trick: false-sharing-proof layout

```cpp
alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // producer writes
std::size_t cached_tail_{0};                            // producer-private
alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // consumer writes
std::size_t cached_head_{0};                            // consumer-private
```

Producer-written state and consumer-written state sit on **separate cache
lines** (`kCacheLine` = `std::hardware_destructive_interference_size`). If
`head_` and `tail_` shared a line, every producer store would invalidate the
consumer's copy of the line and vice versa — the MESI ping-pong known as false
sharing, which can cost several-fold throughput. Note each owner's *private*
cache variable is co-located with the atomic that same owner writes: grouping
is by writer, which is the rule that matters.

### 3.5 Trick: power-of-two capacity + trivially-copyable elements

- `(head + 1) & kMask` instead of `% Capacity`: integer division/modulo is
  20–40 cycles and not pipelined well; AND is 1. The
  `requires std::has_single_bit(Capacity)` clause makes an illegal capacity a
  compile error rather than a silent correctness bug.
- `requires std::is_trivially_copyable_v<T>`: slot assignment is a plain
  memcpy-class copy — no constructors, destructors, or exception paths on the
  hot path, and the compiler can vectorize the 32/64-byte message copies.

---

## 4. `common/tsc_clock.hpp` — timestamps

### 4.1 Why rdtsc

`std::chrono::steady_clock::now()` costs ~20–30 ns and may involve a vDSO
call; `rdtsc` reads the CPU's time-stamp counter in ~6–10 ns with sub-ns
resolution. On every x86 CPU from the last decade the TSC is *invariant*:
constant frequency regardless of turbo/power states, and synchronized across
cores by hardware+firmware — which is what makes cross-thread deltas
(`t2` on the strategy core minus `t1` on the feed core) meaningful.

### 4.2 rdtsc vs rdtscp

Plain `rdtsc` can be executed *speculatively, before* earlier instructions
finish — a stamp taken "after the work" can actually be sampled mid-work.
`rdtscp` waits until all prior instructions have retired, so it can't drift
backward into the measured region. The convention in this codebase: cheap
`rdtsc` for non-measurement uses (cooldown checks, pacing), `rdtscp` at every
measurement end-point (`t0`…`t4`).

### 4.3 Trick: calibrate once, convert never (on the hot path)

`TscClock::calibrate()` measures cycles against `steady_clock` over 300 ms at
startup. Hot threads then record **raw cycle deltas** into histograms — integer
subtraction only. The divide-by-frequency happens once per histogram *bucket*
at report time, not once per event. A floating-point division per message is
small, but the discipline ("no float math on the hot path") is the point.

---

## 5. `common/histogram.hpp` — honest latency numbers

### 5.1 Why histograms at all

Averages are useless for latency (they mix a bimodal distribution into a
number nobody experiences), and storing every sample is unbounded memory. The
industry answer is HdrHistogram-style **log-linear bucketing**: O(1) record,
fixed footprint, and percentile queries with bounded relative error.

### 5.2 The bucketing math

Values below 32 get exact buckets. Above that, the value's magnitude (position
of the most significant bit, found with `std::countl_zero` — a single `lzcnt`
instruction) selects a *group*, and the next 5 bits below the MSB select a
linear sub-bucket within the group:

```cpp
const unsigned msb   = 63u - std::countl_zero(v);
const unsigned group = msb - kSubBits + 1;
const unsigned sub   = (v >> (msb - kSubBits)) & (kSub - 1);
return group * kSub + sub;
```

Each group covers one power of two split into 32 equal steps ⇒ worst-case
relative error ≈ 1/32 ≈ 3%, uniformly across the whole range, in 1920 buckets
(15 KB). `record()` is: one lzcnt, two shifts, one increment. `percentile(q)`
walks the cumulative counts and returns the bucket's midpoint representative.

### 5.3 Ownership discipline

Each histogram is written by exactly one thread and read only after the threads
join (or after a phase reset that the owner itself performs). No atomics, no
merging, no sharing — the concurrency problem is designed away rather than
solved.

---

## 6. `common/async_logger.hpp` — logging that never blocks

### 6.1 The problem

`printf`-style logging costs microseconds (formatting) plus worse (I/O, locks
inside the CRT). One log line inside the hot path would dominate the entire
tick-to-trade budget.

### 6.2 The design (NanoLog's core idea, simplified)

The hot thread enqueues a fixed 40-byte record:

```cpp
struct LogRecord {
    std::uint64_t tsc;
    const char*   fmt;    // pointer to a string LITERAL
    std::uint64_t a, b, c;
};
```

The trick is the `fmt` field: string literals live for the whole program, so
the hot path stores an 8-byte *pointer* and defers all formatting to the
background thread, which sleeps 200 µs between drains, formats with
`fprintf(fmt, a, b, c)`, and writes to the file. Hot-path cost: one SPSC push,
~10 ns.

Two policies worth noticing:

- **Drop, never block.** If a log queue fills, the record is discarded and a
  drop counter increments (reported at exit). A trading system must never
  trade latency for a log line; observability systems get sized so drops are
  rare, and *counted* so you know when they weren't.
- **One queue per hot thread** (`AsyncLogger::kQueues = 4`) keeps every queue
  single-producer, so the same wait-free ring is reused — no MPSC complexity.

---

## 7. `proto/wire.hpp` — message design

### 7.1 Fixed-point prices

`using Price = std::int64_t` — integer ticks, never floating point:

- float compares are slower and `==` on computed floats is a bug generator;
- float arithmetic is non-associative, so accumulated qty/price math drifts;
- unrepresentable decimal prices (0.1!) create phantom price levels in a book.

Every real exchange protocol (ITCH, MDP3, OUCH) ships integer prices for the
same reasons. Convert to decimal at the human boundary, nowhere else.

### 7.2 Trivially-copyable PODs, size-asserted

All messages are packed PODs whose sizes are pinned by `static_assert`:

- `MdMsg` = **32 bytes** — two per cache line; a 64-byte line fetch decodes two
  messages.
- `NewOrder` = **exactly 64 bytes** — one full line, so an order never
  straddles two lines (a straddle costs a second cache miss at every hop).

The asserts also act as tripwires: anyone who adds a field learns the layout
contract immediately at compile time, not from a latency regression a month
later.

`#pragma pack(1)` on `MdMsg` removes padding, but the field *order* is chosen
so every member is naturally aligned anyway (1+1+2+4, then 8s) — packed layout
without unaligned access penalties.

### 7.3 Timestamps ride in the message

Alternative designs log timestamps to thread-local buffers keyed by an ID and
join them later. Carrying `t0..t3` inside the message costs 32 bytes but makes
attribution trivial, allocation-free, and impossible to mis-correlate. For a
lab system measuring itself, in-band wins; wire-tap systems (Corvil) win when
you can't trust the hosts.

---

## 8. `md/order_book.hpp` — the L3 book

The feed handler's data structure is the single biggest latency lever in any
market-data path. This one is built from two pieces, both allocation-free
after construction.

### 8.1 The flat price ladder

```cpp
std::array<Level, kLadder> bids_, asks_;   // kLadder = 8192 ticks
int best_bid_ = -1, best_ask_ = kLadder;
Price anchor_;                             // tick value of index 0
```

A price maps to an array index by plain subtraction: `idx = price − anchor_`.
Level access is therefore **one indexed load** — no `std::map` (a red-black
tree walk = 3–5 dependent cache misses, ~100+ ns), no hash on price, no
allocation per level. Prices cluster tightly around the mid in real markets,
so a band (here ±4096 ticks) covers the day; production systems re-anchor on
the rare drift out of band.

Best-price maintenance is incremental:

- **Add** at a better price: update `best_bid_`/`best_ask_` directly (one
  compare).
- **Remove** that empties the best level: walk the array toward worse prices
  until a non-empty level (`advance_best`). This scan is bounded, touches
  sequential memory the prefetcher loves, and its branch pattern (usually
  "next level occupied") predicts well. The sparse-book runs showed this cost:
  decode p50 rose from ~170 ns (dense book, next level always occupied) to
  ~520 ns (thin book, longer walks) — an instructive trade visible in data.

Levels store aggregate `qty` and order `count` — everything the top-of-book
consumer needs; per-order FIFO queues (for queue-position modeling) would hang
an intrusive list off each level, same skeleton.

### 8.2 The order-id map

`Execute/Cancel/Delete` messages carry only an order id (as in real ITCH), so
the book needs id → {price, qty, side}. Requirements: millions of lookups/sec,
no allocation, no degradation over a day. `std::unordered_map` fails all three
(node allocation per insert, pointer-chasing buckets, ~1.5 cache misses per
lookup). The custom map:

- **Open addressing + linear probing** over power-of-two arrays: probes walk
  *sequential* memory (prefetcher-friendly) instead of chasing bucket
  pointers.
- **SoA split** — `keys_` and `vals_` in separate arrays: a probe scans a
  dense line of 8 keys without dragging 16-byte values through the cache; the
  value line is touched exactly once, on the hit.
- **Fibonacci hashing** (`id * 0x9E37…>> (64−log2 cap)`): one multiply, one
  shift; the golden-ratio constant spreads sequential ids (which exchange ids
  are!) uniformly. Taking the *top* bits — the well-mixed ones — instead of
  masking the bottom bits matters for multiplicative hashes.
- **Backward-shift deletion** instead of tombstones (`erase_at`): on erase,
  subsequent probe-chain members are shifted back to fill the hole, using the
  cyclic-distance test `((j − ideal) & mask) >= ((j − i) & mask)` to decide
  which elements may legally move. Tombstone schemes let probe chains grow all
  day — a feed handler processes tens of millions of deletes per session, and
  "gets slower as the day goes on" is a career-limiting property. Backward
  shift keeps chains permanently as short as if the deleted keys had never
  existed.
- **Empty = key 0** sentinel (order ids start at 1): no separate occupancy
  bitmap, one compare does double duty.

### 8.3 `apply()` returns "did the top change?"

The book reports whether the touch moved rather than making the caller diff
snapshots. That single bool is what gates downstream traffic: 10M messages in,
213k `BookUpdate`s out — a 47× fan-in reduction before the strategy ever runs.
Cutting work as early as possible in the pipeline is itself a latency
technique.

---

## 9. The pipeline stages

### 9.1 `md/feed_handler.hpp`

The run loop is the template every stage follows:

```cpp
for (;;) {
    if ((++iter & 0xFFF) == 0) [[unlikely]] { /* phase check, §10.2 */ }
    if (!in_.try_pop(m)) { cpu_relax(); continue; }   // busy-spin, §10.1
    // ... the actual work ...
}
```

Logic: pop → bounds-check symbol → `book.apply(m)` → stamp `t1` → record
`t1−t0` → if the touch changed and both sides exist, push a `BookUpdate`.
A full downstream ring increments `dropped_updates_` — the honest lab stand-in
for **conflation** (production feed handlers keep *the latest* state per
symbol when a consumer lags, because stale intermediate states are worthless).

### 9.2 `strategy/imbalance_taker.hpp` — CRTP dispatch

```cpp
template <typename Derived>
class StrategyBase {
    void run(RunControl& rc) {
        ...
        self().on_book(u);      // static dispatch: no vtable
    }
    Derived& self() { return *static_cast<Derived*>(this); }
};
class ImbalanceTaker final : public StrategyBase<ImbalanceTaker> { ... };
```

**Why not `virtual on_book()`?** A virtual call is an indirect branch through
the vtable. The branch target predictor handles a *monomorphic* call site
decently, but: the call can't be inlined, so the optimizer can't fold the
strategy's compares into the loop, keep book fields in registers across the
boundary, or eliminate the function-call frame. CRTP makes the strategy body
part of the loop body at compile time — the measured 151 ns decision p50 *is*
that inlining. The framework cost of supporting multiple strategies is paid at
compile time (one loop instantiation per strategy type), not per event.

**Signal logic** (deliberately simple — the plumbing is the point): if spread
≤ 4 ticks and one touch side is ≥ 1.5× heavier, take liquidity on the thin
side with an IOC. Guards, in evaluation order (cheapest and most-often-false
first): spread sanity → cooldown (`rdtsc` delta vs a precomputed cycle count —
no clock syscall, no division) → imbalance (integer cross-multiplication
`bid*den >= ask*num`, no floating-point ratio) → position limit.

`on_exec` maintains position from fills; `on_measure_start()` is a CRTP hook
so the base's phase logic can reset derived-class counters without a virtual.

### 9.3 `oe/order_gateway.hpp` — pre-trade risk

Real firms are legally required (SEC 15c3-5) to run risk checks *between*
strategy and exchange, in-line, on every order. The lesson of this component
is that a regulatory checkpoint doesn't have to cost microseconds:

```cpp
if (o.qty == 0 || o.qty > risk_.max_qty ||
    o.price < risk_.px_min || o.price > risk_.px_max) [[unlikely]] { reject }
if (total_sent_ >= risk_.max_total) [[unlikely]] { reject }   // kill switch
o.t3 = rdtscp();
if (o.t3 - last_send_ < min_gap_cycles_) [[unlikely]] { throttle }
```

Four integer compares and a stamp; every rejection branch is `[[unlikely]]`
so the pass path compiles to straight-line code. The throttle reuses the
already-taken `t3` stamp rather than reading the clock twice. The kill-switch
counter deliberately survives the measurement-phase reset — a session limit
that forgets itself on a stopwatch boundary isn't a safety control.

### 9.4 `sim/exchange_sim.hpp` — the venue

Three responsibilities:

**Market generation.** Per emission: 55% add near the touch (side, offset,
qty from RNG bits), 25% delete a random resting order, 20% execute up to 100
shares of one. Live orders per symbol are capped at 512 — a deliberately
*thin* book whose touch flickers between imbalance states, giving the strategy
realistic firing opportunities (a saturated book's touch aggregates so many
orders that imbalance signals vanish; this was tuned empirically). The mid
random-walks ±1 tick every 256 emissions, clamped inside the ladder band.
Removal from the live vector is swap-with-back-and-pop — O(1), order
irrelevant.

**Pacing.** Target rate → `cycles_per_msg` once at startup; the loop emits
when `rdtsc() >= next_emit` and advances `next_emit += cycles_per_msg`
(with a resync clamp after long stalls). TSC-based token pacing costs nothing
and jitters far less than any sleep-based scheme. `--rate 0` disables pacing
to find the throughput cliff.

**Fills.** The sim applies every generated message to its own mirror
`OrderBook` (same class the feed handler uses), so when an IOC arrives it can
match against exactly the touch the strategy acted on: buy fills at the ask if
`price >= ask`, else killed. Order entry is drained *before* each emission
step — the latency-critical direction gets priority.

**Hot-path RNG.** `splitmix64`: three multiply-xor-shift steps, ~1.5 ns, and
statistically far better than `rand()`. `std::mt19937` costs more and its
2.5 KB state evicts hotter data from L1. One 64-bit draw is bit-sliced for
several decisions (symbol, action roll, side, offset, qty).

**Trick: Lemire's bounded reduction.** Mapping a random `x` into `[0, n)`
without the modulo operator:

```cpp
static std::uint32_t bounded(std::uint64_t x32, std::uint32_t n) {
    return static_cast<std::uint32_t>(((x32 & 0xFFFFFFFF) * n) >> 32);
}
```

`(x * n) >> 32` scales the 32-bit range down to `n` with one multiply and one
shift — no 20–40-cycle division, and (unlike `x % n`) no modulo bias.

### 9.5 `src/main.cpp` — orchestration

Setup order matters and is commented in the code: calibrate TSC (before any
component precomputes cycle constants) → allocate rings on the heap
(`make_unique`; multi-MB objects don't belong on 1 MB thread stacks; C++17
aligned `new` honors the `alignas(64)` members) → construct logger, then
components, then start `std::jthread`s, each of which pins itself and
optionally raises priority before entering its loop. `main` then just sleeps
through the phases, flips `rc.phase`, joins, and prints the report from the
now-quiescent components' histograms — reading them needs no synchronization
beyond the joins themselves.

Pinning defaults to logical CPUs 2,4,6,8: Windows enumerates SMT siblings as
(2n, 2n+1), so even numbers land each stage on its **own physical core**, and
skipping 0 avoids the core where the OS parks housekeeping. This was measured,
not theorized: with two spinning stages on HT siblings of one core, p99.9 was
~900 µs; on separate physical cores, ~6 µs.

---

## 10. Cross-cutting techniques

### 10.1 Busy-spinning with `_mm_pause`

Every stage polls its ring in a spin loop; nothing ever blocks on a mutex,
condition variable, or I/O. Blocking costs a syscall to sleep, a syscall to
wake, a scheduler decision, and cold caches on resume — microseconds to
milliseconds of wakeup latency, i.e., more than this system's entire budget.
The price is a core pegged at 100% per stage; HFT firms consider cores the
cheapest thing they buy.

`cpu_relax()` (the `PAUSE` instruction) in the empty-poll path: tells the CPU
this is a spin-wait, which (a) stops speculative memory-order violations from
flushing the pipeline when the awaited line finally changes, (b) yields
execution resources to the SMT sibling, (c) drops power draw slightly — which
on turbo-clocked parts means the *other* cores can clock higher.

### 10.2 Amortized control-plane checks

`if ((++iter & 0xFFF) == 0) { check phase atomic }` — coordination is sampled
once per 4096 iterations. Even a relaxed atomic load per iteration is a
per-message tax and, worse, a shared line touched by another thread. The
amortized version costs one increment+test+predictable-branch per message and
bounds stop/reset latency to a few thousand loop iterations (microseconds) —
the right trade in both directions.

### 10.3 Branch shaping: `[[likely]]` / `[[unlikely]]`

Used on every hot-path conditional whose skew is structural (ring full/empty,
risk reject, out-of-band price, phase check). These attributes steer *code
layout*: the likely path becomes the fall-through, keeping it dense in the
instruction cache and the front-end fetch stream; unlikely blocks get moved
out of line. They do not program the hardware branch predictor — the runtime
predictor learns actual behavior — but layout is the compiler's to decide and
worth deciding correctly.

### 10.4 Zero allocation, zero exceptions, zero RTTI on the hot path

- Every container is pre-sized at construction (`vector::reserve`, fixed
  `std::array`, fixed-capacity map). After startup, `new`/`malloc` is never
  called by a pinned thread — no allocator locks, no page faults, no
  amortized-O(1) reallocation spikes hiding in the tail.
- All hot functions are `noexcept`; no exception can cross the hot path.
  (`/EHsc` remains on for the standard library in setup code; shops commonly
  build hot processes with `-fno-exceptions` outright.)
- `/GR-` removes RTTI — nothing uses `dynamic_cast`, and the vtable-free
  design (§9.2) makes it moot anyway.

### 10.5 Compile-time configuration

Ring capacities, ladder size, map size, message layouts are all compile-time
constants (`config.hpp`, template parameters, `static_assert`s). The compiler
folds masks and bounds into immediates, and illegal configurations (non-power-
of-two capacity, oversized message) fail the *build*, not the p99.

### 10.6 What the report itself teaches

Run the system in its two regimes and the report demonstrates the two halves
of latency engineering:

- **Paced (1M msg/s):** p50 517 ns decode, tick-to-trade p50 2.1 µs / p99
  4.6 µs — clean, code-dominated latency.
- **Saturated (`--rate 0`, ~4.1M msg/s):** p50 barely moves (590 ns) but p99
  explodes to milliseconds and the ring sheds 0.17% of input. Nothing about
  the *code* got slower — the latency is **queueing delay** (utilization → 1,
  wait → ∞, Little's law in action). Real systems are capacity-planned so the
  hot path never operates near saturation, and this is why.

---

## 11. Honesty section: lab vs production

| This project | A production system |
|---|---|
| SPSC ring as "the wire" | Kernel-bypass NICs (Solarflare ef_vi, DPDK, or FPGAs): the packet lands in user-space memory without a syscall or kernel copy. The ring here stands in for that path shape. |
| One packed md format | Per-type big-endian layouts (ITCH/MDP3): decode = `bswap` + `std::bit_cast` per type, plus A/B feed arbitration and sequence-gap recovery state machines. |
| `SetThreadAffinityMask` | Linux `isolcpus`/`nohz_full` (kernel evicted from trading cores), IRQ affinity steering, NUMA-local allocation, explicit huge pages to eliminate TLB misses. |
| Invariant-TSC host stamps | PTP-disciplined NIC hardware timestamps; independent wire-tap capture for compliance-grade numbers. |
| Toy IOC fill at the touch | A price-time-priority matching engine (natural next project on this codebase). |
| Imbalance heuristic | Actual alpha. The transferable part is everything *around* the strategy: book, queues, risk, measurement. |

---

## 12. Index: trick → where

| # | Technique | File |
|---|---|---|
| 1 | Wait-free SPSC ring, acquire/release message passing | `common/spsc_ring.hpp` |
| 2 | Cached counterpart index (coherence-traffic elimination) | `common/spsc_ring.hpp` |
| 3 | False-sharing padding, writer-grouped cache lines | `common/spsc_ring.hpp` |
| 4 | Power-of-two + mask instead of modulo | `spsc_ring.hpp`, `order_book.hpp` |
| 5 | Busy-spin polling + `_mm_pause` | all `run()` loops |
| 6 | Thread pinning, SMT-sibling avoidance, priority boost | `platform.hpp`, `main.cpp` |
| 7 | Amortized phase checks (relaxed atomic, 1/4096 iters) | all `run()` loops |
| 8 | rdtsc/rdtscp stamps, one-time calibration, cycles-not-ns on hot path | `common/tsc_clock.hpp` |
| 9 | Log-linear O(1) histograms, thread-owned, warmup reset | `common/histogram.hpp` |
| 10 | Deferred-formatting async logger, drop-never-block | `common/async_logger.hpp` |
| 11 | Fixed-point integer prices | `proto/wire.hpp` |
| 12 | Size-asserted, cache-line-sized POD messages | `proto/wire.hpp` |
| 13 | In-band per-hop timestamps | `proto/wire.hpp` + all stages |
| 14 | Flat price-ladder book (O(1) level access) | `md/order_book.hpp` |
| 15 | Open-addressing SoA order map, Fibonacci hash | `md/order_book.hpp` |
| 16 | Backward-shift deletion (no tombstone decay) | `md/order_book.hpp` |
| 17 | Early fan-in reduction (TOB-change gating) | `md/feed_handler.hpp` |
| 18 | CRTP static dispatch, strategy inlined into the loop | `strategy/imbalance_taker.hpp` |
| 19 | Straight-line integer risk checks, `[[unlikely]]` rejects | `oe/order_gateway.hpp` |
| 20 | splitmix64 RNG, bit-slicing one draw | `sim/exchange_sim.hpp` |
| 21 | Lemire bounded reduction (no division, no bias) | `sim/exchange_sim.hpp` |
| 22 | TSC-paced emission (token pacing without sleeps) | `sim/exchange_sim.hpp` |
| 23 | Zero hot-path allocation (pre-sized everything, swap-pop) | everywhere |
| 24 | `noexcept` hot path, `/GR-`, LTO, `/arch:AVX2`, `/Ob3` | code + `CMakeLists.txt` |
| 25 | Compile-time capacities and layout `static_assert`s | `config.hpp`, `wire.hpp` |
| 26 | Warmup/measure phase separation | `run_control.hpp`, `main.cpp` |
