// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>

#include "classifier/generation.h"
#include "classifier/result_plan.h"
#include "rcu/rcu_domain.h"
#include "rcu/rcu_ptr.h"

namespace {

using bess::classifier::BackendInfo;
using bess::classifier::ExtractPlan;
using bess::classifier::Generation;
using bess::classifier::ResultPlan;
using bess::classifier::RuntimeClassifierSchema;
using bess::classifier::RuntimeKeyField;
using bess::classifier::SourceKind;
using bess::rcu::RcuDomain;
using bess::rcu::RcuPtr;

struct FakeBackend {
  int value = 0;
};

using TestGeneration = Generation<FakeBackend>;

std::unique_ptr<const TestGeneration> BuildGeneration(int value) {
  RuntimeClassifierSchema schema{
      .key_size = 1,
      .key_fields = {{SourceKind::kPacket, 0, 0, 1}},
  };
  auto extract = ExtractPlan::Compile(schema);
  auto result = ResultPlan::Compile(schema);
  EXPECT_TRUE(extract);
  EXPECT_TRUE(result);
  if (!extract || !result) {
    return nullptr;
  }
  return std::unique_ptr<const TestGeneration>(new TestGeneration(
      std::move(*extract), FakeBackend{value}, std::move(*result),
      BackendInfo{}));
}

TEST(GenerationTest, RcuPublishesWholeImmutableGeneration) {
  RcuDomain domain(2);
  constexpr uint32_t reader = 0;
  ASSERT_TRUE(domain.Register(reader).has_value());
  domain.Online(reader);

  RcuPtr<TestGeneration> published(domain);
  published.Initialize(BuildGeneration(1));
  const TestGeneration *old_generation = published.Read();
  ASSERT_NE(nullptr, old_generation);
  ASSERT_EQ(1, old_generation->backend().value);

  const auto token = published.Publish(BuildGeneration(2));
  EXPECT_EQ(1, old_generation->backend().value);
  EXPECT_EQ(2, published.Read()->backend().value);
  EXPECT_FALSE(domain.IsComplete(token));

  domain.Quiescent(reader);
  EXPECT_TRUE(domain.IsComplete(token));
  EXPECT_EQ(1u, domain.ReclaimReady());

  domain.Offline(reader);
  domain.Unregister(reader);
}

}  // namespace
