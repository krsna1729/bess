// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor their contributors may be
// used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "classifier/extract_plan.h"

namespace {

using bess::classifier::BoundsPolicy;
using bess::classifier::ConstBytes;
using bess::classifier::ExtractKernel;
using bess::classifier::ExtractPlan;
using bess::classifier::MutableBytes;
using bess::classifier::RuntimeClassifierSchema;
using bess::classifier::RuntimeKeyField;
using bess::classifier::SourceKind;
using bess::classifier::SourceView;

std::byte Byte(unsigned value) {
  return static_cast<std::byte>(value);
}

ConstBytes Source(const SourceView &source, SourceKind kind) {
  return kind == SourceKind::kPacket ? source.packet : source.metadata;
}

bool Fits(size_t offset, size_t size, size_t limit) {
  return offset <= limit && size <= limit - offset;
}

bool ReferenceExtract(const RuntimeClassifierSchema &schema,
                      const SourceView &source, MutableBytes key) {
  if (key.size() < schema.key_size) {
    return false;
  }
  for (const auto &field : schema.key_fields) {
    ConstBytes bytes = Source(source, field.source);
    if (schema.bounds == bess::classifier::BoundsPolicy::kCheck &&
        !Fits(field.source_offset, field.size, bytes.size())) {
      return false;
    }
    if (!Fits(field.key_offset, field.size, key.size())) {
      return false;
    }
    std::memcpy(key.data() + field.key_offset,
                bytes.data() + field.source_offset, field.size);
  }
  return true;
}

TEST(ExtractPlanTest, CopiesPacketAndMetadataFieldsExactly) {
  RuntimeClassifierSchema schema{
      .key_size = 9,
      .key_fields = {{SourceKind::kMetadata, 2, 0, 4},
                     {SourceKind::kPacket, 1, 4, 3},
                     {SourceKind::kPacket, 4, 7, 2}},
  };
  auto compiled = ExtractPlan::Compile(schema);
  ASSERT_TRUE(compiled);
  const ExtractPlan &plan = *compiled;
  EXPECT_EQ(ExtractKernel::kGeneric, plan.kernel());

  const std::array<std::byte, 6> packet =
      {Byte(0), Byte(1), Byte(2), Byte(3), Byte(4), Byte(5)};
  const std::array<std::byte, 7> metadata =
      {Byte(10), Byte(11), Byte(12), Byte(13), Byte(14), Byte(15), Byte(16)};
  const SourceView source{packet, metadata};
  std::array<std::byte, 9> expected{};
  std::array<std::byte, 9> key{};
  ASSERT_TRUE(ReferenceExtract(schema, source, MutableBytes(expected)));
  ASSERT_TRUE(plan.Execute(source, MutableBytes(key)));
  EXPECT_EQ(expected, key);
}

TEST(ExtractPlanTest, CoalescesAdjacentFieldsAndUsesSingleKernel) {
  RuntimeClassifierSchema schema{
      .key_size = 6,
      .key_fields = {{SourceKind::kPacket, 0, 0, 2},
                     {SourceKind::kPacket, 2, 2, 4}},
  };
  auto compiled = ExtractPlan::Compile(schema);
  ASSERT_TRUE(compiled);
  EXPECT_EQ(1u, compiled->ops().size());
  EXPECT_EQ(ExtractKernel::kSinglePacket, compiled->kernel());
}

TEST(ExtractPlanTest, BatchStrideIsExplicitAndNoAllocationOccurs) {
  RuntimeClassifierSchema schema{
      .key_size = 3,
      .key_fields = {{SourceKind::kPacket, 0, 0, 3}},
  };
  auto compiled = ExtractPlan::Compile(schema);
  ASSERT_TRUE(compiled);

  const std::array<std::byte, 3> first = {Byte(1), Byte(2), Byte(3)};
  const std::array<std::byte, 3> second = {Byte(4), Byte(5), Byte(6)};
  const std::array<SourceView, 2> sources = {
      SourceView{first, {}}, SourceView{second, {}}};
  std::array<std::byte, 10> output{};

  EXPECT_EQ(0x3ull, compiled->ExecuteBatch(sources, MutableBytes(output), 5));
  EXPECT_EQ(Byte(1), output[0]);
  EXPECT_EQ(Byte(2), output[1]);
  EXPECT_EQ(Byte(3), output[2]);
  EXPECT_EQ(Byte(4), output[5]);
  EXPECT_EQ(Byte(5), output[6]);
  EXPECT_EQ(Byte(6), output[7]);
}

TEST(ExtractPlanTest, ExactBoundaryFieldsDoNotOverRead) {
  RuntimeClassifierSchema schema{
      .key_size = 11,
      .key_fields = {{SourceKind::kPacket, 0, 0, 1},
                     {SourceKind::kPacket, 1, 1, 3},
                     {SourceKind::kPacket, 4, 4, 7}},
  };
  auto compiled = ExtractPlan::Compile(schema);
  ASSERT_TRUE(compiled);

  const std::array<std::byte, 11> packet = {
      Byte(0), Byte(1), Byte(2), Byte(3), Byte(4), Byte(5),
      Byte(6), Byte(7), Byte(8), Byte(9), Byte(10)};
  std::array<std::byte, 11> key{};
  ASSERT_TRUE(compiled->Execute(SourceView{packet, {}}, MutableBytes(key)));
  EXPECT_EQ(packet, key);

  const std::array<std::byte, 10> short_packet{};
  EXPECT_FALSE(
      compiled->Execute(SourceView{short_packet, {}}, MutableBytes(key)));
}

TEST(ExtractPlanTest, ExactWidthFieldsEndAtTheSourceBoundary) {
  const auto check_width = [](size_t width) {
    RuntimeClassifierSchema schema{
        .key_size = width,
        .key_fields = {{SourceKind::kPacket, 0, 0, width}},
    };
    auto compiled = ExtractPlan::Compile(schema);
    EXPECT_TRUE(compiled);
    if (!compiled) {
      return;
    }

    std::vector<std::byte> packet(width);
    std::vector<std::byte> key(width);
    for (size_t i = 0; i < width; i++) {
      packet[i] = Byte(static_cast<unsigned>(i + 1));
    }
    EXPECT_TRUE(compiled->Execute(
        SourceView{ConstBytes(packet), {}}, MutableBytes(key)));
    EXPECT_EQ(packet, key);
  };

  check_width(1);
  check_width(3);
  check_width(7);
}

TEST(ExtractPlanTest, NormalizationMaskApplied) {
  // Single 4-byte packet field with mask {0xFF, 0x00, 0xFF, 0x0F}.
  // Source bytes 0xAB, 0xCD, 0xEF, 0x12 → masked: 0xAB, 0x00, 0xEF, 0x02.
  RuntimeClassifierSchema schema{
      .key_size = 4,
      .key_fields = {{SourceKind::kPacket, 0, 0, 4,
                      bess::classifier::Normalization{
                          {Byte(0xFF), Byte(0x00), Byte(0xFF), Byte(0x0F)}}}},
  };
  auto compiled = ExtractPlan::Compile(schema);
  ASSERT_TRUE(compiled);

  const std::array<std::byte, 4> packet = {Byte(0xAB), Byte(0xCD), Byte(0xEF),
                                           Byte(0x12)};
  std::array<std::byte, 4> key{};
  ASSERT_TRUE(compiled->Execute(SourceView{packet, {}}, MutableBytes(key)));

  EXPECT_EQ(Byte(0xAB), key[0]);
  EXPECT_EQ(Byte(0x00), key[1]);
  EXPECT_EQ(Byte(0xEF), key[2]);
  EXPECT_EQ(Byte(0x02), key[3]);

  // Coalescing must NOT merge masked ops into single kernel.
  EXPECT_EQ(1u, compiled->ops().size());
  EXPECT_EQ(ExtractKernel::kSinglePacket, compiled->kernel());

  // Also verify via ExecuteBatch.
  std::array<std::byte, 4> batch_out{};
  const std::array<SourceView, 1> views = {SourceView{packet, {}}};
  EXPECT_EQ(0x1ull, compiled->ExecuteBatch(views, MutableBytes(batch_out), 4));
  EXPECT_EQ(key, batch_out);
}

TEST(ExtractPlanTest, MaskedExactWidthsStayInBounds) {
  for (size_t width : {1u, 2u, 4u, 8u}) {
    RuntimeClassifierSchema schema;
    schema.key_size = width;

    std::vector<std::byte> packet(width);
    std::vector<std::byte> expected(width);
    std::vector<std::byte> output(width);
    RuntimeKeyField field{SourceKind::kPacket, 0, 0, width};
    field.normalization.mask.resize(width);
    for (size_t i = 0; i < width; i++) {
      packet[i] = Byte(0xA0u + static_cast<unsigned>(i));
      field.normalization.mask[i] =
          Byte(0xF0u | static_cast<unsigned>(i));
      expected[i] = packet[i] & field.normalization.mask[i];
    }
    schema.key_fields.push_back(std::move(field));

    auto compiled = ExtractPlan::Compile(schema);
    ASSERT_TRUE(compiled) << "width=" << width;

    const std::array<SourceView, 1> sources = {
        SourceView{ConstBytes(packet), {}}};
    EXPECT_EQ(1ull, compiled->ExecuteBatch(sources, MutableBytes(output),
                                            width))
        << "width=" << width;
    EXPECT_EQ(expected, output) << "width=" << width;
  }
}

TEST(ExtractPlanTest, AllOnesMaskEqualsNoMask) {
  // All-ones normalization preserves the extracted bytes. Explicit masks
  // remain normalization operations, so this test checks semantics rather
  // than coalescing.
  RuntimeClassifierSchema schema_masked{
      .key_size = 3,
      .key_fields = {{SourceKind::kPacket, 0, 0, 3,
                      bess::classifier::Normalization{
                          {Byte(0xFF), Byte(0xFF), Byte(0xFF)}}}},
  };
  RuntimeClassifierSchema schema_plain{
      .key_size = 3,
      .key_fields = {{SourceKind::kPacket, 0, 0, 3}},
  };

  auto masked = ExtractPlan::Compile(schema_masked);
  auto plain = ExtractPlan::Compile(schema_plain);
  ASSERT_TRUE(masked);
  ASSERT_TRUE(plain);

  const std::array<std::byte, 3> packet = {Byte(0xDE), Byte(0xAD), Byte(0xBE)};
  std::array<std::byte, 3> key_masked{};
  std::array<std::byte, 3> key_plain{};

  ASSERT_TRUE(masked->Execute(SourceView{packet, {}}, MutableBytes(key_masked)));
  ASSERT_TRUE(plain->Execute(SourceView{packet, {}}, MutableBytes(key_plain)));
  EXPECT_EQ(key_plain, key_masked);
}

TEST(ExtractPlanTest, PerPacketBoundsFailureInExecuteBatch) {
  RuntimeClassifierSchema schema{
      .key_size = 4,
      .bounds = BoundsPolicy::kCheck,
      .key_fields = {{SourceKind::kPacket, 0, 0, 4}},
  };
  auto compiled = ExtractPlan::Compile(schema);
  ASSERT_TRUE(compiled);

  const std::array<std::byte, 4> good_packet = {Byte(1), Byte(2), Byte(3), Byte(4)};
  const std::array<std::byte, 2> short_packet = {Byte(5), Byte(6)};
  const std::array<std::byte, 4> another_good = {Byte(7), Byte(8), Byte(9), Byte(10)};

  const std::array<SourceView, 3> sources = {
      SourceView{good_packet, {}},
      SourceView{short_packet, {}},
      SourceView{another_good, {}},
  };
  std::array<std::byte, 3 * 4> output{};

  // Packet 0 is valid (bit 0), packet 1 truncated (bit 1 clear), packet 2 valid (bit 2)
  // Expected mask: 0b101 = 5
  const uint64_t valid_mask = compiled->ExecuteBatch(sources, MutableBytes(output), 4);
  EXPECT_EQ(0x5ull, valid_mask);

  // Packet 0 output correct
  EXPECT_EQ(Byte(1), output[0]);
  EXPECT_EQ(Byte(2), output[1]);
  EXPECT_EQ(Byte(3), output[2]);
  EXPECT_EQ(Byte(4), output[3]);

  // Packet 2 output correct
  EXPECT_EQ(Byte(7), output[8]);
  EXPECT_EQ(Byte(8), output[9]);
  EXPECT_EQ(Byte(9), output[10]);
  EXPECT_EQ(Byte(10), output[11]);
}

TEST(ExtractPlanTest, GappedLayoutLeavesGapBytesUntouched) {
  // The general schema permits gaps between key fields. Extraction writes
  // only the covered bytes; full-key initialization is the caller's job
  // (ExactMatch zero-fills its batch scratch for this reason).
  RuntimeClassifierSchema schema{
      .key_size = 8,
      .bounds = BoundsPolicy::kCheck,
      .key_fields = {{SourceKind::kPacket, 0, 0, 2},
                     {SourceKind::kPacket, 10, 6, 2}},
  };
  auto compiled = ExtractPlan::Compile(schema);
  ASSERT_TRUE(compiled);

  const std::array<std::byte, 12> packet = {Byte(1), Byte(2), Byte(3), Byte(4),
                                            Byte(5), Byte(6), Byte(7), Byte(8),
                                            Byte(9), Byte(10), Byte(11),
                                            Byte(12)};
  const std::array<SourceView, 1> sources = {SourceView{packet, {}}};
  // Pre-fill with a marker: gap bytes [2,6) must survive extraction.
  std::array<std::byte, 8> output;
  output.fill(Byte(0xA5));

  EXPECT_EQ(0x1ull, compiled->ExecuteBatch(sources, MutableBytes(output), 8));
  EXPECT_EQ(Byte(1), output[0]);
  EXPECT_EQ(Byte(2), output[1]);
  for (size_t i = 2; i < 6; i++) {
    EXPECT_EQ(Byte(0xA5), output[i]) << "gap byte " << i;
  }
  EXPECT_EQ(Byte(11), output[6]);
  EXPECT_EQ(Byte(12), output[7]);
}

TEST(ExtractPlanTest, FullyCoversKeyDenseLayouts) {
  // Dense layouts (single or multi-op, masked or not) tile [0, key_size).
  const auto expect_covered = [](RuntimeClassifierSchema schema) {
    auto compiled = ExtractPlan::Compile(schema);
    ASSERT_TRUE(compiled);
    EXPECT_TRUE(compiled->fully_covers_key());
  };
  expect_covered(RuntimeClassifierSchema{
      .key_size = 4,
      .key_fields = {{SourceKind::kPacket, 0, 0, 4}}});
  expect_covered(RuntimeClassifierSchema{
      .key_size = 8,
      .key_fields = {{SourceKind::kPacket, 26, 0, 4},
                     {SourceKind::kPacket, 30, 4, 4}}});
  expect_covered(RuntimeClassifierSchema{
      .key_size = 8,
      .key_fields = {{SourceKind::kPacket, 26, 0, 4,
                      bess::classifier::Normalization{
                          {Byte(0xFF), Byte(0x00), Byte(0xFF), Byte(0x0F)}}},
                     {SourceKind::kMetadata, 16, 4, 4}}});
}

TEST(ExtractPlanTest, FullyCoversKeyRejectsGapsAndSlack) {
  const auto expect_gapped = [](RuntimeClassifierSchema schema) {
    auto compiled = ExtractPlan::Compile(schema);
    ASSERT_TRUE(compiled);
    EXPECT_FALSE(compiled->fully_covers_key());
  };
  // Interior gap between fields.
  expect_gapped(RuntimeClassifierSchema{
      .key_size = 8,
      .key_fields = {{SourceKind::kPacket, 0, 0, 2},
                     {SourceKind::kPacket, 10, 6, 2}}});
  // Trailing slack past the last field.
  expect_gapped(RuntimeClassifierSchema{
      .key_size = 8,
      .key_fields = {{SourceKind::kPacket, 0, 0, 4}}});
}

TEST(ExtractPlanTest, FailedExtractionWritesNothing) {
  // One required-bytes check per source runs before any copy: a short
  // source fails without partial writes, so the caller's invalid-row zero
  // sees pristine scratch.
  RuntimeClassifierSchema schema{
      .key_size = 8,
      .key_fields = {{SourceKind::kPacket, 0, 0, 4},
                     {SourceKind::kPacket, 10, 4, 4}},
  };
  auto compiled = ExtractPlan::Compile(schema);
  ASSERT_TRUE(compiled);
  ASSERT_TRUE(compiled->fully_covers_key());

  const std::array<std::byte, 6> short_packet = {
      Byte(1), Byte(2), Byte(3), Byte(4), Byte(5), Byte(6)};
  const std::array<SourceView, 1> sources = {SourceView{short_packet, {}}};
  std::array<std::byte, 8> output;
  output.fill(Byte(0xA5));
  EXPECT_EQ(0u, compiled->ExecuteBatch(sources, MutableBytes(output), 8));
  for (size_t i = 0; i < 8; i++) {
    EXPECT_EQ(Byte(0xA5), output[i]) << "byte " << i;
  }
}

}  // namespace
