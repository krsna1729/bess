// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>

#include "classifier/result_plan.h"

namespace {

using bess::classifier::BufferKind;
using bess::classifier::ClassifierErrorCode;
using bess::classifier::ConstBytes;
using bess::classifier::DestinationView;
using bess::classifier::MutableBytes;
using bess::classifier::ResultKernel;
using bess::classifier::ResultPlan;
using bess::classifier::RuntimeClassifierSchema;
using bess::classifier::RuntimeResultField;

std::byte Byte(unsigned value) {
  return static_cast<std::byte>(value);
}

RuntimeResultField Meta(size_t value_offset, size_t destination_offset,
                        size_t size) {
  return RuntimeResultField{.destination = BufferKind::kMetadata,
                            .value_offset = value_offset,
                            .destination_offset = destination_offset,
                            .size = size};
}

DestinationView MetadataOnly(std::span<std::byte> metadata) {
  return DestinationView{.packet = {}, .metadata = MutableBytes(metadata)};
}

TEST(ResultPlanTest, CoalescesCompatiblePlacementFields) {
  RuntimeClassifierSchema schema{
      .key_size = 1,
      .value_size = 7,
      .result_fields = {Meta(3, 4, 2), Meta(5, 6, 2), Meta(0, 12, 1)},
  };
  auto compiled = ResultPlan::Compile(schema);
  ASSERT_TRUE(compiled);
  EXPECT_EQ(ResultKernel::kTwoOp, compiled->kernel());
  ASSERT_EQ(2u, compiled->ops().size());
  // One merged 4-byte op at destination 4, one 1-byte op at 12. Bucket order,
  // not destination order, decides which is first.
  for (const auto &op : compiled->ops()) {
    if (op.destination_offset == 4) {
      EXPECT_EQ(4u, op.size);
      EXPECT_EQ(3u, op.value_offset);
    } else {
      EXPECT_EQ(12u, op.destination_offset);
      EXPECT_EQ(1u, op.size);
      EXPECT_EQ(0u, op.value_offset);
    }
  }
}

TEST(ResultPlanTest, ACoalescedRunStopsAtThePackedWidth) {
  // Each field fits PackedOp's 16-bit width; their adjacent run does not, so
  // the plan keeps two ops instead of building one no record can hold.
  RuntimeClassifierSchema schema{
      .key_size = 1,
      .value_size = size_t{131070},
      .result_fields = {Meta(0, 0, 65535), Meta(65535, 65535, 65535)},
  };
  ASSERT_TRUE(schema.Validate().has_value());
  auto compiled = ResultPlan::Compile(schema);
  ASSERT_TRUE(compiled);
  EXPECT_EQ(2u, compiled->ops().size());
}

TEST(ResultPlanTest, MasksPreventCoalescingAndNormalizeTheValue) {
  RuntimeClassifierSchema schema{
      .key_size = 1,
      .value_size = 4,
      .result_fields =
          {RuntimeResultField{.destination = BufferKind::kMetadata,
                              .value_offset = 0,
                              .destination_offset = 0,
                              .size = 2,
                              .normalization = {.mask = {Byte(0x0f), Byte(0xf0)}}},
           RuntimeResultField{.destination = BufferKind::kMetadata,
                              .value_offset = 2,
                              .destination_offset = 2,
                              .size = 2,
                              .normalization = {.mask = {Byte(0xff), Byte(0x00)}}}},
  };
  auto compiled = ResultPlan::Compile(schema);
  ASSERT_TRUE(compiled);
  ASSERT_EQ(2u, compiled->ops().size()) << "masked fields do not merge";

  const std::array<std::byte, 4> value = {Byte(0xab), Byte(0xcd), Byte(0x12),
                                          Byte(0x34)};
  std::array<std::byte, 4> metadata{};
  ASSERT_TRUE(compiled->Apply(ConstBytes(value), MetadataOnly(metadata)));
  EXPECT_EQ(Byte(0x0b), metadata[0]);
  EXPECT_EQ(Byte(0xc0), metadata[1]);
  EXPECT_EQ(Byte(0x12), metadata[2]);
  EXPECT_EQ(Byte(0x00), metadata[3]);
}

TEST(ResultPlanTest, AppliesExactValueRangesToMetadata) {
  RuntimeClassifierSchema schema{
      .key_size = 1,
      .value_size = 7,
      .result_fields = {Meta(0, 1, 3), Meta(3, 8, 4)},
  };
  auto compiled = ResultPlan::Compile(schema);
  ASSERT_TRUE(compiled);

  const std::array<std::byte, 7> value = {
      Byte(10), Byte(11), Byte(12), Byte(13), Byte(14), Byte(15), Byte(16)};
  std::array<std::byte, 13> metadata{};
  ASSERT_TRUE(compiled->Apply(ConstBytes(value), MetadataOnly(metadata)));
  EXPECT_EQ(Byte(10), metadata[1]);
  EXPECT_EQ(Byte(11), metadata[2]);
  EXPECT_EQ(Byte(12), metadata[3]);
  EXPECT_EQ(Byte(13), metadata[8]);
  EXPECT_EQ(Byte(14), metadata[9]);
  EXPECT_EQ(Byte(15), metadata[10]);
  EXPECT_EQ(Byte(16), metadata[11]);
}

TEST(ResultPlanTest, WritesPacketBytesAndMetadataInOneCall) {
  RuntimeClassifierSchema schema{
      .key_size = 1,
      .value_size = 8,
      .result_fields =
          {RuntimeResultField{.destination = BufferKind::kPacket,
                              .value_offset = 0,
                              .destination_offset = 12,
                              .size = 4},
           RuntimeResultField{.destination = BufferKind::kMetadata,
                              .value_offset = 4,
                              .destination_offset = 2,
                              .size = 4}},
  };
  auto compiled = ResultPlan::Compile(schema);
  ASSERT_TRUE(compiled);
  EXPECT_EQ(12u + 4, compiled->required_packet_bytes());
  EXPECT_EQ(2u + 4, compiled->required_metadata_bytes());

  const std::array<std::byte, 8> value = {Byte(1), Byte(2), Byte(3), Byte(4),
                                          Byte(5), Byte(6), Byte(7), Byte(8)};
  std::array<std::byte, 32> packet{};
  std::array<std::byte, 8> metadata{};
  DestinationView destination{.packet = MutableBytes(packet),
                              .metadata = MutableBytes(metadata)};
  ASSERT_TRUE(compiled->Apply(ConstBytes(value), destination));
  EXPECT_EQ(Byte(1), packet[12]);
  EXPECT_EQ(Byte(4), packet[15]);
  EXPECT_EQ(Byte(5), metadata[2]);
  EXPECT_EQ(Byte(8), metadata[5]);
  EXPECT_EQ(Byte(0), metadata[1]) << "the bytes before the field are untouched";
}

TEST(ResultPlanTest, RejectsOffsetsThePackedFormCannotHold) {
  RuntimeClassifierSchema schema{
      .key_size = 1,
      .value_size = 4,
      .result_fields = {Meta(0, static_cast<size_t>(uint64_t{1} << uint64_t{32}), 4)},
  };
  // The limit is the schema's, not only the plan's: Validate() and both
  // Compile()s agree, so a caller cannot build a schema no plan can hold.
  auto valid = schema.Validate();
  ASSERT_FALSE(valid);
  EXPECT_EQ(ClassifierErrorCode::kResultOutOfBounds, valid.error().code);
  auto compiled = ResultPlan::Compile(schema);
  ASSERT_FALSE(compiled);
  EXPECT_EQ(ClassifierErrorCode::kResultOutOfBounds, compiled.error().code);
}

TEST(ResultPlanTest, BatchPlacementUsesOnePlanCallAndChecksSpans) {
  RuntimeClassifierSchema schema{
      .key_size = 1,
      .value_size = 3,
      .result_fields = {Meta(0, 2, 3)},
  };
  auto compiled = ResultPlan::Compile(schema);
  ASSERT_TRUE(compiled);

  const std::array<std::byte, 3> first = {Byte(1), Byte(2), Byte(3)};
  const std::array<std::byte, 3> second = {Byte(4), Byte(5), Byte(6)};
  const std::array<ConstBytes, 2> values = {ConstBytes(first), ConstBytes(second)};
  std::array<std::byte, 10> storage{};
  const std::array<DestinationView, 2> destinations = {
      DestinationView{.packet = {}, .metadata = MutableBytes(storage).subspan(0, 5)},
      DestinationView{.packet = {}, .metadata = MutableBytes(storage).subspan(5, 5)}};

  ASSERT_TRUE(compiled->ApplyBatch(values, destinations));
  EXPECT_EQ(Byte(1), storage[2]);
  EXPECT_EQ(Byte(2), storage[3]);
  EXPECT_EQ(Byte(3), storage[4]);
  EXPECT_EQ(Byte(4), storage[7]);
  EXPECT_EQ(Byte(5), storage[8]);
  EXPECT_EQ(Byte(6), storage[9]);
  EXPECT_FALSE(
      compiled->Apply(ConstBytes(first), MetadataOnly(std::span<std::byte>(storage).first(4))));
}

}  // namespace
