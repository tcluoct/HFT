#pragma once
#include <cstdint>
#include "plat/core/types.hpp"

namespace plat {
// Symmetric inclusive band. Integer quotient/remainder avoids overflow in
// reference*bps, and inward rounding never admits an extra price unit.
constexpr bool auction_price_ok(Price price, Price reference, std::uint32_t bps) noexcept {
  if (price <= 0 || reference <= 0 || bps == 0 || bps > 10'000) return false;
  const Price width = (reference / 10'000) * bps + (reference % 10'000) * bps / 10'000;
  const Price deviation = price >= reference ? price - reference : reference - price;
  return deviation <= width;
}
} // namespace plat
