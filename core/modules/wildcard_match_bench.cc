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

// K3.6 module integration tax: the migrated WildcardMatch path
// (ExtractPlan kCheck + RuntimeMaskedBackend) against the mechanism the module
// used before the cutover, reimplemented here so no deleted module code is
// needed.
//
//   Legacy : per-field unaligned 8-byte packet loads into an 8-byte-word key,
//            then per-tuple CuckooMap keyed by the masked word key with the
//            legacy CRC32C-chained hash
//   Module : dense ExtractPlan key, then RuntimeMaskedBackend
//
// K3.5 already measured tuple-space scaling in isolation; this benchmark only
// measures what module integration adds. No backend-selection conclusions
// should be drawn from it.
//
// Args: {tuples, batch, dist, metadata} where
//   dist: 0 = single hit, 1 = multi-tuple hit, 2 = all miss
//   metadata: 0 = packet-only fields, 1 = one packet field + one metadata field
//
// Field layout mirrors the module test: one 4-byte packet field at IP src
// (offset 26) plus, for the mixed case, a 2-byte metadata field.

#include <benchmark/benchmark.h>

#include <rte_hash_crc.h>

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <span>
#include <vector>

#include "classifier/extract_plan.h"
#include "classifier/masked_exact.h"
#include "classifier/runtime_schema.h"
#include "module.h"
#include "utils/cuckoo_map.h"

namespace {

using bess::classifier::BoundsPolicy;
using bess::classifier::Byte;
using bess::classifier::ConstBytes;
using bess::classifier::ExtractPlan;
using bess::classifier::MutableBytes;
using bess::classifier::RuntimeClassifierSchema;
using bess::classifier::RuntimeKeyField;
using bess::classifier::RuntimeMaskedBackend;
using bess::classifier::RuntimeMaskedRule;
using bess::classifier::SourceKind;
using bess::classifier::SourceView;
using bess::utils::CuckooMap;
using bess::utils::HashResult;

constexpr size_t kMaxBatch = 32;
constexpr size_t kMaxTuples = 8;
constexpr size_t kRulesPerTuple = 8;
constexpr size_t kPacketBytes = 128;
constexpr size_t kMetadataBytes = 128;
constexpr size_t kWordBytes = 64;  // legacy wm_hkey_t storage
constexpr gate_idx_t kDefaultGate = 63;

// ---------------------------------------------------------------------------
// Workload
// ---------------------------------------------------------------------------

struct WmConfig {
  size_t tuples = 4;
  size_t batch = 8;
  int dist = 0;
  int metadata = 0;

  static WmConfig FromArgs(const benchmark::State &state) {
    return WmConfig{
        .tuples = static_cast<size_t>(state.range(0)),
        .batch = static_cast<size_t>(state.range(1)),
        .dist = static_cast<int>(state.range(2)),
        .metadata = static_cast<int>(state.range(3)),
    };
  }
};

uint64_t Probe(size_t a, size_t b, size_t salt) {
  uint64_t x = 0x9E3779B97F4A7C15ull * (a + 1) +
               0xBF58476D1CE4E5B9ull * (b + 1) +
               0x94D049BB133111EBull * (salt + 1);
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  return x;
}

uint8_t MaskByte(size_t tuple, size_t byte) {
  const uint64_t hash = Probe(tuple, byte, 7);
  uint8_t out = 0;
  for (int i = 0; i < 6; i++) {  // ~75% density
    out = static_cast<uint8_t>(out |
                               (1u << static_cast<unsigned>((hash >> (i * 5)) & 7u)));
  }
  return out;
}

struct WmWorkload {
  WmConfig config;
  size_t key_size = 0;             // dense key bytes
  std::vector<size_t> field_sizes; // per field
  std::vector<size_t> packet_offsets;
  std::vector<std::vector<Byte>> masks;       // [tuple][key_size]
  std::vector<std::vector<Byte>> values;      // [tuple * rules + rule]
  std::array<std::array<uint8_t, kPacketBytes>, kMaxBatch> packets{};
  std::array<std::array<uint8_t, kMetadataBytes>, kMaxBatch> metas{};
  std::array<SourceView, kMaxBatch> views{};
  double avg_tuple_matches = 0.0;

  std::vector<Byte> &Value(size_t tuple, size_t rule) {
    return values[tuple * kRulesPerTuple + rule];
  }
  const std::vector<Byte> &Value(size_t tuple, size_t rule) const {
    return values[tuple * kRulesPerTuple + rule];
  }

  bool Build() {
    if (config.tuples == 0 || config.tuples > kMaxTuples || config.batch == 0 ||
        config.batch > kMaxBatch) {
      return false;
    }
    field_sizes = {4};
    packet_offsets = {26};
    if (config.metadata) {
      field_sizes.push_back(2);
      packet_offsets.push_back(0);  // unused for metadata
    }
    // metadata == 2 is the control for metadata == 1: same field count and key
    // width, but the second field reads packet bytes instead of metadata, so a
    // difference isolates the metadata source from the extra field.
    const bool second_is_packet = config.metadata == 2;
    key_size = 0;
    for (size_t s : field_sizes) {
      key_size += s;
    }

    masks.resize(config.tuples);
    for (size_t t = 0; t < config.tuples; t++) {
      masks[t].resize(key_size);
      for (size_t b = 0; b < key_size; b++) {
        masks[t][b] = static_cast<Byte>(MaskByte(t, b));
      }
    }

    values.resize(config.tuples * kRulesPerTuple);
    for (size_t t = 0; t < config.tuples; t++) {
      for (size_t r = 0; r < kRulesPerTuple; r++) {
        auto &value = Value(t, r);
        value.resize(key_size);
        // dist 1 needs every tuple to hold the same probe so one key matches
        // them all; other distributions use tuple-private probes.
        const uint64_t probe =
            config.dist == 1 ? Probe(0, r, 1) : Probe(t, r, 0);
        for (size_t b = 0; b < key_size; b++) {
          const uint8_t word =
              static_cast<uint8_t>((probe >> (8 * (b % 8))) & 0xff);
          value[b] = static_cast<Byte>(
              word & std::to_integer<uint8_t>(masks[t][b]));
        }
      }
    }

    // Packets carry the field bytes at their configured offsets.
    std::vector<uint8_t> union_mask(key_size, 0);
    for (size_t t = 0; t < config.tuples; t++) {
      for (size_t b = 0; b < key_size; b++) {
        union_mask[b] = static_cast<uint8_t>(
            union_mask[b] | std::to_integer<uint8_t>(masks[t][b]));
      }
    }

    for (size_t i = 0; i < config.batch; i++) {
      const size_t rule = i % kRulesPerTuple;
      uint64_t probe = 0;
      if (config.dist == 0) {
        probe = Probe(0, rule, 0);
      } else if (config.dist == 1) {
        probe = Probe(0, rule, 1);
      } else {
        probe = 0xA5A5A5A5A5A5A5A5ull;
      }
      std::vector<uint8_t> key_bytes(key_size);
      for (size_t b = 0; b < key_size; b++) {
        uint8_t byte = static_cast<uint8_t>((probe >> (8 * (b % 8))) & 0xff);
        if (config.dist != 2) {
          // Only bits no tuple's mask covers may be randomized, so the key
          // still matches the tuples it is built from.
          const uint8_t wild = static_cast<uint8_t>(~union_mask[b]);
          byte = static_cast<uint8_t>(byte | (wild & 0x5a));
        }
        key_bytes[b] = byte;
      }
      packets[i].fill(0);
      metas[i].fill(0);
      // Packet field 0 is 4 bytes at offset 26; the second field follows.
      std::memcpy(packets[i].data() + 26, key_bytes.data(), 4);
      if (config.metadata == 1) {
        std::memcpy(metas[i].data() + 16, key_bytes.data() + 4, 2);
      } else if (second_is_packet) {
        std::memcpy(packets[i].data() + 40, key_bytes.data() + 4, 2);
      }
      views[i].packet = ConstBytes(
          reinterpret_cast<const Byte *>(packets[i].data()), kPacketBytes);
      views[i].metadata = ConstBytes(
          reinterpret_cast<const Byte *>(metas[i].data()), kMetadataBytes);
    }

    avg_tuple_matches = MeasureTupleMatches();
    return true;
  }

  double MeasureTupleMatches() const {
    size_t total = 0;
    for (size_t i = 0; i < config.batch; i++) {
      // Reconstruct the dense key the extraction would produce.
      std::vector<uint8_t> key(key_size);
      std::memcpy(key.data(), packets[i].data() + 26, 4);
      if (config.metadata) {
        std::memcpy(key.data() + 4, metas[i].data() + 16, 2);
      }
      for (size_t t = 0; t < config.tuples; t++) {
        bool matched = false;
        for (size_t r = 0; r < kRulesPerTuple && !matched; r++) {
          const auto &value = Value(t, r);
          bool equal = true;
          for (size_t b = 0; b < key_size && equal; b++) {
            equal = (key[b] & std::to_integer<uint8_t>(masks[t][b])) ==
                    std::to_integer<uint8_t>(value[b]);
          }
          matched = equal;
        }
        total += matched ? 1 : 0;
      }
    }
    return static_cast<double>(total) / static_cast<double>(config.batch);
  }

  RuntimeClassifierSchema Schema() const {
    RuntimeClassifierSchema schema;
    schema.key_size = key_size;
    schema.bounds = BoundsPolicy::kCheck;
    size_t pos = 0;
    for (size_t i = 0; i < field_sizes.size(); i++) {
      RuntimeKeyField field;
      field.key_offset = pos;
      field.size = field_sizes[i];
      if (i == 0) {
        field.source = SourceKind::kPacket;
        field.source_offset = 26;
      } else if (config.metadata == 1) {
        field.source = SourceKind::kMetadata;
        field.source_offset = 16;
      } else {
        field.source = SourceKind::kPacket;
        field.source_offset = 40;
      }
      schema.key_fields.push_back(std::move(field));
      pos += field_sizes[i];
    }
    return schema;
  }

  std::vector<RuntimeMaskedRule<gate_idx_t, int64_t>> Rules() const {
    std::vector<RuntimeMaskedRule<gate_idx_t, int64_t>> rules;
    rules.reserve(config.tuples * kRulesPerTuple);
    for (size_t t = 0; t < config.tuples; t++) {
      for (size_t r = 0; r < kRulesPerTuple; r++) {
        rules.push_back(RuntimeMaskedRule<gate_idx_t, int64_t>{
            .value = ConstBytes(Value(t, r)),
            .mask = ConstBytes(masks[t]),
            .priority = static_cast<int64_t>(r),
            .result = static_cast<gate_idx_t>(t + 1),
        });
      }
    }
    return rules;
  }
};

// ---------------------------------------------------------------------------
// Legacy mechanism: 8-byte-word key, per-tuple CuckooMap
// ---------------------------------------------------------------------------

struct WmWordKey {
  uint64_t words[kWordBytes / 8];
};

struct WmLegacyHash {
  size_t words = 1;
  HashResult operator()(const WmWordKey &key) const noexcept {
    HashResult hash = 0;
#if __x86_64
    for (size_t i = 0; i < words; i++) {
      hash = static_cast<HashResult>(crc32c_sse42_u64(key.words[i], hash));
    }
    return hash;
#else
    return static_cast<HashResult>(
        rte_hash_crc(&key, words * sizeof(uint64_t), hash));
#endif
  }
};

struct WmLegacyEqual {
  size_t words = 1;
  bool operator()(const WmWordKey &lhs, const WmWordKey &rhs) const noexcept {
    for (size_t i = 0; i < words; i++) {
      if (lhs.words[i] != rhs.words[i]) {
        return false;
      }
    }
    return true;
  }
};

struct WmLegacyTuple {
  WmWordKey mask{};
  CuckooMap<WmWordKey, gate_idx_t, WmLegacyHash, WmLegacyEqual> table;
};

void BM_Wm_Legacy(benchmark::State &state) {
  WmWorkload workload;
  workload.config = WmConfig::FromArgs(state);
  if (!workload.Build()) {
    state.SkipWithError("workload build failed");
    return;
  }
  const WmConfig &config = workload.config;
  const size_t total_bytes = workload.key_size;
  const size_t words = (total_bytes + 7) / 8;

  std::vector<WmLegacyTuple> tuples(config.tuples);
  for (size_t t = 0; t < config.tuples; t++) {
    std::memset(&tuples[t].mask, 0, sizeof(WmWordKey));
    std::memcpy(&tuples[t].mask, workload.masks[t].data(), total_bytes);
    for (size_t r = 0; r < kRulesPerTuple; r++) {
      WmWordKey key{};
      std::memcpy(&key, workload.Value(t, r).data(), total_bytes);
      if (tuples[t].table.Insert(key, static_cast<gate_idx_t>(t + 1),
                                 WmLegacyHash{words},
                                 WmLegacyEqual{words}) == nullptr) {
        state.SkipWithError("legacy table insertion failed");
        return;
      }
    }
  }

  std::array<gate_idx_t, kMaxBatch> gates{};
  for (auto _ : state) {
    for (size_t i = 0; i < config.batch; i++) {
      // Legacy key build: one unaligned 8-byte load per field, written at the
      // field's cumulative position.
      WmWordKey key{};
      std::memcpy(&key, workload.packets[i].data() + 26, 8);
      if (config.metadata) {
        std::memcpy(reinterpret_cast<uint8_t *>(&key) + 4,
                    workload.metas[i].data() + 16, 8);
      }
      gate_idx_t best = kDefaultGate;
      int64_t best_priority = INT64_MIN;
      for (const WmLegacyTuple &tuple : tuples) {
        WmWordKey masked{};
        for (size_t w = 0; w < words; w++) {
          masked.words[w] = key.words[w] & tuple.mask.words[w];
        }
        const auto *entry = tuple.table.Find(masked, WmLegacyHash{words},
                                             WmLegacyEqual{words});
        if (entry != nullptr) {
          // The legacy table stored {priority, ogate}; priorities here are the
          // per-tuple rule index, so recover it from the stored gate's tuple.
          const int64_t priority = static_cast<int64_t>(
              (static_cast<size_t>(entry->second) - 1) % kRulesPerTuple);
          if (priority >= best_priority) {
            best_priority = priority;
            best = entry->second;
          }
        }
      }
      gates[i] = best;
    }
    benchmark::DoNotOptimize(gates);
  }
  state.counters["avg_tuple_matches"] =
      benchmark::Counter(workload.avg_tuple_matches);
  state.SetItemsProcessed(state.iterations() * config.batch);
}

// ---------------------------------------------------------------------------
// Migrated module mechanism: ExtractPlan + RuntimeMaskedBackend
// ---------------------------------------------------------------------------

void BM_Wm_Module(benchmark::State &state) {
  WmWorkload workload;
  workload.config = WmConfig::FromArgs(state);
  if (!workload.Build()) {
    state.SkipWithError("workload build failed");
    return;
  }
  const WmConfig &config = workload.config;
  auto plan = ExtractPlan::Compile(workload.Schema());
  if (!plan) {
    state.SkipWithError("extraction plan compile failed");
    return;
  }
  const auto rules = workload.Rules();
  auto built =
      RuntimeMaskedBackend<gate_idx_t, int64_t>::Build(workload.key_size, rules);
  if (!built) {
    state.SkipWithError(built.error().message.c_str());
    return;
  }
  const auto &backend = *built;

  std::array<std::byte, kMaxBatch * 64> keys{};
  std::array<gate_idx_t, kMaxBatch> gates{};
  for (auto _ : state) {
    const uint64_t valid = plan->ExecuteBatch(
        std::span<const SourceView>(workload.views).first(config.batch),
        MutableBytes(keys).first(config.batch * workload.key_size),
        workload.key_size);
    const uint64_t all_valid = (uint64_t{1} << config.batch) - 1;
    if ((valid & all_valid) != all_valid) {
      for (size_t i = 0; i < config.batch; i++) {
        if (!(valid & (uint64_t{1} << i))) {
          std::memset(keys.data() + i * workload.key_size, 0,
                      workload.key_size);
        }
      }
    }
    uint64_t hits = backend.lookup_batch(
        ConstBytes(keys.data(), config.batch * workload.key_size),
        workload.key_size, std::span<gate_idx_t>(gates).first(config.batch));
    hits &= valid;
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(gates);
  }
  state.counters["tuples"] =
      benchmark::Counter(static_cast<double>(backend.tuple_count()));
  state.counters["avg_tuple_matches"] =
      benchmark::Counter(workload.avg_tuple_matches);
  state.SetItemsProcessed(state.iterations() * config.batch);
}

// ---------------------------------------------------------------------------
// Generation rebuild cost (control plane): plan compile + masked backend build
// ---------------------------------------------------------------------------

void BM_Wm_Rebuild(benchmark::State &state) {
  const size_t tuples = static_cast<size_t>(state.range(0));
  const size_t rules_per_tuple = static_cast<size_t>(state.range(1));
  WmWorkload workload;
  workload.config = WmConfig{.tuples = tuples, .batch = 8, .dist = 0,
                             .metadata = 1};
  if (!workload.Build()) {
    state.SkipWithError("workload build failed");
    return;
  }
  const size_t key_size = workload.key_size;
  std::vector<Byte> mask_storage(tuples * key_size);
  std::vector<Byte> value_storage(tuples * rules_per_tuple * key_size);
  std::vector<RuntimeMaskedRule<gate_idx_t, int64_t>> rules;
  rules.reserve(tuples * rules_per_tuple);
  for (size_t t = 0; t < tuples; t++) {
    std::memcpy(mask_storage.data() + t * key_size, workload.masks[t].data(),
                key_size);
    for (size_t r = 0; r < rules_per_tuple; r++) {
      std::byte *dst = value_storage.data() + (t * rules_per_tuple + r) * key_size;
      for (size_t b = 0; b < key_size; b++) {
        const uint8_t word =
            static_cast<uint8_t>((Probe(t, r, 0) >> (8 * (b % 8))) & 0xff);
        dst[b] = static_cast<Byte>(
            word & std::to_integer<uint8_t>(workload.masks[t][b]));
      }
      rules.push_back(RuntimeMaskedRule<gate_idx_t, int64_t>{
          .value = ConstBytes(dst, key_size),
          .mask = ConstBytes(mask_storage.data() + t * key_size, key_size),
          .priority = static_cast<int64_t>(r),
          .result = static_cast<gate_idx_t>(t + 1),
      });
    }
  }

  for (auto _ : state) {
    auto plan = ExtractPlan::Compile(workload.Schema());
    if (!plan) {
      state.SkipWithError("plan compile failed");
      return;
    }
    auto backend =
        RuntimeMaskedBackend<gate_idx_t, int64_t>::Build(key_size, rules);
    if (!backend) {
      state.SkipWithError("backend build failed");
      return;
    }
    benchmark::DoNotOptimize(plan->fully_covers_key());
    benchmark::DoNotOptimize(backend->tuple_count());
  }
  state.SetItemsProcessed(state.iterations() * rules.size());
}

using BenchFn = void (*)(benchmark::State &);

void RegisterWm(const char *name, BenchFn fn, const WmConfig &config) {
  benchmark::RegisterBenchmark(name, fn)
      ->Args({static_cast<int64_t>(config.tuples),
              static_cast<int64_t>(config.batch),
              static_cast<int64_t>(config.dist),
              static_cast<int64_t>(config.metadata)});
}

[[maybe_unused]] const bool kWmBenchmarksRegistered = [] {
  std::vector<WmConfig> points;
  auto add = [&points](WmConfig config) { points.push_back(config); };
  for (size_t tuples : {size_t{1}, size_t{2}, size_t{4}, size_t{8}}) {
    for (size_t batch : {size_t{1}, size_t{8}, size_t{16}, size_t{32}}) {
      for (int dist : {0, 1, 2}) {
        add(WmConfig{tuples, batch, dist, 0});
      }
    }
  }
  for (size_t tuples : {size_t{1}, size_t{4}, size_t{8}}) {
    for (size_t batch : {size_t{1}, size_t{32}}) {
      for (int dist : {0, 2}) {
        add(WmConfig{tuples, batch, dist, 1});
        add(WmConfig{tuples, batch, dist, 2});
      }
    }
  }
  for (const WmConfig &config : points) {
    RegisterWm("BM_Wm_Legacy", &BM_Wm_Legacy, config);
    RegisterWm("BM_Wm_Module", &BM_Wm_Module, config);
  }
  BENCHMARK(BM_Wm_Rebuild)
      ->Args({1, 8})
      ->Args({4, 8})
      ->Args({8, 8})
      ->Args({8, 64})
      ->Args({8, 256});
  return true;
}();

}  // namespace
