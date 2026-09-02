#pragma once
// plat/core/phase.hpp — venue session-phase state machine (§4.2, riskgate
// check #1). Which order actions are legal depends on the phase; the schedule
// is data, built per venue class.

#include <array>
#include <cstdint>
#include <string_view>

#include "plat/core/time.hpp"
#include "plat/core/types.hpp"

namespace plat {

enum class Phase : std::uint8_t {
  PreOpen,             // session not yet accepting orders
  OpenAuction,         // call auction, orders + cancels accepted
  OpenAuctionLocked,   // call auction, cancels prohibited (stocks 09:20–09:25)
  Continuous,          // continuous matching
  Break,               // lunch / intra-day pause: nothing accepted
  CloseAuction,        // closing call auction, cancels prohibited
  Closed,
};

constexpr std::string_view phase_name(Phase p) noexcept {
  switch (p) {
    case Phase::PreOpen:           return "PreOpen";
    case Phase::OpenAuction:       return "OpenAuction";
    case Phase::OpenAuctionLocked: return "OpenAuctionLocked";
    case Phase::Continuous:        return "Continuous";
    case Phase::Break:             return "Break";
    case Phase::CloseAuction:      return "CloseAuction";
    case Phase::Closed:            return "Closed";
  }
  return "?";
}

constexpr bool phase_accepts_orders(Phase p) noexcept {
  return p == Phase::OpenAuction || p == Phase::OpenAuctionLocked || p == Phase::Continuous ||
         p == Phase::CloseAuction;
}
constexpr bool phase_accepts_cancels(Phase p) noexcept {
  return p == Phase::OpenAuction || p == Phase::Continuous;
}
// Only continuous phases match on arrival; auctions match at phase end.
constexpr bool phase_matches_continuously(Phase p) noexcept { return p == Phase::Continuous; }

// A day's phase timetable: ordered transition list. Fixed capacity — this is
// reference data, loaded at start of day (§9.6), never grown intraday.
struct PhaseSchedule {
  struct Entry {
    Nanos when;
    Phase phase;
  };
  std::array<Entry, 12> entries{};
  std::size_t count = 0;

  void add(Nanos when, Phase phase) noexcept {
    if (count < entries.size()) entries[count++] = {when, phase};
  }
  [[nodiscard]] Phase phase_at(Nanos t) const noexcept {
    Phase p = Phase::PreOpen;
    for (std::size_t i = 0; i < count; ++i) {
      if (entries[i].when <= t) p = entries[i].phase;
      else break;
    }
    return p;
  }
};

// SSE/SZSE cash session (§4.2): 09:15 auction, 09:20 cancel lock, 09:25–09:30
// pre-open pause, continuous to 14:57, closing auction to 15:00.
inline PhaseSchedule stock_schedule() {
  PhaseSchedule s;
  s.add(at_time(9, 15), Phase::OpenAuction);
  s.add(at_time(9, 20), Phase::OpenAuctionLocked);
  s.add(at_time(9, 25), Phase::Break);
  s.add(at_time(9, 30), Phase::Continuous);
  s.add(at_time(11, 30), Phase::Break);
  s.add(at_time(13, 0), Phase::Continuous);
  s.add(at_time(14, 57), Phase::CloseAuction);
  s.add(at_time(15, 0), Phase::Closed);
  return s;
}

// Futures day session (simplified single-segment day; night sessions are the
// same machine with a different timetable): 08:55 auction, 09:00 continuous,
// 10:15–10:30 pause, 11:30–13:30 lunch, close 15:00.
inline PhaseSchedule futures_schedule() {
  PhaseSchedule s;
  s.add(at_time(8, 55), Phase::OpenAuction);
  s.add(at_time(8, 59), Phase::OpenAuctionLocked);
  s.add(at_time(9, 0), Phase::Continuous);
  s.add(at_time(10, 15), Phase::Break);
  s.add(at_time(10, 30), Phase::Continuous);
  s.add(at_time(11, 30), Phase::Break);
  s.add(at_time(13, 30), Phase::Continuous);
  s.add(at_time(15, 0), Phase::Closed);
  return s;
}

}  // namespace plat
