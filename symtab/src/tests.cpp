// Scenario tests for symtab. The perfect hash's guarantees are absolute —
// every member resolves, no unknown key can false-hit, the table is exactly
// minimal, the build is deterministic — so the tests assert them absolutely:
// exhaustive membership over the universe, a million adversarial unknowns,
// a bijection check over every slot. The snapshot-swap layer is tested for
// the property that matters: a pinned reader's snapshot is never freed
// under it, and everything unpinned is eventually reclaimed. No framework —
// a CHECK macro.

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "st/gen.hpp"
#include "st/mph.hpp"
#include "st/oa_map.hpp"
#include "st/sym.hpp"
#include "st/table.hpp"

using namespace st;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_fail;                                                     \
    }                                                               \
  } while (0)

void t_sym_key() {
  std::puts("sym: padded-word key == the wire bytes");
  const Sym a = Sym::from("IBM");
  CHECK(std::memcmp(&a.v, "IBM     ", 8) == 0);
  const char wire[8] = {'I', 'B', 'M', ' ', ' ', ' ', ' ', ' '};
  CHECK(Sym::from_wire(wire) == a);
  char out[9];
  a.to_chars(out);
  CHECK(std::memcmp(out, "IBM     ", 9) == 0);
  CHECK(!(Sym::from("IBM") == Sym::from("IBN")));
  CHECK(Sym::from("ABCDEFGHIJ") == Sym::from("ABCDEFGH"));  // truncates at 8
  CHECK(Sym::from("A").v != 0 && Sym::from("").v != 0);     // never the sentinel
}

void t_mph_membership() {
  std::puts("mph: exhaustive membership + 1M unknowns cannot false-hit");
  const auto uni = gen_universe(5000, 7);
  std::vector<Sym> keys;
  std::vector<u32> ids;
  for (const auto& ins : uni) {
    keys.push_back(ins.sym);
    ids.push_back(ins.id);
  }
  Mph mph;
  CHECK(mph.build(keys, ids));
  CHECK(mph.size() == 5000);  // minimal: exactly n slots

  for (const auto& ins : uni) CHECK(mph.lookup(ins.sym) == ins.id);

  std::unordered_set<u64> members;
  for (const Sym& k : keys) members.insert(k.v);
  Rng rng(99);
  u64 false_hits = 0;
  for (int i = 0; i < 1'000'000; ++i) {
    // adversarial unknowns: same alphabet and shape as real members
    const u64 len = 1 + rng.below(7);
    char buf[8];
    for (u64 j = 0; j < len; ++j) buf[j] = static_cast<char>('A' + rng.below(26));
    const Sym k = Sym::from({buf, static_cast<std::size_t>(len)});
    if (members.count(k.v)) continue;
    if (mph.lookup(k) != Mph::kMiss) ++false_hits;
  }
  CHECK(false_hits == 0);
}

void t_mph_bijection_and_determinism() {
  std::puts("mph: slot bijection, deterministic digest, duplicates rejected");
  const auto uni = gen_universe(3000, 11);
  std::vector<Sym> keys;
  std::vector<u32> ids;
  for (const auto& ins : uni) {
    keys.push_back(ins.sym);
    ids.push_back(ins.id);
  }
  Mph a, b;
  CHECK(a.build(keys, ids));
  CHECK(b.build(keys, ids));
  CHECK(a.digest() == b.digest());  // same universe -> same table, bit for bit

  // Bijection: every slot holds a distinct member with its right id.
  std::unordered_set<u64> seen;
  std::unordered_set<u64> members(0);
  for (const Sym& k : keys) members.insert(k.v);
  bool all_good = true;
  for (u64 pos = 0; pos < a.size(); ++pos) {
    const MphSlot& sl = a.slot(pos);
    if (!members.count(sl.key) || !seen.insert(sl.key).second) all_good = false;
    if (!(keys[sl.id].v == sl.key)) all_good = false;
  }
  CHECK(all_good);
  CHECK(seen.size() == keys.size());

  std::vector<Sym> dup = keys;
  dup.push_back(keys[42]);
  std::vector<u32> dup_ids = ids;
  dup_ids.push_back(9999);
  Mph c;
  CHECK(!c.build(dup, dup_ids));  // duplicate symbol is a build error
}

void t_mph_small() {
  std::puts("mph: tiny universes (n = 1, 2, 5)");
  for (u64 n : {u64{1}, u64{2}, u64{5}}) {
    const auto uni = gen_universe(n, 100 + n);
    std::vector<Sym> keys;
    std::vector<u32> ids;
    for (const auto& ins : uni) {
      keys.push_back(ins.sym);
      ids.push_back(ins.id);
    }
    Mph mph;
    CHECK(mph.build(keys, ids));
    for (const auto& ins : uni) CHECK(mph.lookup(ins.sym) == ins.id);
    CHECK(mph.lookup(Sym::from("ZZZZZZZQ")) == Mph::kMiss);
  }
}

void t_mph_batch() {
  std::puts("mph: batched lookup == scalar lookup");
  const auto uni = gen_universe(2000, 13);
  std::vector<Sym> keys;
  std::vector<u32> ids;
  for (const auto& ins : uni) {
    keys.push_back(ins.sym);
    ids.push_back(ins.id);
  }
  Mph mph;
  CHECK(mph.build(keys, ids));
  Sym batch[16];
  u32 out[16];
  Rng rng(5);
  bool equal = true;
  for (int r = 0; r < 1000; ++r) {
    for (int i = 0; i < 16; ++i)
      batch[i] = (rng.below(4) == 0) ? Sym::from("NOPE") : keys[rng.below(keys.size())];
    mph.lookup_batch<16>(batch, out);
    for (int i = 0; i < 16; ++i)
      if (out[i] != mph.lookup(batch[i])) equal = false;
  }
  CHECK(equal);
}

void t_oa_agrees() {
  std::puts("oa baseline: agrees with mph on every hit and miss");
  const auto uni = gen_universe(4000, 17);
  std::vector<Sym> keys;
  std::vector<u32> ids;
  for (const auto& ins : uni) {
    keys.push_back(ins.sym);
    ids.push_back(ins.id);
  }
  Mph mph;
  OaMap oa;
  CHECK(mph.build(keys, ids));
  oa.build(keys, ids);
  for (const Sym& k : keys) CHECK(mph.lookup(k) == oa.lookup(k));
  Rng rng(3);
  bool agree = true;
  for (int i = 0; i < 100'000; ++i) {
    Sym k;
    k.v = rng.next() | 1;  // arbitrary junk keys (nonzero)
    if (mph.lookup(k) != oa.lookup(k)) agree = false;
  }
  CHECK(agree);
}

void t_table_snapshots() {
  std::puts("table: stable ids across adds, pinned reader blocks reclaim");
  SymTable tab;
  CHECK(tab.load(gen_universe(500, 19)));
  auto& rd = tab.register_reader();

  const Snapshot* s1 = tab.pin(rd);
  CHECK(s1->version == 1);
  const Instrument* first = s1->find(s1->byid[0].sym);
  CHECK(first != nullptr && first->id == 0);
  CHECK(s1->find(Sym::from("QQZZWWXX")) == nullptr);

  // Add while a reader is pinned on v1: v1 must survive.
  Instrument novel;
  novel.sym = Sym::from("NEWLIST");
  CHECK(tab.add(novel));
  CHECK(tab.version() == 2);
  CHECK(tab.retired_pending() == 1 && tab.freed() == 0);
  CHECK(s1->find(s1->byid[7].sym) != nullptr);  // old snapshot still readable
  CHECK(s1->find(novel.sym) == nullptr);        // and still consistent (no NEWLIST)

  // New pin sees v2, with the new symbol at the next stable id.
  tab.unpin(rd);
  const Snapshot* s2 = tab.pin(rd);
  CHECK(s2->version == 2);
  const Instrument* found = s2->find(novel.sym);
  CHECK(found != nullptr && found->id == 500);
  CHECK(s2->byid[123].sym == s1->byid[123].sym);  // ids stable across rebuild

  // Reader has moved on -> the next publish can reclaim v1.
  Instrument novel2;
  novel2.sym = Sym::from("NEWLISTB");
  CHECK(tab.add(novel2));
  CHECK(tab.freed() == 1);
  tab.unpin(rd);
  tab.reclaim();
  CHECK(tab.retired_pending() == 0 && tab.freed() == 2);
}

void t_table_threads() {
  std::puts("table: 4 readers hammer through 200 swaps, zero canary misses");
  SymTable tab;
  auto uni = gen_universe(1000, 23);
  CHECK(tab.load(uni));
  const Sym canary = uni[500].sym;

  std::atomic<bool> stop{false};
  std::atomic<u64> misses{0}, reads{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      auto& rd = tab.register_reader();
      Rng rng(41);
      while (!stop.load(std::memory_order_relaxed)) {
        const Snapshot* s = tab.pin(rd);
        for (int i = 0; i < 64; ++i) {
          const Instrument* ins = s->find(canary);
          if (ins == nullptr || !(ins->sym == canary)) misses.fetch_add(1);
          const Instrument* other = s->find(s->byid[rng.below(s->byid.size())].sym);
          if (other == nullptr) misses.fetch_add(1);
        }
        reads.fetch_add(128);
        tab.unpin(rd);
      }
    });
  }
  for (int i = 0; i < 200; ++i) {
    Instrument ins;
    // 7-letter names cannot collide with the universe (1..6-letter tickers)
    char name[8] = {'Z', 'Z', 'Z', 'Z', 'Z', 0, 0, 0};
    name[5] = static_cast<char>('A' + i / 26);
    name[6] = static_cast<char>('A' + i % 26);
    ins.sym = Sym::from({name, 7});
    CHECK(tab.add(ins));
  }
  stop.store(true);
  for (auto& th : threads) th.join();
  tab.reclaim();
  std::printf("  %llu reads across swaps, %llu misses, %llu snapshots freed\n",
              static_cast<unsigned long long>(reads.load()),
              static_cast<unsigned long long>(misses.load()),
              static_cast<unsigned long long>(tab.freed()));
  CHECK(misses.load() == 0);
  CHECK(tab.version() == 201);
  CHECK(tab.retired_pending() == 0);  // all quiescent -> everything reclaimed
}

}  // namespace

int main() {
  t_sym_key();
  t_mph_membership();
  t_mph_bijection_and_determinism();
  t_mph_small();
  t_mph_batch();
  t_oa_agrees();
  t_table_snapshots();
  t_table_threads();
  if (g_fail == 0) {
    std::puts("\nall tests passed");
    return 0;
  }
  std::printf("\n%d CHECK(s) failed\n", g_fail);
  return 1;
}
