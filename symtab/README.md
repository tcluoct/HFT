# symtab — the instrument master as a minimal perfect hash

Every message a plant touches starts the same way: an 8-byte, space-padded
ticker comes off the wire and must become a dense instrument id — the index
into every SoA table the firm owns (tick size, lot, risk class, position).
That lookup runs billions of times a day against a universe that changes a
handful of times a day, which is the textbook case for paying at *build*
time to make *read* time perfect. symtab keeps the wire bytes as the key
(one 8-byte load, one 64-bit compare — no parsing, no string hash), builds a
CHD-style hash-and-displace **minimal perfect hash** over the universe at
startup (n keys in exactly n slots, zero collisions by construction), and
handles intraday listings with an immutable-snapshot swap reclaimed by
**reader epochs** — readers never lock, never retry, never fence a write.
Tenth project in the family (`tick2trade` = the hot path, `matchd` = the
exchange, `ticklab` = research infra, `seqbus` = transport, `riskgate` =
pre-trade checks, `feedarb` = feed arbitration, `wirecodec` = the codec,
`sesslink` = the session layer, `omsd` = order state).

```
 wire bytes "AAPL    " ──► u64 key (one load)
                              │
                              ▼
      bucket = fastrange(mix(key, B), m)      seeds[]: u32 x n/4  (L1)
      slot   = fastrange(mix(key, seeds[b]), n)
      hit    = slots[slot].key == key         slots[]: {key,id} x n (L2)
                              │                        load factor 1.00
                              ▼
      dense id ──► SoA static data (tick, lot, venue, status)

 control plane (rare):  clone universe ─► rebuild MPH ─► publish (1 atomic
 exchange) ─► free old snapshot once every reader epoch has moved past it
```

## Measured results

Windows 10 VM on AMD EPYC 7552 (20 vCPUs, no SMT), VS2026 (MSVC 14.51)
Release + LTO, threads pinned, time-critical priority. Universe: 8,192
random 1–6 letter tickers (US-equities scale); lookup tables ~160 KB
(cache-resident, like the real thing).

**Build** — 8,192 keys → 8,192 slots, 2,048 buckets, first attempt:

```
4.9 ms | max bucket 13 | displacement seeds: max 33,629, avg 228
digest e2e489032293490f (deterministic: same universe -> same table)
```

**Lookup** (ns/op, best of 5 rounds of 4M, signal-fenced rdtscp pairs):

```
                       mph      oa map (50% load)   std::unordered_map
throughput, hits      5.6            8.0                 19.4
throughput, misses    4.8           15.9                 21.6
dependent chain      17.2            9.2                 17.9
batch<16> prefetch    5.5             -                    -
```

**Churn** — 500 intraday adds (clone + rebuild + publish) at 1 ms spacing
under a full-rate reader:

```
writer rebuild+publish: p50 4.5 ms   p99 5.9 ms
reader 64-lookup batch: p50 382 ns   p99 663 ns   p99.9 2.8 us
reader misses 0 | snapshots freed 500/500, pending 0 -> OK
```

Three readings:

- **Misses cost the same as hits.** The perfect hash does identical work
  either way: two multiply-mix chains, one seed read, one slot read, one
  compare. The open-addressing baseline doubles on misses (15.9 vs 8.0 ns —
  a miss must probe to an empty slot) and `unordered_map` is 3–4x across the
  board. Feed handlers see plenty of misses (odd-lot venues, test symbols,
  other firms' listings) — a table whose *worst case equals its best case*
  is worth a startup build.
- **Latency and throughput are different questions.** In a dependent chain
  (each result needed before the next key is known) the MPH's two serial
  hash+load stages cost 17 ns and the OA map's usually-one-probe wins at
  9 ns. But symbol resolution is never chained in practice — a packet
  carries many independent symbols — and with 16 in flight (compute slots,
  prefetch all, then verify) the MPH is back to 5.5 ns/op. The lesson
  generalizes: report both shapes, because "ns per op" quietly means
  throughput.
- **The reader never notices a relist.** While the writer clones the
  universe and spends ~4.5 ms rebuilding the hash 500 times, the reading
  thread's 64-lookup batches hold p50 382 ns / p99.9 2.8 µs — indistinguishable
  from quiet-table numbers — because a swap is one pointer exchange and
  readers touch only immutable memory. Epoch reclamation freed all 500
  retired snapshots with zero misses. (The once-per-run ~270 µs max is the
  usual hypervisor descheduling on this box, not the swap.)

## Why the design is what it is

- **The wire format is the key.** ITCH/OUCH carry symbols as fixed 8-byte
  space-padded ASCII; symtab's `Sym` is those exact bytes in one `u64`.
  Resolution starts with a single unaligned load — no trim, no case-fold, no
  per-character hashing — and key equality is one CMP. (Key 0 is the empty
  sentinel; padded ASCII can never encode it.)
- **Hash-and-displace, largest bucket first.** Keys split into ~n/4 buckets
  by one hash; each bucket searches for a `u32` seed under which *all* its
  keys land in distinct free slots, hardest buckets placed first while the
  table is emptiest. Lookup never sees any of that effort — just
  `seeds[bucket]` then one verified slot — and the verify makes false hits
  impossible, not just unlikely. `fastrange` (multiply-high) replaces every
  modulo, which is what lets the table be exactly n slots instead of the
  next power of two.
- **Epochs, not seqlocks, for reload.** riskgate's limits table is seqlock'd
  because its rows mutate in place; symtab's snapshots never mutate, so
  readers shouldn't pay a version re-check per lookup. A reader announces
  its epoch (one relaxed store + fence per *batch*, not per lookup), and the
  writer frees a retired snapshot only when every announced epoch has moved
  past it. Readers can't tear, can't dangle, can't retry; the writer absorbs
  all the cost — the correct asymmetry for a read-mostly table.
- **Stable ids survive rebuilds.** The dense id is assigned once at listing
  and carried through every rebuild (the slot index would be free but would
  renumber the world on every add — positions and risk limits key on ids).
  `slots[]` stores `{key, id}` in 16 bytes so the verify compare and the id
  it yields share a cache line.
- **The build is deterministic and provable.** Same universe, same lambda →
  bit-identical table (bucket placement is size-then-id ordered), FNV digest
  asserted in tests; the bijection test walks every slot and proves n keys
  occupy n distinct slots with their right ids.

## Build & run

```
cmake --preset vs2026            # or configure from the solution root
cmake --build --preset vs2026-release
build\vs2026\Release\symtab_tests.exe
build\vs2026\Release\symtab.exe --rt
```

Options: `--n/--lookups/--swaps/--swap-us/--seed/--core/--no-pin/--rt`.

## Lab vs production

| Here | A production instrument master |
|---|---|
| 8,192 equities, one venue namespace | Millions of instruments with options; sharded per asset class |
| Tickers only | ISIN/FIGI/venue-alias graph, corporate actions, multi-day identity |
| Rebuild on the control thread (~4.5 ms) | Rebuild off-box or async; publish the same way (pointer swap) |
| Fixed 16 reader slots, register-only | Dynamic reader registry, per-NUMA replicas |
| Static data invented by a generator | Golden-source feed (Bloomberg/Refinitiv) with validation |

## Layout

```
include/st/sym.hpp     Sym wire-word key, mix, fastrange
include/st/mph.hpp     hash-and-displace MPH: build, lookup, batch, digest
include/st/oa_map.hpp  open-addressing baseline (the map to beat)
include/st/table.hpp   Instrument, immutable Snapshot, epoch-reclaimed swap
include/st/gen.hpp     deterministic universe generator
src/main.cpp           build / lookup-shapes / churn benchmark
src/tests.cpp          membership, bijection, determinism, epoch safety
```
