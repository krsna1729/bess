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

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "classifier/byte_key.h"
#include "classifier/cuckoo_exact.h"
#include "classifier/direct_exact.h"
#include "classifier/extract_plan.h"
#include "classifier/packed_value_store.h"
#include "classifier/result_plan.h"
#include "classifier/rte_hash_exact.h"
#include "classifier/small_exact.h"
#include "classifier/typed_exact.h"
#include "dpdk.h"
namespace {

using bess::classifier::Byte;
using bess::classifier::ByteKey;
using bess::classifier::ConstBytes;
using bess::classifier::ExactTable;
using bess::classifier::MutableBytes;
using bess::classifier::ResultPlan;
using bess::classifier::RuntimeClassifierSchema;
using bess::classifier::RuntimeKeyField;
using bess::classifier::RuntimeResultField;
using bess::classifier::SourceKind;
using bess::classifier::SourceView;

constexpr size_t kBatch = 32;

RuntimeClassifierSchema ExtractionSchema() {
  return RuntimeClassifierSchema{
      .key_size = 16,
      .key_fields = {{SourceKind::kPacket, 8, 0, 16}},
  };
}

RuntimeClassifierSchema ResultSchema() {
  return RuntimeClassifierSchema{
      .key_size = 1,
      .value_size = 16,
      .result_fields = {{0, 12, 16}},
  };
}

void BM_DirectExtract(benchmark::State &state) {
  const std::array<std::byte, 32> packet{};
  std::array<std::byte, kBatch * 16> output{};
  const size_t batch = static_cast<size_t>(state.range(0));
  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      std::memcpy(output.data() + i * 16, packet.data() + 8, 16);
    }
    benchmark::DoNotOptimize(output);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_CompiledExtract(benchmark::State &state) {
  const std::array<std::byte, 32> packet{};
  const std::array<SourceView, kBatch> sources = [&]() {
    std::array<SourceView, kBatch> value{};
    for (SourceView &source : value) {
      source.packet = ConstBytes(packet);
    }
    return value;
  }();
  std::array<std::byte, kBatch * 16> output{};
  const size_t batch = static_cast<size_t>(state.range(0));
  auto plan = bess::classifier::ExtractPlan::Compile(ExtractionSchema());
  if (!plan.has_value()) {
    state.SkipWithError("failed to compile extraction plan");
    return;
  }
  for (auto _ : state) {
    if (!plan->ExecuteBatch(
            std::span<const SourceView>(sources).first(batch),
            MutableBytes(output), 16)) {
      state.SkipWithError("extraction plan execution failed");
      return;
    }
    benchmark::DoNotOptimize(output);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_DirectPlace(benchmark::State &state) {
  const std::array<std::byte, 16> value{};
  std::array<std::byte, kBatch * 32> metadata{};
  const size_t batch = static_cast<size_t>(state.range(0));
  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      std::memcpy(metadata.data() + i * 32 + 12, value.data(), value.size());
    }
    benchmark::DoNotOptimize(metadata);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_CompiledPlace(benchmark::State &state) {
  const std::array<std::byte, 16> value{};
  const std::array<ConstBytes, kBatch> values = [&]() {
    std::array<ConstBytes, kBatch> result{};
    for (ConstBytes &entry : result) {
      entry = ConstBytes(value);
    }
    return result;
  }();
  std::array<std::byte, kBatch * 32> metadata{};
  std::array<MutableBytes, kBatch> destinations = [&]() {
    std::array<MutableBytes, kBatch> result{};
    for (size_t i = 0; i < kBatch; i++) {
      result[i] = MutableBytes(metadata).subspan(i * 32, 32);
    }
    return result;
  }();
  const size_t batch = static_cast<size_t>(state.range(0));
  auto plan = ResultPlan::Compile(ResultSchema());
  if (!plan.has_value()) {
    state.SkipWithError("failed to compile result plan");
    return;
  }
  for (auto _ : state) {
    if (!plan->ApplyBatch(
            std::span<const ConstBytes>(values).first(batch),
            std::span<MutableBytes>(destinations).first(batch))) {
      state.SkipWithError("result plan execution failed");
      return;
    }
    benchmark::DoNotOptimize(metadata);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

struct TypedBackend {
  uint32_t value = 7;

  const uint32_t *lookup(const ByteKey<16> &key) const noexcept {
    return key[0] == Byte{0} ? &value : nullptr;
  }

  size_t size() const noexcept { return 1; }
};

using TypedTable = ExactTable<ByteKey<16>, uint32_t, TypedBackend>;

const uint32_t *DirectTypedLookup(const ByteKey<16> &key,
                                  const uint32_t *value) noexcept {
  return key[0] == Byte{0} ? value : nullptr;
}

void BM_DirectTypedLookup(benchmark::State &state) {
  std::array<ByteKey<16>, kBatch> keys{};
  uint32_t value = 7;
  const size_t batch = static_cast<size_t>(state.range(0));
  uint32_t sink = 0;
  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      const uint32_t *result = DirectTypedLookup(keys[i], &value);
      sink ^= result == nullptr ? 0 : *result;
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_TypedTableLookup(benchmark::State &state) {
  std::array<ByteKey<16>, kBatch> keys{};
  const TypedTable table(TypedBackend{});
  const size_t batch = static_cast<size_t>(state.range(0));
  uint32_t sink = 0;
  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      const uint32_t *result = table.lookup(keys[i]);
      sink ^= result == nullptr ? 0 : *result;
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

BENCHMARK(BM_DirectExtract)->Arg(1)->Arg(8)->Arg(16)->Arg(32);
BENCHMARK(BM_CompiledExtract)->Arg(1)->Arg(8)->Arg(16)->Arg(32);
BENCHMARK(BM_DirectPlace)->Arg(1)->Arg(8)->Arg(16)->Arg(32);
BENCHMARK(BM_CompiledPlace)->Arg(1)->Arg(8)->Arg(16)->Arg(32);
BENCHMARK(BM_DirectTypedLookup)->Arg(1)->Arg(8)->Arg(16)->Arg(32);
BENCHMARK(BM_TypedTableLookup)->Arg(1)->Arg(8)->Arg(16)->Arg(32);


// ---------------------------------------------------------------------------
// K3.2 Backend Laboratory Benchmarks
// ---------------------------------------------------------------------------

using bess::classifier::CuckooExactBackend;
using bess::classifier::DirectExactBackend;
using bess::classifier::RteHashDataBackend;
using bess::classifier::RteHashPositionBackend;
using bess::classifier::SmallExactBackend;
using bess::classifier::SortedFlatBackend;

ByteKey<8> MakeKey8(uint64_t v) {
  ByteKey<8> k{};
  std::memcpy(k.data(), &v, 8);
  return k;
}

void BM_Cuckoo_TypedBatch_Hit(benchmark::State &state) {
  const size_t batch = static_cast<size_t>(state.range(0));
  CuckooExactBackend<ByteKey<8>, uint32_t> backend;
  for (uint64_t i = 0; i < 64; i++) {
    backend.insert(MakeKey8(i + 1), static_cast<uint32_t>(i + 1));
  }
  ExactTable<ByteKey<8>, uint32_t, CuckooExactBackend<ByteKey<8>, uint32_t>> table(
      std::move(backend));

  std::array<ByteKey<8>, kBatch> keys{};
  for (size_t i = 0; i < kBatch; i++) {
    keys[i] = MakeKey8((i % 64) + 1);
  }
  std::array<uint32_t, kBatch> results{};

  for (auto _ : state) {
    uint64_t hits = table.lookup_batch(
        std::span<const ByteKey<8>>(keys).first(batch),
        std::span<uint32_t>(results).first(batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_Small_TypedBatch_Hit(benchmark::State &state) {
  const size_t batch = static_cast<size_t>(state.range(0));
  SmallExactBackend<ByteKey<8>, uint32_t> backend;
  for (uint64_t i = 0; i < 32; i++) {
    backend.insert(MakeKey8(i + 1), static_cast<uint32_t>(i + 1));
  }
  ExactTable<ByteKey<8>, uint32_t, SmallExactBackend<ByteKey<8>, uint32_t>> table(
      std::move(backend));

  std::array<ByteKey<8>, kBatch> keys{};
  for (size_t i = 0; i < kBatch; i++) {
    keys[i] = MakeKey8((i % 32) + 1);
  }
  std::array<uint32_t, kBatch> results{};

  for (auto _ : state) {
    uint64_t hits = table.lookup_batch(
        std::span<const ByteKey<8>>(keys).first(batch),
        std::span<uint32_t>(results).first(batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_RteHashData_Bulk_Hit(benchmark::State &state) {
  const size_t batch = static_cast<size_t>(state.range(0));
  using gate_idx_t = uint16_t;
  RteHashDataBackend<gate_idx_t> backend(8, 64);
  if (!backend.valid()) {
    state.SkipWithError("RteHashDataBackend creation failed");
    return;
  }
  std::array<ByteKey<8>, 64> rule_keys{};
  for (uint64_t i = 0; i < 64; i++) {
    rule_keys[i] = MakeKey8(i + 1);
    backend.add_key_data(rule_keys[i], static_cast<gate_idx_t>(i + 1));
  }

  std::array<ConstBytes, kBatch> keys{};
  for (size_t i = 0; i < kBatch; i++) {
    keys[i] = rule_keys[i % 64];
  }
  std::array<gate_idx_t, kBatch> results{};

  for (auto _ : state) {
    uint64_t hits = backend.lookup_batch(
        std::span<const ConstBytes>(keys).first(batch),
        std::span<gate_idx_t>(results).first(batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_RteHashPosition_Bulk_Hit(benchmark::State &state) {
  const size_t batch = static_cast<size_t>(state.range(0));
  RteHashPositionBackend backend(8, 64);
  if (!backend.valid()) {
    state.SkipWithError("RteHashPositionBackend creation failed");
    return;
  }
  std::array<ByteKey<8>, 64> rule_keys{};
  for (uint64_t i = 0; i < 64; i++) {
    rule_keys[i] = MakeKey8(i + 1);
    backend.add_key(rule_keys[i]);
  }

  std::array<ConstBytes, kBatch> keys{};
  for (size_t i = 0; i < kBatch; i++) {
    keys[i] = rule_keys[i % 64];
  }
  std::array<int32_t, kBatch> results{};

  for (auto _ : state) {
    uint64_t hits = backend.lookup_batch(
        std::span<const ConstBytes>(keys).first(batch),
        std::span<int32_t>(results).first(batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_DirectExact_Bulk_Hit(benchmark::State &state) {
  const size_t batch = static_cast<size_t>(state.range(0));
  DirectExactBackend<uint8_t, uint16_t> backend;
  for (uint8_t i = 0; i < 64; i++) {
    backend.insert(i, static_cast<uint16_t>(i + 1));
  }

  std::array<uint8_t, kBatch> keys{};
  for (size_t i = 0; i < kBatch; i++) {
    keys[i] = static_cast<uint8_t>(i % 64);
  }
  std::array<uint16_t, kBatch> results{};

  for (auto _ : state) {
    uint64_t hits = backend.lookup_batch(
        std::span<const uint8_t>(keys).first(batch),
        std::span<uint16_t>(results).first(batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

BENCHMARK(BM_Cuckoo_TypedBatch_Hit)->Arg(1)->Arg(8)->Arg(16)->Arg(32);
BENCHMARK(BM_Small_TypedBatch_Hit)->Arg(1)->Arg(8)->Arg(16)->Arg(32);
BENCHMARK(BM_RteHashData_Bulk_Hit)->Arg(1)->Arg(8)->Arg(16)->Arg(32);
BENCHMARK(BM_RteHashPosition_Bulk_Hit)->Arg(1)->Arg(8)->Arg(16)->Arg(32);
BENCHMARK(BM_DirectExact_Bulk_Hit)->Arg(1)->Arg(8)->Arg(16)->Arg(32);
}  // namespace
