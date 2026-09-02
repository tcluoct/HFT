#pragma once
// plat/ref/instrument.hpp — the instrument master (§9.6, symtab graduation).
// Reference data is compiled into an immutable per-day snapshot before the
// session; nothing on the hot path ever does a live reference lookup — an
// InstrumentId is an index into this table.

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "plat/core/hash.hpp"
#include "plat/core/types.hpp"

namespace plat {

struct Instrument {
  char symbol[16]{};
  InstrumentId id = kNoInstrument;
  Venue venue = Venue::SSE;
  Board board = Board::MainBoard;
  Price tick = 0;
  Qty lot = 0;              // order size granularity (shares per lot / 1 for futures)
  std::int32_t multiplier = 1;  // contract multiplier (1 for cash)
  Price prev_close = 0;
  Price upper_limit = 0;    // computed from board rules at snapshot build
  Price lower_limit = 0;
  std::uint32_t margin_bps = 0;  // futures initial margin, 1e-4 of notional

  [[nodiscard]] std::string_view sym() const noexcept { return symbol; }
  [[nodiscard]] bool is_stock() const noexcept {
    return board != Board::Future && board != Board::Option;
  }
};

// Price-limit band per board (§4.2), rounded to tick toward the inside so a
// limit price is always a legal tick.
constexpr std::uint32_t limit_bps_for(Board b) noexcept {
  switch (b) {
    case Board::MainBoard:  return 1000;   // ±10%
    case Board::Etf:        return 1000;
    case Board::Star:       return 2000;   // ±20%
    case Board::ChiNext:    return 2000;
    case Board::StSpecial:  return 500;    // ±5%
    case Board::Future:     return 800;    // representative ±8%; per-product in prod
    case Board::Option:     return 2000;
  }
  return 1000;
}

class InstrumentTable {
 public:
  // Registers an instrument and computes its limit band. Returns the id.
  InstrumentId add(std::string_view symbol, Venue venue, Board board, Price tick, Qty lot,
                   std::int32_t multiplier, Price prev_close, std::uint32_t margin_bps = 0) {
    Instrument in{};
    const auto n = symbol.size() < sizeof(in.symbol) - 1 ? symbol.size() : sizeof(in.symbol) - 1;
    std::memcpy(in.symbol, symbol.data(), n);
    in.id = static_cast<InstrumentId>(rows_.size());
    in.venue = venue;
    in.board = board;
    in.tick = tick;
    in.lot = lot;
    in.multiplier = multiplier;
    in.prev_close = prev_close;
    in.margin_bps = margin_bps;
    const auto bps = static_cast<std::int64_t>(limit_bps_for(board));
    const Price up_raw = prev_close + prev_close * bps / 10'000;
    const Price dn_raw = prev_close - prev_close * bps / 10'000;
    in.upper_limit = up_raw - up_raw % tick;                          // round down
    in.lower_limit = dn_raw % tick ? dn_raw + (tick - dn_raw % tick)  // round up
                                   : dn_raw;
    rows_.push_back(in);
    return in.id;
  }

  // Production override: the venue publishes the authoritative price-limit
  // band (CTP depth query UpperLimitPrice/LowerLimitPrice); it supersedes the
  // board approximation computed by add().
  void set_limit_band(InstrumentId id, Price upper, Price lower) noexcept {
    rows_[id].upper_limit = upper;
    rows_[id].lower_limit = lower;
  }
  void set_prev_close(InstrumentId id, Price prev_close) noexcept {
    rows_[id].prev_close = prev_close;
  }

  [[nodiscard]] const Instrument& at(InstrumentId id) const noexcept { return rows_[id]; }
  [[nodiscard]] std::size_t size() const noexcept { return rows_.size(); }

  [[nodiscard]] InstrumentId find(std::string_view symbol) const noexcept {
    for (const auto& r : rows_)
      if (r.sym() == symbol) return r.id;
    return kNoInstrument;
  }

  [[nodiscard]] std::uint64_t state_hash() const noexcept {
    StateHash h;
    for (const auto& r : rows_) h.mix(r);
    return h.value();
  }

  // Flat-POD serialization: the per-day snapshot is persisted next to the
  // journal so replay/recovery reconstructs the exact table the session used.
  void save(std::vector<std::uint8_t>& out) const {
    const std::uint64_t n = rows_.size();
    const auto* nb = reinterpret_cast<const std::uint8_t*>(&n);
    out.insert(out.end(), nb, nb + sizeof(n));
    const auto* rb = reinterpret_cast<const std::uint8_t*>(rows_.data());
    out.insert(out.end(), rb, rb + rows_.size() * sizeof(Instrument));
  }
  bool load(const std::uint8_t* p, std::size_t avail) {
    std::uint64_t n = 0;
    if (avail < sizeof(n)) return false;
    std::memcpy(&n, p, sizeof(n));
    if (avail < sizeof(n) + n * sizeof(Instrument)) return false;
    rows_.resize(n);
    std::memcpy(rows_.data(), p + sizeof(n), n * sizeof(Instrument));
    return true;
  }

 private:
  std::vector<Instrument> rows_;
};

}  // namespace plat
