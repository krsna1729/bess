// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/table_schema.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "dataplane/resource.h"

namespace {

using bess::classifier::BufferKind;
using bess::classifier::ClassifierErrorCode;
using bess::classifier::CompileKey;
using bess::classifier::ConstBytes;
using bess::classifier::DestinationView;
using bess::classifier::KeyLayout;
using bess::classifier::kSkippedOffset;
using bess::classifier::kUnresolvedOffset;
using bess::classifier::MutableBytes;
using bess::classifier::ResultFieldSpec;
using bess::classifier::ResultSchema;
using bess::classifier::SourceKind;
using bess::dataplane::EncodeKey;
using bess::dataplane::Reference;

ResultFieldSpec Field(uint32_t value_offset, uint32_t size, std::string reference = "",
                      bool be32 = false, uint32_t count = 1) {
  ResultFieldSpec spec;
  spec.value_offset = value_offset;
  spec.size = size;
  spec.name = "f" + std::to_string(value_offset);
  spec.reference = std::move(reference);
  spec.reference_be32 = be32;
  spec.reference_count = count;
  return spec;
}

std::vector<std::byte> Bytes(std::initializer_list<unsigned> values) {
  std::vector<std::byte> out;
  for (unsigned v : values) out.push_back(static_cast<std::byte>(v));
  return out;
}

// D-172: BE32 ids are decoded per position (0 names nothing there), a raw
// field is its bytes, an all-zero field names nothing, and the bytes are read
// as the record zero-padded to its width (shorter input pads, longer input is
// cut), as a later replacement reads the arena.
TEST(TableSchemaTest, ReferencesDecodeTheZeroPaddedRecord) {
  ResultSchema schema;
  schema.Add(Field(0, 8, "ids", /*be32=*/true, /*count=*/2));
  schema.Add(Field(8, 2, "raw"));
  schema.Add(Field(10, 2, "unset"));
  ASSERT_EQ(schema.record_size(), 12U);
  EXPECT_EQ(schema.reference_names(), (std::vector<std::string>{"ids", "raw", "unset"}));

  const auto bytes = Bytes({0, 0, 0, 0, 0, 0, 1, 2, 0xab, 0xcd, 0, 0, 0xff});
  EXPECT_EQ(schema.References(ConstBytes(bytes)),
            (std::vector<Reference>{{"ids", EncodeKey(uint32_t{0x0102})},
                                    {"raw", std::string("\xab\xcd", 2)}}));

  // Ten bytes: "unset" lies past them and reads as zero.
  const auto short_bytes = Bytes({0, 0, 0, 7, 0, 0, 0, 0, 0, 1});
  EXPECT_EQ(schema.References(ConstBytes(short_bytes)),
            (std::vector<Reference>{{"ids", EncodeKey(uint32_t{7})},
                                    {"raw", std::string("\x00\x01", 2)}}));
  EXPECT_TRUE(schema.References({}).empty());
}

TEST(TableSchemaTest, CheckRefusesMalformedFields) {
  const auto message = [](const ResultFieldSpec &field) {
    auto checked = ResultSchema::Check(field);
    return checked ? std::string() : checked.error().message;
  };
  EXPECT_EQ(message(Field(0, 4)), "");
  EXPECT_EQ(message(Field(0, 0)), "a result field has zero bytes");
  EXPECT_EQ(message(Field(0xfffffffeU, 4)), "a result field overflows its record");
  EXPECT_EQ(message(Field(0, 4, "", true)), "a BE32 reference names no resource");
  EXPECT_EQ(message(Field(0, 6, "r", true, 2)),
            "a BE32 reference carries reference_count 4-byte ids: num_bytes "
            "must be 4 * reference_count");
  EXPECT_EQ(message(Field(0, 4, "r", false, 2)),
            "reference_count applies to BE32 references only");
}

TEST(TableSchemaTest, CompileLeavesOutSkippedAndRefusesUnresolved) {
  ResultSchema schema;
  schema.Add(Field(0, 2));
  ResultFieldSpec packet = Field(2, 2);
  packet.destination = BufferKind::kPacket;
  schema.Add(packet);
  EXPECT_TRUE(schema.needs_metadata());

  const std::array<size_t, 2> skipped = {kSkippedOffset, kSkippedOffset};
  auto none = schema.Compile(skipped, /*key_size=*/4);
  ASSERT_TRUE(none.has_value());
  EXPECT_FALSE(none->has_value());

  const std::array<size_t, 2> unresolved = {kUnresolvedOffset, 6};
  auto refused = schema.Compile(unresolved, 4);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().message, "result field 'f0' has no readable metadata offset");

  const std::array<size_t, 2> packet_only = {kSkippedOffset, 6};
  auto plan = schema.Compile(packet_only, 4);
  ASSERT_TRUE(plan.has_value() && plan->has_value());
  EXPECT_EQ((*plan)->required_packet_bytes(), 8U);
  EXPECT_EQ((*plan)->required_metadata_bytes(), 0U);
  std::array<std::byte, 8> pkt{};
  std::array<std::byte, 8> meta{};
  const auto record = Bytes({1, 2, 3, 4});
  ASSERT_TRUE((*plan)->Apply(ConstBytes(record),
                             DestinationView{.packet = MutableBytes(pkt),
                                             .metadata = MutableBytes(meta)}));
  EXPECT_EQ(pkt[6], std::byte{3});
  EXPECT_EQ(pkt[7], std::byte{4});
  EXPECT_EQ(meta, (std::array<std::byte, 8>{}));
}

TEST(TableSchemaTest, KeyLayoutMarksUnresolvedSources) {
  KeyLayout layout;
  layout.Append(SourceKind::kPacket, 26, 4);
  layout.Append(SourceKind::kMetadata, kUnresolvedOffset, 2);
  EXPECT_FALSE(layout.sources_resolved);
  EXPECT_EQ(layout.key_size, 6U);
  EXPECT_EQ(layout.baked_offsets, (std::vector<size_t>{26, kUnresolvedOffset}));
  EXPECT_EQ(layout.key_fields[1].key_offset, 4U);
  EXPECT_EQ(layout.key_fields[1].source_offset, 0U);  // placeholder

  auto key = CompileKey(std::move(layout));
  ASSERT_TRUE(key.has_value());
  EXPECT_TRUE(key->valid);
  EXPECT_EQ(key->key_size, 6U);
  EXPECT_TRUE(key->extract.fully_covers_key());
  EXPECT_EQ(key->baked_offsets, (std::vector<size_t>{26, kUnresolvedOffset}));
}

TEST(TableSchemaTest, CompileKeyRefusesALayoutWithGaps) {
  KeyLayout layout;
  layout.Append(SourceKind::kPacket, 26, 4);
  layout.key_fields[0].key_offset = 2;  // bytes 0-1 written by no field
  layout.key_size = 6;
  auto key = CompileKey(std::move(layout));
  ASSERT_FALSE(key.has_value());
  EXPECT_EQ(key.error().code, ClassifierErrorCode::kInvalidPlan);
}

}  // namespace
