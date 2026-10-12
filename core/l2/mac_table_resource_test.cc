// SPDX-License-Identifier: BSD-3-Clause

// MacTableResource (D-114): a PackedMacTable changed by transactions, all or
// nothing, with new MACs placed at the miss value and taken back on a refusal.

#include "l2/mac_table_resource.h"

#include <gtest/gtest.h>

#include <any>
#include <cstdint>
#include <memory>
#include <vector>

#include "dataplane/table_policy.h"
#include "dataplane/transaction_engine.h"
#include "l2/packed_mac_table.h"
#include "runtime/runtime_state.h"

namespace bess::l2 {
namespace {

struct NoCold {};
using Table = PackedMacTable<NoCold, dataplane::SingleWriter>;
using Resource = MacTableResource<Table>;
using dataplane::Op;
using Outcome = dataplane::TransactionEngine::Outcome;

constexpr uint16_t kMissValue = 77;  // what readers treat as a miss

uint64_t Mac(uint64_t n) { return n << 16; }

Op Set(uint64_t n, uint16_t value) {
  return Op::Upsert("t/macs", Resource::Key(Mac(n)), std::any(value));
}
Op Unset(uint64_t n) { return Op::Erase("t/macs", Resource::Key(Mac(n))); }

class MacTableResourceTest : public ::testing::Test {
 protected:
  void SetUp() override { Make(64); }
  void TearDown() override { Drop(); }

  void Make(size_t capacity) {
    table_ = Table::Create(capacity, /*seed=*/12345);
    ASSERT_NE(nullptr, table_);
    resource_ = std::make_unique<Resource>("t/macs", *table_, [] { return kMissValue; });
    ASSERT_TRUE(runtime::runtime().transactions().Register(resource_.get()));
  }
  void Drop() {
    if (resource_) {
      ASSERT_TRUE(runtime::runtime().transactions().Unregister("t/macs"));
      resource_.reset();
      table_.reset();
    }
  }

  Outcome Apply(std::vector<Op> ops) {
    return runtime::runtime().transactions().Apply(ops).outcome;
  }
  uint32_t Lookup(uint64_t n) const { return table_->Lookup(Mac(n)); }

  std::unique_ptr<Table> table_;
  std::unique_ptr<Resource> resource_;
};

TEST_F(MacTableResourceTest, EntriesCommitTogether) {
  ASSERT_EQ(Outcome::kApplied, Apply({Set(1, 2), Set(2, 3), Set(3, 4)}));
  EXPECT_EQ(2u, Lookup(1));
  EXPECT_EQ(3u, Lookup(2));
  EXPECT_EQ(4u, Lookup(3));
  EXPECT_EQ(3u, resource_->LiveCount());

  // Re-pointed and erased in one transaction.
  ASSERT_EQ(Outcome::kApplied, Apply({Set(1, 9), Unset(2)}));
  EXPECT_EQ(9u, Lookup(1));
  EXPECT_EQ(0u, Lookup(2));
  EXPECT_EQ(2u, resource_->LiveCount());
}

// A refused transaction leaves no MAC it placed (a placeholder that stayed
// would send its traffic to the miss value's gate for good).
TEST_F(MacTableResourceTest, ARefusalTakesThePlacedEntriesBack) {
  ASSERT_EQ(Outcome::kApplied, Apply({Set(1, 2)}));
  EXPECT_NE(Outcome::kApplied, Apply({Set(5, 3), Set(6, 4), Unset(7)}));  // 7: not found
  EXPECT_EQ(1u, table_->size()) << "a placeholder stayed";
  EXPECT_EQ(0u, Lookup(5));
  EXPECT_EQ(0u, Lookup(6));
  EXPECT_EQ(2u, Lookup(1));
}

// Filling a table in one transaction makes inserts move earlier placed keys;
// Publish finds every key again, wherever it went.
TEST_F(MacTableResourceTest, PublishFindsKeysThatLaterInsertsMoved) {
  Drop();
  Make(256);
  std::vector<Op> ops;
  for (uint64_t n = 1; n <= 240; n++) {
    ops.push_back(Set(n, static_cast<uint16_t>(n % 100 + 1)));
  }
  ASSERT_EQ(Outcome::kApplied, Apply(ops));
  for (uint64_t n = 1; n <= 240; n++) {
    ASSERT_EQ(n % 100 + 1, Lookup(n)) << n;
  }
}

// No room: refused with nothing visible.
TEST_F(MacTableResourceTest, NoRoomRefusesTheWholeTransaction) {
  Drop();
  Make(2);
  EXPECT_NE(Outcome::kApplied, Apply({Set(1, 2), Set(2, 3), Set(3, 4)}));
  EXPECT_EQ(0u, table_->size());
  EXPECT_EQ(0u, Lookup(1));
}

TEST_F(MacTableResourceTest, BadKeysAndValuesAreRefused) {
  EXPECT_NE(Outcome::kApplied, Apply({Unset(1)})) << "not found";
  EXPECT_NE(Outcome::kApplied, Apply({Set(1, 0)})) << "0 reads as a miss";
  EXPECT_NE(Outcome::kApplied, Apply({Set(1, Table::kMaxValue + 1)}));
  EXPECT_NE(Outcome::kApplied,
            Apply({Op::Upsert("t/macs", Resource::Key(Mac(1)), std::any(uint32_t{2}))}))
      << "wrong value type";
  EXPECT_NE(Outcome::kApplied,
            Apply({Op::Upsert("t/macs", dataplane::EncodeKey(uint64_t{0x1234}),
                              std::any(uint16_t{2}))}))
      << "a nonzero domain is no MAC key";
  EXPECT_EQ(0u, table_->size());
}

}  // namespace
}  // namespace bess::l2
