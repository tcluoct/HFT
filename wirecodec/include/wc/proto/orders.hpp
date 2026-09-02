#pragma once
// The lab's order-entry protocol (OUCH-flavored): three fixed-layout,
// big-endian, packed messages. The static_asserts below are the wire
// contract — if a schema edit moves a byte, the build breaks, not the
// exchange session.

#include <cstdint>

#include "wc/core/codec.hpp"

namespace wc::proto {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

using NewOrder = Message<"NewOrder", 'N', ByteOrder::kBig,
                         Field<"cl_ord_id", u64>,  // offset 0
                         Field<"symbol", u32>,     // offset 8
                         Field<"side", u8>,        // offset 12
                         Field<"qty", u32>,        // offset 13 (unaligned, like real wires)
                         Field<"price", i64>,      // offset 17
                         Field<"tif", u8>>;        // offset 25

using CancelOrder = Message<"CancelOrder", 'C', ByteOrder::kBig,
                            Field<"cl_ord_id", u64>,
                            Field<"qty", u32>>;

using ExecReport = Message<"ExecReport", 'E', ByteOrder::kBig,
                           Field<"cl_ord_id", u64>,
                           Field<"exec_id", u64>,
                           Field<"fill_qty", u32>,
                           Field<"fill_px", i64>,
                           Field<"leaves", u32>>;

static_assert(NewOrder::body_size == 26);
static_assert(NewOrder::offset_of<"cl_ord_id">() == 0);
static_assert(NewOrder::offset_of<"symbol">() == 8);
static_assert(NewOrder::offset_of<"side">() == 12);
static_assert(NewOrder::offset_of<"qty">() == 13);
static_assert(NewOrder::offset_of<"price">() == 17);
static_assert(NewOrder::offset_of<"tif">() == 25);
static_assert(std::is_same_v<NewOrder::field_type<"price">, i64>);

static_assert(CancelOrder::body_size == 12);
static_assert(ExecReport::body_size == 32);
static_assert(ExecReport::offset_of<"leaves">() == 28);

}  // namespace wc::proto
