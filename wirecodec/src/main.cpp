// wirecodec harness: encodes and decodes NewOrder frames over a 4096-slot
// arena (136 KB — L2-resident, so the numbers are codec cost, not DRAM
// misses). A single encode or decode is a handful of nanoseconds — well
// under the ~20 ns rdtscp floor on this box — so latency is sampled in
// 16-message batches and reported per message; throughput is measured over
// the whole loop.

#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>

#include "hft/common/histogram.hpp"
#include "hft/common/platform.hpp"
#include "hft/common/tsc_clock.hpp"
#include "wc/core/frame.hpp"
#include "wc/proto/orders.hpp"

namespace {

using namespace wc;
using namespace wc::proto;

struct Args {
  std::uint64_t msgs = 50'000'000;
  std::uint64_t warmup = 5'000'000;
  unsigned core = 2;
  bool pin = true;
  bool rt = false;
  bool crc = true;
};

bool parse_u64(const char* s, std::uint64_t& v) {
  const auto r = std::from_chars(s, s + std::strlen(s), v);
  return r.ec == std::errc{} && *r.ptr == '\0';
}

bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const char* s = argv[i];
    std::uint64_t n{};
    if (!std::strcmp(s, "--msgs") && i + 1 < argc && parse_u64(argv[++i], a.msgs)) continue;
    if (!std::strcmp(s, "--warmup") && i + 1 < argc && parse_u64(argv[++i], a.warmup)) continue;
    if (!std::strcmp(s, "--core") && i + 1 < argc && parse_u64(argv[++i], n)) {
      a.core = static_cast<unsigned>(n);
      continue;
    }
    if (!std::strcmp(s, "--no-pin")) {
      a.pin = false;
      continue;
    }
    if (!std::strcmp(s, "--rt")) {
      a.rt = true;
      continue;
    }
    if (!std::strcmp(s, "--no-crc")) {
      a.crc = false;
      continue;
    }
    std::printf(
        "wirecodec [--msgs N] [--warmup N] [--core N] [--no-pin] [--rt] [--no-crc]\n"
        "  --no-crc   skip seal/verify to isolate field codec cost from CRC32-C\n");
    return false;
  }
  return true;
}

constexpr std::size_t kSlots = 4096;
constexpr std::size_t kStride = kFrameSize<NewOrder>;  // 34 bytes, packed

struct Report {
  const char* name;
  hft::LatencyHistogram batch_lat;  // cycles per 16-message batch
  std::uint64_t total_cycles = 0;
  std::uint64_t msgs = 0;

  void print() const {
    auto ns16 = [&](std::uint64_t cyc) { return hft::TscClock::to_ns(cyc) / 16.0; };
    const double secs =
        static_cast<double>(total_cycles) / (hft::TscClock::cycles_per_ns() * 1e9);
    const double mps = static_cast<double>(msgs) / secs;
    std::printf(
        "%-22s %6.1fM msg/s  %7.2f GB/s   ns/msg: p50 %5.1f  p99 %5.1f  p99.9 %6.1f\n",
        name, mps / 1e6, mps * kStride / 1e9, ns16(batch_lat.percentile(0.50)),
        ns16(batch_lat.percentile(0.99)), ns16(batch_lat.percentile(0.999)));
  }
};

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return 1;
  std::printf("wirecodec: %llu msgs (%llu warmup), NewOrder frames (%zu B), crc %s\n",
              static_cast<unsigned long long>(a.msgs),
              static_cast<unsigned long long>(a.warmup), kStride, a.crc ? "on" : "off");

  hft::TscClock::calibrate();
  if (a.pin && !hft::pin_current_thread(a.core)) std::puts("warn: pin failed");
  if (a.rt && !hft::boost_current_thread_priority()) std::puts("warn: rt priority failed");

  auto arena = std::make_unique<std::byte[]>(kSlots * kStride);
  auto slot = [&](std::uint64_t i) {
    return std::span<std::byte>(arena.get() + (i & (kSlots - 1)) * kStride, kStride);
  };

  const std::uint64_t total = a.warmup + a.msgs;

  // ---- encode ----
  Report enc{"encode+seal"};
  if (!a.crc) enc.name = "encode (no crc)";
  for (std::uint64_t i = 0; i < total; i += 16) {
    const std::uint64_t t0 = hft::rdtscp();
    for (std::uint64_t j = i; j < i + 16; ++j) {
      auto e = begin_frame<NewOrder>(slot(j));
      e.set<"cl_ord_id">(j)
          .set<"symbol">(static_cast<std::uint32_t>(j) * 2654435761u)
          .set<"side">(static_cast<std::uint8_t>(j & 1))
          .set<"qty">(static_cast<std::uint32_t>(j % 997) + 1)
          .set<"price">(10'000 + static_cast<std::int64_t>(j % 201) - 100)
          .set<"tif">(0);
      if (a.crc) seal_frame<NewOrder>(slot(j));
    }
    const std::uint64_t t1 = hft::rdtscp();
    if (i >= a.warmup) {
      enc.batch_lat.record(t1 - t0);
      enc.total_cycles += t1 - t0;
      enc.msgs += 16;
    }
  }

  // ---- decode ----
  Report dec{"parse+decode"};
  if (!a.crc) dec.name = "decode (no crc)";
  std::uint64_t sum = 0, parse_fail = 0;
  for (std::uint64_t i = 0; i < total; i += 16) {
    const std::uint64_t t0 = hft::rdtscp();
    for (std::uint64_t j = i; j < i + 16; ++j) {
      const auto s = slot(j);
      std::span<const std::byte> body;
      if (a.crc) {
        FrameInfo fi;
        if (try_parse(s, fi) != FrameError::kOk) [[unlikely]] {
          ++parse_fail;
          continue;
        }
        body = fi.body;
      } else {
        body = std::span<const std::byte>(s).subspan(kHeaderSize, NewOrder::body_size);
      }
      View<NewOrder> v(body);
      sum += v.get<"cl_ord_id">() + v.get<"symbol">() + v.get<"side">() + v.get<"qty">() +
             static_cast<std::uint64_t>(v.get<"price">()) + v.get<"tif">();
    }
    const std::uint64_t t1 = hft::rdtscp();
    if (i >= a.warmup) {
      dec.batch_lat.record(t1 - t0);
      dec.total_cycles += t1 - t0;
      dec.msgs += 16;
    }
  }

  std::puts("\nper-message cost from 16-message batches (arena is L2-resident):");
  enc.print();
  dec.print();
  std::printf("\nintegrity: parse failures %llu, field checksum %llx (DCE guard)\n",
              static_cast<unsigned long long>(parse_fail),
              static_cast<unsigned long long>(sum));
  return parse_fail ? 1 : 0;
}
