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

// K3.3.2 lookup isolation benchmark: split the end-to-end migration
// comparison into independently measurable stages so a dataplane gap can be
// attributed instead of guessed:
//
//   extraction : BM_LegacyExtract vs BM_NewExtract
//   lookup     : legacy, direct runtime, materialized runtime, borrowed,
//                prehashed, batch, and type-erased runtime rungs
//   hashing    : BM_HashFNV_Key vs BM_HashCRC_Key vs BM_HashCRC_FixedProbe
//                vs BM_HashCRC_Bytes vs BM_HashLegacy_Chunk
//   zeroing    : BM_Zero8 (the scratch memset, measured alone)
//   end-to-end : BM_LegacyEndToEnd vs BM_NewEndToEnd
//   control    : BM_RebuildCost (generation build at 1K/10K/100K rules)
//
// Schema matrix (arg 0), all 2x4-byte fields, 8-byte logical keys, 256 rules:
//   0 = contiguous, default masks (coalesces to one single-packet op)
//   1 = masked partial masks (two masked ops, generic kernel)
//   2 = non-contiguous packet offsets (two unmasked ops, generic kernel)
//   3 = packet + metadata (two mixed-source ops, generic kernel)
//
// The contiguous default-mask case exercises the easiest possible extraction
// kernel; variants 1-3 characterize the generic execution plan.
//
// NOTE on variant 3 (metadata): ExactMatchTable::MakeKeys(const void **) only
// serves packet-offset fields, so the legacy metadata extraction below is a
// faithful inline of DoMakeKeys (utils/exact_match_table.h) with per-field
// base pointers: same padding zero, same unaligned 8-byte load, same mask,
// same positional store. The new path uses the real ExtractPlan API.

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <rte_hash_crc.h>

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
using bess::utils::ExactMatchKey;
using bess::utils::ExactMatchRuleFields;
using bess::utils::ExactMatchTable;
using bess::classifier::detail::RuntimeCuckooEqual;

using bess::classifier::detail::RuntimeCuckooFixedProbeEqual;
using bess::classifier::detail::RuntimeCuckooFixedProbeHash;
using bess::classifier::detail::RuntimeCuckooHash;
using bess::classifier::detail::RuntimeCuckooKey;
using bess::classifier::detail::RuntimeCuckooProbe;
using bess::classifier::detail::RuntimeCuckooProbeEqual;
using bess::classifier::detail::RuntimeCuckooProbeHash;
using bess::classifier::detail::RuntimeCuckooLookupBatchFixed;
using bess::classifier::detail::RuntimeCuckooState;

constexpr size_t kMaxBatch = 32;
constexpr size_t kBufBytes = 128;
constexpr size_t kMetaBytes = 128;
constexpr size_t kNumRules = 256;
constexpr size_t kFieldBytes = 4;
constexpr size_t kKeySize = 2 * kFieldBytes;
constexpr std::array<uint64_t, 4> kHashValues = {
    0x123456789ABCDEF0ull,
    0xB0B0B0B0B0B0B0B0ull,
    0x4E5A6B7C8D9EAFB1ull,
    0xDEADBEEFCAFEBABEull};

uint64_t AllValidMask(size_t count) {
  return count == 64 ? ~uint64_t{0} : (uint64_t{1} << count) - 1;
}

std::array<std::array<std::byte, 8>, 4> HashValues() {
  std::array<std::array<std::byte, 8>, 4> values{};
  for (size_t i = 0; i < values.size(); i++) {
    std::memcpy(values[i].data(), &kHashValues[i], sizeof(kHashValues[i]));
  }
  return values;
}

struct FieldDesc {
  bool is_meta;
  size_t pkt_offset;   // packet byte offset, packet fields
  size_t meta_offset;  // metadata base, metadata fields
  size_t size;
  uint64_t raw_mask;  // 0 == legacy default (all-ones)
};

constexpr FieldDesc kVariants[4][2] = {
    // 0: contiguous, default masks.
    {{false, 26, 0, kFieldBytes, 0}, {false, 30, 0, kFieldBytes, 0}},
    // 1: contiguous, partial masks (blocks coalescing).
    {{false, 26, 0, kFieldBytes, 0xFFFFFF00ull},
     {false, 30, 0, kFieldBytes, 0x00FFFFFFull}},
    // 2: non-contiguous packet offsets, default masks.
    {{false, 0, 0, kFieldBytes, 0}, {false, 64, 0, kFieldBytes, 0}},
    // 3: packet + metadata.
    {{false, 26, 0, kFieldBytes, 0}, {true, 0, 16, kFieldBytes, 0}},
};

std::vector<std::byte> MaskBytesFromU64(uint64_t converted, size_t size) {
  std::vector<std::byte> out(size);
  for (size_t j = 0; j < size; j++) {
    out[j] = static_cast<Byte>((converted >> (8 * j)) & 0xFFu);
  }
  return out;
}

// Test fixture: two 4-byte fields per the selected variant, 256 rules.
// Buffers hold one full key's worth of readable bytes per slot.
struct Fixture {
  int variant = 0;
  ExactMatchTable<gate_idx_t> legacy;
  ExtractPlan plan = *ExtractPlan::Compile(
      RuntimeClassifierSchema{.key_size = kFieldBytes,
                              .bounds = BoundsPolicy::kCheck,
                              .key_fields = std::vector<RuntimeKeyField>{
                                  {SourceKind::kPacket, 0, 0, kFieldBytes}}});
  bess::classifier::RuntimeExactBackend<gate_idx_t> backend;
  using RuntimeState = RuntimeCuckooState<8, gate_idx_t>;
  RuntimeState runtime_state;

  std::array<std::array<uint8_t, kBufBytes>, kMaxBatch> packets{};
  std::array<std::array<uint8_t, kMetaBytes>, kMaxBatch> metas{};
  std::array<SourceView, kMaxBatch> views{};
  // Per-field base pointers for the legacy metadata extraction mirror.
  std::array<std::array<const uint8_t *, kMaxBatch>, 2> legacy_bases{};

  bool Init(int in_variant) {
    variant = in_variant;
    const FieldDesc *desc = kVariants[variant];

    for (int f = 0; f < 2; f++) {
      if (desc[f].is_meta) {
        if (legacy
                .AddResolvedAttrField(100 + f, static_cast<int>(desc[f].size),
                                      desc[f].raw_mask, f)
                .first != 0) {
          return false;
        }
      } else {
        if (legacy
                .AddField(static_cast<int>(desc[f].pkt_offset),
                          static_cast<int>(desc[f].size), desc[f].raw_mask, f)
                .first != 0) {
          return false;
        }
      }
    }

    RuntimeClassifierSchema schema;
    schema.key_size = kKeySize;
    schema.bounds = BoundsPolicy::kCheck;
    for (int f = 0; f < 2; f++) {
      RuntimeKeyField kf;
      kf.key_offset = static_cast<size_t>(f) * kFieldBytes;
      kf.size = kFieldBytes;
      if (desc[f].is_meta) {
        kf.source = SourceKind::kMetadata;
        kf.source_offset = desc[f].meta_offset;
      } else {
        kf.source = SourceKind::kPacket;
        kf.source_offset = desc[f].pkt_offset;
      }
      if (desc[f].raw_mask != 0) {
        kf.normalization.mask = MaskBytesFromU64(
            legacy.get_field(static_cast<size_t>(f)).mask, kFieldBytes);
      }
      schema.key_fields.push_back(std::move(kf));
    }
    auto compiled = ExtractPlan::Compile(schema);
    if (!compiled) {
      return false;
    }
    plan = std::move(*compiled);

    std::vector<std::byte> storage(kNumRules * kKeySize);
    std::vector<RuntimeExactRule<gate_idx_t>> backend_rules;
    backend_rules.reserve(kNumRules);
    for (size_t r = 0; r < kNumRules; r++) {
      ExactMatchRuleFields legacy_rule;
      legacy_rule.reserve(2);
      for (int f = 0; f < 2; f++) {
        // Rule bytes are the little-endian encoding of (r * 31 + f * const).
        const uint32_t v =
            static_cast<uint32_t>(r * 31 + f * 0x9E3779B9u);
        std::vector<uint8_t> field_bytes(kFieldBytes);
        std::memcpy(field_bytes.data(), &v, kFieldBytes);
        std::memcpy(storage.data() + r * kKeySize + f * kFieldBytes,
                    field_bytes.data(), kFieldBytes);
        legacy_rule.push_back(std::move(field_bytes));
      }
      backend_rules.push_back(RuntimeExactRule<gate_idx_t>{
          .key = ConstBytes(storage.data() + r * kKeySize, kKeySize),
          .result = static_cast<gate_idx_t>(r % 2048),
      });
      if (legacy.AddRule(static_cast<gate_idx_t>(r % 2048), legacy_rule)
              .first != 0) {
        return false;
      }
    }
    auto be = BuildRuntimeCuckooBackend<gate_idx_t>(kKeySize, backend_rules);
    if (!be) {
      return false;
    }
    backend = std::move(*be);
    runtime_state.logical_key_size = kKeySize;
    const RuntimeCuckooHash<8> runtime_hash{kKeySize};
    const bess::classifier::detail::RuntimeCuckooEqual<8> runtime_equal{
        kKeySize};
    for (const auto &rule : backend_rules) {
      RuntimeCuckooKey<8> key{};
      std::memcpy(key.bytes.data(), rule.key.data(), kKeySize);
      if (runtime_state.map.Insert(key, rule.result, runtime_hash,
                                   runtime_equal) == nullptr) {
        return false;
      }
    }

    if (!VerifyHashParity()) {
      return false;
    }

    for (size_t i = 0; i < kMaxBatch; i++) {
      views[i].packet = ConstBytes(
          reinterpret_cast<const Byte *>(packets[i].data()), kBufBytes);
      views[i].metadata = ConstBytes(
          reinterpret_cast<const Byte *>(metas[i].data()), kMetaBytes);
      for (int f = 0; f < 2; f++) {
        legacy_bases[f][i] =
            desc[f].is_meta
                ? metas[i].data() + desc[f].meta_offset
                : packets[i].data() + desc[f].pkt_offset;
      }
    }
    return true;
  }
  bool VerifyHashParity() const {

    const auto values = HashValues();
    const RuntimeCuckooHash<8> stored_hash{kKeySize};
    const RuntimeCuckooFixedProbeHash<8> probe_hash;
    for (size_t i = 0; i < values.size(); i++) {
      RuntimeCuckooKey<8> stored{};
      std::memcpy(stored.bytes.data(), values[i].data(), kKeySize);
      const RuntimeCuckooProbe probe{values[i].data(), kKeySize};
      const size_t fixed = probe_hash(probe);
      if (fixed != stored_hash(stored) ||
          fixed != rte_hash_crc(values[i].data(), kKeySize, 0) ||
          fixed != crc32c_sse42_u64(kHashValues[i], 0)) {
        return false;
      }
    }
    return true;
  }

  uint8_t *FieldBytes(size_t slot, int f) {
    const FieldDesc &d = kVariants[variant][f];
    return d.is_meta ? metas[slot].data() + d.meta_offset
                     : packets[slot].data() + d.pkt_offset;
  }

  // Fill batch buffers: hit traffic replays rule keys, miss traffic uses
  // bytes that match no rule, mixed alternates.
  void Fill(size_t batch, int mix) {
    for (size_t i = 0; i < batch; i++) {
      const bool hit = mix == 0 || (mix == 2 && (i % 2 == 0));
      for (int f = 0; f < 2; f++) {
        uint8_t *dst = FieldBytes(i, f);
        if (hit) {
          const uint32_t r = static_cast<uint32_t>((i * 17) % kNumRules);
          const uint32_t v = r * 31 + static_cast<uint32_t>(f * 0x9E3779B9u);
          std::memcpy(dst, &v, kFieldBytes);
        } else {
          std::memset(dst, 0xA5, kFieldBytes);
        }
      }
    }
  }

  // Legacy packet extraction over the batch. Variant 3 uses the mirror below
  // because the const-void** MakeKeys path cannot provide metadata buffers.
  void LegacyExtractPacket(const void **bufs, ExactMatchKey *keys,
                           size_t n) const {
    legacy.MakeKeys(bufs, keys, n);
  }

  // Faithful inline of ExactMatchTable::DoMakeKeys with per-field base
  // pointers: same padding zero, same unaligned 8-byte load and mask, same
  // positional store per field. memcpy preserves those exact widths without
  // making the benchmark depend on unaligned pointer casts.
  void LegacyExtractMirror(ExactMatchKey *keys, size_t n) const {
    const size_t last = (legacy.total_key_size() - 1) / 8;
    for (size_t i = 0; i < n; i++) {
      keys[i].u64_arr[last] = 0;
    }
    for (int f = 0; f < 2; f++) {
      const auto &lf = legacy.get_field(static_cast<size_t>(f));
      const uint64_t mask = lf.mask;
      const int pos = lf.pos;
      for (size_t j = 0; j < n; j++) {
        uint8_t *k = reinterpret_cast<uint8_t *>(keys[j].u64_arr) + pos;
        uint64_t loaded;
        std::memcpy(&loaded, legacy_bases[f][j], sizeof(loaded));
        loaded &= mask;
        std::memcpy(k, &loaded, sizeof(loaded));
      }
    }
  }
};

// mix: 0 = all-hit, 1 = all-miss, 2 = alternating.
void BM_LegacyExtract(benchmark::State &state) {
  const int variant = static_cast<int>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  Fixture fix;
  if (!fix.Init(variant)) {
    state.SkipWithError("legacy fixture build failed");
    return;
  }
  fix.Fill(batch, mix);

  std::array<const void *, kMaxBatch> bufs{};
  for (size_t i = 0; i < batch; i++) {
    bufs[i] = fix.packets[i].data();
  }
  std::array<ExactMatchKey, kMaxBatch> keys{};
  if (variant == 3) {
    for (auto _ : state) {
      fix.LegacyExtractMirror(keys.data(), batch);
      benchmark::DoNotOptimize(keys);
    }
  } else {
    for (auto _ : state) {
      fix.LegacyExtractPacket(bufs.data(), keys.data(), batch);
      benchmark::DoNotOptimize(keys);
    }
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_NewExtract(benchmark::State &state) {
  const int variant = static_cast<int>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  Fixture fix;
  if (!fix.Init(variant)) {
    state.SkipWithError("new-path fixture build failed");
    return;
  }
  fix.Fill(batch, mix);

  std::array<std::byte, kMaxBatch * kKeySize> keys{};
  for (auto _ : state) {
    // Pure extraction: no pre-zeroing (measured separately as BM_Zero8).
    const uint64_t valid = fix.plan.ExecuteBatch(
        std::span<const SourceView>(fix.views).first(batch),
        MutableBytes(keys).first(batch * kKeySize), kKeySize);
    benchmark::DoNotOptimize(valid);
    benchmark::DoNotOptimize(keys);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_Zero8(benchmark::State &state) {
  const size_t batch = static_cast<size_t>(state.range(0));
  std::array<std::byte, kMaxBatch * kKeySize> keys{};
  for (auto _ : state) {
    std::memset(keys.data(), 0, batch * kKeySize);
    benchmark::DoNotOptimize(keys);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

// Prebuilt lookup keys: one extraction pass at setup, then timed lookups.
struct LookupFixture : Fixture {
  std::array<ExactMatchKey, kMaxBatch> legacy_keys{};
  std::array<std::byte, kMaxBatch * kKeySize> packed_keys{};
  std::array<RuntimeCuckooKey<8>, kMaxBatch> runtime_keys{};

  bool InitLookup(int in_variant, size_t batch, int mix) {
    if (!Init(in_variant)) {
      return false;
    }
    Fill(batch, mix);
    if (in_variant == 3) {
      LegacyExtractMirror(legacy_keys.data(), batch);
    } else {
      std::array<const void *, kMaxBatch> bufs{};
      for (size_t i = 0; i < batch; i++) {
        bufs[i] = packets[i].data();
      }
      LegacyExtractPacket(bufs.data(), legacy_keys.data(), batch);
    }
    const uint64_t valid = plan.ExecuteBatch(
        std::span<const SourceView>(views).first(batch),
        MutableBytes(packed_keys).first(batch * kKeySize), kKeySize);
    if (valid != AllValidMask(batch)) {
      return false;
    }
    for (size_t i = 0; i < batch; i++) {
      runtime_keys[i] = RuntimeCuckooKey<8>{};
      std::memcpy(runtime_keys[i].bytes.data(),
                  packed_keys.data() + i * kKeySize, kKeySize);
    }
    return true;
  }
};

void BM_LegacyLookup(benchmark::State &state) {
  const int variant = static_cast<int>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  LookupFixture fix;
  if (!fix.InitLookup(variant, batch, mix)) {
    state.SkipWithError("legacy lookup fixture build failed");
    return;
  }
  std::array<gate_idx_t, kMaxBatch> gates{};
  for (auto _ : state) {
    fix.legacy.Find(fix.legacy_keys.data(), gates.data(), batch, 0);
    benchmark::DoNotOptimize(gates);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_RuntimeDirectLookup(benchmark::State &state) {
  const int variant = static_cast<int>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  LookupFixture fix;
  if (!fix.InitLookup(variant, batch, mix)) {
    state.SkipWithError("runtime direct lookup fixture build failed");
    return;
  }
  const RuntimeCuckooHash<8> hash{kKeySize};
  const RuntimeCuckooEqual<8> equal{kKeySize};
  std::array<gate_idx_t, kMaxBatch> gates{};
  for (auto _ : state) {
    uint64_t hits = 0;
    for (size_t i = 0; i < batch; i++) {
      const auto *entry =
          fix.runtime_state.map.Find(fix.runtime_keys[i], hash, equal);
      if (entry != nullptr) {
        gates[i] = entry->second;
        hits |= (uint64_t{1} << i);
      }
    }
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(gates);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_RuntimeMaterializedLookup(benchmark::State &state) {
  const int variant = static_cast<int>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  LookupFixture fix;
  if (!fix.InitLookup(variant, batch, mix)) {
    state.SkipWithError("runtime materialized lookup fixture build failed");
    return;
  }
  const RuntimeCuckooHash<8> hash{kKeySize};
  const RuntimeCuckooEqual<8> equal{kKeySize};
  RuntimeCuckooKey<8> key;
  std::array<gate_idx_t, kMaxBatch> gates{};
  for (auto _ : state) {
    uint64_t hits = 0;
    for (size_t i = 0; i < batch; i++) {
      std::memcpy(key.bytes.data(),
                  fix.packed_keys.data() + i * kKeySize, kKeySize);
      const auto *entry = fix.runtime_state.map.Find(key, hash, equal);
      if (entry != nullptr) {
        gates[i] = entry->second;
        hits |= (uint64_t{1} << i);
      }
    }
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(gates);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_RuntimeBorrowedLookup(benchmark::State &state) {
  const int variant = static_cast<int>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  LookupFixture fix;
  if (!fix.InitLookup(variant, batch, mix)) {
    state.SkipWithError("runtime borrowed lookup fixture build failed");
    return;
  }
  const RuntimeCuckooProbeHash hash;
  const RuntimeCuckooProbeEqual<8> equal;
  std::array<gate_idx_t, kMaxBatch> gates{};
  for (auto _ : state) {
    uint64_t hits = 0;
    for (size_t i = 0; i < batch; i++) {
      const RuntimeCuckooProbe probe{
          fix.packed_keys.data() + i * kKeySize, kKeySize};
      const auto *entry = fix.runtime_state.map.FindAs(probe, hash, equal);
      if (entry != nullptr) {
        gates[i] = entry->second;
        hits |= (uint64_t{1} << i);
      }
    }
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(gates);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_RuntimePrehashedLookup(benchmark::State &state) {
  const int variant = static_cast<int>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  LookupFixture fix;
  if (!fix.InitLookup(variant, batch, mix)) {
    state.SkipWithError("runtime prehashed lookup fixture build failed");
    return;
  }
  const RuntimeCuckooProbeHash hash;
  const RuntimeCuckooProbeEqual<8> equal;
  std::array<gate_idx_t, kMaxBatch> gates{};
  for (auto _ : state) {
    uint64_t hits = 0;
    for (size_t i = 0; i < batch; i++) {
      const RuntimeCuckooProbe probe{
          fix.packed_keys.data() + i * kKeySize, kKeySize};
      const auto *entry = fix.runtime_state.map.FindPrehashedAs(
          static_cast<bess::utils::HashResult>(hash(probe)), probe, equal);
      if (entry != nullptr) {
        gates[i] = entry->second;
        hits |= (uint64_t{1} << i);
      }
    }
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(gates);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_RuntimeBatchLookup(benchmark::State &state) {
  const int variant = static_cast<int>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  LookupFixture fix;
  if (!fix.InitLookup(variant, batch, mix)) {
    state.SkipWithError("runtime batch lookup fixture build failed");
    return;
  }
  std::array<gate_idx_t, kMaxBatch> gates{};
  for (auto _ : state) {
    const uint64_t hits = RuntimeCuckooLookupBatchFixed<8, 8, gate_idx_t>(
        &fix.runtime_state,
        ConstBytes(fix.packed_keys.data(), batch * kKeySize), kKeySize,
        std::span<gate_idx_t>(gates).first(batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(gates);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_RuntimeProbeStats(benchmark::State &state) {
  const int variant = static_cast<int>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  LookupFixture fix;
  if (!fix.InitLookup(variant, batch, mix)) {
    state.SkipWithError("runtime probe stats fixture build failed");
    return;
  }
  const RuntimeCuckooFixedProbeHash<8> hash;
  const RuntimeCuckooFixedProbeEqual<8, 8> equal;
  using RuntimeMap =
      bess::utils::CuckooMap<RuntimeCuckooKey<8>, gate_idx_t,
                             RuntimeCuckooHash<8>, RuntimeCuckooEqual<8>>;
  RuntimeMap::LookupStats stats;
  std::array<gate_idx_t, kMaxBatch> gates{};
  for (auto _ : state) {
    uint64_t hits = 0;
    for (size_t i = 0; i < batch; i++) {
      const RuntimeCuckooProbe probe{
          fix.packed_keys.data() + i * kKeySize, kKeySize};
      const auto *entry = fix.runtime_state.map.FindPrehashedAsWithStats(
          static_cast<bess::utils::HashResult>(hash(probe)), probe, equal,
          stats);
      if (entry != nullptr) {
        gates[i] = entry->second;
        hits |= (uint64_t{1} << i);
      }
    }
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(gates);
  }
  const double total =
      static_cast<double>(state.iterations() * static_cast<int64_t>(batch));
  state.counters["primary_pct"] =
      benchmark::Counter(100.0 * stats.primary_hits / total);
  state.counters["secondary_pct"] =
      benchmark::Counter(100.0 * stats.secondary_hits / total);
  state.counters["miss_pct"] =
      benchmark::Counter(100.0 * stats.misses / total);
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_NewLookup(benchmark::State &state) {
  const int variant = static_cast<int>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  LookupFixture fix;
  if (!fix.InitLookup(variant, batch, mix)) {
    state.SkipWithError("new lookup fixture build failed");
    return;
  }
  std::array<gate_idx_t, kMaxBatch> gates{};
  for (auto _ : state) {
    uint64_t hits = fix.backend.lookup_batch(
        ConstBytes(fix.packed_keys.data(), batch * kKeySize), kKeySize,
        std::span<gate_idx_t>(gates).first(batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(gates);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

// Hash micro-benchmarks over identical logical bytes: cycle four 8-byte
// values through the stack-key materialization each runtime lookup performs.
// FNV-1a below is the retired K3.3 runtime hash, kept as a local reference so
// the CRC adoption stays measured; the live runtime functor is exercised by
// BM_HashCRC_Key and BM_NewLookup.
size_t Fnv1aRetired(const Byte *bytes, size_t len) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < len; i++) {
    hash ^= static_cast<uint8_t>(bytes[i]);
    hash *= 1099511628211ull;
  }
  return static_cast<size_t>(hash);
}

void BM_HashFNV_Key(benchmark::State &state) {
  const size_t batch = static_cast<size_t>(state.range(0));
  auto values = HashValues();
  std::array<std::byte, 8> key{};
  size_t sink = 0;
  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      std::memcpy(key.data(), values[i % 4].data(), 8);
      sink += Fnv1aRetired(key.data(), 8);
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_HashCRC_Key(benchmark::State &state) {
  const size_t batch = static_cast<size_t>(state.range(0));
  auto values = HashValues();
  const RuntimeCuckooHash<8> hasher{8};
  size_t sink = 0;
  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      RuntimeCuckooKey<8> key{};
      std::memcpy(key.bytes.data(), values[i % 4].data(), 8);
      sink += hasher(key);
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_HashCRC_FixedProbe(benchmark::State &state) {
  const size_t batch = static_cast<size_t>(state.range(0));
  auto values = HashValues();
  const RuntimeCuckooFixedProbeHash<8> hasher;
  size_t sink = 0;
  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      const RuntimeCuckooProbe probe{values[i % 4].data(), 8};
      sink += hasher(probe);
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_HashCRC_Bytes(benchmark::State &state) {
  const size_t batch = static_cast<size_t>(state.range(0));
  auto values = HashValues();
  size_t sink = 0;
  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      sink += rte_hash_crc(values[i % 4].data(), 8, 0);
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_HashLegacy_Chunk(benchmark::State &state) {
  const size_t batch = static_cast<size_t>(state.range(0));
  std::array<uint64_t, 4> values = kHashValues;
  uint32_t sink = 0;
  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      sink += static_cast<uint32_t>(
          crc32c_sse42_u64(values[i % 4], 0));
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_LegacyEndToEnd(benchmark::State &state) {
  const int variant = static_cast<int>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  Fixture fix;
  if (!fix.Init(variant)) {
    state.SkipWithError("legacy fixture build failed");
    return;
  }
  fix.Fill(batch, mix);

  std::array<const void *, kMaxBatch> bufs{};
  for (size_t i = 0; i < batch; i++) {
    bufs[i] = fix.packets[i].data();
  }
  std::array<ExactMatchKey, kMaxBatch> keys{};
  std::array<gate_idx_t, kMaxBatch> gates{};
  for (auto _ : state) {
    if (variant == 3) {
      fix.LegacyExtractMirror(keys.data(), batch);
    } else {
      fix.legacy.MakeKeys(bufs.data(), keys.data(), batch);
    }
    fix.legacy.Find(keys.data(), gates.data(), batch, 0);
    benchmark::DoNotOptimize(gates);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_NewEndToEnd(benchmark::State &state) {
  const int variant = static_cast<int>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  Fixture fix;
  if (!fix.Init(variant)) {
    state.SkipWithError("new-path fixture build failed");
    return;
  }
  fix.Fill(batch, mix);
  // All matrix variants are dense; assert the coverage contract the module
  // relies on instead of silently benchmarking a different regime.
  if (!fix.plan.fully_covers_key()) {
    state.SkipWithError("variant is not fully covered");
    return;
  }

  std::array<std::byte, kMaxBatch * kKeySize> keys{};
  std::array<gate_idx_t, kMaxBatch> gates{};
  const uint64_t all_valid = AllValidMask(batch);
  for (auto _ : state) {
    // Covered plan: no pre-zero; zero only invalid rows before lookup.
    const uint64_t valid = fix.plan.ExecuteBatch(
        std::span<const SourceView>(fix.views).first(batch),
        MutableBytes(keys).first(batch * kKeySize), kKeySize);
    if ((valid & all_valid) != all_valid) {
      for (size_t i = 0; i < batch; i++) {
        if (!(valid & (uint64_t{1} << i))) {
          std::memset(keys.data() + i * kKeySize, 0, kKeySize);
        }
      }
    }
    uint64_t hits = fix.backend.lookup_batch(
        ConstBytes(keys.data(), batch * kKeySize), kKeySize,
        std::span<gate_idx_t>(gates).first(batch));
    hits &= valid;
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(gates);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_RebuildCost(benchmark::State &state) {
  const size_t num_rules = static_cast<size_t>(state.range(0));
  std::vector<std::byte> storage(num_rules * kKeySize);
  for (size_t r = 0; r < num_rules; r++) {
    for (int f = 0; f < 2; f++) {
      const uint32_t v =
          static_cast<uint32_t>(r * 31 + f * 0x9E3779B9u);
      std::memcpy(storage.data() + r * kKeySize + f * kFieldBytes, &v,
                  kFieldBytes);
    }
  }
  std::vector<RuntimeExactRule<gate_idx_t>> rules;
  rules.reserve(num_rules);
  for (size_t r = 0; r < num_rules; r++) {
    rules.push_back(RuntimeExactRule<gate_idx_t>{
        .key = ConstBytes(storage.data() + r * kKeySize, kKeySize),
        .result = static_cast<gate_idx_t>(r % 2048),
    });
  }
  for (auto _ : state) {
    auto be = BuildRuntimeCuckooBackend<gate_idx_t>(kKeySize, rules);
    if (!be) {
      state.SkipWithError("backend rebuild failed (capacity?)");
      return;
    }
    benchmark::DoNotOptimize(be->info().rule_count);
  }
  state.SetItemsProcessed(state.iterations() * num_rules);
}

#define EM_MATRIX_ARGS(BM)                                                    \
  BM->Args({0, 8, 0})->Args({0, 8, 1})->Args({0, 8, 2})->Args({0, 1, 2})->     \
      Args({0, 16, 2})->Args({0, 32, 2})->Args({1, 8, 2})->Args({1, 32, 2})-> \
      Args({2, 8, 2})->Args({2, 32, 2})->Args({3, 8, 2})->Args({3, 32, 2})

EM_MATRIX_ARGS(BENCHMARK(BM_LegacyExtract));
EM_MATRIX_ARGS(BENCHMARK(BM_NewExtract));
EM_MATRIX_ARGS(BENCHMARK(BM_LegacyLookup));
EM_MATRIX_ARGS(BENCHMARK(BM_RuntimeDirectLookup));
EM_MATRIX_ARGS(BENCHMARK(BM_RuntimeMaterializedLookup));
EM_MATRIX_ARGS(BENCHMARK(BM_RuntimeBorrowedLookup));
EM_MATRIX_ARGS(BENCHMARK(BM_RuntimePrehashedLookup));
EM_MATRIX_ARGS(BENCHMARK(BM_RuntimeBatchLookup));
EM_MATRIX_ARGS(BENCHMARK(BM_RuntimeProbeStats));
EM_MATRIX_ARGS(BENCHMARK(BM_NewLookup));
EM_MATRIX_ARGS(BENCHMARK(BM_LegacyEndToEnd));
EM_MATRIX_ARGS(BENCHMARK(BM_NewEndToEnd));
BENCHMARK(BM_Zero8)->Arg(1)->Arg(8)->Arg(16)->Arg(32);
BENCHMARK(BM_HashFNV_Key)->Arg(1)->Arg(8)->Arg(32);
BENCHMARK(BM_HashCRC_Key)->Arg(1)->Arg(8)->Arg(32);
BENCHMARK(BM_HashCRC_FixedProbe)->Arg(1)->Arg(8)->Arg(32);
BENCHMARK(BM_HashCRC_Bytes)->Arg(1)->Arg(8)->Arg(32);
BENCHMARK(BM_HashLegacy_Chunk)->Arg(1)->Arg(8)->Arg(32);
BENCHMARK(BM_RebuildCost)->Arg(1000)->Arg(10000)->Arg(100000);

}  // namespace
