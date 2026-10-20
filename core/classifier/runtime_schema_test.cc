// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <limits>

#include "classifier/runtime_schema.h"

namespace {

using bess::classifier::BufferKind;
using bess::classifier::ClassifierErrorCode;
using bess::classifier::RuntimeClassifierSchema;
using bess::classifier::SourceKind;

TEST(RuntimeSchemaTest, ValidResolvedSchemaDoesNotNeedNamesOrProtobuf) {
  RuntimeClassifierSchema schema{
      .key_size = 9,
      .value_size = 7,
      .key_fields = {{SourceKind::kPacket, 26, 4, 3},
                     {SourceKind::kMetadata, 8, 0, 4},
                     {SourceKind::kPacket, 42, 7, 2}},
      .result_fields = {{.destination = BufferKind::kMetadata, .value_offset = 0, .destination_offset = 10, .size = 3}, {.destination = BufferKind::kMetadata, .value_offset = 3, .destination_offset = 20, .size = 4}},
  };

  EXPECT_TRUE(schema.Validate().has_value());
}

TEST(RuntimeSchemaTest, RejectsZeroAndOutOfRangeFields) {
  RuntimeClassifierSchema zero_field{
      .key_size = 4,
      .key_fields = {{SourceKind::kPacket, 0, 0, 0}},
  };
  ASSERT_FALSE(zero_field.Validate());
  EXPECT_EQ(ClassifierErrorCode::kZeroFieldSize,
            zero_field.Validate().error().code);

  RuntimeClassifierSchema out_of_range{
      .key_size = 4,
      .key_fields = {{SourceKind::kPacket, 0, 3, 2}},
  };
  ASSERT_FALSE(out_of_range.Validate());
  EXPECT_EQ(ClassifierErrorCode::kKeyOutOfBounds,
            out_of_range.Validate().error().code);

  RuntimeClassifierSchema offset_overflow{
      .key_size = 4,
      .key_fields = {{SourceKind::kPacket,
                      0,
                      std::numeric_limits<size_t>::max(),
                      1}},
  };
  ASSERT_FALSE(offset_overflow.Validate());
  EXPECT_EQ(ClassifierErrorCode::kKeyOutOfBounds,
            offset_overflow.Validate().error().code);

  RuntimeClassifierSchema result_overflow{
      .key_size = 1,
      .value_size = 1,
      .result_fields = {{.destination = BufferKind::kMetadata, .value_offset = 0, .destination_offset = std::numeric_limits<size_t>::max(), .size = 1}},
  };
  ASSERT_FALSE(result_overflow.Validate());
  EXPECT_EQ(ClassifierErrorCode::kResultOutOfBounds,
            result_overflow.Validate().error().code);
}

TEST(RuntimeSchemaTest, RejectsOverlappingKeyAndResultDestinations) {
  RuntimeClassifierSchema schema{
      .key_size = 8,
      .value_size = 8,
      .key_fields = {{SourceKind::kPacket, 0, 0, 4},
                     {SourceKind::kPacket, 8, 2, 4}},
      .result_fields = {{.destination = BufferKind::kMetadata, .value_offset = 0, .destination_offset = 10, .size = 4}, {.destination = BufferKind::kMetadata, .value_offset = 4, .destination_offset = 12, .size = 4}},
  };

  ASSERT_FALSE(schema.Validate());
  EXPECT_EQ(ClassifierErrorCode::kOverlappingFields,
            schema.Validate().error().code);
}

TEST(RuntimeSchemaTest, AcceptsOddRuntimeWidths) {
  RuntimeClassifierSchema schema{
      .key_size = 37,
      .value_size = 7,
      .key_fields = {{SourceKind::kPacket, 1, 0, 3},
                     {SourceKind::kMetadata, 7, 3, 14},
                     {SourceKind::kPacket, 31, 17, 7},
                     {SourceKind::kPacket, 64, 24, 13}},
      .result_fields = {{.destination = BufferKind::kMetadata, .value_offset = 0, .destination_offset = 3, .size = 7}},
  };

  EXPECT_TRUE(schema.Validate().has_value());
}

// The same offset in the packet and in metadata are different destinations, so
// they do not collide; two fields in one buffer still do.
TEST(RuntimeSchemaTest, ResultDestinationsArePerBuffer) {
  RuntimeClassifierSchema distinct{
      .key_size = 1,
      .value_size = 8,
      .result_fields = {{.destination = BufferKind::kPacket,
                         .value_offset = 0,
                         .destination_offset = 4,
                         .size = 4},
                        {.destination = BufferKind::kMetadata,
                         .value_offset = 4,
                         .destination_offset = 4,
                         .size = 4}},
  };
  EXPECT_TRUE(distinct.Validate().has_value());

  RuntimeClassifierSchema overlapping{
      .key_size = 1,
      .value_size = 8,
      .result_fields = {{.destination = BufferKind::kPacket,
                         .value_offset = 0,
                         .destination_offset = 4,
                         .size = 4},
                        {.destination = BufferKind::kPacket,
                         .value_offset = 4,
                         .destination_offset = 6,
                         .size = 4}},
  };
  ASSERT_FALSE(overlapping.Validate());
  EXPECT_EQ(ClassifierErrorCode::kOverlappingFields,
            overlapping.Validate().error().code);
}

TEST(RuntimeSchemaTest, RejectsAResultMaskOfTheWrongSize) {
  RuntimeClassifierSchema schema{
      .key_size = 1,
      .value_size = 4,
      .result_fields = {{.destination = BufferKind::kMetadata,
                         .value_offset = 0,
                         .destination_offset = 0,
                         .size = 4,
                         .normalization = {.mask = {std::byte{1}}}}},
  };
  ASSERT_FALSE(schema.Validate());
  EXPECT_EQ(ClassifierErrorCode::kInvalidPlan, schema.Validate().error().code);
}

}  // namespace
