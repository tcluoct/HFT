#pragma once
// plat/core/types.hpp — platform-wide vocabulary (ARCHITECTURE.md §5, §9.6).
//
// Fixed-point money: Price is int64 in units of 1e-4 CNY (0.1厘). Covers the
// full range from 0.01-CNY stock ticks to five-digit commodity prices with
// exact arithmetic; doubles never touch the trading path.

#include <cstdint>
#include <cstring>
#include <string_view>

namespace plat {

using Nanos = std::int64_t;  // session time: ns since midnight of the trading day
using Price = std::int64_t;  // 1 unit = 1e-4 CNY
using Qty   = std::int64_t;  // shares (stocks) or lots (futures)

inline constexpr std::int64_t kPxScale = 10'000;

// Config/report boundary only — the hot path never converts.
constexpr Price px_from_cny(double cny) noexcept {
  return static_cast<Price>(cny * static_cast<double>(kPxScale) + (cny >= 0 ? 0.5 : -0.5));
}
constexpr double px_to_cny(Price p) noexcept {
  return static_cast<double>(p) / static_cast<double>(kPxScale);
}

using InstrumentId = std::uint32_t;  // index into the per-day instrument snapshot
using AccountId    = std::uint16_t;
using StrategyId   = std::uint16_t;
using SiteId       = std::uint8_t;

inline constexpr InstrumentId kNoInstrument = 0xFFFF'FFFFu;

// Client order id: activation epoch in the top 16 bits (§11.2 — a standby
// takeover bumps the epoch so in-flight ids can never be reused), sequence
// in the low 48.
using OrderId = std::uint64_t;
inline constexpr OrderId kNoOrder = 0;

constexpr OrderId make_order_id(std::uint16_t epoch, std::uint64_t seq) noexcept {
  return (static_cast<OrderId>(epoch) << 48) | (seq & 0xFFFF'FFFF'FFFFull);
}
constexpr std::uint16_t order_epoch(OrderId id) noexcept {
  return static_cast<std::uint16_t>(id >> 48);
}

enum class Venue : std::uint8_t { SSE, SZSE, CFFEX, SHFE, INE, DCE, CZCE, GFEX };
inline constexpr std::size_t kVenueCount = 8;

constexpr bool is_stock_venue(Venue v) noexcept { return v == Venue::SSE || v == Venue::SZSE; }
constexpr bool is_futures_venue(Venue v) noexcept { return !is_stock_venue(v); }

constexpr std::string_view venue_name(Venue v) noexcept {
  switch (v) {
    case Venue::SSE:   return "SSE";
    case Venue::SZSE:  return "SZSE";
    case Venue::CFFEX: return "CFFEX";
    case Venue::SHFE:  return "SHFE";
    case Venue::INE:   return "INE";
    case Venue::DCE:   return "DCE";
    case Venue::CZCE:  return "CZCE";
    case Venue::GFEX:  return "GFEX";
  }
  return "?";
}

// Board decides price-limit and fee rules (§4.2).
enum class Board : std::uint8_t {
  MainBoard,   // ±10%
  Star,        // 科创板 ±20%
  ChiNext,     // 创业板 ±20%
  StSpecial,   // ST ±5%
  Etf,         // ±10%
  Future,      // venue/product-specific limit
  Option,      // data/hedge leg only in v1
};

enum class Side : std::uint8_t { Buy, Sell };
constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }
constexpr std::string_view side_name(Side s) noexcept { return s == Side::Buy ? "Buy" : "Sell"; }

// Futures position effect. Stocks always trade Offset::Open (the T+1 ledger
// in pnl decides sellability, not the order).
enum class Offset : std::uint8_t { Open, CloseToday, CloseYesterday };

enum class Tif : std::uint8_t { Day, Ioc };

// Price-limit state of an instrument's book — first-class because strategies
// and risk both branch on it (§4.2).
enum class LimitState : std::uint8_t { Normal, AtUpperLimit, AtLowerLimit };

}  // namespace plat
