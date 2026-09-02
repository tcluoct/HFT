#pragma once
// Core domain types. Integer ticks for prices, 32-bit index links for
// intrusive structures (half the size of pointers, relocatable,
// trivially serializable).

#include <cstdint>

namespace me {

using Price = std::int64_t;   // integer ticks
using Qty = std::uint32_t;
using OrderId = std::uint64_t;  // slotmap handle: (generation << kSlotBits) | slot
using Token = std::uint64_t;    // client-assigned order token
using ParticipantId = std::uint16_t;
using SymbolId = std::uint16_t;

inline constexpr std::uint32_t kNil = 0xFFFFFFFFu;  // null index link

inline constexpr std::uint8_t kBuy = 0;
inline constexpr std::uint8_t kSell = 1;

inline constexpr std::uint8_t kTifDay = 0;  // rest if not fully matched
inline constexpr std::uint8_t kTifIoc = 1;  // immediate-or-cancel
inline constexpr std::uint8_t kTifFok = 2;  // fill-or-kill (all or nothing)

enum class CancelReason : std::uint8_t {
  User = 0,       // client asked
  Ioc = 1,        // IOC remainder
  Fok = 2,        // FOK could not be fully filled
  SelfMatch = 3,  // self-match prevention canceled the resting order
};

enum class RejectReason : std::uint8_t {
  BadQty = 0,
  BadPrice = 1,
  BadSymbol = 2,
  UnknownOrder = 3,  // cancel/replace referenced a dead or never-live id
  PoolFull = 4,
};

}  // namespace me
