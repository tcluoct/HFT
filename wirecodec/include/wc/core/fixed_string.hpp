#pragma once
// Compile-time string usable as a non-type template parameter (C++20
// structural type). This is what lets a field be *named* in the schema —
// msg.get<"price">() — with the name resolved to an offset at compile time
// and gone from the binary.

#include <cstddef>

namespace wc {

template <std::size_t N>
struct FixedString {
  char v[N]{};

  consteval FixedString(const char (&s)[N]) {
    for (std::size_t i = 0; i < N; ++i) v[i] = s[i];
  }

  [[nodiscard]] static consteval std::size_t size() { return N - 1; }

  template <std::size_t M>
  [[nodiscard]] consteval bool equals(const FixedString<M>& o) const {
    if constexpr (N != M) {
      return false;
    } else {
      for (std::size_t i = 0; i < N; ++i)
        if (v[i] != o.v[i]) return false;
      return true;
    }
  }
};

}  // namespace wc
