// SPDX-License-Identifier: BSD-3-Clause

#include "framework/instance_registry.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace {

using bess::framework::InstanceError;
using bess::framework::InstanceLease;
using bess::framework::InstanceRegistry;

struct Upf {
  explicit Upf(int sessions = 0) : sessions(sessions) { alive++; }
  ~Upf() { alive--; }
  int sessions;
  static inline int alive = 0;
};

struct VSwitch {};

TEST(InstanceRegistryTest, CreateForwardsConstructorArguments) {
  InstanceRegistry registry;
  auto lease = registry.Create<Upf>("upf0", 7);
  ASSERT_TRUE(lease.has_value());
  EXPECT_EQ(7, lease->get()->sessions);
  EXPECT_EQ(1u, registry.size());
}

TEST(InstanceRegistryTest, DuplicateCreateIsAnErrorAndConstructsNothing) {
  InstanceRegistry registry;
  auto first = registry.Create<Upf>("upf0", 1);
  ASSERT_TRUE(first.has_value());
  const int alive = Upf::alive;
  auto dup = registry.Create<Upf>("upf0", 2);
  ASSERT_FALSE(dup.has_value());
  EXPECT_EQ(InstanceError::kExists, dup.error());
  EXPECT_EQ(alive, Upf::alive);
  EXPECT_EQ(1, first->get()->sessions);  // the original is untouched
  // A duplicate name is an error even for a different type.
  auto other_type = registry.Create<VSwitch>("upf0");
  ASSERT_FALSE(other_type.has_value());
  EXPECT_EQ(InstanceError::kExists, other_type.error());
}

TEST(InstanceRegistryTest, LookupNeverCreates) {
  InstanceRegistry registry;
  auto missing = registry.Lookup<Upf>("upf0");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(InstanceError::kNotFound, missing.error());
  EXPECT_EQ(0u, registry.size());
}

TEST(InstanceRegistryTest, LookupWithTheWrongTypeIsAnError) {
  InstanceRegistry registry;
  auto created = registry.Create<Upf>("x", 3);
  ASSERT_TRUE(created.has_value());
  auto wrong = registry.Lookup<VSwitch>("x");
  ASSERT_FALSE(wrong.has_value());
  EXPECT_EQ(InstanceError::kTypeMismatch, wrong.error());
  EXPECT_EQ(1u, registry.Describe()[0].leases);  // a failed lookup leaks no lease
}

TEST(InstanceRegistryTest, LeasesShareTheOneInstance) {
  InstanceRegistry registry;
  auto a = registry.Create<Upf>("upf0", 1);
  auto b = registry.Lookup<Upf>("upf0");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(a->get(), b->get());
  EXPECT_EQ(2u, registry.Describe()[0].leases);
}

TEST(InstanceRegistryTest, DestroyIsRefusedWhileAnyLeaseIsOutstanding) {
  InstanceRegistry registry;
  auto creator = registry.Create<Upf>("upf0");
  auto user = registry.Lookup<Upf>("upf0");
  ASSERT_TRUE(creator.has_value() && user.has_value());

  creator->Release();
  auto refused = registry.Destroy("upf0");
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(InstanceError::kInUse, refused.error());
  EXPECT_EQ(1, Upf::alive);  // still alive, still usable
  EXPECT_EQ(0, user->get()->sessions);

  user->Release();
  EXPECT_TRUE(registry.Destroy("upf0").has_value());
  EXPECT_EQ(0, Upf::alive);
  EXPECT_EQ(0u, registry.size());
}

TEST(InstanceRegistryTest, DestroyOfAnUnknownNameIsAnError) {
  InstanceRegistry registry;
  auto r = registry.Destroy("nope");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(InstanceError::kNotFound, r.error());
}

TEST(InstanceRegistryTest, NameIsReusableAfterDestroyWithAnotherType) {
  InstanceRegistry registry;
  ASSERT_TRUE(registry.Create<Upf>("x").has_value());  // lease dropped at once
  ASSERT_TRUE(registry.Destroy("x").has_value());
  EXPECT_TRUE(registry.Create<VSwitch>("x").has_value());
}

TEST(InstanceRegistryTest, MovedLeaseKeepsTheCountAndMovedFromIsInert) {
  InstanceRegistry registry;
  auto created = registry.Create<Upf>("upf0");
  ASSERT_TRUE(created.has_value());
  InstanceLease<Upf> moved = std::move(*created);
  EXPECT_FALSE(static_cast<bool>(*created));
  created->Release();  // must not drop the moved lease's count
  EXPECT_EQ(1u, registry.Describe()[0].leases);
  moved.Release();
  moved.Release();  // idempotent
  EXPECT_EQ(0u, registry.Describe()[0].leases);
}

TEST(InstanceRegistryTest, DescribeListsNamesTypesAndLeasesInNameOrder) {
  InstanceRegistry registry;
  auto b = registry.Create<VSwitch>("b");
  auto a = registry.Create<Upf>("a");
  ASSERT_TRUE(a.has_value() && b.has_value());
  b->Release();
  const auto info = registry.Describe();
  ASSERT_EQ(2u, info.size());
  EXPECT_EQ("a", info[0].name);
  EXPECT_NE(std::string::npos, info[0].type.find("Upf"));
  EXPECT_EQ(1u, info[0].leases);
  EXPECT_EQ("b", info[1].name);
  EXPECT_NE(std::string::npos, info[1].type.find("VSwitch"));
  EXPECT_EQ(0u, info[1].leases);
}

}  // namespace
