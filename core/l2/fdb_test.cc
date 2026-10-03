// SPDX-License-Identifier: BSD-3-Clause

// L2 FDB (M14, D-064): the roadmap's correctness list (duplicate MAC,
// learning and aging, static not aged, domain isolation, flood on unknown,
// invalid interface fail-closed) and a differential test against the legacy
// Bridge module's learning/forwarding semantics.

#include "l2/fdb.h"
#include "l2/mac_table.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <vector>

namespace bess::l2 {
namespace {

using dataplane::InterfaceId;
using dataplane::kInvalidInterfaceId;

constexpr Fdb::Tick kAging = 1000;

MacAddress Mac(uint32_t i) {
  return MacAddress{{0x02, 0x00, static_cast<uint8_t>(i >> 24), static_cast<uint8_t>(i >> 16),
                     static_cast<uint8_t>(i >> 8), static_cast<uint8_t>(i)}};
}
const BridgeDomainId kD0(0), kD1(1);

std::unique_ptr<Fdb> Make(size_t capacity = 64) {
  Fdb::Config c;
  c.capacity = capacity;
  c.aging = kAging;
  c.granularity_shift = 0;
  c.max_domains = 16;
  return Fdb::Create(c).value();
}

TEST(FdbTest, LearnRefreshMoveAndLookup) {
  auto fdb = Make();
  EXPECT_EQ(kInvalidInterfaceId, fdb->Lookup(kD0, Mac(1)));
  EXPECT_EQ(LearnResult::kLearned, fdb->Learn(kD0, Mac(1), InterfaceId(3), 0));
  EXPECT_EQ(InterfaceId(3), fdb->Lookup(kD0, Mac(1)));
  EXPECT_EQ(LearnResult::kRefreshed, fdb->Learn(kD0, Mac(1), InterfaceId(3), 10));
  // Duplicate MAC on another interface: one entry, moved.
  EXPECT_EQ(LearnResult::kMoved, fdb->Learn(kD0, Mac(1), InterfaceId(4), 20));
  EXPECT_EQ(InterfaceId(4), fdb->Lookup(kD0, Mac(1)));
  EXPECT_EQ(1u, fdb->size());
  EXPECT_EQ(1u, fdb->dynamic_entries());
}

TEST(FdbTest, AgingRemovesOnlyUnrefreshedDynamicEntries) {
  auto fdb = Make();
  ASSERT_EQ(LearnResult::kLearned, fdb->Learn(kD0, Mac(1), InterfaceId(1), 0));
  ASSERT_EQ(LearnResult::kLearned, fdb->Learn(kD0, Mac(2), InterfaceId(2), 0));
  ASSERT_EQ(ProgramResult::kAdded, fdb->AddStatic(kD0, Mac(3), InterfaceId(3)));
  ASSERT_EQ(LearnResult::kRefreshed, fdb->Learn(kD0, Mac(2), InterfaceId(2), 900));
  EXPECT_EQ(0u, fdb->Age(kAging, 1u << 20));      // usable through now - learned == aging
  EXPECT_EQ(1u, fdb->Age(kAging + 1, 1u << 20));  // Mac(1) only
  EXPECT_EQ(kInvalidInterfaceId, fdb->Lookup(kD0, Mac(1)));
  EXPECT_EQ(InterfaceId(2), fdb->Lookup(kD0, Mac(2)));
  EXPECT_EQ(1u, fdb->Age(900 + kAging + 1, 1u << 20));  // Mac(2)
  // The static entry never ages.
  EXPECT_EQ(0u, fdb->Age(1'000'000, 1u << 20));
  EXPECT_EQ(InterfaceId(3), fdb->Lookup(kD0, Mac(3)));
  EXPECT_EQ(1u, fdb->size());
  EXPECT_EQ(0u, fdb->dynamic_entries());
}

TEST(FdbTest, StaticEntriesWinOverLearningAndCancelAging) {
  auto fdb = Make();
  ASSERT_EQ(LearnResult::kLearned, fdb->Learn(kD0, Mac(1), InterfaceId(1), 0));
  EXPECT_EQ(ProgramResult::kReplaced, fdb->AddStatic(kD0, Mac(1), InterfaceId(9)));
  EXPECT_EQ(0u, fdb->dynamic_entries());  // its timer was cancelled
  EXPECT_EQ(LearnResult::kStatic, fdb->Learn(kD0, Mac(1), InterfaceId(2), 5));
  EXPECT_EQ(InterfaceId(9), fdb->Lookup(kD0, Mac(1)));
  EXPECT_EQ(0u, fdb->Age(10 * kAging, 1u << 20));
  EXPECT_EQ(InterfaceId(9), fdb->Lookup(kD0, Mac(1)));
}

TEST(FdbTest, BridgeDomainsAreIsolated) {
  auto fdb = Make();
  ASSERT_EQ(LearnResult::kLearned, fdb->Learn(kD0, Mac(1), InterfaceId(1), 0));
  EXPECT_EQ(kInvalidInterfaceId, fdb->Lookup(kD1, Mac(1)));
  ASSERT_EQ(LearnResult::kLearned, fdb->Learn(kD1, Mac(1), InterfaceId(7), 0));
  EXPECT_EQ(InterfaceId(1), fdb->Lookup(kD0, Mac(1)));
  EXPECT_EQ(InterfaceId(7), fdb->Lookup(kD1, Mac(1)));
  EXPECT_TRUE(fdb->Remove(kD1, Mac(1)));
  EXPECT_EQ(InterfaceId(1), fdb->Lookup(kD0, Mac(1)));
  const InterfaceId g0[] = {InterfaceId(1), InterfaceId(2)}, g1[] = {InterfaceId(5)};
  ASSERT_TRUE(fdb->SetFloodGroup(kD0, g0));
  ASSERT_TRUE(fdb->SetFloodGroup(kD1, g1));
  EXPECT_EQ(2u, fdb->FloodGroup(kD0).size());
  EXPECT_EQ(InterfaceId(5), fdb->FloodGroup(kD1)[0]);
}

TEST(FdbTest, InvalidInterfacesAndUnknownDomainsFailClosed) {
  auto fdb = Make();
  EXPECT_EQ(ProgramResult::kInvalid, fdb->AddStatic(kD0, Mac(1), kInvalidInterfaceId));
  EXPECT_EQ(LearnResult::kIgnored, fdb->Learn(kD0, Mac(1), kInvalidInterfaceId, 0));
  MacAddress bcast{{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
  EXPECT_EQ(LearnResult::kIgnored, fdb->Learn(kD0, bcast, InterfaceId(1), 0));
  EXPECT_EQ(ProgramResult::kInvalid, fdb->AddStatic(kD0, bcast, InterfaceId(1)));
  const BridgeDomainId out_of_range(100);
  EXPECT_EQ(LearnResult::kIgnored, fdb->Learn(out_of_range, Mac(1), InterfaceId(1), 0));
  EXPECT_TRUE(fdb->FloodGroup(out_of_range).empty());
  const InterfaceId bad[] = {InterfaceId(1), kInvalidInterfaceId};
  EXPECT_FALSE(fdb->SetFloodGroup(kD0, bad));
  EXPECT_TRUE(fdb->FloodGroup(kD0).empty());
  EXPECT_EQ(0u, fdb->size());
  // kNoDomain with the broadcast MAC is the table's empty-slot word: after a
  // removal leaves a free slot, it must still miss, and Remove must refuse it
  // without touching the count.
  // A one-entry FDB has two buckets; learning and removing many MACs leaves
  // free slots in both that last held interface 3.
  auto tiny = Make(1);
  for (uint32_t i = 0; i < 32; i++) {
    ASSERT_EQ(LearnResult::kLearned, tiny->Learn(kD0, Mac(i), InterfaceId(3), 0));
    ASSERT_TRUE(tiny->Remove(kD0, Mac(i)));
  }
  EXPECT_EQ(kInvalidInterfaceId, tiny->Lookup(Fdb::kNoDomain, bcast));
  const FdbKey reserved = MakeKey(Fdb::kNoDomain, bcast);
  InterfaceId got[1];
  EXPECT_EQ(0u, tiny->LookupBatch(std::span<const FdbKey>(&reserved, 1), got));
  EXPECT_EQ(kInvalidInterfaceId, got[0]);
  EXPECT_FALSE(tiny->Remove(Fdb::kNoDomain, bcast));
  EXPECT_EQ(0u, tiny->size());
  // VLANs not mapped resolve to no domain.
  EXPECT_EQ(Fdb::kNoDomain, fdb->DomainOfVlan(100));
  fdb->MapVlan(100, kD1);
  EXPECT_EQ(kD1, fdb->DomainOfVlan(100));
  EXPECT_EQ(Fdb::kNoDomain, fdb->DomainOfVlan(5000));
}

TEST(FdbTest, FullTableRefusesAndAgingMakesRoom) {
  auto fdb = Make(4);
  for (uint32_t i = 0; i < 4; i++) {
    ASSERT_EQ(LearnResult::kLearned, fdb->Learn(kD0, Mac(i), InterfaceId(1), 0));
  }
  EXPECT_EQ(LearnResult::kFull, fdb->Learn(kD0, Mac(9), InterfaceId(1), 0));
  EXPECT_EQ(ProgramResult::kFull, fdb->AddStatic(kD0, Mac(9), InterfaceId(1)));
  EXPECT_EQ(4u, fdb->Age(kAging + 1, 1u << 20));
  EXPECT_EQ(LearnResult::kLearned, fdb->Learn(kD0, Mac(9), InterfaceId(1), kAging + 2));
  fdb->Flush();
  EXPECT_EQ(0u, fdb->size());

  // A learn limit keeps room for static entries.
  Fdb::Config c;
  c.capacity = 4;
  c.learn_limit = 2;
  c.aging = kAging;
  c.granularity_shift = 0;
  auto limited = Fdb::Create(c).value();
  EXPECT_EQ(LearnResult::kLearned, limited->Learn(kD0, Mac(1), InterfaceId(1), 0));
  EXPECT_EQ(LearnResult::kLearned, limited->Learn(kD0, Mac(2), InterfaceId(1), 0));
  EXPECT_EQ(LearnResult::kFull, limited->Learn(kD0, Mac(3), InterfaceId(1), 0));
  EXPECT_EQ(LearnResult::kRefreshed, limited->Learn(kD0, Mac(1), InterfaceId(1), 1));
  EXPECT_EQ(ProgramResult::kAdded, limited->AddStatic(kD0, Mac(4), InterfaceId(2)));
  EXPECT_EQ(ProgramResult::kAdded, limited->AddStatic(kD0, Mac(5), InterfaceId(2)));
  EXPECT_EQ(ProgramResult::kFull, limited->AddStatic(kD0, Mac(6), InterfaceId(2)));
}

TEST(FdbTest, BatchLookupMatchesScalar) {
  auto fdb = Make(256);
  std::mt19937 rng(14);
  for (uint32_t i = 0; i < 100; i++) {
    ASSERT_EQ(LearnResult::kLearned,
              fdb->Learn(BridgeDomainId(i % 3), Mac(i), InterfaceId(1 + i % 7), 0));
  }
  std::vector<FdbKey> keys(Fdb::kMaxBatch);
  std::vector<InterfaceId> out(keys.size());
  for (int round = 0; round < 20; round++) {
    for (auto &k : keys) {
      const uint32_t i = rng() % 150;
      k = MakeKey(BridgeDomainId(rng() % 3), Mac(i));
    }
    const uint64_t hits = fdb->LookupBatch(keys, out);
    for (size_t i = 0; i < keys.size(); i++) {
      MacAddress m;
      std::memcpy(m.bytes.data(), keys[i].mac, 6);
      const InterfaceId want = fdb->Lookup(BridgeDomainId(keys[i].domain), m);
      EXPECT_EQ(want, out[i]);
      EXPECT_EQ(want != kInvalidInterfaceId, (hits >> i & 1) != 0);
    }
  }
}

// The cuckoo table against a map oracle at full load: inserts that need
// alternate-bucket moves (MakeRoom) keep every resident entry findable with
// its value and its cold word, and erases free slots for reuse.
TEST(MacTableTest, DifferentialWithMovesAtFullLoad) {
  constexpr size_t kCap = 4096;
  auto t = MacTable<uint64_t>::Create(kCap);
  ASSERT_NE(nullptr, t);
  using T = MacTable<uint64_t>;
  std::map<uint64_t, uint16_t> oracle;
  std::mt19937_64 rng(0xcafe);
  size_t refused = 0;
  for (int op = 0; op < 200000; op++) {
    const uint64_t key = rng() % (2 * kCap) + 1;
    uint32_t s = t->Find(key);
    ASSERT_EQ(oracle.count(key) != 0, s != T::kNotFound);
    if (s != T::kNotFound && rng() % 2) {
      ASSERT_EQ(oracle[key], t->value(s));
      ASSERT_EQ(key & 0xff, t->flags(s));
      ASSERT_EQ(key * 3, t->cold(s));  // the cold word moved with its slot
      t->Erase(s);
      oracle.erase(key);
    } else if (s == T::kNotFound) {
      s = t->Insert(key);
      if (s == T::kNotFound) {
        ASSERT_EQ(kCap, oracle.size());  // refused only at capacity
        refused++;
        continue;
      }
      t->set_value(s, static_cast<uint16_t>(key));
      t->set_flags(s, static_cast<uint8_t>(key));
      t->cold(s) = key * 3;
      oracle[key] = static_cast<uint16_t>(key);
    }
    ASSERT_EQ(oracle.size(), t->size());
  }
  EXPECT_GT(refused, 0u);  // the table was driven to capacity
  size_t seen = 0;
  t->ForEach([&](uint32_t s) {
    ASSERT_EQ(oracle.at(t->key(s)), t->value(s));
    ASSERT_EQ(t->key(s) * 3, t->cold(s));
    seen++;
  });
  EXPECT_EQ(oracle.size(), seen);
}

// The legacy Bridge module (modules/bridge.cc before M14) as an oracle: an
// unordered map MAC -> {gate, last_seen, static}; learning refreshes or moves
// dynamic entries and never touches static ones; a dynamic entry is usable
// while now - last_seen <= aging; learning stops at the table size. The FDB,
// aged before each packet, must forward every packet the same way.
TEST(FdbTest, DifferentialAgainstLegacyBridgeSemantics) {
  constexpr size_t kCap = 32;
  auto fdb = Make(kCap);
  struct Legacy {
    uint16_t gate;
    Fdb::Tick last_seen;
    bool is_static;
  };
  std::map<uint32_t, Legacy> oracle;
  std::mt19937 rng(0xb1d6e);
  for (uint32_t i = 0; i < 4; i++) {
    ASSERT_EQ(ProgramResult::kAdded, fdb->AddStatic(kD0, Mac(1000 + i), InterfaceId(10 + i)));
    oracle[1000 + i] = {static_cast<uint16_t>(10 + i), 0, true};
  }
  Fdb::Tick now = 0;
  int forwarded = 0, flooded = 0;
  for (int pkt = 0; pkt < 20000; pkt++) {
    now += rng() % 40;
    // The legacy module drops entries lazily; the FDB ages on the wheel. Age
    // first, then compare: an entry is present iff the oracle says usable.
    fdb->Age(now, 1u << 20);
    for (auto it = oracle.begin(); it != oracle.end();) {
      it = (!it->second.is_static && now - it->second.last_seen > kAging) ? oracle.erase(it)
                                                                          : std::next(it);
    }
    const uint32_t src = rng() % 48, dst = rng() % 52;
    const uint16_t igate = static_cast<uint16_t>(1 + rng() % 5);
    // Learn.
    const LearnResult r = fdb->Learn(kD0, Mac(src), InterfaceId(igate), now);
    auto it = oracle.find(src);
    if (it != oracle.end()) {
      if (!it->second.is_static) {
        it->second.gate = igate;
        it->second.last_seen = now;
      }
    } else if (oracle.size() < kCap) {
      oracle[src] = {igate, now, false};
      ASSERT_EQ(LearnResult::kLearned, r);
    } else {
      ASSERT_EQ(LearnResult::kFull, r);
    }
    // Forward.
    const uint32_t d = rng() % 8 == 0 ? 1000 + rng() % 4 : dst;
    const InterfaceId got = fdb->Lookup(kD0, Mac(d));
    auto o = oracle.find(d);
    const InterfaceId want = o == oracle.end() ? kInvalidInterfaceId : InterfaceId(o->second.gate);
    ASSERT_EQ(want, got) << "packet " << pkt << " dst " << d;
    (got == kInvalidInterfaceId ? flooded : forwarded)++;
  }
  // Both paths were exercised.
  EXPECT_GT(forwarded, 1000);
  EXPECT_GT(flooded, 1000);
}

}  // namespace
}  // namespace bess::l2
