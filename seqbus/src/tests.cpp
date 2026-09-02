// Scenario tests for the bus protocol. A transport is correctness-critical
// infrastructure: every protocol behavior — ordering, wrap, late join, lap
// detection, in-progress visibility — gets a deterministic scripted scenario,
// and the two properties that cannot be scripted (torn reads and loss
// accounting under real contention) get stress tests that hammer a tiny ring
// and assert the invariants exactly. No test framework — a CHECK macro,
// like matchd.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "hft/common/platform.hpp"
#include "sb/core/bus.hpp"
#include "sb/core/shm.hpp"
#include "sb/harness/msg.hpp"

using namespace sb;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_fail;                                                     \
    }                                                               \
  } while (0)

// Each scenario gets its own uniquely named segment (pid + counter) so runs
// never collide with each other or with a concurrently running bench.
int g_seg = 0;
std::string seg_name() { return "t" + std::to_string(hft::current_pid()) + "_" + std::to_string(g_seg++); }

struct TestBus {
  SharedSegment seg;
  Bus bus;
};

TestBus make_bus(std::uint64_t capacity) {
  TestBus t;
  t.seg = SharedSegment::create(seg_name(), bus_bytes(capacity));
  if (t.seg.valid()) t.bus = Bus::format(t.seg.data(), t.seg.size(), capacity);
  return t;
}

BusMsg pub(Writer& w, std::uint64_t seq) {
  const BusMsg m = make_msg(seq, MsgKind::kMeasured, 0);
  w.publish(m);
  return m;
}

// ---------------------------------------------------------------------------

void t_shm_exclusive_create() {
  std::printf("shm: exclusive create / open semantics\n");
  const std::string name = seg_name();
  SharedSegment a = SharedSegment::create(name, 4096);
  CHECK(a.valid());
  SharedSegment dup = SharedSegment::create(name, 4096);  // stale-bus guard
  CHECK(!dup.valid());
  SharedSegment o = SharedSegment::open(name, 4096);
  CHECK(o.valid());
  CHECK(o.data() != a.data());  // separate views of the same physical pages
  static_cast<std::atomic<std::uint64_t>*>(a.data())->store(0xBEEF, std::memory_order_release);
  CHECK(static_cast<std::atomic<std::uint64_t>*>(o.data())->load(std::memory_order_acquire) ==
        0xBEEF);
  SharedSegment missing = SharedSegment::open(seg_name(), 4096);
  CHECK(!missing.valid());
}

void t_format_attach_validation() {
  std::printf("bus: format / attach header validation\n");
  auto t = make_bus(64);
  CHECK(t.bus.valid());
  CHECK(t.bus.capacity() == 64);

  Bus att = Bus::attach(t.seg.data(), t.seg.size());
  CHECK(att.valid());

  // Unformatted memory (no magic) must be rejected.
  SharedSegment raw = SharedSegment::create(seg_name(), bus_bytes(64));
  CHECK(!Bus::attach(raw.data(), raw.size()).valid());

  // A view too small for the declared capacity must be rejected.
  CHECK(!Bus::attach(t.seg.data(), sizeof(BusHeader) + 8 * sizeof(Slot)).valid());

  // Non-power-of-two capacity refused at format time.
  CHECK(!Bus::format(raw.data(), raw.size(), 63).valid());
}

void t_publish_consume_in_order() {
  std::printf("bus: lockstep publish/consume, payload integrity\n");
  auto t = make_bus(64);
  Writer w(t.bus);
  Reader r(t.bus, Reader::Start::kOrigin);
  for (std::uint64_t s = 0; s < 1000; ++s) {
    const BusMsg sent = pub(w, s);
    BusMsg got{};
    const Poll p = r.poll(got);
    CHECK(p.kind == PollKind::kMsg);
    CHECK(std::memcmp(&got, &sent, sizeof(BusMsg)) == 0);
    CHECK(verify_msg(got));
    CHECK(got.seq == s);
  }
  BusMsg m{};
  CHECK(r.poll(m).kind == PollKind::kEmpty);
  CHECK(r.dropped() == 0 && r.gaps() == 0);
}

void t_small_payload_padding() {
  std::printf("bus: payloads smaller than a slot round-trip\n");
  struct Tiny {
    std::uint32_t x;
    std::uint16_t y;
  };
  static_assert(BusPayload<Tiny>);
  auto t = make_bus(8);
  Writer w(t.bus);
  Reader r(t.bus, Reader::Start::kOrigin);
  w.publish(Tiny{0xC0FFEE, 42});
  Tiny got{};
  CHECK(r.poll(got).kind == PollKind::kMsg);
  CHECK(got.x == 0xC0FFEE && got.y == 42);
}

void t_wraparound() {
  std::printf("bus: ring wraparound (capacity 8, 100 msgs lockstep)\n");
  auto t = make_bus(8);
  Writer w(t.bus);
  Reader r(t.bus, Reader::Start::kOrigin);
  for (std::uint64_t s = 0; s < 100; ++s) {
    pub(w, s);
    BusMsg got{};
    CHECK(r.poll(got).kind == PollKind::kMsg);
    CHECK(got.seq == s && verify_msg(got));
  }
  CHECK(r.dropped() == 0);
}

void t_late_join_live_edge() {
  std::printf("bus: late joiner starts at the live edge\n");
  auto t = make_bus(64);
  Writer w(t.bus);
  for (std::uint64_t s = 0; s < 50; ++s) pub(w, s);
  Reader r(t.bus);  // default: live edge
  BusMsg m{};
  CHECK(r.poll(m).kind == PollKind::kEmpty);  // history is not replayed
  for (std::uint64_t s = 50; s < 60; ++s) pub(w, s);
  for (std::uint64_t s = 50; s < 60; ++s) {
    CHECK(r.poll(m).kind == PollKind::kMsg);
    CHECK(m.seq == s);
  }
  CHECK(r.poll(m).kind == PollKind::kEmpty);
  CHECK(r.dropped() == 0);
}

void t_origin_reads_history() {
  std::printf("bus: origin reader replays history still in the ring\n");
  auto t = make_bus(64);
  Writer w(t.bus);
  for (std::uint64_t s = 0; s < 50; ++s) pub(w, s);
  Reader r(t.bus, Reader::Start::kOrigin);
  BusMsg m{};
  for (std::uint64_t s = 0; s < 50; ++s) {
    CHECK(r.poll(m).kind == PollKind::kMsg);
    CHECK(m.seq == s && verify_msg(m));
  }
  CHECK(r.poll(m).kind == PollKind::kEmpty);
}

void t_lap_detection_and_resync() {
  std::printf("bus: lapped reader detects the gap and resyncs\n");
  auto t = make_bus(8);
  Writer w(t.bus);
  Reader r(t.bus, Reader::Start::kOrigin);
  for (std::uint64_t s = 0; s < 100; ++s) pub(w, s);  // 12.5 laps, reader never polled

  BusMsg m{};
  const Poll p = r.poll(m);
  CHECK(p.kind == PollKind::kGap);
  CHECK(p.gap == 100);  // everything unread was overwritten or is behind the edge
  CHECK(r.dropped() == 100 && r.gaps() == 1);
  CHECK(r.next_seq() == 100);
  CHECK(r.poll(m).kind == PollKind::kEmpty);

  for (std::uint64_t s = 100; s < 105; ++s) pub(w, s);
  for (std::uint64_t s = 100; s < 105; ++s) {
    CHECK(r.poll(m).kind == PollKind::kMsg);
    CHECK(m.seq == s && verify_msg(m));
  }
  CHECK(r.dropped() == 100);  // no further loss after resync
}

void t_busy_slot_not_delivered() {
  std::printf("bus: in-progress (odd) stamp is never delivered\n");
  auto t = make_bus(8);
  Reader r(t.bus, Reader::Start::kOrigin);

  // Hand-run the writer protocol for seq 0, pausing mid-write: busy stamp
  // down, garbage payload, no done stamp. The reader must see "empty".
  Slot& sl = t.bus.slot(0);
  sl.stamp.store(stamp_busy(0), std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (std::size_t i = 0; i < kPayloadWords; ++i)
    sl.word[i].store(0xDEADDEADDEADDEADull, std::memory_order_relaxed);

  BusMsg m{};
  CHECK(r.poll(m).kind == PollKind::kEmpty);

  // Finish the publish properly; now it must be delivered.
  const BusMsg real = make_msg(0, MsgKind::kMeasured, 0);
  std::uint64_t w[kPayloadWords] = {};
  std::memcpy(w, &real, sizeof(real));
  for (std::size_t i = 0; i < kPayloadWords; ++i)
    sl.word[i].store(w[i], std::memory_order_relaxed);
  sl.stamp.store(stamp_done(0), std::memory_order_release);
  t.bus.header().pub_seq.store(1, std::memory_order_release);

  CHECK(r.poll(m).kind == PollKind::kMsg);
  CHECK(verify_msg(m) && m.seq == 0);
}

// A reader loop used by both stress scenarios: consumes until the cursor
// passes `total`, verifying every delivered message in isolation.
struct StressReader {
  std::uint64_t delivered = 0;
  std::uint64_t dropped = 0;
  std::uint64_t bad_csum = 0;
  std::uint64_t bad_seq = 0;

  void run(Bus bus, std::uint64_t total, std::uint64_t stall_every = 0) {
    Reader r(bus, Reader::Start::kOrigin);
    BusMsg m{};
    while (r.next_seq() < total) {
      const Poll p = r.poll(m);
      if (p.kind == PollKind::kMsg) {
        if (!verify_msg(m)) ++bad_csum;
        if (m.seq != r.next_seq() - 1) ++bad_seq;
        ++delivered;
        if (stall_every && (delivered % stall_every) == 0)
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
      } else if (p.kind == PollKind::kEmpty) {
        hft::cpu_relax();
      }
    }
    dropped = r.dropped();
  }
};

void t_torn_read_stress() {
  std::printf("bus: torn-read stress — tiny ring, writer at full speed, 2 readers\n");
  constexpr std::uint64_t kTotal = 2'000'000;
  auto t = make_bus(64);  // 64 slots: maximum lap pressure
  StressReader ra, rb;
  {
    std::jthread ta([&] { ra.run(t.bus, kTotal); });
    std::jthread tb([&] { rb.run(t.bus, kTotal); });
    std::jthread tw([&] {
      Writer w(t.bus);
      for (std::uint64_t s = 0; s < kTotal; ++s) pub(w, s);
    });
  }
  for (const StressReader* r : {&ra, &rb}) {
    CHECK(r->bad_csum == 0);                      // no torn read was ever delivered
    CHECK(r->bad_seq == 0);                       // in-order, no duplicates
    CHECK(r->delivered + r->dropped == kTotal);   // every message accounted for
  }
  std::printf("    reader A: %llu delivered, %llu dropped | reader B: %llu delivered, %llu dropped\n",
              static_cast<unsigned long long>(ra.delivered),
              static_cast<unsigned long long>(ra.dropped),
              static_cast<unsigned long long>(rb.delivered),
              static_cast<unsigned long long>(rb.dropped));
}

void t_slow_reader_isolation() {
  std::printf("bus: a stalling reader loses data, hurts nobody, stays consistent\n");
  constexpr std::uint64_t kTotal = 2'000'000;
  auto t = make_bus(4096);
  StressReader fast, slow;
  {
    std::jthread tf([&] { fast.run(t.bus, kTotal); });
    std::jthread ts([&] { slow.run(t.bus, kTotal, /*stall_every=*/20'000); });
    std::jthread tw([&] {
      Writer w(t.bus);
      for (std::uint64_t s = 0; s < kTotal; ++s) pub(w, s);
      CHECK(w.next_seq() == kTotal);  // writer finished all publishes, reader-independent
    });
  }
  CHECK(slow.dropped > 0);  // a 2ms stall at full writer speed must overrun a 4k ring
  for (const StressReader* r : {&fast, &slow}) {
    CHECK(r->bad_csum == 0 && r->bad_seq == 0);
    CHECK(r->delivered + r->dropped == kTotal);
  }
  std::printf("    fast: %llu delivered, %llu dropped | stalling: %llu delivered, %llu dropped\n",
              static_cast<unsigned long long>(fast.delivered),
              static_cast<unsigned long long>(fast.dropped),
              static_cast<unsigned long long>(slow.delivered),
              static_cast<unsigned long long>(slow.dropped));
}

}  // namespace

int main() {
  t_shm_exclusive_create();
  t_format_attach_validation();
  t_publish_consume_in_order();
  t_small_payload_padding();
  t_wraparound();
  t_late_join_live_edge();
  t_origin_reads_history();
  t_lap_detection_and_resync();
  t_busy_slot_not_delivered();
  t_torn_read_stress();
  t_slow_reader_isolation();

  if (g_fail) {
    std::printf("\n%d CHECK(s) FAILED\n", g_fail);
    return 1;
  }
  std::printf("\nALL PASS (11 scenarios)\n");
  return 0;
}
