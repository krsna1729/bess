// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/record_arena.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "framework/memory_scope.h"
#include "runtime/runtime_state.h"
#include "utils/dpdk_memory.h"

namespace bess::classifier {
namespace {

using Bytes = std::array<std::byte, 8>;

Bytes B(uint64_t v) {
  Bytes b;
  std::memcpy(b.data(), &v, sizeof(v));
  return b;
}

void Write(RecordArena &arena, RecordArena::Id id, const Bytes &b) {
  const MutableBytes record = arena.Record(id);
  ASSERT_EQ(record.size(), sizeof(Bytes));
  std::memcpy(record.data(), b.data(), b.size());
}

uint64_t Read(RecordArena &arena, RecordArena::Id id) {
  uint64_t v = 0;
  const ConstBytes record = arena.Lookup(id);
  EXPECT_EQ(record.size(), sizeof(Bytes));
  std::memcpy(&v, record.data(), sizeof(v));
  return v;
}

TEST(RecordArenaTest, StoresAndLooksUpExactBytes) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  RecordArena arena(sizeof(Bytes), domain);
  const RecordArena::Id a = arena.Allocate();
  const RecordArena::Id b = arena.Allocate();
  ASSERT_NE(a, 0u);
  ASSERT_NE(b, 0u);
  ASSERT_NE(a, b);
  Write(arena, a, B(0x1111));
  Write(arena, b, B(0x2222));
  EXPECT_EQ(0x1111u, Read(arena, a));
  EXPECT_EQ(0x2222u, Read(arena, b));
  EXPECT_EQ(2u, arena.in_use());
  EXPECT_EQ(0u, arena.retiring());
}

TEST(RecordArenaTest, IdZeroAndOutOfRangeAreEmpty) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  RecordArena arena(sizeof(Bytes), domain);
  EXPECT_TRUE(arena.Lookup(0).empty());
  EXPECT_TRUE(arena.Lookup(99).empty());
  EXPECT_TRUE(arena.Record(0).empty());
}

TEST(RecordArenaTest, AbortReusesTheIdImmediately) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  RecordArena arena(sizeof(Bytes), domain);
  const RecordArena::Id a = arena.Allocate();
  arena.Record(a)[0] = std::byte{0xFF};
  arena.Abort(a);
  EXPECT_EQ(0u, arena.in_use());
  const RecordArena::Id b = arena.Allocate();
  EXPECT_EQ(a, b);
}

TEST(RecordArenaTest, RetiredIdWaitsForOnlineReaders) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  constexpr uint32_t kReader = 24;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  domain.Online(kReader);

  RecordArena arena(sizeof(Bytes), domain);
  const RecordArena::Id a = arena.Allocate();
  Write(arena, a, B(1));
  arena.Retire(a);
  EXPECT_EQ(1u, arena.retiring());
  EXPECT_EQ(1u, arena.in_use());
  EXPECT_NE(a, arena.Allocate())
      << "a reader may still hold the retired record";

  domain.Quiescent(kReader);
  // Reclaim happens on allocation.
  const RecordArena::Id c = arena.Allocate();
  EXPECT_EQ(0u, arena.retiring());
  domain.Offline(kReader);
  domain.Unregister(kReader);
  EXPECT_NE(0u, c);
}

// A teardown only retires: Allocate() never runs, so Retire() itself must
// reclaim, or the queue grows to every id ever erased (and, reserved one
// slot at a time, was copied on every reservation).
TEST(RecordArenaTest, EraseOnlyWorkloadKeepsRetiringBounded) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  RecordArena arena(sizeof(Bytes), domain);
  constexpr size_t kRecords = 4096;
  std::vector<RecordArena::Id> ids;
  for (size_t i = 0; i < kRecords; i++) {
    ids.push_back(arena.Allocate());
    ASSERT_NE(0u, ids.back());
  }
  // No reader is online, so every grace period is complete at once.
  for (const RecordArena::Id id : ids) {
    arena.ReserveRetirement(1);
    arena.Retire(id);
    ASSERT_LE(arena.retiring(), 1u) << "completed retirements must be freed";
  }
  EXPECT_LE(arena.in_use(), 1u);
}

// Sixteen readers each pass a quiescent state every 16K retirements,
// staggered 1K apart: about 16K retirements stay pending, and completions
// arrive 1K at a time. A compaction per 1K completions moved the whole
// backlog (~15K entries) each time; compacting only when half the queue is
// dead moves each id at most once (CL-8).
TEST(RecordArenaTest, LaggingReadersMoveEachRetiredIdAtMostOnce) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  constexpr uint32_t kFirstReader = 40;
  constexpr uint32_t kReaders = 16;
  constexpr size_t kStep = 1024;
  constexpr size_t kRetires = 200 * kStep;
  for (uint32_t r = 0; r < kReaders; r++) {
    ASSERT_TRUE(domain.Register(kFirstReader + r).has_value());
    domain.Online(kFirstReader + r);
  }
  RecordArena arena(sizeof(Bytes), domain);
  size_t max_retiring = 0;
  for (size_t i = 0; i < kRetires; i++) {
    if (i % kStep == 0) {
      domain.Quiescent(kFirstReader +
                       static_cast<uint32_t>((i / kStep) % kReaders));
    }
    const RecordArena::Id id = arena.Allocate();
    ASSERT_NE(0u, id);
    arena.ReserveRetirement(1);
    arena.Retire(id);
    max_retiring = std::max(max_retiring, arena.retiring());
  }
  for (uint32_t r = 0; r < kReaders; r++) {
    domain.Offline(kFirstReader + r);
    domain.Unregister(kFirstReader + r);
  }
  // The backlog is the lag the readers impose, not the ids ever retired.
  EXPECT_GE(max_retiring, (kReaders - 1) * kStep);
  EXPECT_LE(max_retiring, (kReaders + 1) * kStep);
  RecordProperty("max_retiring", static_cast<int>(max_retiring));
  RecordProperty("compaction_moves",
                 static_cast<int>(arena.compaction_moves()));
  EXPECT_LE(arena.compaction_moves(), kRetires);
}

TEST(RecordArenaTest, CrossesChunks) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  RecordArena arena(sizeof(Bytes), domain);
  constexpr size_t kRecords = (size_t{1} << RecordArena::kChunkBits) + 3;
  std::array<RecordArena::Id, kRecords> ids;
  for (size_t i = 0; i < kRecords; i++) {
    ids[i] = arena.Allocate();
    ASSERT_NE(0u, ids[i]);
    Write(arena, ids[i], B(i));
  }
  for (size_t i = 0; i < kRecords; i++) {
    EXPECT_EQ(i, Read(arena, ids[i])) << "record " << i;
  }
}

TEST(RecordArenaTest, RecordsAreAlignedAndDistinct) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  RecordArena arena(3, domain);
  EXPECT_EQ(3u, arena.record_size());
  const RecordArena::Id a = arena.Allocate();
  const RecordArena::Id b = arena.Allocate();
  const auto *pa = arena.Lookup(a).data();
  const auto *pb = arena.Lookup(b).data();
  EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(pa) % alignof(std::max_align_t));
  EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(pb) % alignof(std::max_align_t));
  EXPECT_GE(pb - pa, 3);
}

// D-198: Bytes() is what the constructor allocates, whatever the record
// size (the published-chunk array); chunks come with Allocate.
TEST(RecordArenaTest, BytesMatchesConstruction) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  for (const size_t record_size : {size_t{1}, size_t{24}, size_t{4096}}) {
    RecordArena arena(record_size, domain);
    EXPECT_EQ(bess::framework::MemoryCost<RecordArena>::Bytes(record_size,
                                                              domain),
              arena.bytes());
    ASSERT_NE(0u, arena.Allocate());
    EXPECT_EQ(bess::framework::MemoryCost<RecordArena>::Bytes(record_size,
                                                              domain) +
                  RecordArena::ChunkBytes(record_size),
              arena.bytes());
  }
}

struct RecordingGrowth final : bess::utils::MemoryGrowth {
  std::vector<size_t> grown;
  size_t shrunk = 0;
  bool refuse = false;
  void Grow(size_t bytes, size_t glibc_bytes) override {
    EXPECT_EQ(0u, glibc_bytes);  // chunks are DPDK memory
    if (refuse) {
      throw bess::utils::MemoryRefused(
          bess::utils::MemoryRefused::Reason::kNormalLimit, bytes, "refused");
    }
    grown.push_back(bytes);
  }
  void Shrink(size_t bytes, size_t) noexcept override { shrunk += bytes; }
};

TEST(RecordArenaTest, GrowthAdmitsEachChunkFirst) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  RecordArena arena(sizeof(Bytes), domain);
  RecordingGrowth growth;
  arena.set_memory_growth(&growth);
  const size_t initial = arena.bytes();

  // Refused: the arena is unchanged and nothing was given back twice.
  growth.refuse = true;
  EXPECT_THROW((void)arena.Allocate(), bess::utils::MemoryRefused);
  EXPECT_EQ(initial, arena.bytes());
  EXPECT_EQ(0u, arena.in_use());
  EXPECT_TRUE(growth.grown.empty());

  // Admitted: one chunk for the first 1023 ids (id 0 is never a record).
  growth.refuse = false;
  for (uint32_t i = 1; i < (1u << RecordArena::kChunkBits); i++) {
    ASSERT_NE(0u, arena.Allocate());
  }
  ASSERT_EQ(1u, growth.grown.size());
  EXPECT_EQ(RecordArena::ChunkBytes(sizeof(Bytes)), growth.grown[0]);
  EXPECT_EQ(initial + growth.grown[0], arena.bytes());
  // The next id opens the second chunk.
  ASSERT_NE(0u, arena.Allocate());
  ASSERT_EQ(2u, growth.grown.size());
  EXPECT_EQ(initial + 2 * RecordArena::ChunkBytes(sizeof(Bytes)),
            arena.bytes());
  EXPECT_EQ(0u, growth.shrunk);
}

}  // namespace
}  // namespace bess::classifier
