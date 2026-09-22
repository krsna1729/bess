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
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
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

// K3.3 differential test: the legacy ExactMatchTable (packet + metadata
// fields, masks applied on extraction but NOT on rule insertion) is the
// oracle; the new runtime classifier (ExtractPlan with per-field byte masks
// taken from the legacy table's converted mask + BuildRuntimeCuckooBackend
// over densely packed rule bytes) must select identical gates.
//
// The legacy table is kept available to this test only (hash_lb still uses
// the header for key construction). Short-packet inputs are exercised on the
// new path alone: the legacy 8-byte loads would over-read them, while the
// new kCheck plan must report them invalid and route them to the default
// gate without touching out-of-range bytes.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <set>
#include <span>
#include <vector>

#include "classifier/backend.h"
#include "classifier/byte_key.h"
#include "classifier/cuckoo_exact.h"
#include "classifier/extract_plan.h"
#include "classifier/runtime_schema.h"
#include "utils/exact_match_table.h"

namespace {

using bess::classifier::BoundsPolicy;
using bess::classifier::BuildRuntimeCuckooBackend;
using bess::classifier::Byte;
using bess::classifier::ConstBytes;
using bess::classifier::ExtractPlan;
using bess::classifier::MutableBytes;
using bess::classifier::RuntimeClassifierSchema;
using bess::classifier::RuntimeExactRule;
using bess::classifier::RuntimeKeyField;
using bess::classifier::SourceKind;
using bess::classifier::SourceView;
using bess::utils::ExactMatchField;
using bess::utils::ExactMatchKey;
using bess::utils::ExactMatchRuleFields;
using bess::utils::ExactMatchTable;

constexpr gate_idx_t kDefaultGate = 999;
constexpr size_t kMetadataSize = 128;

// One logical field shared by both implementations.
struct TestField {
  bool is_packet;
  // Packet byte offset, or pseudo metadata offset (attr ids are faked: the
  // test's buffer_fn maps attr_id -> metadata + kMetaBase[attr]).
  size_t offset;
  size_t size;
  uint64_t raw_mask;  // 0 == legacy default (all-ones)
};

std::vector<std::byte> MaskBytesFromLegacy(uint64_t converted, size_t size) {
  std::vector<std::byte> out(size);
  for (size_t j = 0; j < size; j++) {
    out[j] = static_cast<Byte>((converted >> (8 * j)) & 0xFFu);
  }
  return out;
}

struct Fixture {
  ExactMatchTable<gate_idx_t> legacy;
  ExtractPlan plan = *ExtractPlan::Compile(
      RuntimeClassifierSchema{.key_size = 1, .bounds = BoundsPolicy::kCheck,
                              .key_fields = std::vector<RuntimeKeyField>{
                                  {SourceKind::kPacket, 0, 0, 1}}});
  bess::classifier::RuntimeExactBackend<gate_idx_t> backend;
  size_t key_size = 0;
  size_t packet_span = 0;  // bytes the new path may read per packet
  std::vector<TestField> fields;
  // attr pseudo-base, parallel to fields (0 for packet fields).
  std::vector<size_t> meta_base;

  // Builds both sides from `fields` + `rules` (per-field byte vectors).
  // Returns false on setup failure (test bug, not a mismatch).
  bool Build(const std::vector<TestField> &in_fields,
             const std::vector<std::vector<std::vector<uint8_t>>> &rules) {
    fields = in_fields;
    meta_base.assign(fields.size(), 0);
    key_size = 0;
    size_t meta_cursor = 0;
    for (size_t i = 0; i < fields.size(); i++) {
      const TestField &f = fields[i];
      if (f.is_packet) {
        if (legacy.AddField(static_cast<int>(f.offset),
                            static_cast<int>(f.size), f.raw_mask,
                            static_cast<int>(i))
                .first != 0) {
          return false;
        }
      } else {
        // Fake attribute ids; buffer_fn below resolves them into the
        // 128-byte metadata scratch at a per-field base.
        const int attr_id = 100 + static_cast<int>(i);
        if (legacy.AddResolvedAttrField(attr_id, static_cast<int>(f.size),
                                        f.raw_mask, static_cast<int>(i))
                .first != 0) {
          return false;
        }
        meta_base[i] = meta_cursor;
        meta_cursor += f.size;
        if (meta_cursor + 8 > kMetadataSize) {
          return false;
        }
      }
      key_size += f.size;
    }

    RuntimeClassifierSchema schema;
    schema.key_size = key_size;
    schema.bounds = BoundsPolicy::kCheck;
    size_t pos = 0;
    for (size_t i = 0; i < fields.size(); i++) {
      const ExactMatchField &lf = legacy.get_field(i);
      RuntimeKeyField kf;
      kf.key_offset = pos;
      kf.size = fields[i].size;
      if (fields[i].is_packet) {
        kf.source = SourceKind::kPacket;
        kf.source_offset = fields[i].offset;
      } else {
        kf.source = SourceKind::kMetadata;
        kf.source_offset = meta_base[i];
      }
      if (fields[i].raw_mask != 0) {
        kf.normalization.mask =
            MaskBytesFromLegacy(lf.mask, fields[i].size);
      }
      schema.key_fields.push_back(std::move(kf));
      pos += fields[i].size;
    }
    auto compiled = ExtractPlan::Compile(schema);
    if (!compiled) {
      return false;
    }
    plan = std::move(*compiled);

    // Densely packed rule keys, WITHOUT applying masks (legacy gather_key).
    std::vector<std::byte> storage(rules.size() * key_size);
    std::vector<RuntimeExactRule<gate_idx_t>> backend_rules;
    backend_rules.reserve(rules.size());
    for (size_t r = 0; r < rules.size(); r++) {
      if (rules[r].size() != fields.size()) {
        return false;
      }
      std::byte *dst = storage.data() + r * key_size;
      size_t p = 0;
      for (size_t i = 0; i < fields.size(); i++) {
        if (rules[r][i].size() != fields[i].size) {
          return false;
        }
        std::memcpy(dst + p, rules[r][i].data(), fields[i].size);
        p += fields[i].size;
      }
      backend_rules.push_back(RuntimeExactRule<gate_idx_t>{
          .key = ConstBytes(dst, key_size),
          .result = static_cast<gate_idx_t>(r),
      });
      if (legacy.AddRule(static_cast<gate_idx_t>(r), rules[r]).first != 0) {
        return false;
      }
    }
    auto be = BuildRuntimeCuckooBackend<gate_idx_t>(key_size, backend_rules);
    if (!be) {
      return false;
    }
    backend = std::move(*be);

    packet_span = 0;
    for (const TestField &f : fields) {
      if (f.is_packet) {
        packet_span = std::max(packet_span, f.offset + f.size);
      }
    }
    // Legacy reads 8 bytes at every packet field offset; keep the oracle
    // in-bounds by padding the presented buffers.
    packet_span += 8;
    return true;
  }

  // Legacy gate for one packet + metadata pair (buffers must satisfy the
  // padding contract above).
  gate_idx_t LegacyGate(const std::vector<uint8_t> &packet,
                        const std::array<uint8_t, kMetadataSize> &meta) const {
    ExactMatchKey key{};
    // Reproduce MakeKeys batch path for a single packet.
    const size_t last = (legacy.total_key_size() - 1) / 8;
    key.u64_arr[last] = 0;
    for (size_t i = 0; i < fields.size(); i++) {
      const ExactMatchField &f = legacy.get_field(i);
      const uint8_t *base = f.attr_id >= 0
                                ? meta.data() + meta_base[i]
                                : packet.data() + fields[i].offset;
      uint64_t loaded = 0;
      std::memcpy(&loaded, base, sizeof(loaded));
      uint8_t *k =
          reinterpret_cast<uint8_t *>(key.u64_arr) + f.pos;
      *reinterpret_cast<uint64_t *>(k) = loaded & f.mask;
    }
    return legacy.Find(key, kDefaultGate);
  }

  // New-path gate; returns {gate, valid}.
  std::pair<gate_idx_t, bool> NewGate(ConstBytes packet,
                                      ConstBytes meta) const {
    SourceView view{packet, meta};
    std::vector<std::byte> out(key_size, Byte{0});
    uint64_t valid = plan.ExecuteBatch(std::span<const SourceView>(&view, 1),
                                       MutableBytes(out), key_size);
    if (!(valid & 1u)) {
      return {kDefaultGate, false};
    }
    std::array<gate_idx_t, 1> results{};
    uint64_t hits =
        backend.lookup_batch(ConstBytes(out), key_size, results);
    if (!(hits & 1u)) {
      return {kDefaultGate, true};
    }
    return {results[0], true};
  }
};

std::vector<uint8_t> RandomBytes(std::mt19937_64 &rng, size_t n) {
  std::vector<uint8_t> out(n);
  for (size_t i = 0; i < n; i++) {
    out[i] = static_cast<uint8_t>(rng());
  }
  return out;
}

// Unique packed rule keys (avoids depending on duplicate-overwrite order,
// which the module-level UpsertRule and python tests pin separately).
std::vector<std::vector<std::vector<uint8_t>>> UniqueRules(
    std::mt19937_64 &rng, const std::vector<TestField> &fields, size_t n) {
  std::vector<std::vector<std::vector<uint8_t>>> rules;
  std::set<std::vector<uint8_t>> seen;
  while (rules.size() < n) {
    std::vector<std::vector<uint8_t>> rule;
    std::vector<uint8_t> packed;
    for (const TestField &f : fields) {
      auto b = RandomBytes(rng, f.size);
      packed.insert(packed.end(), b.begin(), b.end());
      rule.push_back(std::move(b));
    }
    if (seen.insert(packed).second) {
      rules.push_back(std::move(rule));
    }
  }
  return rules;
}

void RunDifferential(const std::vector<TestField> &fields, uint64_t seed,
                     size_t num_rules, size_t num_packets) {
  std::mt19937_64 rng(seed);
  Fixture fix;
  auto rules = UniqueRules(rng, fields, num_rules);
  ASSERT_TRUE(fix.Build(fields, rules));

  // Random traffic.
  size_t mismatches = 0;
  for (size_t t = 0; t < num_packets; t++) {
    auto packet = RandomBytes(rng, fix.packet_span);
    std::array<uint8_t, kMetadataSize> meta{};
    auto meta_rand = RandomBytes(rng, kMetadataSize);
    std::memcpy(meta.data(), meta_rand.data(), kMetadataSize);

    const gate_idx_t want = fix.LegacyGate(packet, meta);
    ConstBytes packet_view(reinterpret_cast<const Byte *>(packet.data()),
                           packet.size());
    ConstBytes meta_view(reinterpret_cast<const Byte *>(meta.data()),
                         meta.size());
    auto [got, valid] = fix.NewGate(packet_view, meta_view);
    if (!valid || want != got) {
      if (mismatches < 5) {
        EXPECT_TRUE(valid) << "t=" << t;
        EXPECT_EQ(want, got) << "t=" << t;
      }
      mismatches++;
    }
  }
  EXPECT_EQ(0u, mismatches);

  // Planted hits: packets built from rule bytes must hit identically,
  // including rules whose masked-off bytes are nonzero (unmatchable rules
  // must agree too).
  size_t plant_mismatches = 0;
  for (size_t r = 0; r < rules.size(); r++) {
    std::vector<uint8_t> packet(fix.packet_span, 0);
    std::array<uint8_t, kMetadataSize> meta{};
    for (size_t i = 0; i < fields.size(); i++) {
      uint8_t *dst = fields[i].is_packet
                         ? packet.data() + fields[i].offset
                         : meta.data() + fix.meta_base[i];
      std::memcpy(dst, rules[r][i].data(), fields[i].size);
    }
    const gate_idx_t want = fix.LegacyGate(packet, meta);
    ConstBytes packet_view(reinterpret_cast<const Byte *>(packet.data()),
                           packet.size());
    ConstBytes meta_view(reinterpret_cast<const Byte *>(meta.data()),
                         meta.size());
    auto [got, valid] = fix.NewGate(packet_view, meta_view);
    if (!valid || want != got) {
      if (plant_mismatches < 5) {
        EXPECT_TRUE(valid) << "rule=" << r;
        EXPECT_EQ(want, got) << "rule=" << r;
      }
      plant_mismatches++;
    }
  }
  EXPECT_EQ(0u, plant_mismatches);
}

TEST(ExactMatchMigration, SingleIpFieldDefaultMask) {
  RunDifferential({{true, 26, 4, 0}}, 0x1234, 16, 2000);
}

TEST(ExactMatchMigration, TwoFieldsPartialMasks) {
  RunDifferential({{true, 26, 4, 0xFFFFFF00}, {true, 30, 4, 0x00FFFFFF}}, 0x5678,
                  32, 2000);
}

TEST(ExactMatchMigration, MixedPacketMetadataMaskedOffBits) {
  // Metadata mask ignores the low 4 bits; rules carry nonzero low bits, so
  // some planted rules are unmatchable on both sides identically.
  RunDifferential({{true, 12, 2, 0xFFF0}, {false, 0, 2, 0xFFF0}}, 0x9ABC, 16,
                  2000);
}

TEST(ExactMatchMigration, MaxFieldsOddSizes) {
  RunDifferential({{true, 0, 1, 0},
                   {true, 2, 2, 0xFFFF},
                   {true, 5, 3, 0},
                   {true, 9, 5, 0xFFFFFFFFFFull},
                   {true, 16, 8, 0x0102030405060708ull},
                   {false, 0, 1, 0xF0},
                   {false, 0, 4, 0},
                   {false, 0, 8, 0x00FF00FF00FF00FFull}},
                  0xDEF0, 64, 2000);
}

TEST(ExactMatchMigration, SingleBitMask) {
  RunDifferential({{true, 4, 8, 0x1}}, 0x1357, 8, 2000);
}

TEST(ExactMatchMigration, ShortPacketsTakeDefaultGate) {
  std::mt19937_64 rng(0x2468);
  Fixture fix;
  const std::vector<TestField> fields = {{true, 10, 4, 0}, {true, 20, 2, 0}};
  auto rules = UniqueRules(rng, fields, 8);
  ASSERT_TRUE(fix.Build(fields, rules));

  std::array<uint8_t, kMetadataSize> meta{};
  // Lengths that truncate inside (or before) the second field. The first
  // field needs bytes [10,14); the second [20,22).
  for (size_t len : {0u, 5u, 10u, 13u, 14u, 19u, 20u, 21u}) {
    auto raw = RandomBytes(rng, std::max<size_t>(len, 1));
    ConstBytes packet_view(reinterpret_cast<const Byte *>(raw.data()), len);
    ConstBytes meta_view(reinterpret_cast<const Byte *>(meta.data()),
                         meta.size());
    auto [got, valid] = fix.NewGate(packet_view, meta_view);
    if (len < 22) {
      EXPECT_FALSE(valid) << "len=" << len;
      EXPECT_EQ(kDefaultGate, got) << "len=" << len;
    } else {
      EXPECT_TRUE(valid) << "len=" << len;
    }
  }
}

TEST(ExactMatchMigration, MultisegmentTailTakesDefaultGate) {
  // A field reaching past the first segment must not be assembled from later
  // segments: the new path only sees first-segment bytes.
  std::mt19937_64 rng(0xABCD);
  Fixture fix;
  const std::vector<TestField> fields = {{true, 0, 8, 0}};
  auto rules = UniqueRules(rng, fields, 4);
  ASSERT_TRUE(fix.Build(fields, rules));

  std::array<uint8_t, kMetadataSize> meta{};
  auto first_seg = RandomBytes(rng, 6);  // shorter than the 8-byte field
  ConstBytes packet_view(
      reinterpret_cast<const Byte *>(first_seg.data()), first_seg.size());
  ConstBytes meta_view(reinterpret_cast<const Byte *>(meta.data()),
                       meta.size());
  auto [got, valid] = fix.NewGate(packet_view, meta_view);
  EXPECT_FALSE(valid);
  EXPECT_EQ(kDefaultGate, got);
}

}  // namespace
