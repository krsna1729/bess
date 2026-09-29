// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <limits>

#include "classifier/runtime_schema.h"

namespace {

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
      .result_fields = {{0, 10, 3}, {3, 20, 4}},
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
      .result_fields = {{0, std::numeric_limits<size_t>::max(), 1}},
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
      .result_fields = {{0, 10, 4}, {4, 12, 4}},
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
      .result_fields = {{0, 3, 7}},
  };

  EXPECT_TRUE(schema.Validate().has_value());
}

}  // namespace
