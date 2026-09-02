#pragma once
// Wire formats. All messages are fixed-size, trivially-copyable PODs so the
// SPSC rings move them with plain copies and zero serialization work.
//
//  * Prices are integer ticks (int64). Floating point never touches the hot
//    path: it is slower to compare, non-associative, and unrepresentable
//    prices create phantom price levels.
//  * The market-data message uses ONE packed 32-byte layout for all types.
//    (Real ITCH/MDP3 has per-type layouts and big-endian fields; the decode
//    step there is a bswap + std::bit_cast per type. Same idea, more cases.)
//  * Timestamps ride inside the messages so each hop's latency can be
//    attributed precisely at the point where the data completes its journey.

#include <cstdint>

namespace t2t {

using Price = std::int64_t;    // integer ticks
using Qty = std::uint32_t;
using OrderId = std::uint64_t;
using SymbolId = std::uint16_t;

inline constexpr char kMsgAdd = 'A';
inline constexpr char kMsgExecute = 'E';
inline constexpr char kMsgCancel = 'X';
inline constexpr char kMsgDelete = 'D';

inline constexpr std::uint8_t kBuy = 0;
inline constexpr std::uint8_t kSell = 1;

#pragma pack(push, 1)
struct MdMsg {                // exchange -> feed handler ("the wire")
  char type;
  std::uint8_t side;
  SymbolId symbol;
  Qty qty;
  OrderId order_id;
  Price price;
  std::uint64_t ts_origin;    // t0: stamped by the exchange at send
};
#pragma pack(pop)
static_assert(sizeof(MdMsg) == 32, "MdMsg must stay two-per-cache-line");

struct BookUpdate {           // feed handler -> strategy (normalized top-of-book)
  std::uint64_t t0;           // exchange wire stamp
  std::uint64_t t1;           // feed handler: book updated
  Price bid_px, ask_px;
  std::uint64_t bid_qty, ask_qty;
  SymbolId symbol;
};

struct NewOrder {             // strategy -> gateway -> exchange
  OrderId client_id;
  std::uint64_t t0, t1, t2, t3;  // t2: strategy decision, t3: gateway out
  Price price;
  Price ref_px;               // touch price the decision used (risk collar ref)
  Qty qty;
  SymbolId symbol;
  std::uint8_t side;
  std::uint8_t tif;           // 0 = IOC (only type this sim supports)
};
static_assert(sizeof(NewOrder) == 64, "NewOrder should fill exactly one cache line");

enum class ExecStatus : std::uint8_t { Filled = 0, Killed = 1, Rejected = 2 };

struct ExecReport {           // exchange -> strategy
  OrderId client_id;
  Price fill_px;
  Qty fill_qty;
  SymbolId symbol;
  std::uint8_t side;
  ExecStatus status;
};

}  // namespace t2t
