// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <concepts>
#include <cstdint>
#include <unordered_set>

#include "dataplane/action_id.h"
#include "dataplane/generation_handle.h"
#include "dataplane/interface_id.h"

namespace {

using bess::dataplane::ActionId;
using bess::dataplane::GenerationHandle;
using bess::dataplane::GenerationHandleHash;
using bess::dataplane::InterfaceId;
using bess::dataplane::kInvalidInterfaceId;
using bess::dataplane::StrongIdHash;

// Different identity domains do not compare with each other or convert.
static_assert(!std::equality_comparable_with<InterfaceId, ActionId>);
static_assert(!std::convertible_to<InterfaceId, ActionId>);
static_assert(!std::constructible_from<InterfaceId, ActionId>);

// A handle needs a 32-bit id (one 64-bit word); InterfaceId is 16 bits (D-060).
using Handle = GenerationHandle<ActionId>;
static_assert(sizeof(Handle) == 2 * sizeof(uint32_t));
static_assert(std::is_trivially_copyable_v<Handle>);

TEST(InterfaceIdTest, ZeroIsTheInvalidInterface) {
  EXPECT_EQ(InterfaceId{}, kInvalidInterfaceId);
  EXPECT_EQ(0u, kInvalidInterfaceId.value());
  EXPECT_NE(InterfaceId(1), kInvalidInterfaceId);
}

TEST(InterfaceIdTest, OrdersAndHashesByValue) {
  EXPECT_LT(InterfaceId(1), InterfaceId(2));
  std::unordered_set<InterfaceId, StrongIdHash<InterfaceId>> set;
  set.insert(InterfaceId(7));
  set.insert(InterfaceId(7));
  set.insert(InterfaceId(8));
  EXPECT_EQ(2u, set.size());
}

TEST(GenerationHandleTest, DiffersWhenOnlyTheGenerationDiffers) {
  // The reason the type exists: the same slot id reused by a later object must
  // not compare equal to a handle issued for the earlier one.
  const Handle issued{ActionId(5), 1};
  const Handle reused{ActionId(5), 2};
  EXPECT_NE(issued, reused);
  EXPECT_EQ(issued, (Handle{ActionId(5), 1}));
  EXPECT_NE(issued, (Handle{ActionId(6), 1}));
}

TEST(GenerationHandleTest, HashSeparatesGenerationsOfOneId) {
  const GenerationHandleHash<ActionId> hash;
  EXPECT_NE(hash(Handle{ActionId(5), 1}), hash(Handle{ActionId(5), 2}));
  EXPECT_NE(hash(Handle{ActionId(5), 1}), hash(Handle{ActionId(6), 1}));
  std::unordered_set<Handle, GenerationHandleHash<ActionId>> set;
  set.insert(Handle{ActionId(5), 1});
  set.insert(Handle{ActionId(5), 2});
  EXPECT_EQ(2u, set.size());
}

}  // namespace
