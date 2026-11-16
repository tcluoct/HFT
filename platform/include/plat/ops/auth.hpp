#pragma once
// plat/ops/auth.hpp — HTTP Basic authentication for the platops console.
//
// The ops console can halt, flatten and place risk-checked manual orders, so
// it is an authenticated control plane, not a dashboard: every request carries
// `Authorization: Basic base64(user:password)`. Basic auth is deliberate — it
// is one header the browser sends automatically for the same origin, and a
// cross-origin page cannot attach it without a CORS preflight the server never
// answers, which closes the CSRF hole a token-in-cookie design would leave.
//
// The header value is compared in constant time; the server never decodes it
// (it compares against the precomputed base64 of the configured credentials).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace plat::ops {

inline std::string base64_encode(std::string_view in) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const std::uint32_t v = (static_cast<std::uint8_t>(in[i]) << 16) |
                            (static_cast<std::uint8_t>(in[i + 1]) << 8) |
                            static_cast<std::uint8_t>(in[i + 2]);
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.push_back(kAlphabet[(v >> 6) & 63]);
    out.push_back(kAlphabet[v & 63]);
  }
  if (i < in.size()) {
    const std::uint32_t v = static_cast<std::uint8_t>(in[i]) << 16;
    const bool two = i + 1 < in.size();
    const std::uint32_t v2 = two ? v | (static_cast<std::uint8_t>(in[i + 1]) << 8) : v;
    out.push_back(kAlphabet[(v2 >> 18) & 63]);
    out.push_back(kAlphabet[(v2 >> 12) & 63]);
    out.push_back(two ? kAlphabet[(v2 >> 6) & 63] : '=');
    out.push_back('=');
  }
  return out;
}

// "Basic <base64(user:password)>" — the exact header value clients must send.
inline std::string basic_token(std::string_view user, std::string_view pass) {
  std::string raw(user);
  raw.push_back(':');
  raw.append(pass);
  return "Basic " + base64_encode(raw);
}

inline bool ascii_iequal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const char ca = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
    const char cb = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
    if (ca != cb) return false;
  }
  return true;
}

// Constant-time comparison of two same-purpose secrets (no early exit).
inline bool secret_equal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  unsigned diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i)
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  return diff == 0;
}

// True when auth is disabled (empty expected token) or the request's
// Authorization header is exactly the configured Basic credential.
inline bool basic_auth_ok(std::string_view authorization,
                          std::string_view expected_token) noexcept {
  if (expected_token.empty()) return true;  // explicitly disabled (loopback dev)
  const std::size_t esp = expected_token.find(' ');
  if (esp == std::string_view::npos) return false;
  if (!ascii_iequal(expected_token.substr(0, esp), "Basic")) return false;
  const std::size_t sp = authorization.find(' ');
  if (sp == std::string_view::npos) return false;
  if (!ascii_iequal(authorization.substr(0, sp), "Basic")) return false;
  return secret_equal(authorization.substr(sp + 1), expected_token.substr(esp + 1));
}

}  // namespace plat::ops
