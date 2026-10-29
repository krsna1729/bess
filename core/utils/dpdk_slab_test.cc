// SPDX-License-Identifier: BSD-3-Clause

#include "utils/dpdk_slab.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <stdexcept>
#include <vector>

#include "framework/memory_scope.h"

namespace bess::utils {
namespace {

struct Counted {
  explicit Counted(int v) : value(v) { alive++; }
  ~Counted() { alive--; }
  int value;
  static inline int alive = 0;
};

struct alignas(64) Wide {
  uint64_t words[9];  // 72 bytes: stride rounds to 128
};

struct Tiny {
  char c;
};

struct Throws {
  explicit Throws(bool fail) {
    if (fail) {
      throw std::runtime_error("ctor");
    }
  }
};

TEST(ObjectSlabTest, StrideFitsTheObjectAndTheFreeLink) {
  EXPECT_EQ(sizeof(void *), ObjectSlab<Tiny>::Stride());
  EXPECT_EQ(128u, ObjectSlab<Wide>::Stride());
  EXPECT_EQ(8u, ObjectSlab<Counted>::Stride());
}

TEST(ObjectSlabTest, MakeBuildsAndTheDeleterDestroysAndFrees) {
  auto slab = ObjectSlab<Counted>::Create();
  {
    SlabUnique<Counted> a = slab->Make(7);
    SlabUnique<const Counted> b = slab->Make(8);  // converts to const
    EXPECT_EQ(7, a->value);
    EXPECT_EQ(8, b->value);
    EXPECT_EQ(2, Counted::alive);
    EXPECT_EQ(2u, slab->live());
    EXPECT_EQ(ObjectSlab<Counted>::kObjectsPerChunk * ObjectSlab<Counted>::Stride(),
              slab->bytes());
  }
  EXPECT_EQ(0, Counted::alive);
  EXPECT_EQ(0u, slab->live());
}

TEST(ObjectSlabTest, AFreedSlotIsReusedFirst) {
  auto slab = ObjectSlab<Counted>::Create();
  SlabUnique<Counted> keep = slab->Make(1);
  SlabUnique<Counted> a = slab->Make(2);
  const Counted *freed = a.get();
  a.reset();
  SlabUnique<Counted> b = slab->Make(3);
  EXPECT_EQ(freed, b.get());
  EXPECT_EQ(3, b->value);
}

TEST(ObjectSlabTest, AThrowingConstructorReturnsItsSlot) {
  auto slab = ObjectSlab<Throws>::Create();
  EXPECT_THROW(slab->Make(true), std::runtime_error);
  EXPECT_EQ(0u, slab->live());
  SlabUnique<Throws> ok = slab->Make(false);
  EXPECT_EQ(1u, slab->live());
}

TEST(ObjectSlabTest, ObjectsAreAlignedAndDistinct) {
  auto slab = ObjectSlab<Wide>::Create();
  std::vector<SlabUnique<Wide>> objects;
  std::set<const Wide *> seen;
  for (int i = 0; i < 100; i++) {
    objects.push_back(slab->Make());
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(objects.back().get()) % alignof(Wide));
    EXPECT_TRUE(seen.insert(objects.back().get()).second);
  }
}

// Readers may hold an object until a grace period: growing the slab must
// never move one.
TEST(ObjectSlabTest, PointersStayStableAcrossChunkGrowth) {
  auto slab = ObjectSlab<Counted>::Create();
  constexpr size_t kCount = ObjectSlab<Counted>::kObjectsPerChunk * 3 + 5;
  std::vector<SlabUnique<Counted>> objects;
  std::vector<const Counted *> addresses;
  for (size_t i = 0; i < kCount; i++) {
    objects.push_back(slab->Make(static_cast<int>(i)));
    addresses.push_back(objects.back().get());
  }
  EXPECT_EQ(4 * ObjectSlab<Counted>::kObjectsPerChunk *
                ObjectSlab<Counted>::Stride(),
            slab->bytes());
  for (size_t i = 0; i < kCount; i++) {
    EXPECT_EQ(addresses[i], objects[i].get());
    EXPECT_EQ(static_cast<int>(i), objects[i]->value);
  }
  objects.clear();
  EXPECT_EQ(0u, slab->live());
  EXPECT_EQ(0, Counted::alive);
}

TEST(ObjectSlabTest, RawSlotsRoundTrip) {
  auto slab = ObjectSlab<Counted>::Create();
  void *slot = slab->Allocate();
  EXPECT_EQ(1u, slab->live());
  slab->Free(slot);
  EXPECT_EQ(0u, slab->live());
  EXPECT_EQ(slot, slab->Allocate());
  slab->Free(slot);
}

// An owner that goes first (a SlotTable destroyed before the engine that
// still holds one of its objects): the slab outlives it until the last
// object is freed, then deletes itself.
TEST(ObjectSlabTest, AReleasedSlabLivesUntilItsLastObjectIsFreed) {
  ObjectSlab<Counted>::Owner owner = ObjectSlab<Counted>::Create();
  SlabUnique<Counted> a = owner->Make(1);
  SlabUnique<const Counted> b = owner->Make(2);
  owner.reset();  // Release with two objects live
  EXPECT_EQ(1, a->value);
  a.reset();  // the slab is still there: its mutex and free list work
  EXPECT_EQ(2, b->value);
  b.reset();  // the last one deletes the slab (ASan/LSan see a leak if not)
  EXPECT_EQ(0, Counted::alive);
}

TEST(ObjectSlabTest, ReleasingAnEmptySlabDeletesItAtOnce) {
  ObjectSlab<Counted>::Owner owner = ObjectSlab<Counted>::Create();
  owner->Make(1).reset();
  EXPECT_EQ(0u, owner->live());
  owner.reset();
  EXPECT_EQ(0, Counted::alive);
}

// Records the growth a slab asks (D-198); refuses it when `refuse` is set.
struct RecordingGrowth final : MemoryGrowth {
  std::vector<size_t> grown;
  size_t shrunk = 0;
  bool refuse = false;
  void Grow(size_t bytes, size_t glibc_bytes) override {
    EXPECT_EQ(0u, glibc_bytes);
    if (refuse) {
      throw MemoryRefused(MemoryRefused::Reason::kNormalLimit, bytes,
                          "refused");
    }
    grown.push_back(bytes);
  }
  void Shrink(size_t bytes, size_t) noexcept override { shrunk += bytes; }
};

TEST(ObjectSlabTest, CreateCostsNothingAndBytesMatches) {
  EXPECT_EQ(0u, bess::framework::MemoryCost<ObjectSlab<Counted>>::Bytes());
  auto slab = ObjectSlab<Counted>::Create();
  EXPECT_EQ(bess::framework::MemoryCost<ObjectSlab<Counted>>::Bytes(),
            slab->bytes());
  auto wide = ObjectSlab<Wide>::Create(MemoryNode());
  EXPECT_EQ(bess::framework::MemoryCost<ObjectSlab<Wide>>::Bytes(MemoryNode()),
            wide->bytes());
  EXPECT_EQ(ObjectSlab<Wide>::kObjectsPerChunk * 128,
            ObjectSlab<Wide>::chunk_bytes());
}

// Each new chunk is admitted before it is allocated; reused slots and the
// rest of a chunk are not.
TEST(ObjectSlabTest, EachChunkIsAdmittedFirst) {
  auto slab = ObjectSlab<Counted>::Create();
  RecordingGrowth growth;
  slab->set_memory_growth(&growth);
  std::vector<SlabUnique<Counted>> objects;
  for (size_t i = 0; i < ObjectSlab<Counted>::kObjectsPerChunk + 1; i++) {
    objects.push_back(slab->Make(static_cast<int>(i)));
  }
  const size_t chunk = ObjectSlab<Counted>::chunk_bytes();
  EXPECT_EQ((std::vector<size_t>{chunk, chunk}), growth.grown);
  EXPECT_EQ(2 * chunk, slab->bytes());
  objects.pop_back();
  objects.push_back(slab->Make(7));  // the freed slot: no growth
  EXPECT_EQ(2u, growth.grown.size());
  EXPECT_EQ(0u, growth.shrunk);
}

// A refused growth adds no chunk and loses no slot; the next one may.
TEST(ObjectSlabTest, ARefusedChunkLeavesTheSlabUnchanged) {
  auto slab = ObjectSlab<Counted>::Create();
  RecordingGrowth growth;
  slab->set_memory_growth(&growth);
  growth.refuse = true;
  EXPECT_THROW(slab->Make(1), MemoryRefused);
  EXPECT_EQ(0u, slab->bytes());
  EXPECT_EQ(0u, slab->live());
  EXPECT_EQ(0, Counted::alive);
  growth.refuse = false;
  SlabUnique<Counted> object = slab->Make(2);
  EXPECT_EQ(ObjectSlab<Counted>::chunk_bytes(), slab->bytes());
  EXPECT_EQ(1u, growth.grown.size());
  EXPECT_EQ(0u, growth.shrunk);
}

}  // namespace
}  // namespace bess::utils

