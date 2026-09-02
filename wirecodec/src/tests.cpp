// Scenario tests for the wire codec. The wire contract is asserted three
// independent ways: compile-time (static_asserts in the schema), exact
// hand-written byte images, and a bitwise software CRC32-C cross-checking
// the hardware one. No test framework — a CHECK macro.

#include <array>
#include <cstdio>
#include <cstring>
#include <span>

#include "wc/core/frame.hpp"
#include "wc/proto/orders.hpp"

using namespace wc;
using namespace wc::proto;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_fail;                                                     \
    }                                                               \
  } while (0)

// Bitwise reference CRC32-C (reflected poly 0x82F63B78): slow, obviously
// correct, and independent of the SSE4.2 path under test.
std::uint32_t crc32c_ref(std::span<const std::byte> d) {
  std::uint32_t c = 0xFFFFFFFFu;
  for (const std::byte b : d) {
    c ^= static_cast<std::uint8_t>(b);
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
  }
  return ~c;
}

void t_layout_contract() {
  std::puts("schema layout matches the wire contract (also static_assert'd)");
  CHECK(NewOrder::body_size == 26);
  CHECK(NewOrder::offset_of<"qty">() == 13);  // straddles alignment on purpose
  CHECK(NewOrder::offset_of<"price">() == 17);
  CHECK(CancelOrder::offset_of<"qty">() == 8);  // same name, message-local offset
  CHECK(kFrameSize<NewOrder> == 4 + 26 + 4);
}

void t_round_trip() {
  std::puts("encode -> view round-trips every field, edges included");
  std::array<std::byte, NewOrder::body_size> body{};
  Encoder<NewOrder>(body)
      .set<"cl_ord_id">(~std::uint64_t{0})
      .set<"symbol">(0xDEADBEEFu)
      .set<"side">(1)
      .set<"qty">(0)
      .set<"price">(-987'654'321'012ll)
      .set<"tif">(3);
  View<NewOrder> v{std::span<const std::byte>(body)};
  CHECK(v.get<"cl_ord_id">() == ~std::uint64_t{0});
  CHECK(v.get<"symbol">() == 0xDEADBEEFu);
  CHECK(v.get<"side">() == 1);
  CHECK(v.get<"qty">() == 0);
  CHECK(v.get<"price">() == -987'654'321'012ll);
  CHECK(v.get<"tif">() == 3);
}

void t_exact_bytes() {
  std::puts("encoded bytes match a hand-written big-endian image");
  std::array<std::byte, kFrameSize<CancelOrder>> buf{};
  begin_frame<CancelOrder>(buf).set<"cl_ord_id">(0x0102030405060708ull).set<"qty">(0x0A0B0C0Du);
  seal_frame<CancelOrder>(buf);

  const std::uint8_t expect[16] = {
      0x00, 'C',                                       // msg_type, big-endian
      0x00, 0x0C,                                      // body_len = 12
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // cl_ord_id
      0x0A, 0x0B, 0x0C, 0x0D,                          // qty
  };
  CHECK(std::memcmp(buf.data(), expect, sizeof expect) == 0);

  // Trailer: hardware CRC32-C must agree with the bitwise reference.
  const auto stored =
      wire_swap<ByteOrder::kBig>(load_raw<std::uint32_t>(buf.data() + sizeof expect));
  CHECK(stored == crc32c_ref(std::span<const std::byte>(buf).first(sizeof expect)));
}

void t_hw_crc_matches_reference() {
  std::puts("hardware CRC32-C == bitwise reference on assorted lengths");
  std::array<std::byte, 61> d;  // deliberately not a multiple of 8
  for (std::size_t i = 0; i < d.size(); ++i) d[i] = static_cast<std::byte>(i * 37 + 11);
  for (std::size_t n : {0u, 1u, 7u, 8u, 9u, 32u, 61u}) {
    const auto s = std::span<const std::byte>(d).first(n);
    CHECK(crc32c(s) == crc32c_ref(s));
  }
}

void t_flyweight_over_foreign_bytes() {
  std::puts("view reads a buffer built by hand (no Encoder involved)");
  const std::uint8_t raw[12] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x2A,               // cl_ord_id = 42
                                0x00, 0x00, 0x01, 0x00};  // qty = 256
  View<CancelOrder> v{std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw), 12)};
  CHECK(v.get<"cl_ord_id">() == 42);
  CHECK(v.get<"qty">() == 256);
}

void t_frame_validation() {
  std::puts("parser: ok / corrupted byte / truncated stream");
  std::array<std::byte, kFrameSize<CancelOrder>> buf{};
  begin_frame<CancelOrder>(buf).set<"cl_ord_id">(7).set<"qty">(9);
  seal_frame<CancelOrder>(buf);

  FrameInfo fi;
  CHECK(try_parse(buf, fi) == FrameError::kOk);
  CHECK(fi.msg_type == 'C');
  CHECK(fi.body.size() == CancelOrder::body_size);
  CHECK(fi.frame_len == buf.size());

  auto bad = buf;
  bad[6] ^= std::byte{0x01};  // one bit, anywhere
  CHECK(try_parse(bad, fi) == FrameError::kBadCrc);

  CHECK(try_parse(std::span<const std::byte>(buf).first(buf.size() - 1), fi) ==
        FrameError::kTruncated);
  CHECK(try_parse(std::span<const std::byte>(buf).first(3), fi) == FrameError::kTruncated);
}

void t_back_to_back_frames() {
  std::puts("mixed frames parse back-to-back off one rx buffer");
  std::array<std::byte, kFrameSize<NewOrder> + kFrameSize<CancelOrder> + kFrameSize<ExecReport>>
      buf{};
  std::size_t w = 0;
  begin_frame<NewOrder>(std::span(buf).subspan(w))
      .set<"cl_ord_id">(1)
      .set<"symbol">(77)
      .set<"side">(0)
      .set<"qty">(100)
      .set<"price">(10'000)
      .set<"tif">(0);
  seal_frame<NewOrder>(std::span(buf).subspan(w));
  w += kFrameSize<NewOrder>;
  begin_frame<CancelOrder>(std::span(buf).subspan(w)).set<"cl_ord_id">(1).set<"qty">(0);
  seal_frame<CancelOrder>(std::span(buf).subspan(w));
  w += kFrameSize<CancelOrder>;
  begin_frame<ExecReport>(std::span(buf).subspan(w))
      .set<"cl_ord_id">(1)
      .set<"exec_id">(555)
      .set<"fill_qty">(40)
      .set<"fill_px">(9'999)
      .set<"leaves">(60);
  seal_frame<ExecReport>(std::span(buf).subspan(w));

  std::span<const std::byte> rx(buf);
  int news = 0, cancels = 0, execs = 0;
  FrameInfo fi;
  while (try_parse(rx, fi) == FrameError::kOk) {
    switch (fi.msg_type) {
      case 'N': {
        ++news;
        CHECK(View<NewOrder>(fi.body).get<"qty">() == 100);
        break;
      }
      case 'C': {
        ++cancels;
        CHECK(View<CancelOrder>(fi.body).get<"cl_ord_id">() == 1);
        break;
      }
      case 'E': {
        ++execs;
        CHECK(View<ExecReport>(fi.body).get<"leaves">() == 60);
        break;
      }
      default:
        CHECK(false);
    }
    rx = rx.subspan(fi.frame_len);
  }
  CHECK(rx.empty());
  CHECK(news == 1 && cancels == 1 && execs == 1);
}

void t_wire_swap() {
  std::puts("wire_swap: involution on wide types, identity on bytes");
  CHECK(wire_swap<ByteOrder::kBig>(wire_swap<ByteOrder::kBig>(0x1122334455667788ull)) ==
        0x1122334455667788ull);
  CHECK(wire_swap<ByteOrder::kBig>(std::uint8_t{0xAB}) == 0xAB);
  CHECK(wire_swap<ByteOrder::kLittle>(0x11223344u) == 0x11223344u);  // native
  CHECK(wire_swap<ByteOrder::kBig>(std::uint16_t{0x1234}) == 0x3412);
}

}  // namespace

int main() {
  std::puts("wirecodec scenario tests");
  std::puts("========================");
  t_layout_contract();
  t_round_trip();
  t_exact_bytes();
  t_hw_crc_matches_reference();
  t_flyweight_over_foreign_bytes();
  t_frame_validation();
  t_back_to_back_frames();
  t_wire_swap();
  std::puts("========================");
  if (g_fail == 0) {
    std::puts("ALL TESTS PASSED");
    return 0;
  }
  std::printf("%d CHECKS FAILED\n", g_fail);
  return 1;
}
