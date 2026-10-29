// SPDX-License-Identifier: BSD-3-Clause

#include "dataplane/slot_table.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "dataplane/strong_id.h"
#include "framework/memory_scope.h"

namespace bess::dataplane {
namespace {

struct ObjTag {};
using ObjId = StrongId<ObjTag, uint32_t>;

struct Obj {
  uint64_t value;
};

using Table = SlotTable<ObjId, Obj>;

// Records the growth a table asks (D-198); refuses it when `refuse` is set.
struct RecordingGrowth final : utils::MemoryGrowth {
  std::vector<size_t> grown;
  size_t shrunk = 0;
  bool refuse = false;
  void Grow(size_t bytes, size_t glibc_bytes) override {
    EXPECT_EQ(0u, glibc_bytes);
    if (refuse) {
      throw utils::MemoryRefused(utils::MemoryRefused::Reason::kNormalLimit,
                                 bytes, "refused");
    }
    grown.push_back(bytes);
  }
  void Shrink(size_t bytes, size_t) noexcept override { shrunk += bytes; }
};

// What the constructor allocates is what the table reports holding.
TEST(SlotTableTest, BytesMatchesTheConstructor) {
  for (size_t capacity : {1u, 100u, 4096u}) {
    Table table(capacity);
    EXPECT_EQ(bess::framework::MemoryCost<Table>::Bytes(capacity),
              table.bytes());
    EXPECT_EQ(9 * (capacity + 1), Table::Bytes(capacity));
  }
}

TEST(SlotTableTest, PublishAndLookup) {
  Table table(4);
  EXPECT_EQ(nullptr, table.Lookup(ObjId(1)));
  EXPECT_TRUE(table.Publish(ObjId(1), table.Make(Obj{7})) == nullptr);
  ASSERT_NE(nullptr, table.Lookup(ObjId(1)));
  EXPECT_EQ(7u, table.Lookup(ObjId(1))->value);
  EXPECT_EQ(1u, table.size());
  EXPECT_EQ(nullptr, table.Lookup(ObjId(5)));  // out of range
}

// Objects are growth: the slab chunk Make adds is admitted first, through
// the hook the table forwards to its slab.
TEST(SlotTableTest, MakeAdmitsEachSlabChunkFirst) {
  Table table(8);
  const size_t base = table.bytes();
  RecordingGrowth growth;
  table.set_memory_growth(&growth);
  auto a = table.Make(Obj{1});
  auto b = table.Make(Obj{2});  // same chunk: no growth
  const size_t chunk = utils::ObjectSlab<Obj>::chunk_bytes();
  EXPECT_EQ((std::vector<size_t>{chunk}), growth.grown);
  EXPECT_EQ(base + chunk, table.bytes());
  EXPECT_EQ(0u, growth.shrunk);
}

TEST(SlotTableTest, ARefusedChunkLeavesTheTableUnchanged) {
  Table table(8);
  const size_t base = table.bytes();
  RecordingGrowth growth;
  growth.refuse = true;
  table.set_memory_growth(&growth);
  EXPECT_THROW(table.Make(Obj{1}), utils::MemoryRefused);
  EXPECT_EQ(base, table.bytes());
  EXPECT_EQ(0u, table.size());
  growth.refuse = false;
  EXPECT_TRUE(table.Publish(ObjId(2), table.Make(Obj{2})) == nullptr);
  EXPECT_EQ(1u, growth.grown.size());
  EXPECT_EQ(0u, growth.shrunk);
}

}  // namespace
}  // namespace bess::dataplane
