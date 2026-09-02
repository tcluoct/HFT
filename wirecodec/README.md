# wirecodec — a compile-time wire schema, zero-copy codec

Every packet a firm receives and every order it sends crosses an
encode/decode boundary, so codec cost is a tax on *everything* — tick-to-trade
includes it twice. Real venues speak packed binary protocols (OUCH, ITCH,
SBE); real firms generate codecs from schemas. This project does what SBE's
XML-plus-code-generator does, with nothing but C++23: the schema is a type,
the layout is `consteval`-computed, and a misspelled or moved field is a
compile error instead of a malformed order. Seventh project in the family.

```
using NewOrder = Message<"NewOrder", 'N', ByteOrder::kBig,
    Field<"cl_ord_id", u64>, Field<"symbol", u32>, Field<"side", u8>,
    Field<"qty", u32>, Field<"price", i64>, Field<"tif", u8>>;
                     │ consteval: offsets {0,8,12,13,17,25}, size 26
                     ▼
tx: begin_frame<NewOrder>(buf).set<"qty">(q).set<"price">(px)... ; seal_frame
      = byteswap + unaligned store at constant offsets + hardware CRC32-C
rx: try_parse(bytes) ─crc─► View<NewOrder> v(body); v.get<"price">()
      = one unaligned load + byteswap; nothing is copied, ever
```

## Measured results

Windows 10 VM on AMD EPYC 7552 (20 vCPUs, no SMT), VS2026 (MSVC 14.51)
Release + LTO, pinned at time-critical priority. 50M 34-byte NewOrder frames
over a 4096-slot arena (136 KB, L2-resident — codec cost, not DRAM misses).
One encode/decode is far below the ~20 ns `rdtscp` floor, so latency is
sampled per 16-message batch and reported per message.

```
                        ns/msg p50   p99    p99.9      throughput
encode + seal (CRC)         17.1     17.9    47.8     57.9M msg/s   2.0 GB/s
parse + decode (CRC)         8.8     11.9    15.2    114.0M msg/s   3.9 GB/s
encode, no CRC               4.4      6.3     7.6    237.2M msg/s   8.1 GB/s
decode, no CRC               3.1      4.4     5.6    333.7M msg/s  11.4 GB/s
```

Two readings. First, **the codec itself is ~3–4 ns per message** — six named
field accesses compile down to a handful of `MOV`/`BSWAP`s at constant
offsets, indistinguishable from hand-written struct code. Second, **integrity
costs more than encoding**: CRC32-C over a 30-byte frame is ~13 ns of the
17 ns encode — and the hardware instruction is already ~20× faster than a
table-driven software CRC. That ratio is why some venues checksum only at
the transport layer.

## Why a type-level schema is the design

- **The wire contract becomes checkable.** `static_assert(NewOrder::
  offset_of<"price">() == 17)` sits next to the schema; a teammate inserting
  a field breaks the build, not the exchange session. matchd/tick2trade
  static_assert struct *sizes*; this asserts every *offset*.
- **Field access by name, resolved at compile time.** `index_of<Name>()` is
  `consteval`: a typo'd name or duplicate field is a hard compile error. The
  name strings never reach the binary.
- **Packed layouts, like real wires.** `qty` at offset 13 straddles
  alignment on purpose — exchange protocols don't pad. Unaligned scalar
  loads are effectively free on x86-64, and going through `memcpy` in
  `load_raw`/`store_raw` keeps it defined behavior on every platform.
- **Flyweights, not objects.** `Encoder` is a typed pen over the caller's tx
  buffer; `View` is a typed lens over rx bytes. Decode allocates nothing and
  copies nothing — the "deserialized message" never exists as a struct.
- **Trust, but verify — in hardware.** Frames end in CRC32-C via SSE4.2
  `_mm_crc32_u64` (8 bytes/instruction). The tests cross-check it bit-for-bit
  against an independent bitwise software CRC, and prove a single flipped
  bit anywhere in a frame is caught before any field is read.

## The technique inventory (what's new vs the other six projects)

| Technique | Where | Why |
|---|---|---|
| **NTTP `FixedString` field names** | `core/fixed_string.hpp` | C++20 structural-type strings as template parameters: the schema reads like a spec (`Field<"price", i64>`), and name→offset resolution is pure compile time. |
| **`consteval` layout computation** | `core/codec.hpp` | `index_of`/`offset_of` run in the compiler; a wrong name `throw`s *during constant evaluation* = build error. This replaces SBE's entire code-generation step. |
| **`std::byteswap` (C++23) endianness** | `core/codec.hpp` | `wire_swap<Order>` collapses to a single `BSWAP` when wire ≠ native and to nothing when it matches — chosen per message type, at compile time. |
| **Flyweight zero-copy decode over `std::span<const std::byte>`** | `core/codec.hpp` | The rx buffer *is* the message. No intermediate struct, no memcpy of bodies, no allocation — measured 3.1 ns to read six fields. |
| **Hardware CRC32-C framing** | `core/frame.hpp` | `_mm_crc32_u64` loop, 8 B/instruction; validated against a bitwise reference implementation in the tests (independent oracle, not self-agreement). |
| **Offset static_asserts as the wire contract** | `proto/orders.hpp` | The protocol document and the enforcement are the same lines of code. |
| **Sub-floor latency via batched sampling** | `src/main.cpp` | When the operation (~4 ns) is 5× below the rdtscp floor (~20 ns), stamp 16-message batches and divide — stated in the report, not hidden. |

## Build & run

```pwsh
cmake --preset vs2026            # standalone; or build the whole hft solution from ..
cmake --build --preset vs2026-release

build\vs2026\Release\wirecodec_tests.exe   # 8 scenarios incl. CRC cross-check
build\vs2026\Release\wirecodec.exe --rt            # 50M frames, CRC on
build\vs2026\Release\wirecodec.exe --rt --no-crc   # isolate pure codec cost
```

```
--msgs N / --warmup N / --core N / --no-pin / --rt
--no-crc    skip seal/verify to separate codec cost from CRC32-C
```

## Lab vs production

| Here | Production (SBE / OUCH / ITCH codecs) |
|---|---|
| Fixed-size scalar fields only | Variable-length strings, repeating groups, optional fields with presence maps |
| One schema version, asserted at build | Schema evolution: version fields, extension points, rolling upgrades across counterparties |
| CRC32-C per frame | Venue-defined integrity: many rely on UDP/TCP checksums only; some add sequence + session layers (SoupBinTCP, MoldUDP64) |
| `throw` in consteval as the diagnostic | Generated codecs with documented diagnostics, conformance test suites from the venue |
| 34-byte frames in an L2 arena | Real NIC rings; encode directly into the TX descriptor's buffer to skip one more copy |
| ~4 ns codec on a VM | The same idea compiled into FPGA parsers at line rate; software codecs remain for the slow path and back-office |

## Layout

```
../common/include/hft/common/   platform, tsc_clock, histogram (solution-wide toolbox)
include/wc/
  core/     fixed_string.hpp  NTTP string (the schema's field names)
            codec.hpp         Field/Message + consteval layout + Encoder/View
            frame.hpp         header/trailer framing + hardware CRC32-C
  proto/    orders.hpp        NewOrder/CancelOrder/ExecReport + offset asserts
src/main.cpp   bench: arena encode/decode loops, batched sampling, DCE guard
src/tests.cpp  8 scenarios: layout contract, round-trip edges, exact byte
               image, CRC vs bitwise reference, foreign-buffer flyweight,
               corruption/truncation, back-to-back frames, wire_swap
```
