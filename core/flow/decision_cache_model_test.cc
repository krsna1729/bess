// SPDX-License-Identifier: BSD-3-Clause

// DecisionCache (M12, D-062) against a reference model (M22).
//
// Contract under test, as the model encodes it (from decision_cache.h's
// public comments, not its code):
//   - A DecisionGeneration starts at 1. Invalidate() makes G+1 current and
//     returns it. One generation is shared by every cache of a policy scope:
//     an Invalidate() through it reaches all of them at once.
//   - Install(key, id, compiled_at):
//       compiled_at != current G         -> kStaleGeneration, nothing changes;
//       the key has an entry (current or stale)
//                                        -> kReplaced, entry = {id, G}, size unchanged;
//       size == capacity                 -> kFull, nothing changes;
//       otherwise                        -> kInstalled, entry = {id, G}, size + 1.
//   - Lookup(key): no entry -> kMiss; an entry stamped with a generation other
//     than the current one -> kStale; otherwise kHit and *out = its id. *out is
//     written only on a hit.
//   - LookupBatch(keys): for each key the answer Lookup would give at the same
//     generation; bit i of the result set iff kHit, bit i of *stale set iff
//     kStale; out[i] written only on a hit. Duplicated keys in a batch answer
//     alike.
//   - Erase(key): true iff the key had an entry (current or stale); it frees
//     the slot. A stale entry keeps its slot until reinstalled or erased, so
//     size() counts stale entries.
//   - generation() is the shared current generation.
// The model is a std::map key -> {id, generation} per cache and one counter.

#include "flow/decision_cache.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <vector>

namespace bess::flow {
namespace {

struct Tuple {
  uint32_t src, dst;
  uint16_t sport, dport;
  uint8_t proto;
  uint8_t pad[3];
};
static_assert(ByteHashableFlowKey<Tuple>);

Tuple T(uint32_t i) { return Tuple{i * 2654435761u, i, static_cast<uint16_t>(i), 443, 17, {}}; }

using Cache = DecisionCache<Tuple, uint32_t>;

struct ModelEntry {
  uint32_t id;
  uint64_t generation;
};

struct ModelCache {
  size_t capacity;
  std::map<uint32_t, ModelEntry> entries;
};

// One run: two caches (two workers) of different capacities over one shared
// generation, `steps` random operations, every observable result compared.
void RunModel(uint32_t seed, size_t steps) {
  SCOPED_TRACE(::testing::Message() << "seed " << seed);
  std::mt19937 rng(seed);
  DecisionGeneration generation;
  uint64_t model_generation = 1;
  constexpr uint32_t kKeys = 96;
  ModelCache model[2] = {{24, {}}, {57, {}}};
  std::unique_ptr<Cache> cache[2];
  for (int c = 0; c < 2; c++) {
    auto made = Cache::Create(model[c].capacity, generation);
    ASSERT_TRUE(made.has_value());
    cache[c] = std::move(*made);
    ASSERT_EQ(model[c].capacity, cache[c]->capacity());
  }
  // A generation read by a "worker" before it compiled: installing with it
  // after an intervening Invalidate() must be refused.
  uint64_t snapshot = model_generation;
  size_t full = 0, stale_installs = 0, replaced = 0, stale_lookups = 0, hits = 0;

  auto expect_lookup = [&](int c, uint32_t k) -> DecisionLookup {
    auto it = model[c].entries.find(k);
    if (it == model[c].entries.end()) return DecisionLookup::kMiss;
    return it->second.generation == model_generation ? DecisionLookup::kHit : DecisionLookup::kStale;
  };

  for (size_t step = 0; step < steps; step++) {
    SCOPED_TRACE(::testing::Message() << "step " << step);
    const int c = static_cast<int>(rng() % 2);
    ModelCache &m = model[c];
    Cache &dc = *cache[c];
    const uint32_t op = rng() % 100;
    const uint32_t k = rng() % kKeys;
    if (op < 40) {
      // Install, compiled against: the current generation (most), a snapshot
      // taken some steps ago, the previous one, or a future one.
      uint64_t compiled_at = model_generation;
      const uint32_t which = rng() % 10;
      if (which == 7) compiled_at = snapshot;
      if (which == 8) compiled_at = model_generation - 1;
      if (which == 9) compiled_at = model_generation + 1 + rng() % 2;
      const uint32_t id = rng();
      DecisionInstall want;
      if (compiled_at != model_generation) {
        want = DecisionInstall::kStaleGeneration;
        stale_installs++;
      } else if (m.entries.count(k)) {
        want = DecisionInstall::kReplaced;
        m.entries[k] = {id, model_generation};
        replaced++;
      } else if (m.entries.size() == m.capacity) {
        want = DecisionInstall::kFull;
        full++;
      } else {
        want = DecisionInstall::kInstalled;
        m.entries[k] = {id, model_generation};
      }
      ASSERT_EQ(want, dc.Install(T(k), id, compiled_at)) << "key " << k << " compiled_at "
                                                         << compiled_at;
    } else if (op < 65) {
      uint32_t out = 0xdeadbeef;
      const DecisionLookup want = expect_lookup(c, k);
      ASSERT_EQ(want, dc.Lookup(T(k), &out)) << "key " << k;
      if (want == DecisionLookup::kHit) {
        ASSERT_EQ(m.entries.at(k).id, out);
        hits++;
      } else {
        ASSERT_EQ(0xdeadbeefu, out) << "a miss or stale lookup must not write *out";
        stale_lookups += want == DecisionLookup::kStale;
      }
    } else if (op < 80) {
      const size_t n = 1 + rng() % Cache::kMaxBatch;
      std::vector<Tuple> keys(n);
      std::vector<uint32_t> ids(n);
      for (size_t i = 0; i < n; i++) {
        ids[i] = rng() % kKeys;
        keys[i] = T(ids[i]);
      }
      std::vector<uint32_t> out(n, 0xdeadbeef);
      uint64_t stale = 0;
      const bool stale_asked = rng() % 4 != 0;
      const uint64_t got = dc.LookupBatch(keys, out, stale_asked ? &stale : nullptr);
      for (size_t i = 0; i < n; i++) {
        const DecisionLookup want = expect_lookup(c, ids[i]);
        ASSERT_EQ(want == DecisionLookup::kHit, (got >> i & 1) != 0) << "position " << i;
        if (stale_asked) {
          ASSERT_EQ(want == DecisionLookup::kStale, (stale >> i & 1) != 0) << "position " << i;
        }
        ASSERT_EQ(want == DecisionLookup::kHit ? m.entries.at(ids[i]).id : 0xdeadbeefu, out[i])
            << "position " << i;
      }
      if (n < 64) {
        ASSERT_EQ(0u, got >> n) << "no bit beyond the batch";
        ASSERT_EQ(0u, stale >> n);
      }
    } else if (op < 92) {
      const bool want = m.entries.erase(k) != 0;
      ASSERT_EQ(want, dc.Erase(T(k))) << "key " << k;
    } else if (op < 96) {
      // The control side changes policy: one Invalidate() for both caches.
      model_generation++;
      ASSERT_EQ(model_generation, generation.Invalidate());
    } else {
      snapshot = model_generation;
    }
    // After every step: sizes and the shared generation.
    for (int i = 0; i < 2; i++) {
      ASSERT_EQ(model[i].entries.size(), cache[i]->size()) << "cache " << i;
      ASSERT_EQ(model_generation, cache[i]->generation());
    }
    ASSERT_EQ(model_generation, generation.Current());
  }
  // A full sweep of both caches at the end.
  for (int c = 0; c < 2; c++) {
    for (uint32_t k = 0; k < kKeys; k++) {
      uint32_t out = 0;
      ASSERT_EQ(expect_lookup(c, k), cache[c]->Lookup(T(k), &out)) << "cache " << c << " key " << k;
    }
  }
  // The run reached every interesting path.
  EXPECT_GT(full, 0u);
  EXPECT_GT(stale_installs, 0u);
  EXPECT_GT(replaced, 0u);
  EXPECT_GT(stale_lookups, 0u);
  EXPECT_GT(hits, 0u);
}

TEST(DecisionCacheModelTest, RandomOperationsOnTwoCachesSharingAGenerationMatchAModel) {
  for (uint32_t seed = 1; seed <= 12; seed++) {
    RunModel(seed, 30000);
    if (::testing::Test::HasFatalFailure()) return;
  }
}

}  // namespace
}  // namespace bess::flow
