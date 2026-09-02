#pragma once
// plat/ctp/conv.hpp — pure CTP ⇄ platform conversions. Everything here is a
// function of its arguments (no api handles, no state), so the whole mapping
// layer is unit-tested without a counter connection. The adapters
// (md_client.hpp / trader_client.hpp) contain only lifecycle + plumbing.
//
// CTP conventions handled here:
//   · prices are doubles in CNY; absent values are DBL_MAX (or 0 on some
//     fronts) — both map to Price 0, "no level";
//   · times are "HH:MM:SS" strings plus a millisecond field;
//   · enums are single chars (THOST_FTDC_* defines).

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ThostFtdcUserApiDataType.h"
#include "plat/core/time.hpp"
#include "plat/core/types.hpp"

namespace plat::ctp {

// ---------------------------------------------------------------- prices

// CTP double CNY → fixed-point 1e-4 CNY. DBL_MAX means "no such level".
inline Price px_from_ctp(double v) noexcept {
  if (v >= DBL_MAX / 2 || !(v > -DBL_MAX / 2) || std::isnan(v)) return 0;
  return static_cast<Price>(std::llround(v * static_cast<double>(kPxScale)));
}

inline double px_to_ctp(Price p) noexcept {
  return static_cast<double>(p) / static_cast<double>(kPxScale);
}

// Margin ratio double (0.12 = 12%) → basis points ×1e-4 (1200).
inline std::uint32_t margin_bps_from_ratio(double ratio) noexcept {
  if (!(ratio > 0.0) || ratio > 1.0) return 0;
  return static_cast<std::uint32_t>(std::llround(ratio * 10'000.0));
}

// ---------------------------------------------------------------- time

// "HH:MM:SS" + millisec → ns since midnight. Malformed input → -1.
inline Nanos ns_from_ctp_time(const char* hhmmss, int millisec) noexcept {
  if (!hhmmss || std::strlen(hhmmss) < 8) return -1;
  const auto d = [&](int i) { return hhmmss[i] - '0'; };
  const int hh = d(0) * 10 + d(1), mm = d(3) * 10 + d(4), ss = d(6) * 10 + d(7);
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return -1;
  return at_time(hh, mm, ss, static_cast<Nanos>(millisec) * kNsPerMs);
}

// ---------------------------------------------------------------- enums

inline char side_to_ctp(Side s) noexcept {
  return s == Side::Buy ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
}
inline Side side_from_ctp(char d) noexcept {
  return d == THOST_FTDC_D_Buy ? Side::Buy : Side::Sell;
}

inline char offset_to_ctp(Offset o) noexcept {
  switch (o) {
    case Offset::Open:           return THOST_FTDC_OF_Open;
    case Offset::CloseToday:     return THOST_FTDC_OF_CloseToday;
    case Offset::CloseYesterday: return THOST_FTDC_OF_CloseYesterday;
  }
  return THOST_FTDC_OF_Open;
}
inline Offset offset_from_ctp(char o) noexcept {
  switch (o) {
    case THOST_FTDC_OF_CloseToday:     return Offset::CloseToday;
    case THOST_FTDC_OF_CloseYesterday: return Offset::CloseYesterday;
    case THOST_FTDC_OF_Close:          return Offset::CloseYesterday;  // 平仓 = 先平昨
    default:                           return Offset::Open;
  }
}

inline Venue venue_from_exchange_id(const char* ex) noexcept {
  if (!ex) return Venue::SHFE;
  if (std::strcmp(ex, "SHFE") == 0)  return Venue::SHFE;
  if (std::strcmp(ex, "INE") == 0)   return Venue::INE;
  if (std::strcmp(ex, "DCE") == 0)   return Venue::DCE;
  if (std::strcmp(ex, "CZCE") == 0)  return Venue::CZCE;
  if (std::strcmp(ex, "CFFEX") == 0) return Venue::CFFEX;
  if (std::strcmp(ex, "GFEX") == 0)  return Venue::GFEX;
  return Venue::SHFE;
}

// ---------------------------------------------------------------- order ids

// OrderRef carries our 48-bit order sequence as decimal (fits: 2^48 < 1e15,
// TThostFtdcOrderRefType is char[13] → up to 12 digits ≈ 1e12 orders/day).
// The epoch half of the OrderId travels in the registry, not the wire, so a
// takeover with a bumped epoch can never collide on OrderRef within one
// (FrontID, SessionID) scope — a new login is a new scope anyway.
inline void order_ref_from_seq(std::uint64_t seq, char out[13]) noexcept {
  std::snprintf(out, 13, "%llu", static_cast<unsigned long long>(seq));
}
inline std::uint64_t seq_from_order_ref(const char* ref) noexcept {
  if (!ref) return 0;
  std::uint64_t v = 0;
  for (const char* p = ref; *p; ++p) {
    if (*p < '0' || *p > '9') return 0;  // not ours (manual/other-session order)
    v = v * 10 + static_cast<std::uint64_t>(*p - '0');
  }
  return v;
}

// OrderSysID ("       12345" — venue-scoped, space-padded) → numeric venue_no
// for the OMS/report path. Non-digits are skipped; 0 means "no id yet".
inline std::uint64_t venue_no_from_sys_id(const char* sys_id) noexcept {
  if (!sys_id) return 0;
  std::uint64_t v = 0;
  for (const char* p = sys_id; *p; ++p)
    if (*p >= '0' && *p <= '9') v = v * 10 + static_cast<std::uint64_t>(*p - '0');
  return v;
}

// yyyymmdd string → u32 (journal day tag). Malformed → 0.
inline std::uint32_t day_from_ctp(const char* yyyymmdd) noexcept {
  if (!yyyymmdd || std::strlen(yyyymmdd) < 8) return 0;
  std::uint32_t v = 0;
  for (int i = 0; i < 8; ++i) {
    if (yyyymmdd[i] < '0' || yyyymmdd[i] > '9') return 0;
    v = v * 10 + static_cast<std::uint32_t>(yyyymmdd[i] - '0');
  }
  return v;
}

}  // namespace plat::ctp
