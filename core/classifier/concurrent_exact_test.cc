// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/concurrent_exact.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <random>
#include <thread>
#include <vector>

#include "control/runtime_state.h"

namespace bess::classifier {
namespace {

constexpr uint32_t kKeyLen = 12;
using Key = std::array<std::byte, kKeyLen>;

Key K(uint32_t id) {
  Key k{};
  std::memcpy(k.data(), &id, sizeof(id));
  k[8] = std::byte{0x5a};
  return k;
}

ConstBytes B(const Key &k) { return ConstBytes(k.data(), k.size()); }

// Every key maps to a value only it has, so a reader can tell a right answer
// from one that belongs to a different key.
uint64_t V(uint32_t id) { return (uint64_t{id} << 20) | 0xabcde; }

std::unique_ptr<ConcurrentExactTable> MakeTable(uint32_t capacity) {
  auto table = ConcurrentExactTable::Create(kKeyLen, capacity,
                                            control::runtime().rcu());
  EXPECT_TRUE(table.has_value()) << table.error();
  return std::move(*table);
}

uint64_t Lookup(const ConcurrentExactTable &t, uint32_t id, uint64_t *value) {
  const Key k = K(id);
  return t.LookupBatch(B(k), kKeyLen, value, 1);
}

TEST(ConcurrentExactTableTest, InsertUpdateEraseAndIterate) {
  auto t = MakeTable(64);
  using R = ConcurrentExactTable::UpsertResult;
  EXPECT_EQ(R::kInserted, t->Upsert(B(K(1)), V(1)));
  EXPECT_EQ(R::kInserted, t->Upsert(B(K(2)), V(2)));
  EXPECT_EQ(R::kUpdated, t->Upsert(B(K(2)), V(7)));
  EXPECT_EQ(2u, t->size());
  // DPDK contract: adding an existing key swaps its value in place (an
  // atomic exchange readers see whole) and takes no new key slot.
  EXPECT_EQ(2u, t->slots_in_use());

  uint64_t v = 0;
  EXPECT_EQ(1u, Lookup(*t, 2, &v));
  EXPECT_EQ(V(7), v);
  v = 42;
  EXPECT_EQ(0u, Lookup(*t, 3, &v));
  EXPECT_EQ(42u, v) << "a miss must leave the value untouched";

  std::map<uint64_t, uint64_t> seen;
  t->ForEach([&](ConstBytes key, uint64_t value) {
    uint32_t id;
    std::memcpy(&id, key.data(), sizeof(id));
    seen[id] = value;
  });
  EXPECT_EQ((std::map<uint64_t, uint64_t>{{1, V(1)}, {2, V(7)}}), seen);

  EXPECT_TRUE(t->Erase(B(K(1))));
  EXPECT_FALSE(t->Erase(B(K(1))));
  EXPECT_EQ(0u, Lookup(*t, 1, &v));
  EXPECT_EQ(1u, t->size());
}

// Decision D-015: lookups hash inline with a fixed-width kernel and call
// DPDK's prehashed bulk lookup. That is only correct if the kernel is
// bit-identical to the table's own hash (rte_hash_hash, used by every add and
// delete) for every key width and alignment.
TEST(ConcurrentExactTableTest, InlineHashIsBitIdenticalToRteHash) {
  std::mt19937_64 rng(0x15);
  std::vector<std::byte> buffer(64 + 8);
  for (uint32_t width = 1; width <= 64; width++) {
    auto t = ConcurrentExactTable::Create(width, 64, control::runtime().rcu());
    ASSERT_TRUE(t.has_value()) << t.error();
    for (int trial = 0; trial < 64; trial++) {
      for (auto &b : buffer) b = static_cast<std::byte>(rng());
      const size_t offset = static_cast<size_t>(trial % 8);  // alignments
      const ConstBytes key(buffer.data() + offset, width);
      ASSERT_EQ((*t)->DpdkHash(key), (*t)->InlineHash(key))
          << "width " << width << " offset " << offset;
    }
  }
}

// The fixed-width key compare installed into rte_hash must say "equal"
// exactly when the bytes are equal, for every width and any differing byte.
TEST(ConcurrentExactTableTest, FixedWidthCompareIsExactEquality) {
  std::mt19937_64 rng(0x16);
  std::vector<std::byte> a(64), b(64);
  for (size_t width = 1; width <= 64; width++) {
    const detail::CmpFn cmp = detail::SelectCmp(width);
    if (cmp == nullptr) {
      EXPECT_EQ(0u, width % 16) << "only DPDK-specialized widths are skipped";
      continue;
    }
    for (auto &x : a) x = static_cast<std::byte>(rng());
    b = a;
    EXPECT_EQ(0, cmp(a.data(), b.data(), width)) << "width " << width;
    for (size_t byte = 0; byte < width; byte++) {
      b = a;
      b[byte] ^= std::byte{1} << (rng() % 8);
      EXPECT_NE(0, cmp(a.data(), b.data(), width))
          << "width " << width << " byte " << byte;
    }
    b = a;
    if (width < 64) {
      b[width] ^= std::byte{0xff};  // beyond the key: must be ignored
      EXPECT_EQ(0, cmp(a.data(), b.data(), width)) << "width " << width;
    }
  }
}

TEST(ConcurrentExactTableTest, ReportsFullAndReusesErasedSlots) {
  auto t = MakeTable(64);
  uint32_t n = 0;
  while (t->Upsert(B(K(n)), V(n)) !=
         ConcurrentExactTable::UpsertResult::kFull) {
    ASSERT_LT(++n, 1000u);
  }
  EXPECT_GE(n, 64u);
  EXPECT_EQ(n, t->size());
  // No reader is online, so erased slots come back at the next reclaim.
  for (uint32_t i = 0; i < 100; i++) {
    ASSERT_TRUE(t->Erase(B(K(i % n))) || i >= n);
    ASSERT_NE(ConcurrentExactTable::UpsertResult::kFull,
              t->Upsert(B(K(10000 + i)), V(10000 + i)))
        << "erased slot " << i << " never came back";
    ASSERT_TRUE(t->Erase(B(K(10000 + i))));
    ASSERT_EQ(ConcurrentExactTable::UpsertResult::kInserted,
              t->Upsert(B(K(i % n)), V(i % n)));
  }
}

// DPDK contract (rte_hash LF with QSBR in defer-queue mode), deterministically:
// a deleted key's slot is not handed
// back while a registered reader that was online before the delete has not
// reported quiescence -- that reader may have matched the key and be about
// to load its value (DPDK's LF lookup compares the key, then loads the
// value; a reused slot would give it another key's value). Once the reader
// is quiescent, the slot comes back.
// DPDK behaviour G1.2b relies on (D-007): rte_hash_add_key_data on a key
// that is already present updates its value in place and never fails, even
// when the table has no free slot. ExactRuleResource::Publish depends on it.
TEST(ConcurrentExactTableTest, UpsertOfAPresentKeySucceedsWhenFull) {
  auto t = MakeTable(768);
  std::vector<uint32_t> ids;
  for (uint32_t id = 1;; id++) {
    const auto r = t->Upsert(B(K(id)), V(id));
    if (r == ConcurrentExactTable::UpsertResult::kFull) {
      break;
    }
    ids.push_back(id);
    ASSERT_LT(ids.size(), 2000u);
  }
  for (uint32_t id : ids) {
    ASSERT_EQ(t->Upsert(B(K(id)), V(id) + 1),
              ConcurrentExactTable::UpsertResult::kUpdated);
  }
}

// DPDK behaviour relied on (D-007): with the default defer-queue size
// (dq_size = the table's key slots) rte_hash_del_key never falls back to
// rte_rcu_qsbr_synchronize(), which it does when the defer queue is full --
// every outstanding entry is one deleted slot, so the queue cannot fill.
// Deleting every key with a stalled reader must therefore return promptly.
TEST(ConcurrentExactTableTest, DeletesNeverWaitForStalledReaders) {
  rcu::RcuDomain &domain = control::runtime().rcu();
  constexpr rcu::ReaderId kReader = 29;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  domain.Online(kReader);  // stalled: never quiesces while we delete
  auto t = MakeTable(768);
  std::vector<uint32_t> ids;
  for (uint32_t id = 1; t->Upsert(B(K(id)), V(id)) !=
                        ConcurrentExactTable::UpsertResult::kFull;
       id++) {
    ids.push_back(id);
  }
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (uint32_t id : ids) {
      t->Erase(B(K(id)));
    }
    done = true;
  });
  for (int i = 0; i < 300 && !done; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const bool finished = done.load();
  domain.Quiescent(kReader);  // release a blocked writer, if any
  writer.join();
  EXPECT_TRUE(finished) << "rte_hash_del_key waited for a stalled reader";
  domain.Offline(kReader);
  domain.Unregister(kReader);
  t->ReclaimAll();
}

TEST(ConcurrentExactTableTest, ErasedSlotWaitsForOnlineReaders) {
  rcu::RcuDomain &domain = control::runtime().rcu();
  auto t = MakeTable(1024);
  for (uint32_t id = 0; id < 100; id++) {
    ASSERT_EQ(ConcurrentExactTable::UpsertResult::kInserted,
              t->Upsert(B(K(id)), V(id)));
  }
  constexpr uint32_t kReader = 20;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  domain.Online(kReader);

  for (uint32_t id = 0; id < 10; id++) {
    ASSERT_TRUE(t->Erase(B(K(id))));
  }
  // Churn the writer side: every insert must take a fresh slot, never one
  // of the ten held back.
  for (uint32_t id = 1000; id < 1050; id++) {
    t->Upsert(B(K(id)), V(id));
    ASSERT_TRUE(t->Erase(B(K(id))));
  }
  EXPECT_EQ(90u, t->size());
  EXPECT_EQ(150u, t->slots_in_use())
      << "a deleted slot was released while a reader was mid-grace-period";

  domain.Quiescent(kReader);
  for (int i = 0; i < 16; i++) {
    t->Reclaim();  // at most one DPDK batch per call
  }
  EXPECT_EQ(90u, t->slots_in_use())
      << "slots never came back after the reader passed quiescence";

  domain.Offline(kReader);
  domain.Unregister(kReader);
}

// Decision D-010 sizing: capacities are 3/4 of a power of two with room for
// the rules plus headroom, and deletes still in a grace period count against
// that headroom until they are reclaimed.
TEST(ConcurrentExactTableTest, SizingCountsPendingDeletesAgainstHeadroom) {
  using T = ConcurrentExactTable;
  EXPECT_EQ(768u, T::CapacityFor(0));
  EXPECT_EQ(768u, T::CapacityFor(511));
  EXPECT_EQ(1536u, T::CapacityFor(512));
  EXPECT_EQ(3u << 19, T::CapacityFor(1000000));  // 1572864 slots, 5% spare
  EXPECT_EQ(256u, T::Headroom(768));
  EXPECT_EQ(78643u, T::Headroom(3u << 19));

  rcu::RcuDomain &domain = control::runtime().rcu();
  auto t = MakeTable(T::CapacityFor(0));
  ASSERT_EQ(768u, t->capacity());
  for (uint32_t id = 0; id < 500; id++) {
    ASSERT_EQ(T::UpsertResult::kInserted, t->Upsert(B(K(id)), V(id)));
  }
  constexpr uint32_t kReader = 23;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  domain.Online(kReader);
  for (uint32_t id = 0; id < 20; id++) {
    ASSERT_TRUE(t->Erase(B(K(id))));
  }
  EXPECT_EQ(480u, t->size());
  EXPECT_TRUE(t->HasRoomForOne());  // 500 slots taken + 1 + 256 <= 768
  for (uint32_t id = 500; id < 512; id++) {
    ASSERT_EQ(T::UpsertResult::kInserted, t->Upsert(B(K(id)), V(id)));
  }
  EXPECT_FALSE(t->HasRoomForOne())
      << "deletes still in their grace period must count against headroom";

  domain.Quiescent(kReader);
  EXPECT_TRUE(t->HasRoomForOne()) << "reclaimed slots must count as free";
  EXPECT_EQ(492u, t->slots_in_use());
  domain.Offline(kReader);
  domain.Unregister(kReader);
}

// The mode C contract: while one writer churns the table, readers never miss
// a key that stays present, and never see a value that belongs to another
// key. The table is small and nearly full, so nearly every insert reuses a
// slot a delete just released and cuckoo displacement moves the stable keys
// around -- a slot handed back before readers pass a grace period would show
// up as a churned key answering with another key's value, and a displacement
// a reader cannot follow as a stable key missing.
TEST(ConcurrentExactTableTest, ConcurrentReadersOnlySeeJustifiedAnswers) {
  rcu::RcuDomain &domain = control::runtime().rcu();
  constexpr uint32_t kCapacity = 256;
  auto t = MakeTable(kCapacity);

  constexpr uint32_t kStable = 128;
  constexpr uint32_t kChurn = 4096;  // ids kStable .. kStable + kChurn
  for (uint32_t id = 0; id < kStable; id++) {
    ASSERT_EQ(ConcurrentExactTable::UpsertResult::kInserted,
              t->Upsert(B(K(id)), V(id)));
  }
  // Readers look up every stable key and a window of churned ones.
  std::vector<Key> keys;
  std::vector<uint32_t> ids;
  for (uint32_t i = 0; i < 64; i++) {
    ids.push_back(i * 2);                 // stable
    ids.push_back(kStable + (i * 61) % kChurn);  // churned
  }
  for (uint32_t id : ids) {
    keys.push_back(K(id));
  }

  constexpr int kReaders = 2;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> missed{0}, wrong{0}, lookups{0}, churn_hits{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; r++) {
    const uint32_t reader_id = 10 + r;
    ASSERT_TRUE(domain.Register(reader_id).has_value());
    readers.emplace_back([&, reader_id] {
      domain.Online(reader_id);
      uint64_t local = 0, local_churn_hits = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        for (size_t base = 0; base < keys.size(); base += 32) {
          uint64_t values[32];
          const uint64_t hits = t->LookupBatch(
              ConstBytes(keys[base].data(), 32 * kKeyLen), kKeyLen, values,
              32);
          for (size_t i = 0; i < 32; i++) {
            const uint32_t id = ids[base + i];
            const bool hit = (hits >> i) & 1;
            if (hit && values[i] != V(id)) {
              wrong++;
            } else if (!hit && id < kStable) {
              missed++;
            } else if (hit && id >= kStable) {
              local_churn_hits++;
            }
          }
        }
        local += keys.size();
        domain.Quiescent(reader_id);
      }
      lookups += local;
      churn_hits += local_churn_hits;
      domain.Offline(reader_id);
    });
  }

  // Keep the table at its limit: erase a present churned key, insert an
  // absent one.
  std::mt19937_64 rng(0xc0ffee);
  std::vector<uint32_t> present;
  std::vector<bool> in(kChurn, false);
  uint64_t ops = 0, full = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!present.empty() && (present.size() >= kCapacity - kStable ||
                             rng() % 2 == 0)) {
      const size_t at = rng() % present.size();
      const uint32_t c = present[at];
      ASSERT_TRUE(t->Erase(B(K(kStable + c))));
      in[c] = false;
      present[at] = present.back();
      present.pop_back();
    } else {
      const uint32_t c = rng() % kChurn;
      if (in[c]) {
        continue;
      }
      // Transiently full while erased slots wait out a reader grace period;
      // that is the defer queue working.
      if (t->Upsert(B(K(kStable + c)), V(kStable + c)) ==
          ConcurrentExactTable::UpsertResult::kFull) {
        full++;
        continue;
      }
      in[c] = true;
      present.push_back(c);
    }
    ops++;
  }
  stop = true;
  for (auto &th : readers) {
    th.join();
  }
  for (int r = 0; r < kReaders; r++) {
    domain.Unregister(10 + r);
  }

  EXPECT_EQ(0u, wrong.load()) << "of " << lookups.load() << " lookups";
  EXPECT_EQ(0u, missed.load()) << "of " << lookups.load() << " lookups";
  EXPECT_GT(ops, 100000u) << full << " full";
  EXPECT_GT(churn_hits.load(), 0u);
  for (uint32_t id = 0; id < kStable; id++) {
    uint64_t v = 0;
    ASSERT_EQ(1u, Lookup(*t, id, &v));
    ASSERT_EQ(V(id), v);
  }
}

// A shared table (D-028): writers race to add the same flows, as workers
// that all see a new flow's first packets would. Each key must be inserted
// exactly once, every racer must get the winner's value, size() must be
// exact, and lock-free readers running throughout must only ever see a
// key's winning value.
TEST(ConcurrentExactTableTest, SharedWritersInsertEachKeyOnce) {
  rcu::RcuDomain &domain = control::runtime().rcu();
  constexpr uint32_t kKeys = 20000;
  constexpr int kWriters = 4;
  auto t = ConcurrentExactTable::Create(
      kKeyLen, ConcurrentExactTable::CapacityFor(kKeys), domain,
      SOCKET_ID_ANY, ConcurrentExactTable::Writers::kShared);
  ASSERT_TRUE(t.has_value());
  ConcurrentExactTable &table = **t;

  std::atomic<bool> go{false}, stop{false};
  std::atomic<uint64_t> inserted{0}, wrong{0};
  // winner[k]: the writer id whose value landed (writer w writes w+1).
  std::vector<std::atomic<uint64_t>> winner(kKeys);
  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; w++) {
    writers.emplace_back([&, w] {
      while (!go.load()) {
      }
      for (uint32_t k = 0; k < kKeys; k++) {
        // Every writer walks the keys in a different order.
        const uint32_t id = (k * 7919u + static_cast<uint32_t>(w) * 104729u) %
                            kKeys;
        const Key key = K(id);
        const auto r = table.InsertIfAbsent(B(key), w + 1);
        if (r.status == ConcurrentExactTable::InsertResult::Status::kInserted) {
          inserted++;
          uint64_t expected = 0;
          if (!winner[id].compare_exchange_strong(expected, w + 1)) {
            wrong++;  // a second insert of the same key
          }
        } else if (r.status ==
                   ConcurrentExactTable::InsertResult::Status::kExists) {
          if (r.value == static_cast<uint64_t>(w + 1)) {
            wrong++;  // told "exists" with its own value
          }
        } else {
          wrong++;  // full: sized for every key
        }
      }
    });
  }
  // A registered reader: a key it finds holds some writer's value, never
  // anything else.
  constexpr rcu::ReaderId kReader = 28;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  std::atomic<uint64_t> seen{0}, bad_reads{0};
  std::thread reader([&] {
    domain.Online(kReader);
    std::mt19937 rng(5);
    while (!stop.load()) {
      const Key key = K(rng() % kKeys);
      uint64_t value = 0;
      if (table.LookupBatch(B(key), kKeyLen, &value, 1)) {
        seen++;
        if (value < 1 || value > kWriters) {
          bad_reads++;
        }
      }
      domain.Quiescent(kReader);
    }
    domain.Offline(kReader);
  });
  go = true;
  for (auto &w : writers) {
    w.join();
  }
  stop = true;
  reader.join();
  domain.Unregister(kReader);

  EXPECT_EQ(wrong.load(), 0u);
  EXPECT_EQ(bad_reads.load(), 0u);
  EXPECT_EQ(inserted.load(), kKeys);
  EXPECT_EQ(table.size(), kKeys);
  EXPECT_GT(seen.load(), 0u);
  // Every key holds its recorded winner's value.
  for (uint32_t id = 0; id < kKeys; id++) {
    const Key key = K(id);
    uint64_t value = 0;
    ASSERT_EQ(table.LookupBatch(B(key), kKeyLen, &value, 1), 1u);
    ASSERT_EQ(value, winner[id].load()) << id;
  }
}

// Shared writers adding and erasing their own flows concurrently keep an
// exact count, and the table ends with exactly the live flows.
TEST(ConcurrentExactTableTest, SharedWritersChurnKeepsExactCount) {
  rcu::RcuDomain &domain = control::runtime().rcu();
  constexpr int kWriters = 4;
  constexpr uint32_t kLive = 2000, kSteps = 20000;
  auto t = ConcurrentExactTable::Create(
      kKeyLen, ConcurrentExactTable::CapacityFor(kWriters * kLive * 2), domain,
      SOCKET_ID_ANY, ConcurrentExactTable::Writers::kShared);
  ASSERT_TRUE(t.has_value());
  ConcurrentExactTable &table = **t;
  std::vector<std::thread> writers;
  std::atomic<uint64_t> failures{0};
  for (int w = 0; w < kWriters; w++) {
    writers.emplace_back([&, w] {
      const uint32_t base = static_cast<uint32_t>(w) * 1000000u;
      for (uint32_t i = 0; i < kSteps; i++) {
        const Key add = K(base + i);
        if (table.InsertIfAbsent(B(add), i).status !=
            ConcurrentExactTable::InsertResult::Status::kInserted) {
          failures++;
        }
        if (i >= kLive && !table.Erase(B(K(base + i - kLive)))) {
          failures++;
        }
      }
    });
  }
  for (auto &w : writers) {
    w.join();
  }
  EXPECT_EQ(failures.load(), 0u);
  EXPECT_EQ(table.size(), static_cast<size_t>(kWriters) * kLive);
  size_t visited = 0;
  table.ForEach([&](ConstBytes, uint64_t) { visited++; });
  EXPECT_EQ(visited, static_cast<size_t>(kWriters) * kLive);
}

}  // namespace
}  // namespace bess::classifier
