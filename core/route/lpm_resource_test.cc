// SPDX-License-Identifier: BSD-3-Clause

// LpmTableResource (D-111): an LpmRouteTable changed by transactions, all or
// nothing, with new prefixes placed invisibly and taken back on a refusal.

#include "route/lpm_resource.h"

#include <gtest/gtest.h>

#include <any>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "runtime/runtime_state.h"

namespace bess::route {
namespace {

using dataplane::Op;
using Outcome = dataplane::TransactionEngine::Outcome;

constexpr uint32_t kMissValue = 999;  // what readers treat as a miss

constexpr uint32_t Ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return a << 24 | b << 16 | c << 8 | d;
}

Ipv4Prefix P(uint32_t addr, uint8_t len) { return Ipv4Prefix::Make(addr, len).value(); }

Op Set(Ipv4Prefix prefix, uint32_t value) {
  return Op::Upsert("t/routes", LpmTableResource::Key(prefix), std::any(value));
}
Op Unset(Ipv4Prefix prefix) { return Op::Erase("t/routes", LpmTableResource::Key(prefix)); }

class LpmResourceTest : public ::testing::Test {
 protected:
  void SetUp() override { Make(64); }
  void TearDown() override { Drop(); }

  void Make(uint32_t max_routes) {
    LpmRouteTable::Config config;
    config.max_routes = max_routes;
    config.tbl8_groups = 16;
    auto table = LpmRouteTable::Create("t", config, runtime::runtime().rcu());
    ASSERT_TRUE(table.has_value());
    table_ = std::move(*table);
    resource_ = std::make_unique<LpmTableResource>("t/routes", *table_, kMissValue);
    ASSERT_TRUE(runtime::runtime().transactions().Register(resource_.get()));
  }
  void Drop() {
    if (resource_) {
      ASSERT_TRUE(runtime::runtime().transactions().Unregister("t/routes"));
      resource_.reset();
      table_.reset();
    }
  }

  Outcome Apply(std::vector<Op> ops) {
    return runtime::runtime().transactions().Apply(ops).outcome;
  }
  std::optional<uint32_t> Lookup(uint32_t addr) const { return table_->Read().Lookup(addr); }

  std::unique_ptr<LpmRouteTable> table_;
  std::unique_ptr<LpmTableResource> resource_;
};

TEST_F(LpmResourceTest, RoutesAndTheDefaultCommitTogether) {
  ASSERT_EQ(Outcome::kApplied,
            Apply({Set(P(Ip(10, 0, 0, 0), 8), 1), Set(P(Ip(10, 1, 0, 0), 16), 2),
                   Set(P(0, 0), 7)}));
  EXPECT_EQ(2u, Lookup(Ip(10, 1, 2, 3)));
  EXPECT_EQ(1u, Lookup(Ip(10, 2, 0, 0)));
  EXPECT_EQ(7u, Lookup(Ip(192, 168, 0, 1))) << "the default route";
  EXPECT_EQ(3u, resource_->LiveCount());

  // Re-pointed and erased in one transaction.
  ASSERT_EQ(Outcome::kApplied,
            Apply({Set(P(Ip(10, 0, 0, 0), 8), 4), Unset(P(Ip(10, 1, 0, 0), 16)), Unset(P(0, 0))}));
  EXPECT_EQ(4u, Lookup(Ip(10, 1, 2, 3)));
  EXPECT_EQ(std::nullopt, Lookup(Ip(192, 168, 0, 1)));
  EXPECT_EQ(1u, resource_->LiveCount());
}

// A refused transaction leaves no prefix it placed: lookups and the route
// count are what they were (a placeholder that stayed would be a route).
TEST_F(LpmResourceTest, ARefusalTakesThePlacedPrefixesBack) {
  ASSERT_EQ(Outcome::kApplied, Apply({Set(P(Ip(10, 0, 0, 0), 8), 1)}));
  const Outcome refused =
      Apply({Set(P(Ip(10, 1, 0, 0), 16), 2), Set(P(Ip(172, 16, 0, 0), 12), 3),
             Unset(P(Ip(192, 168, 0, 0), 16))});  // not found: refused
  EXPECT_NE(Outcome::kApplied, refused);
  EXPECT_EQ(1u, resource_->LiveCount());
  EXPECT_EQ(1u, table_->size()) << "a placeholder stayed";
  EXPECT_EQ(1u, Lookup(Ip(10, 1, 2, 3)));
  EXPECT_EQ(std::nullopt, Lookup(Ip(172, 16, 0, 1)));
}

// No room: refused with nothing visible.
TEST_F(LpmResourceTest, NoRoomRefusesTheWholeTransaction) {
  Drop();
  Make(2);
  const Outcome refused = Apply({Set(P(Ip(10, 0, 0, 0), 8), 1), Set(P(Ip(11, 0, 0, 0), 8), 2),
                                 Set(P(Ip(12, 0, 0, 0), 8), 3)});
  EXPECT_NE(Outcome::kApplied, refused);
  EXPECT_EQ(0u, table_->size());
  EXPECT_EQ(std::nullopt, Lookup(Ip(10, 0, 0, 1)));
}

TEST_F(LpmResourceTest, BadKeysAndValuesAreRefused) {
  EXPECT_NE(Outcome::kApplied, Apply({Unset(P(Ip(10, 0, 0, 0), 8))})) << "not found";
  EXPECT_NE(Outcome::kApplied,
            Apply({Set(P(Ip(10, 0, 0, 0), 8), LpmRouteTable::kMaxValue + 1)}));
  EXPECT_NE(Outcome::kApplied,
            Apply({Op::Upsert("t/routes", LpmTableResource::Key(P(Ip(10, 0, 0, 0), 8)),
                              std::any(uint64_t{1}))}))
      << "wrong value type";
  EXPECT_EQ(0u, table_->size());
}

}  // namespace
}  // namespace bess::route
