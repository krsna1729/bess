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

// K3.3 end-to-end migration benchmark: the complete legacy ExactMatch path
// (MakeKeys from raw buffers + CuckooMap lookup) against the complete new
// path (ExtractPlan::ExecuteBatch + RuntimeExactBackend<gate_idx_t>). Both
// sides match the same rules over the same buffers; traffic mixes (hit /
// miss / mixed) and batch sizes are parameters. A second family measures
// generation rebuild (BuildRuntimeCuckooBackend) cost at 1K/10K/100K rules.
//
// NOTE: fixed 4-byte packet fields, default (all-ones) masks. This pins the
// migration penalty/benefit for the existing generic user; it is not a
// backend-selection experiment (see the K3.2 smoke-baseline disclaimer in
// classifier_bench.cc).

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

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

constexpr size_t kMaxBatch = 32;
constexpr size_t kFieldBytes = 4;

// Test fixture: `num_fields` 4-byte packet fields at consecutive offsets,
// `num_rules` rules, all-ones masks. Buffers hold one full key each.
struct Fixture {
  size_t num_fields = 0;
  size_t key_size = 0;
  ExactMatchTable<gate_idx_t> legacy;
  ExtractPlan plan = *ExtractPlan::Compile(
      RuntimeClassifierSchema{.key_size = kFieldBytes,
                              .bounds = BoundsPolicy::kCheck,
                              .key_fields = std::vector<RuntimeKeyField>{
                                  {SourceKind::kPacket, 0, 0, kFieldBytes}}});
  bess::classifier::RuntimeExactBackend<gate_idx_t> backend;
  // One buffer per batch slot; buffer i holds key bytes for lookup i.
  std::array<std::vector<uint8_t>, kMaxBatch> buffers{};
  std::array<SourceView, kMaxBatch> views{};

  bool Init(size_t in_fields, size_t num_rules) {
    num_fields = in_fields;
    key_size = num_fields * kFieldBytes;
    for (size_t i = 0; i < num_fields; i++) {
      if (legacy
              .AddField(static_cast<int>(i * kFieldBytes),
                        static_cast<int>(kFieldBytes), 0, static_cast<int>(i))
              .first != 0) {
        return false;
      }
    }

    RuntimeClassifierSchema schema;
    schema.key_size = key_size;
    schema.bounds = BoundsPolicy::kCheck;
    for (size_t i = 0; i < num_fields; i++) {
      schema.key_fields.push_back(RuntimeKeyField{
          .source = SourceKind::kPacket,
          .source_offset = i * kFieldBytes,
          .key_offset = i * kFieldBytes,
          .size = kFieldBytes,
      });
    }
    auto compiled = ExtractPlan::Compile(schema);
    if (!compiled) {
      return false;
    }
    plan = std::move(*compiled);

    std::vector<std::byte> storage(num_rules * key_size);
    std::vector<RuntimeExactRule<gate_idx_t>> backend_rules;
    backend_rules.reserve(num_rules);
    for (size_t r = 0; r < num_rules; r++) {
      ExactMatchRuleFields legacy_rule;
      legacy_rule.reserve(num_fields);
      for (size_t f = 0; f < num_fields; f++) {
        // Rule bytes are the little-endian encoding of (r * 31 + f).
        const uint32_t v =
            static_cast<uint32_t>(r * 31 + f * 0x9E3779B9u);
        std::vector<uint8_t> field_bytes(kFieldBytes);
        std::memcpy(field_bytes.data(), &v, kFieldBytes);
        std::memcpy(storage.data() + r * key_size + f * kFieldBytes,
                    field_bytes.data(), kFieldBytes);
        legacy_rule.push_back(std::move(field_bytes));
      }
      backend_rules.push_back(RuntimeExactRule<gate_idx_t>{
          .key = ConstBytes(storage.data() + r * key_size, key_size),
          .result = static_cast<gate_idx_t>(r % 2048),
      });
      if (legacy.AddRule(static_cast<gate_idx_t>(r % 2048), legacy_rule)
              .first != 0) {
        return false;
      }
    }
    auto be = BuildRuntimeCuckooBackend<gate_idx_t>(key_size, backend_rules);
    if (!be) {
      return false;
    }
    backend = std::move(*be);
    return true;
  }

  // Fill batch buffers: hit traffic replays rule keys, miss traffic uses
  // bytes that match no rule, mixed alternates.
  void Fill(size_t batch, int mix) {
    for (size_t i = 0; i < batch; i++) {
      buffers[i].assign(key_size + 8, 0);
      uint8_t *dst = buffers[i].data();
      const bool hit = mix == 0 || (mix == 2 && (i % 2 == 0));
      if (hit) {
        const uint32_t r = static_cast<uint32_t>((i * 17) % 256);
        for (size_t f = 0; f < num_fields; f++) {
          const uint32_t v = r * 31 + static_cast<uint32_t>(f * 0x9E3779B9u);
          std::memcpy(dst + f * kFieldBytes, &v, kFieldBytes);
        }
      } else {
        std::memset(dst, 0xA5, key_size);
      }
      views[i].packet =
          ConstBytes(reinterpret_cast<const Byte *>(dst), key_size + 8);
      views[i].metadata = ConstBytes{};
    }
  }
};

// mix: 0 = all-hit, 1 = all-miss, 2 = alternating.
void BM_LegacyMakeKeysFind(benchmark::State &state) {
  const size_t num_fields = static_cast<size_t>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  Fixture fix;
  if (!fix.Init(num_fields, 256)) {
    state.SkipWithError("legacy fixture build failed");
    return;
  }
  fix.Fill(batch, mix);

  std::array<const void *, kMaxBatch> bufs{};
  for (size_t i = 0; i < batch; i++) {
    bufs[i] = fix.buffers[i].data();
  }
  std::array<ExactMatchKey, kMaxBatch> keys{};
  std::array<gate_idx_t, kMaxBatch> gates{};
  for (auto _ : state) {
    fix.legacy.MakeKeys(bufs.data(), keys.data(), batch);
    fix.legacy.Find(keys.data(), gates.data(), batch, 0);
    benchmark::DoNotOptimize(gates);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_NewExtractBackend(benchmark::State &state) {
  const size_t num_fields = static_cast<size_t>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int mix = static_cast<int>(state.range(2));
  Fixture fix;
  if (!fix.Init(num_fields, 256)) {
    state.SkipWithError("new-path fixture build failed");
    return;
  }
  fix.Fill(batch, mix);

  std::array<std::byte, kMaxBatch * 8 * 4> keys{};
  std::array<gate_idx_t, kMaxBatch> gates{};
  for (auto _ : state) {
    std::memset(keys.data(), 0, batch * fix.key_size);
    const uint64_t valid = fix.plan.ExecuteBatch(
        std::span<const SourceView>(fix.views).first(batch),
        MutableBytes(keys).first(batch * fix.key_size), fix.key_size);
    uint64_t hits = fix.backend.lookup_batch(
        ConstBytes(keys.data(), batch * fix.key_size), fix.key_size,
        std::span<gate_idx_t>(gates).first(batch));
    hits &= valid;
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(gates);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_RebuildCost(benchmark::State &state) {
  const size_t num_rules = static_cast<size_t>(state.range(0));
  const size_t num_fields = 2;
  const size_t key_size = num_fields * kFieldBytes;
  std::vector<std::byte> storage(num_rules * key_size);
  for (size_t r = 0; r < num_rules; r++) {
    for (size_t f = 0; f < num_fields; f++) {
      const uint32_t v =
          static_cast<uint32_t>(r * 31 + f * 0x9E3779B9u);
      std::memcpy(storage.data() + r * key_size + f * kFieldBytes, &v,
                  kFieldBytes);
    }
  }
  std::vector<RuntimeExactRule<gate_idx_t>> rules;
  rules.reserve(num_rules);
  for (size_t r = 0; r < num_rules; r++) {
    rules.push_back(RuntimeExactRule<gate_idx_t>{
        .key = ConstBytes(storage.data() + r * key_size, key_size),
        .result = static_cast<gate_idx_t>(r % 2048),
    });
  }
  for (auto _ : state) {
    auto be = BuildRuntimeCuckooBackend<gate_idx_t>(key_size, rules);
    if (!be) {
      state.SkipWithError("backend rebuild failed (capacity?)");
      return;
    }
    benchmark::DoNotOptimize(be->info().rule_count);
  }
  state.SetItemsProcessed(state.iterations() * num_rules);
}

#define EM_BENCH_ARGS(BM)                                               \
  BM->Args({1, 8, 0})->Args({1, 8, 1})->Args({1, 8, 2})->Args({2, 8, 0})-> \
      Args({2, 8, 1})->Args({2, 8, 2})->Args({4, 8, 0})->Args({4, 8, 1})-> \
      Args({4, 8, 2})->Args({8, 8, 0})->Args({8, 8, 1})->Args({8, 8, 2})-> \
      Args({2, 1, 2})->Args({2, 16, 2})->Args({2, 32, 2})

EM_BENCH_ARGS(BENCHMARK(BM_LegacyMakeKeysFind));
EM_BENCH_ARGS(BENCHMARK(BM_NewExtractBackend));
BENCHMARK(BM_RebuildCost)->Arg(1000)->Arg(10000)->Arg(100000);

}  // namespace
