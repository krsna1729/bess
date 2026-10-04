// SPDX-License-Identifier: BSD-3-Clause

// PackedMacTable (table policy TP2): a reference-checked random sequence for
// every writer model, its limits, and the property shared readers rely on: a
// key present the whole time is never missed, however the writer moves it.

#define BESS_PACKED_MAC_TABLE_TESTING 1
#include "l2/packed_mac_table.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dataplane/table_policy.h"

namespace bess::l2 {
namespace {

using dataplane::MultiWriter;
using dataplane::OwnerWrites;
using dataplane::SingleWriter;

// A domain-0 FDB key word for a 48-bit MAC.
uint64_t Key(uint64_t mac) { return (mac & ((uint64_t{1} << 48) - 1)) << 16; }

template <typename Sync>
class PackedMacTableTest : public ::testing::Test {};
using Syncs = ::testing::Types<OwnerWrites, SingleWriter, MultiWriter>;
TYPED_TEST_SUITE(PackedMacTableTest, Syncs);

TYPED_TEST(PackedMacTableTest, RandomOperationsMatchAReference) {
  using Table = PackedMacTable<uint32_t, TypeParam>;
  constexpr size_t kCapacity = 512;
  auto t = Table::Create(kCapacity);
  ASSERT_NE(t, nullptr);
  std::unordered_map<uint64_t, std::pair<uint16_t, uint8_t>> ref;
  std::mt19937_64 rng(7);
  for (int step = 0; step < 200000; step++) {
    const uint64_t key = Key(rng() % 1024);
    [[maybe_unused]] auto guard = t->Lock();
    const uint32_t s = t->Find(key);
    ASSERT_EQ(s != Table::kNotFound, ref.count(key) == 1) << step;
    switch (rng() % 4) {
      case 0:
      case 1:
        if (s == Table::kNotFound) {
          const uint16_t v = static_cast<uint16_t>(1 + rng() % Table::kMaxValue);
          const uint8_t f = rng() % 2;
          const uint32_t at = t->Insert(key, v, f);
          if (ref.size() < kCapacity) {
            ASSERT_NE(at, Table::kNotFound) << "refused below capacity at " << ref.size();
            t->cold(at) = static_cast<uint32_t>(key >> 16);
            ref[key] = {v, f};
          } else {
            ASSERT_EQ(at, Table::kNotFound);
          }
        } else {
          const uint16_t v = static_cast<uint16_t>(1 + rng() % Table::kMaxValue);
          t->set_value(s, v);
          t->set_flags(s, 1);
          ref[key] = {v, 1};
        }
        break;
      case 2:
        if (s != Table::kNotFound) {
          t->Erase(s);
          ref.erase(key);
        }
        break;
      default:
        break;
    }
    ASSERT_EQ(t->size(), ref.size());
  }
  // Values, flags and cold words followed their entries through every move.
  for (const auto &[key, vf] : ref) {
    const uint32_t s = t->Find(key);
    ASSERT_NE(s, Table::kNotFound);
    EXPECT_EQ(t->value(s), vf.first);
    EXPECT_EQ(t->flags(s), vf.second);
    EXPECT_EQ(t->key(s), key);
    EXPECT_EQ(t->cold(s), static_cast<uint32_t>(key >> 16));
    EXPECT_EQ(t->Lookup(key), vf.first);
  }
  std::vector<uint64_t> keys;
  for (uint64_t m = 0; m < 64; m++) {
    keys.push_back(Key(m));
  }
  uint16_t values[64];
  const uint64_t hits = t->LookupBatch(keys, values);
  for (size_t i = 0; i < keys.size(); i++) {
    const auto it = ref.find(keys[i]);
    EXPECT_EQ((hits >> i & 1) != 0, it != ref.end());
    EXPECT_EQ(values[i], it != ref.end() ? it->second.first : 0);
  }
}

TYPED_TEST(PackedMacTableTest, FillsToCapacity) {
  using Table = PackedMacTable<uint32_t, TypeParam>;
  for (size_t capacity : {1u, 7u, 1000u, 65536u}) {
    auto t = Table::Create(capacity);
    ASSERT_NE(t, nullptr);
    std::mt19937_64 rng(capacity);
    size_t n = 0;
    while (n < capacity) {
      const uint64_t key = Key(rng());
      if (t->Find(key) != Table::kNotFound) {
        continue;
      }
      ASSERT_NE(t->Insert(key, 1, 0), Table::kNotFound) << "capacity " << capacity << " at " << n;
      n++;
    }
    EXPECT_EQ(t->Insert(Key(rng()), 1, 0), Table::kNotFound) << "past capacity";
  }
}

TYPED_TEST(PackedMacTableTest, KeysWithADomainNeverMatch) {
  using Table = PackedMacTable<uint32_t, TypeParam>;
  auto t = Table::Create(16);
  ASSERT_NE(t->Insert(Key(0x0200'0000'0001), 5, 0), Table::kNotFound);
  EXPECT_EQ(t->Lookup(Key(0x0200'0000'0001)), 5u);
  EXPECT_EQ(t->Lookup(Key(0x0200'0000'0001) | 1), 0u);
  EXPECT_EQ(t->Find(Key(0x0200'0000'0001) | 0xFFFF), Table::kNotFound);
  EXPECT_EQ(t->Insert(Key(0x0200'0000'0002) | 3, 5, 0), Table::kNotFound);
  // Value 0 would read as a miss, and values wider than the slot do not fit.
  EXPECT_EQ(t->Insert(Key(0x0200'0000'0004), 0, 0), Table::kNotFound);
  EXPECT_EQ(t->Insert(Key(0x0200'0000'0004), Table::kMaxValue + 1, 0), Table::kNotFound);
  // The all-ones MAC is an ordinary key.
  ASSERT_NE(t->Insert(Key(~uint64_t{0}), 9, 1), Table::kNotFound);
  EXPECT_EQ(t->Lookup(Key(~uint64_t{0})), 9u);
}

// Deterministic (D-007): a reader at the exact point inside a move.

// One key through the scalar or the batch reader.
template <typename Table>
uint32_t Read(const Table &t, uint64_t key, bool batch) {
  if (!batch) {
    return t.Lookup(key);
  }
  uint16_t value = 0xFFFF;
  const uint64_t hits = t.LookupBatch(std::span<const uint64_t>(&key, 1), &value);
  EXPECT_EQ(hits != 0, value != 0);
  return value;
}

template <typename Sync>
class PackedMacTableSharedTest : public ::testing::Test {};
using SharedSyncs = ::testing::Types<SingleWriter, MultiWriter>;
TYPED_TEST_SUITE(PackedMacTableSharedTest, SharedSyncs);

// Between a move's two stores the entry is in both slots: a reader finds it.
TYPED_TEST(PackedMacTableSharedTest, AReaderMidMoveFindsTheEntry) {
  using Table = PackedMacTable<uint32_t, TypeParam>;
  auto t = Table::Create(64);
  const uint64_t k = Key(0x0200'0000'0042);
  ASSERT_NE(t->Insert(k, 7, 0), Table::kNotFound);
  const uint32_t from = t->Find(k);
  const size_t at = from / Table::kWays;
  const size_t other = at == t->PrimaryBucketForTesting(k) ? t->AltBucketForTesting(k)
                                                           : t->PrimaryBucketForTesting(k);
  const uint32_t to = t->FreeSlotForTesting(other);
  ASSERT_NE(to, Table::kNotFound);
  int calls = 0;
  // One probe of each bucket, no retry (a reader's Lookup would wait for the
  // move to end, and the writer is this thread): the entry must be in one.
  t->mid_move_hook = [&](uint64_t) {
    calls++;
    const uint32_t s = t->Find(k);
    ASSERT_NE(s, Table::kNotFound) << "the moving entry was in neither bucket mid-move";
    EXPECT_EQ(t->value(s), 7u);
  };
  t->MoveForTesting(to, from);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(t->Find(k), to);
  EXPECT_EQ(t->Lookup(k), 7u);
}

// A reader misses the primary, then the entry moves from its alternate bucket
// into the primary before the reader probes the alternate: both probes miss,
// and only the move sequence tells the reader to look again.
TYPED_TEST(PackedMacTableSharedTest, AReaderBetweenProbesRetriesAfterAMove) {
  for (const bool batch : {false, true}) {
    SCOPED_TRACE(batch ? "LookupBatch" : "Lookup");
    using Table = PackedMacTable<uint32_t, TypeParam>;
    auto t = Table::Create(256);
    const uint64_t k = Key(0x0200'0000'0001);
    const size_t primary = t->PrimaryBucketForTesting(k);
    // Fill k's primary bucket with keys whose primary it also is.
    std::vector<uint64_t> fillers;
    for (uint64_t m = 0x0400'0000'0000; fillers.size() < Table::kWays; m++) {
      const uint64_t f = Key(m);
      if (t->PrimaryBucketForTesting(f) == primary) {
        ASSERT_NE(t->Insert(f, 1, 0), Table::kNotFound);
        ASSERT_EQ(t->Find(f) / Table::kWays, primary);
        fillers.push_back(f);
      }
    }
    ASSERT_NE(t->Insert(k, 9, 0), Table::kNotFound);
    const uint32_t from = t->Find(k);
    ASSERT_EQ(from / Table::kWays, t->AltBucketForTesting(k)) << "k went to its alternate";
    t->Erase(t->Find(fillers[0]));
    const uint32_t to = t->FreeSlotForTesting(primary);
    ASSERT_NE(to, Table::kNotFound);
    int moved = 0;
    t->between_probes_hook = [&](uint64_t key) {
      if (key == k && moved++ == 0) {
        t->MoveForTesting(to, from);
      }
    };
    EXPECT_EQ(Read(*t, k, batch), 9u) << "the reader missed an entry that moved under it";
    EXPECT_EQ(moved, 1) << "the retry found k in its primary";
    t->between_probes_hook = nullptr;
    EXPECT_EQ(t->Find(k), to);
  }
}

// A reader that starts while a move path is being written (the sequence is
// odd), misses the primary, and then sees the entry leave the alternate for
// the primary: it must not trust its misses until the path ends.
TYPED_TEST(PackedMacTableSharedTest, AReaderStartingInsideAMovePathDoesNotTrustAMiss) {
  for (const bool batch : {false, true}) {
    SCOPED_TRACE(batch ? "LookupBatch" : "Lookup");
    using Table = PackedMacTable<uint32_t, TypeParam>;
    auto t = Table::Create(256);
    const uint64_t k = Key(0x0200'0000'0003);
    const size_t primary = t->PrimaryBucketForTesting(k);
    std::vector<uint64_t> fillers;
    for (uint64_t m = 0x0600'0000'0000; fillers.size() < Table::kWays; m++) {
      const uint64_t f = Key(m);
      if (t->PrimaryBucketForTesting(f) == primary) {
        ASSERT_NE(t->Insert(f, 1, 0), Table::kNotFound);
        fillers.push_back(f);
      }
    }
    ASSERT_NE(t->Insert(k, 4, 0), Table::kNotFound);
    const uint32_t from = t->Find(k);
    ASSERT_EQ(from / Table::kWays, t->AltBucketForTesting(k));
    t->Erase(t->Find(fillers[0]));
    const uint32_t to = t->FreeSlotForTesting(primary);
    t->BeginMovesForTesting();  // the writer is inside a path
    int calls = 0;
    t->between_probes_hook = [&](uint64_t key) {
      if (key != k) {
        return;
      }
      if (calls++ == 0) {
        t->MoveStepForTesting(to, from);  // k leaves the alternate; the path goes on
      } else {
        t->EndMovesForTesting();  // the retry waited for the path to end
      }
    };
    EXPECT_EQ(Read(*t, k, batch), 4u) << "a miss inside a move path was trusted";
    t->between_probes_hook = nullptr;
    if (calls < 2) {
      t->EndMovesForTesting();
    }
  }
}

// A writer that stops inside a move path (preempted) must not stall readers:
// after kMaxRetries a miss stands.
TYPED_TEST(PackedMacTableSharedTest, AReaderGivesUpOnAWriterStuckInAMovePath) {
  for (const bool batch : {false, true}) {
    SCOPED_TRACE(batch ? "LookupBatch" : "Lookup");
    using Table = PackedMacTable<uint32_t, TypeParam>;
    auto t = Table::Create(64);
    const uint64_t absent = Key(0x0200'0000'0077);
    unsigned probes = 0;
    t->between_probes_hook = [&](uint64_t) { probes++; };
    t->BeginMovesForTesting();  // and never ends it during the lookup
    EXPECT_EQ(Read(*t, absent, batch), 0u);
    // The batch probes once itself, then retries through Lookup.
    EXPECT_EQ(probes, Table::kMaxRetries + (batch ? 2u : 1u));
    t->EndMovesForTesting();
    probes = 0;
    EXPECT_EQ(Read(*t, absent, batch), 0u);
    EXPECT_EQ(probes, 1u) << "a stable miss is final at once";
  }
}

// Readers look up keys that are present the whole time while the writer
// inserts and erases others at high load, so inserts move residents (also
// the stable keys) between their buckets. A reader must never miss a stable
// key nor read another key's value.
template <typename Sync>
void ReadersNeverMissWhileTheWriterMoves() {
  using Table = PackedMacTable<uint32_t, Sync>;
  constexpr size_t kCapacity = 4096;
  constexpr uint64_t kStable = 1024;
  auto t = Table::Create(kCapacity);
  for (uint64_t m = 0; m < kStable; m++) {
    ASSERT_NE(t->Insert(Key(m), static_cast<uint16_t>(1 + m % Table::kMaxValue), 0),
              Table::kNotFound);
  }
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> misses{0}, wrong{0}, reads{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 2; r++) {
    readers.emplace_back([&, r] {
      std::mt19937_64 rng(r);
      uint64_t keys[32];
      uint16_t values[32];
      while (!stop.load(std::memory_order_relaxed)) {
        for (auto &k : keys) {
          k = Key(rng() % kStable);
        }
        const uint64_t hits = t->LookupBatch(keys, values);
        for (int i = 0; i < 32; i++) {
          if (!(hits >> i & 1)) {
            misses++;
          } else if (values[i] != 1 + (keys[i] >> 16) % Table::kMaxValue) {
            wrong++;
          }
        }
        const uint64_t k = Key(rng() % kStable);
        if (t->Lookup(k) != 1 + (k >> 16) % Table::kMaxValue) {
          misses++;
        }
        reads += 33;
      }
    });
  }
  std::mt19937_64 rng(99);
  std::vector<uint64_t> churn;
  size_t peak = 0;
  size_t cycles = 0;
  bool filling = true;  // fill to capacity, drain to half, repeat
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  size_t ops = 0;
  while (std::chrono::steady_clock::now() < end || ops < 200000) {
    [[maybe_unused]] auto guard = t->Lock();
    if (filling) {
      const uint64_t key = Key(kStable + rng() % (uint64_t{1} << 40));
      if (t->Find(key) == Table::kNotFound) {
        if (t->Insert(key, 1, 0) != Table::kNotFound) {
          churn.push_back(key);
          peak = std::max(peak, t->size());
        } else {
          filling = false;  // full, or no free slot reachable
        }
      }
    } else {
      const size_t i = rng() % churn.size();
      t->Erase(t->Find(churn[i]));
      churn[i] = churn.back();
      churn.pop_back();
      if (churn.size() <= (kCapacity - kStable) / 2) {
        filling = true;
        cycles++;
      }
    }
    ops++;
  }
  stop = true;
  for (auto &th : readers) {
    th.join();
  }
  EXPECT_EQ(misses.load(), 0u) << "of " << reads.load();
  EXPECT_EQ(wrong.load(), 0u);
  EXPECT_GT(reads.load(), 0u);
  // Near capacity (50% of the slots) most inserts find both buckets full, so
  // they move residents, the stable keys among them.
  EXPECT_GE(peak, kCapacity * 9 / 10);
  EXPECT_GT(cycles, 0u);
}

TEST(PackedMacTableConcurrentTest, SingleWriterReadersNeverMiss) {
  ReadersNeverMissWhileTheWriterMoves<SingleWriter>();
}
TEST(PackedMacTableConcurrentTest, MultiWriterReadersNeverMiss) {
  ReadersNeverMissWhileTheWriterMoves<MultiWriter>();
}

// Two writers learn disjoint keys through TryLock (skipping on contention,
// as packet-path learning does) and Lock; every key ends up exactly once.
TEST(PackedMacTableConcurrentTest, MultiWritersSerialiseThroughTheLock) {
  using Table = PackedMacTable<uint32_t, MultiWriter>;
  auto t = Table::Create(8192);
  std::atomic<uint64_t> skipped{0};
  std::vector<std::thread> writers;
  for (uint64_t w = 0; w < 2; w++) {
    writers.emplace_back([&, w] {
      for (uint64_t m = 0; m < 3000; m++) {
        const uint64_t key = Key(w << 32 | m);
        for (;;) {
          if (m % 2 == 0) {
            auto g = t->Lock();
            ASSERT_NE(t->Insert(key, static_cast<uint16_t>(1 + w), 0), Table::kNotFound);
            break;
          }
          if (auto g = t->TryLock()) {
            ASSERT_NE(t->Insert(key, static_cast<uint16_t>(1 + w), 0), Table::kNotFound);
            break;
          }
          skipped++;
          std::this_thread::yield();
        }
      }
    });
  }
  for (auto &th : writers) {
    th.join();
  }
  EXPECT_EQ(t->size(), 6000u);
  for (uint64_t w = 0; w < 2; w++) {
    for (uint64_t m = 0; m < 3000; m++) {
      ASSERT_EQ(t->Lookup(Key(w << 32 | m)), 1 + w);
    }
  }
}

}  // namespace
}  // namespace bess::l2
