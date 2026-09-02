#pragma once
// Declarative wire schema -> zero-copy codec, computed entirely at compile
// time (SBE's idea, done with C++23 instead of an XML code generator).
//
// A message is a type: Message<"NewOrder", 'N', ByteOrder::kBig,
//   Field<"cl_ord_id", u64>, Field<"side", u8>, ...>.
// From that one declaration, consteval functions derive every field's byte
// offset in the packed wire layout, so enc.set<"price">(v) compiles to a
// byteswap + one unaligned store at a constant offset — there is no
// serialization loop, no reflection table, no runtime schema anywhere in
// the binary. A misspelled field name or a duplicate field is a compile
// error, not a malformed order on the wire.
//
// Layouts are packed (offset = sum of preceding sizes, no padding), like
// real exchange protocols. Unaligned scalar access costs ~nothing on x86-64;
// memcpy through load_raw/store_raw keeps it defined behavior everywhere.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <tuple>
#include <type_traits>

#include "hft/common/platform.hpp"
#include "wc/core/fixed_string.hpp"

namespace wc {

enum class ByteOrder : std::uint8_t { kLittle, kBig };

template <FixedString Name, typename T>
  requires std::is_integral_v<T>
struct Field {
  static constexpr auto name = Name;
  using type = T;
  static constexpr std::size_t size = sizeof(T);
};

// --- unaligned, endian-aware scalar access -------------------------------

template <typename T>
HFT_FORCE_INLINE T load_raw(const std::byte* p) noexcept {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}

template <typename T>
HFT_FORCE_INLINE void store_raw(std::byte* p, T v) noexcept {
  std::memcpy(p, &v, sizeof v);
}

// std::byteswap (C++23) on the unsigned image; single BSWAP/MOVBE-class
// instruction, identity when the wire order is native or the field is 1 byte.
template <ByteOrder Order, typename T>
[[nodiscard]] constexpr T wire_swap(T v) noexcept {
  constexpr bool native_little = (std::endian::native == std::endian::little);
  constexpr bool need = (sizeof(T) > 1) &&
                        ((Order == ByteOrder::kBig) == native_little);
  if constexpr (need) {
    using U = std::make_unsigned_t<T>;
    return std::bit_cast<T>(std::byteswap(std::bit_cast<U>(v)));
  } else {
    return v;
  }
}

// --- the message schema ---------------------------------------------------

template <FixedString Name, std::uint16_t MsgType, ByteOrder Order, typename... Fields>
struct Message {
  static constexpr auto name = Name;
  static constexpr std::uint16_t msg_type = MsgType;
  static constexpr ByteOrder order = Order;
  static constexpr std::size_t field_count = sizeof...(Fields);
  static constexpr std::size_t body_size = (std::size_t{0} + ... + Fields::size);

  // Resolve a field name to its pack index. consteval: a name that matches
  // zero fields (typo) or two (duplicate) fails the build here.
  template <FixedString N>
  static consteval std::size_t index_of() {
    constexpr std::size_t n = sizeof...(Fields);
    constexpr bool match[] = {Fields::name.equals(N)...};
    std::size_t idx = n, cnt = 0;
    for (std::size_t i = 0; i < n; ++i)
      if (match[i]) {
        if (cnt++ == 0) idx = i;
      }
    if (cnt != 1) throw "field name must match exactly one field";
    return idx;
  }

  template <FixedString N>
  using field_type =
      typename std::tuple_element_t<index_of<N>(), std::tuple<Fields...>>::type;

  // Packed offset: sum of the sizes of everything declared before the field.
  template <FixedString N>
  static consteval std::size_t offset_of() {
    constexpr std::size_t sizes[] = {Fields::size...};
    std::size_t off = 0;
    for (std::size_t i = 0; i < index_of<N>(); ++i) off += sizes[i];
    return off;
  }
};

// --- flyweights ------------------------------------------------------------
// Neither owns memory and neither copies the body: an Encoder is a typed pen
// over the caller's tx buffer, a View is a typed lens over the rx bytes.

template <typename Msg>
class Encoder {
 public:
  explicit Encoder(std::span<std::byte> body) noexcept : p_(body.data()) {}

  template <FixedString N>
  HFT_FORCE_INLINE Encoder& set(typename Msg::template field_type<N> v) noexcept {
    store_raw(p_ + Msg::template offset_of<N>(), wire_swap<Msg::order>(v));
    return *this;  // chainable: enc.set<"qty">(q).set<"price">(p)
  }

 private:
  std::byte* p_;
};

template <typename Msg>
class View {
 public:
  explicit View(std::span<const std::byte> body) noexcept : p_(body.data()) {}

  template <FixedString N>
  [[nodiscard]] HFT_FORCE_INLINE auto get() const noexcept {
    using T = typename Msg::template field_type<N>;
    return wire_swap<Msg::order>(load_raw<T>(p_ + Msg::template offset_of<N>()));
  }

 private:
  const std::byte* p_;
};

}  // namespace wc
