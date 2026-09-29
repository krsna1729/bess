// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>

#include "classifier/result_plan.h"

namespace {

using bess::classifier::ConstBytes;
using bess::classifier::MutableBytes;
using bess::classifier::ResultKernel;
using bess::classifier::ResultPlan;
using bess::classifier::RuntimeClassifierSchema;
using bess::classifier::RuntimeResultField;

std::byte Byte(unsigned value) {
  return static_cast<std::byte>(value);
}

TEST(ResultPlanTest, CoalescesCompatiblePlacementFields) {
  RuntimeClassifierSchema schema{
      .key_size = 1,
      .value_size = 7,
      .result_fields = {{3, 4, 2}, {5, 6, 2}, {0, 12, 1}},
  };
  auto compiled = ResultPlan::Compile(schema);
  ASSERT_TRUE(compiled);
  EXPECT_EQ(ResultKernel::kGeneric, compiled->kernel());
  ASSERT_EQ(2u, compiled->ops().size());
  EXPECT_EQ(4u, compiled->ops()[0].destination_offset);
  EXPECT_EQ(4u, compiled->ops()[0].size);
}

TEST(ResultPlanTest, AppliesExactValueRangesToMetadata) {
  RuntimeClassifierSchema schema{
      .key_size = 1,
      .value_size = 7,
      .result_fields = {{0, 1, 3}, {3, 8, 4}},
  };
  auto compiled = ResultPlan::Compile(schema);
  ASSERT_TRUE(compiled);

  const std::array<std::byte, 7> value = {
      Byte(10), Byte(11), Byte(12), Byte(13), Byte(14), Byte(15), Byte(16)};
  std::array<std::byte, 13> metadata{};
  ASSERT_TRUE(compiled->Apply(ConstBytes(value), MutableBytes(metadata)));
  EXPECT_EQ(Byte(10), metadata[1]);
  EXPECT_EQ(Byte(11), metadata[2]);
  EXPECT_EQ(Byte(12), metadata[3]);
  EXPECT_EQ(Byte(13), metadata[8]);
  EXPECT_EQ(Byte(14), metadata[9]);
  EXPECT_EQ(Byte(15), metadata[10]);
  EXPECT_EQ(Byte(16), metadata[11]);
}

TEST(ResultPlanTest, BatchPlacementUsesOnePlanCallAndChecksSpans) {
  RuntimeClassifierSchema schema{
      .key_size = 1,
      .value_size = 3,
      .result_fields = {{0, 2, 3}},
  };
  auto compiled = ResultPlan::Compile(schema);
  ASSERT_TRUE(compiled);

  const std::array<std::byte, 3> first = {Byte(1), Byte(2), Byte(3)};
  const std::array<std::byte, 3> second = {Byte(4), Byte(5), Byte(6)};
  const std::array<ConstBytes, 2> values = {ConstBytes(first), ConstBytes(second)};
  std::array<std::byte, 10> storage{};
  std::array<MutableBytes, 2> destinations = {
      MutableBytes(storage).subspan(0, 5),
      MutableBytes(storage).subspan(5, 5)};

  ASSERT_TRUE(compiled->ApplyBatch(values, destinations));
  EXPECT_EQ(Byte(1), storage[2]);
  EXPECT_EQ(Byte(2), storage[3]);
  EXPECT_EQ(Byte(3), storage[4]);
  EXPECT_EQ(Byte(4), storage[7]);
  EXPECT_EQ(Byte(5), storage[8]);
  EXPECT_EQ(Byte(6), storage[9]);
  EXPECT_FALSE(compiled->Apply(ConstBytes(first), MutableBytes(storage).first(4)));
}

}  // namespace
