# The `.mdr` market-data format — compression, save, read

This document specifies how `platrec` (the market-data recorder,
DEPLOYMENT.md §9) stores L1 and L2 market data: the on-disk layout,
the delta+varint compression, the write path, and the read path. It is both
a design rationale and a wire spec — enough to write an independent reader.

Implementation: `include/plat/rec/tick_codec.hpp` (codec) and
`tick_file.hpp` (file). Everything is integer-exact — no floating point
touches the stored data, so a decode reproduces the venue's numbers bit for
bit.

---

## 1. Why not just store the raw ticks?

One CTP depth snapshot, laid out as a fixed struct, is **216 bytes** (an
instrument index, three timestamps, last/volume/turnover/open-interest, and
five bid + five ask levels of price×quantity). A liquid contract prints
2–4 snapshots a second; the whole commodity-futures market is thousands of
instruments. Storing 216 B/tick would be tens of GB per day per datacenter —
and most of it is *unchanged*: between two consecutive snapshots of the same
instrument, the deep book levels rarely move, the instrument index never
moves, and the timestamps advance by tiny increments.

The format exploits exactly that redundancy. Two ideas, composed:

1. **Delta coding** — store each field as the difference from the *previous
   record of the same instrument*, not its absolute value. An unchanged
   level becomes a zero.
2. **Zigzag + varint** — encode each (signed) delta in the fewest bytes: a
   small number costs one byte, and zero costs one byte.

Measured result: a real L2 tick lands in **~40–60 bytes**, ~74% smaller than
raw, with no loss. A held-flat book level is literally one byte.

---

## 2. The record — what one tick contains

`MdRecord` (decoded form, `tick_codec.hpp`) holds up to five book levels;
how many are actually stored is the file's **depth** (§2.1):

| Field | Meaning |
|---|---|
| `inst` (u32) | dense index into the file's instrument directory (§3) |
| `recv_ts` | recorder's local receive time, ns since local midnight |
| `exch_ts` | exchange update time (ms precision), ns since midnight |
| `last_px` | last traded price, fixed-point 1e-4 CNY |
| `day_volume` | cumulative day volume — **monotone**, the sequence key (§6) |
| `turnover_e1` | cumulative turnover, 0.1-CNY units |
| `open_interest` | open interest |
| `bid_px[·] / bid_qty[·]` | bid levels (1 for L1, 5 for L2) |
| `ask_px[·] / ask_qty[·]` | ask levels (1 for L1, 5 for L2) |

All prices and quantities are integers (fixed-point), so deltas are exact
and the compression is lossless. `recv_ts` is per-recorder (each datacenter
has its own clock); `exch_ts` and `day_volume` are the venue's own and are
what cross-datacenter comparison aligns on.

### 2.1 L1 and L2 in one format — depth lives in the header

A source may publish only the top of book (**L1** — one bid/ask level, the
CTP commodity-futures default) or five levels (**L2**). The format handles
both with the *same* record type: the recorder writes the number of levels
into `MdrHeader.depth` (1 or 5), and the codec encodes and decodes exactly
that many levels. An L1 file never carries the four deep levels at all — it
is materially smaller, not merely delta-zeroed:

```
same 20 000 ticks:  L1 file ≈ 24 B/tick    L2 file ≈ 56 B/tick
```

Because the depth is in the header, a reader learns the record shape once,
before the first record, and every decode is unambiguous — you never guess
whether a file is L1 or L2. `platrec --dump` prints `L1`/`L2` in its header
line and shows only the levels that exist.

---

## 3. File layout

```
┌─────────────────────────────┐  offset 0
│ MdrHeader            4096 B │  magic, day, instrument_count, source tag,
│                             │  record_count, payload_hash, sealed flag
├─────────────────────────────┤  offset 4096
│ MdrInstrument[0]      40 B  │  symbol[32] + exchange[8]
│ MdrInstrument[1]      40 B  │  … the directory: instrument_count entries,
│ …                           │     in the order the recorder assigned ids
├─────────────────────────────┤  offset 4096 + 40·N
│ encoded record stream       │  variable-length records, back to back
│ …                           │  (each is one MdRecord, delta+varint coded)
└─────────────────────────────┘
```

- **`MdrHeader`** (exactly 4096 bytes — one page, so the payload starts
  page-aligned). `magic` = `"PLMDR1"`; `day` = yyyymmdd; `source` = the
  datacenter tag (`dc1`, `dc2`, …); `instrument_count` = directory length.
  `depth` = book levels per side (1 = L1, 5 = L2). `record_count` and
  `payload_hash` are written **at seal** (§4); a still-recording or crashed
  file has them zero. `sealed` distinguishes the two.
- **Directory** — the instrument index in every record is a position in
  this table, so the stream never repeats a symbol string. A reader resolves
  `inst → (symbol, exchange)` by indexing it.
- **Record stream** — the compressed ticks, described next.

---

## 4. Compression — the codec

### 4.1 Varint and zigzag (the primitives)

A **varint** stores an unsigned integer in 7-bit groups, low group first,
with the high bit of each byte set while more groups follow:

```
value 0        -> 1 byte   (0x00)
value 1..127   -> 1 byte
value 128..16383 -> 2 bytes
… 7 more bits per additional byte
```

**Zigzag** maps a signed integer to an unsigned one so that small-magnitude
values (positive or negative) stay small — the whole point, since deltas are
signed and usually tiny:

```
 0 -> 0    -1 -> 1    1 -> 2    -2 -> 3    2 -> 4   …
zigzag(v)   = (v << 1) ^ (v >> 63)
unzigzag(u) = (u >> 1) ^ -(u & 1)
```

So a delta of 0 (unchanged field) → zigzag 0 → **one 0x00 byte**. A one-tick
move → a 1-byte varint. Only a large jump costs several bytes.

### 4.2 Encoding one record

Against `prev` (the previous record of the *same* instrument):

```
put_varint(inst)                         // absolute — picks the prev/decoder slot
for each remaining field f:
    put_varint( zigzag( cur.f - prev.f ) )
```

Field order is fixed (this IS the wire order):

```
inst,
recv_ts, exch_ts, last_px, day_volume, turnover_e1, open_interest,
then for level i = 0..(depth-1):
    bid_px[i], bid_qty[i], ask_px[i], ask_qty[i]
```

That is 1 leading index + 6 scalars + 4·depth level deltas: 11 varints for
L1, 27 for L2. The first record of an
instrument is encoded against a **zero-initialized** prev, so its deltas are
the absolute values — self-contained, no priming needed. Worst case (every
field a full 64-bit jump) is bounded by `kMaxEncoded = 288` bytes; the
reader uses that bound to know when its buffer holds a whole record.

### 4.3 Why delta-against-same-instrument works

Records for different instruments are interleaved in arrival order, but each
carries its `inst` index, so the encoder keeps a **per-instrument `prev`
array** and diffs each record against that instrument's last one — not
against whatever physically preceded it in the file. This is what makes the
deep levels compress: `bid_px[4]` for cu2609 is diffed against the previous
cu2609 `bid_px[4]`, which is usually identical → one zero byte.

---

## 5. The write path

`TickWriter` (writer thread only — the CTP network thread never does I/O;
see DEPLOYMENT.md §9 for the SPSC-ring handoff):

1. `open(path, day, source, depth, dir)` writes the header (with the L1/L2
   `depth`) and the directory, and allocates the per-instrument `prev` array
   (one `MdRecord` per directory entry, zeroed).
2. `append(r)` delta-encodes `r` against `prev[r.inst]` into a scratch
   buffer, writes those bytes, folds them into a running **FNV-1a checksum**,
   then stores `r` as the new `prev[r.inst]`.
3. The `FILE*` is **1 MiB buffered**; the writer thread calls `flush()` after
   each drain burst, so the OS holds every appended record even if the
   process is killed the next instant.
4. `seal()` (called by `close()`) rewrites the header with the final
   `record_count`, the `payload_hash`, and `sealed = 1`, then flushes.

**Crash behavior is deliberate.** A killed recorder never seals — the header
keeps `sealed = 0` and `record_count = 0` — but every flushed record is
intact on disk. That unsealed state is the crash *signal* (a healthy file is
sealed), and the reader recovers everything up to the last whole record (§6).

---

## 6. The read path

`TickReader` streams the file back:

1. `open(path)` reads and validates the header (`magic`), loads the
   directory, and allocates its own per-instrument `prev` array.
2. `next(out)` maintains a 1 MiB read buffer. When the buffer holds at least
   `kMaxEncoded` bytes (or the file has ended), it:
   - reads the leading `inst` varint (`peek_inst`) to pick `prev[inst]`;
   - decodes the 26 deltas, reconstructing `out.f = prev[inst].f + unzigzag(δ)`;
   - folds the consumed bytes into the checksum, advances, and stores `out`
     as the new `prev[inst]`.
   When the buffer runs low it keeps the unconsumed remainder, refills from
   the file, and continues — so records that straddle a 1 MiB boundary
   decode correctly.
3. End of data is either the sealed end or a **torn tail**: a partial record
   left by a crash decodes to 0 bytes consumed, and `next()` stops there. A
   truncated or corrupt-index record stops the stream at that point too —
   everything before it is valid and returned.

**Verification.** After streaming to the end:

- `sealed()` — was the file closed cleanly?
- `verified()` — sealed **and** the decoded record count equals the header's
  **and** the recomputed payload checksum equals the header's. A sealed file
  that fails this is damaged and must not drive a backtest.
- an unsealed file reports how many records were recovered to the torn tail.

`platrec --dump <file> [count]` prints all of this: the header line
(day, source, count, sealed/unsealed, checksum verdict) followed by
human-readable records.

---

## 7. Sequence correctness (UDP reordering)

Market data is published over UDP: **arrival order is not data order** —
packets can be reordered or duplicated in transit. The file stores records
in the order they arrived, so a single recording may be locally out of
order. This is not lost data, only misordered, and it is repairable because
the venue stamps its own order into every record.

The canonical order within an instrument is the key

```
(exch_ts, cumulative day_volume, open_interest)
```

`day_volume` is **cumulative and therefore monotone** — it only ever
increases through the session — so sorting on this key restores the true
sequence, and any record that still violates monotonicity after sorting is
provably *damaged*, not merely late. The recorder does not reorder on the
write path (that would cost latency and risk dropping ticks); ordering is a
read-time / after-close concern, handled by the compare/merge (§8).

---

## 8. Cross-datacenter compare and the golden file

`platrec --compare A.mdr B.mdr [golden.mdr]` (`tick_merge.hpp`) is the
after-close job that turns N imperfect recordings into one correct dataset:

1. Load each file into per-instrument sequences, **canonically sorted** on
   the §7 key. Sorting repairs each side's UDP reordering (counted as
   *out-of-order repaired*); exact-duplicate consecutive records are dropped.
2. Walk the two sorted sequences per instrument with a two-pointer merge:
   - **equal keys, equal content** → *matched* (the datacenters agree);
   - **equal keys, different content** → *conflict* — a real data problem to
     investigate (a nonzero conflict count is the alarm; exit code 2);
   - **key present on one side only** → that side fills the other's gap
     (a UDP loss one datacenter missed and the other caught).
3. If `golden.mdr` is named, every merged record is written to it in
   canonical order — gap-filled, de-duplicated, sealed, checksummed. That is
   the file a backtester replays.

Content comparison excludes `recv_ts` (each datacenter's local clock
differs); it compares the venue's own fields — `exch_ts`, `last_px`,
`day_volume`, `turnover`, `open_interest`, and all ten book levels.

This is why you deploy recorders in multiple datacenters: independent
captures of the same UDP stream lose *different* packets, and the merge
recovers a sequence more complete and correct than any single recorder saw.

---

## 9. Reading `.mdr` from your own code

```cpp
#include "plat/rec/tick_file.hpp"
using namespace plat::rec;

TickReader r;
if (!r.open("md_20260715_dc1.mdr")) { /* bad/missing file */ }
const int levels = r.depth();          // 1 = L1, 5 = L2 — from the header

MdRecord rec;
while (r.next(rec)) {
    // rec is fully reconstructed; rec.inst indexes r.dir(),
    // rec.bid_px[0..levels-1] / rec.ask_px[...] are the live levels.
    const char* symbol = r.dir()[rec.inst].symbol;
    // … feed your backtest book / book-building here …
}
if (r.sealed() && !r.verified())
    /* file is damaged — do not trust it */;
```

For strategy backtest, run the compare first to produce the golden file,
then replay the golden file: it is canonically ordered per instrument and
free of the transport artifacts a single raw recording carries.
