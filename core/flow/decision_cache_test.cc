// SPDX-License-Identifier: BSD-3-Clause

// DecisionCache (M12, D-062): hit/miss/stale semantics, O(1) generation
// invalidation, the compile-then-install race, capacity, and two reference
// consumers whose decisions share nothing beyond what the cache needs.

#include "flow/decision_cache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "dataplane/slot_table.h"
#include "dataplane/strong_id.h"

namespace bess::flow {
namespace {

struct Tuple {
  uint32_t src, dst;
  uint16_t sport, dport;
  uint8_t proto;
  uint8_t pad[3];
};
static_assert(ByteHashableFlowKey<Tuple>);

Tuple T(uint32_t i) { return Tuple{i, ~i, static_cast<uint16_t>(i), 80, 6, {}}; }

// -- reference consumer 1: a VFP-style layered decision, resolved by id ----------

struct VfpIdTag;
using VfpId = dataplane::StrongId<VfpIdTag, uint32_t>;
struct VfpDecision {
  uint32_t encap_vni;
  uint32_t rewrite_dst;
  uint16_t meter;
};

// -- reference consumer 2: an OVS-style action list, a different id type --------

struct OvsDecision {
  uint8_t actions[24];
  uint8_t count;
};
using OvsId = uint64_t;  // an index into the application's own action store

template <typename Id>
std::unique_ptr<DecisionCache<Tuple, Id>> Make(size_t capacity,
                                               const DecisionGeneration &g) {
  auto cache = DecisionCache<Tuple, Id>::Create(capacity, g);
  EXPECT_TRUE(cache.has_value());
  return std::move(*cache);
}

TEST(DecisionCacheTest, HitMissAndResolveForTwoUnrelatedDecisionTypes) {
  DecisionGeneration g;
  // VFP: ids resolve through an RCU-published SlotTable.
  dataplane::SlotTable<VfpId, VfpDecision> vfp_decisions(8);
  (void)vfp_decisions.Publish(VfpId(3), std::make_unique<const VfpDecision>(
                                            VfpDecision{5001, 0x0a000001, 7}));
  auto vfp = Make<VfpId>(16, g);
  EXPECT_EQ(DecisionInstall::kInstalled, vfp->Install(T(1), VfpId(3), g.Current()));
  VfpId vid;
  ASSERT_EQ(DecisionLookup::kHit, vfp->Lookup(T(1), &vid));
  ASSERT_NE(nullptr, vfp_decisions.Lookup(vid));
  EXPECT_EQ(5001u, vfp_decisions.Lookup(vid)->encap_vni);
  EXPECT_EQ(DecisionLookup::kMiss, vfp->Lookup(T(2), &vid));

  // OVS: a plain integer id into the application's array.
  std::vector<OvsDecision> ovs_actions(4);
  ovs_actions[2].count = 3;
  auto ovs = Make<OvsId>(16, g);
  EXPECT_EQ(DecisionInstall::kInstalled, ovs->Install(T(1), OvsId{2}, g.Current()));
  OvsId oid = 0;
  ASSERT_EQ(DecisionLookup::kHit, ovs->Lookup(T(1), &oid));
  EXPECT_EQ(3, ovs_actions[oid].count);
}

TEST(DecisionCacheTest, InvalidateMakesEveryEntryStaleWithoutTouchingThem) {
  DecisionGeneration g;
  constexpr uint32_t kN = 65536;
  auto cache = Make<uint32_t>(kN, g);
  const uint64_t g0 = g.Current();
  for (uint32_t i = 0; i < kN; i++) {
    ASSERT_EQ(DecisionInstall::kInstalled, cache->Install(T(i), i, g0));
  }
  EXPECT_EQ(g0 + 1, g.Invalidate());  // one increment, whatever the size
  uint32_t id = 0;
  for (uint32_t i = 0; i < kN; i++) {
    ASSERT_EQ(DecisionLookup::kStale, cache->Lookup(T(i), &id)) << i;
  }
  EXPECT_EQ(kN, cache->size());  // stale entries keep their slots

  // Recompiled under the new generation: reused in place.
  EXPECT_EQ(DecisionInstall::kReplaced, cache->Install(T(7), 70, g.Current()));
  ASSERT_EQ(DecisionLookup::kHit, cache->Lookup(T(7), &id));
  EXPECT_EQ(70u, id);
  EXPECT_EQ(kN, cache->size());
}

TEST(DecisionCacheTest, ADecisionCompiledBeforeAPolicyChangeIsRefused) {
  DecisionGeneration g;
  auto cache = Make<uint32_t>(8, g);
  const uint64_t compiled_at = g.Current();  // the application starts compiling
  g.Invalidate();                            // policy changes meanwhile
  EXPECT_EQ(DecisionInstall::kStaleGeneration,
            cache->Install(T(1), 1, compiled_at));
  uint32_t id = 0;
  EXPECT_EQ(DecisionLookup::kMiss, cache->Lookup(T(1), &id));
  EXPECT_EQ(0u, cache->size());

  // An existing entry is not overwritten by a stale install either.
  ASSERT_EQ(DecisionInstall::kInstalled, cache->Install(T(2), 20, g.Current()));
  const uint64_t old = g.Current();
  g.Invalidate();
  ASSERT_EQ(DecisionInstall::kReplaced, cache->Install(T(2), 21, g.Current()));
  EXPECT_EQ(DecisionInstall::kStaleGeneration, cache->Install(T(2), 99, old));
  ASSERT_EQ(DecisionLookup::kHit, cache->Lookup(T(2), &id));
  EXPECT_EQ(21u, id);
}

TEST(DecisionCacheTest, FullCacheRefusesAndEraseMakesRoom) {
  DecisionGeneration g;
  auto cache = Make<uint32_t>(4, g);
  for (uint32_t i = 0; i < 4; i++) {
    ASSERT_EQ(DecisionInstall::kInstalled, cache->Install(T(i), i, g.Current()));
  }
  EXPECT_EQ(DecisionInstall::kFull, cache->Install(T(9), 9, g.Current()));
  uint32_t id = 0;
  EXPECT_EQ(DecisionLookup::kMiss, cache->Lookup(T(9), &id));
  // Stale entries still occupy slots until erased or reused.
  g.Invalidate();
  EXPECT_EQ(DecisionInstall::kFull, cache->Install(T(9), 9, g.Current()));
  EXPECT_TRUE(cache->Erase(T(0)));
  EXPECT_FALSE(cache->Erase(T(0)));
  EXPECT_EQ(DecisionInstall::kInstalled, cache->Install(T(9), 9, g.Current()));
  ASSERT_EQ(DecisionLookup::kHit, cache->Lookup(T(9), &id));
  EXPECT_EQ(9u, id);
}

TEST(DecisionCacheTest, BatchAgreesWithScalarOnHitMissAndStale) {
  DecisionGeneration g;
  auto cache = Make<uint32_t>(256, g);
  // Keys 0..99 installed, then 0..49 made stale by an invalidate and 50..99
  // reinstalled under the new generation; 100.. never installed.
  for (uint32_t i = 0; i < 100; i++) {
    ASSERT_EQ(DecisionInstall::kInstalled, cache->Install(T(i), i, g.Current()));
  }
  g.Invalidate();
  for (uint32_t i = 50; i < 100; i++) {
    ASSERT_EQ(DecisionInstall::kReplaced, cache->Install(T(i), i + 1000, g.Current()));
  }
  std::mt19937 rng(7);
  for (int round = 0; round < 50; round++) {
    const size_t n = 1 + rng() % DecisionCache<Tuple, uint32_t>::kMaxBatch;
    std::vector<Tuple> keys(n);
    for (auto &k : keys) {
      k = T(rng() % 150);
    }
    std::vector<uint32_t> out(n, 0xdead);
    uint64_t stale = 0;
    const uint64_t hits = cache->LookupBatch(keys, out, &stale);
    for (size_t i = 0; i < n; i++) {
      uint32_t id = 0;
      const DecisionLookup r = cache->Lookup(keys[i], &id);
      EXPECT_EQ(r == DecisionLookup::kHit, (hits >> i & 1) != 0) << i;
      EXPECT_EQ(r == DecisionLookup::kStale, (stale >> i & 1) != 0) << i;
      if (r == DecisionLookup::kHit) {
        EXPECT_EQ(id, out[i]);
      } else {
        EXPECT_EQ(0xdeadu, out[i]) << "a miss position must stay untouched";
      }
    }
  }
}

TEST(DecisionCacheTest, OneInvalidateReachesEveryWorkersCache) {
  DecisionGeneration g;
  const uint64_t g0 = g.Current();
  std::atomic<int> ready{0};
  // Two workers, each owning its cache (created and used on its thread), one
  // shared policy generation. Both install under g0, report ready, and wait
  // for the control thread's single Invalidate().
  auto worker = [&](uint32_t base, int *stale_seen, int *hit_before) {
    auto cache = Make<uint32_t>(64, g);
    uint32_t id = 0;
    for (uint32_t i = 0; i < 32; i++) {
      EXPECT_EQ(DecisionInstall::kInstalled, cache->Install(T(base + i), i, g0));
    }
    for (uint32_t i = 0; i < 32; i++) {
      *hit_before += cache->Lookup(T(base + i), &id) == DecisionLookup::kHit;
    }
    ready.fetch_add(1);
    while (g.Current() == g0) {
      std::this_thread::yield();
    }
    for (uint32_t i = 0; i < 32; i++) {
      *stale_seen += cache->Lookup(T(base + i), &id) == DecisionLookup::kStale;
    }
  };
  int stale_a = 0, stale_b = 0, hit_a = 0, hit_b = 0;
  std::thread a(worker, 0, &stale_a, &hit_a);
  std::thread b(worker, 1000, &stale_b, &hit_b);
  while (ready.load() < 2) {
    std::this_thread::yield();
  }
  g.Invalidate();
  a.join();
  b.join();
  EXPECT_EQ(32, hit_a);
  EXPECT_EQ(32, hit_b);
  EXPECT_EQ(32, stale_a);
  EXPECT_EQ(32, stale_b);
}

TEST(DecisionCacheTest, MissPathCompilesAndInstallsWithoutAnyModule) {
  DecisionGeneration g;
  dataplane::SlotTable<VfpId, VfpDecision> decisions(64);
  uint32_t next_id = 1;
  // The application's compiler: rich policy in, immutable decision out.
  auto compile = [&](const Tuple &key) {
    const VfpId id(next_id++);
    (void)decisions.Publish(id, std::make_unique<const VfpDecision>(
                                    VfpDecision{key.src + 1, key.dst, 0}));
    return id;
  };
  auto cache = Make<VfpId>(64, g);
  int compiled = 0;
  auto packet = [&](const Tuple &key) {
    VfpId id;
    if (cache->Lookup(key, &id) != DecisionLookup::kHit) {
      const uint64_t at = g.Current();
      id = compile(key);
      compiled++;
      EXPECT_NE(DecisionInstall::kStaleGeneration, cache->Install(key, id, at));
    }
    return decisions.Lookup(id)->encap_vni;
  };
  EXPECT_EQ(2u, packet(T(1)));
  EXPECT_EQ(2u, packet(T(1)));
  EXPECT_EQ(1, compiled);  // the second packet hit
  g.Invalidate();
  EXPECT_EQ(2u, packet(T(1)));
  EXPECT_EQ(2, compiled);  // recompiled once after the policy change
}

}  // namespace
}  // namespace bess::flow
