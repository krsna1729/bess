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

// K3.5 masked/tuple-space substrate benchmark. Three rungs over one logical
// workload, plus two isolation measurements:
//
//   Naive    : linear scan over every rule, mask-and-compare, best rank wins
//   Legacy   : per-tuple CuckooMap keyed by 8-byte-word keys, CRC32C-chained
//              over round_up(key_bytes, 8) words — the mechanism the existing
//              WildcardMatch module uses, reimplemented here so no module code
//              is involved
//   Substrate: RuntimeMaskedBackend
//   MaskOnly : masking a batch once, no lookup
//   ExactOnly: one full-mask tuple, no masking (the exact-lookup floor)
//
// Args: {key_bytes, tuples, rules_per_tuple, mask_density, diverse, batch, dist}
//   mask_density: 100 / 75 / 50 (% of mask bits set, via 0xff / 0x77 / 0x0f)
//   diverse: 0 = every tuple shares one mask, 1 = a distinct rotated mask per
//            tuple (at density 100 a rotation is the identity, so diverse and
//            identical coincide there by construction)
//   dist: 0 = matches one tuple, 1 = matches every tuple, 2 = all miss
//
// Masks are arbitrary bit patterns, never prefixes, and each configuration
// reports the measured average tuple-match count so the distribution claim is
// checked rather than assumed.
//
// The Legacy rung keeps the first matching tuple on an equal priority (the
// module's ties are undefined); the Substrate rung resolves equal priorities by
// rule ordinal. The rungs therefore agree on hits but not necessarily on which
// equal-priority rule wins.

#include <benchmark/benchmark.h>

#include <rte_hash_crc.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "classifier/masked_exact.h"
#include "utils/cuckoo_map.h"

namespace {

using bess::classifier::Byte;
using bess::classifier::ConstBytes;
using bess::classifier::RuntimeMaskedBackend;
using bess::classifier::RuntimeMaskedRule;
using bess::utils::CuckooMap;
using bess::utils::HashResult;

using Result = uint32_t;

struct Ranked {
  int64_t priority;
  uint64_t ordinal;
  Result result;
};

using Masked = RuntimeMaskedBackend<Result>;

constexpr size_t kMaxBatch = 32;
constexpr size_t kMaxKeyBytes = 64;
constexpr size_t kMaxTuples = 32;
constexpr size_t kMaxRulesPerTuple = 64;
// The legacy mechanism stores 8-byte-word keys, exactly like the module's
// wm_hkey_t (MAX_FIELDS * MAX_FIELD_SIZE).
constexpr size_t kLegacyWords = 8;

// ---------------------------------------------------------------------------
// Workload generation
// ---------------------------------------------------------------------------

struct MaskedConfig {
  size_t key_bytes = 8;
  size_t tuples = 4;
  size_t rules_per_tuple = 16;
  int density = 100;
  int diverse = 0;
  size_t batch = 8;
  int dist = 0;

  static MaskedConfig FromArgs(const benchmark::State &state) {
    return MaskedConfig{
        .key_bytes = static_cast<size_t>(state.range(0)),
        .tuples = static_cast<size_t>(state.range(1)),
        .rules_per_tuple = static_cast<size_t>(state.range(2)),
        .density = static_cast<int>(state.range(3)),
        .diverse = static_cast<int>(state.range(4)),
        .batch = static_cast<size_t>(state.range(5)),
        .dist = static_cast<int>(state.range(6)),
    };
  }
};

// Deterministic per-(tuple, rule) probe word, before masking.
uint64_t ProbeWord(size_t tuple, size_t rule, size_t salt) {
  uint64_t x = 0x9E3779B97F4A7C15ull * (tuple + 1) +
               0xBF58476D1CE4E5B9ull * (rule + 1) +
               0x94D049BB133111EBull * (salt + 1);
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  return x;
}

int DensityBits(int density) {
  switch (density) {
    case 50:
      return 4;
    case 75:
      return 6;
    default:
      return 8;
  }
}

// Deterministic per-(tuple, byte) bit pattern carrying the requested set-bit
// count. Distinct (tuple, byte) pairs give distinct patterns, so the tuple
// count is not capped by a rotation period, and the chosen bit positions are
// arbitrary rather than a prefix.
uint8_t MaskByte(size_t tuple, size_t byte, int density) {
  const uint64_t hash = ProbeWord(tuple, byte, 7);
  const int want = DensityBits(density);
  uint8_t out = 0;
  for (int i = 0; i < want; i++) {
    const unsigned bit = static_cast<unsigned>((hash >> (i * 5)) & 0x7u);
    out = static_cast<uint8_t>(out | (1u << bit));
  }
  return out;
}

struct MaskedWorkload {
  MaskedConfig config;
  std::vector<std::vector<Byte>> tuple_masks;   // [tuple][key_bytes]
  std::vector<std::vector<Byte>> rule_values;   // [tuple * rules + rule]
  std::vector<Byte> packed_keys;                // batch * key_bytes
  double avg_tuple_matches = 0.0;

  std::vector<Byte> &RuleValue(size_t tuple, size_t rule) {
    return rule_values[tuple * config.rules_per_tuple + rule];
  }

  const std::vector<Byte> &RuleValue(size_t tuple, size_t rule) const {
    return rule_values[tuple * config.rules_per_tuple + rule];
  }

  bool Build() {
    if (config.key_bytes == 0 || config.key_bytes > kMaxKeyBytes ||
        config.tuples == 0 || config.tuples > kMaxTuples ||
        config.rules_per_tuple == 0 ||
        config.rules_per_tuple > kMaxRulesPerTuple ||
        config.batch == 0 || config.batch > kMaxBatch) {
      return false;
    }
    tuple_masks.resize(config.tuples);
    for (size_t t = 0; t < config.tuples; t++) {
      tuple_masks[t].resize(config.key_bytes);
      for (size_t b = 0; b < config.key_bytes; b++) {
        // Identical masks share the pattern of tuple 0; diverse masks get a
        // distinct pattern per tuple. At density 100 both are all-ones, which
        // is why the substrate collapses them into one tuple.
        const uint8_t byte = config.diverse ? MaskByte(t, b, config.density)
                                            : MaskByte(0, b, config.density);
        tuple_masks[t][b] = static_cast<Byte>(byte);
      }
    }

    rule_values.resize(config.tuples * config.rules_per_tuple);
    for (size_t t = 0; t < config.tuples; t++) {
      for (size_t r = 0; r < config.rules_per_tuple; r++) {
        auto &value = RuleValue(t, r);
        value.resize(config.key_bytes);
        // dist 1 needs every tuple to hold the same probe so one key can match
        // them all; the other distributions use tuple-private probes.
        const uint64_t probe = config.dist == 1
                                   ? ProbeWord(0, r, 1)
                                   : ProbeWord(t, r, 0);
        for (size_t b = 0; b < config.key_bytes; b++) {
          const uint8_t word_byte =
              static_cast<uint8_t>((probe >> (8 * (b % 8))) & 0xff);
          value[b] = static_cast<Byte>(
              word_byte & std::to_integer<uint8_t>(tuple_masks[t][b]));
        }
      }
    }

    // Query keys. Bits no tuple's mask covers are randomized, so a key is never
    // the masked value itself while still matching the tuples it is built from.
    std::vector<uint8_t> union_mask(config.key_bytes, 0);
    for (size_t t = 0; t < config.tuples; t++) {
      for (size_t b = 0; b < config.key_bytes; b++) {
        union_mask[b] = static_cast<uint8_t>(
            union_mask[b] | std::to_integer<uint8_t>(tuple_masks[t][b]));
      }
    }

    packed_keys.assign(config.batch * config.key_bytes, Byte{0});
    for (size_t i = 0; i < config.batch; i++) {
      const size_t rule = i % config.rules_per_tuple;
      uint64_t probe = 0;
      if (config.dist == 0) {
        probe = ProbeWord(0, rule, 0);
      } else if (config.dist == 1) {
        probe = ProbeWord(0, rule, 1);
      } else {
        probe = 0xA5A5A5A5A5A5A5A5ull;
      }
      // dist 1 must match every tuple, so only bits outside the union of all
      // masks may be randomized; dist 0 only has to match tuple 0.
      for (size_t b = 0; b < config.key_bytes; b++) {
        uint8_t word_byte =
            static_cast<uint8_t>((probe >> (8 * (b % 8))) & 0xff);
        if (config.dist != 2) {
          const uint8_t covered = config.dist == 1
                                      ? union_mask[b]
                                      : std::to_integer<uint8_t>(
                                            tuple_masks[0][b]);
          const uint8_t wild = static_cast<uint8_t>(~covered);
          word_byte = static_cast<uint8_t>(word_byte | (wild & 0x5a));
        }
        packed_keys[i * config.key_bytes + b] = static_cast<Byte>(word_byte);
      }
    }

    avg_tuple_matches = MeasureTupleMatches();
    return true;
  }

  // How many tuples each query key actually matches. Reported as a counter so
  // the distribution labels are verified against the data.
  double MeasureTupleMatches() const {
    size_t total = 0;
    for (size_t i = 0; i < config.batch; i++) {
      const std::byte *key = packed_keys.data() + i * config.key_bytes;
      for (size_t t = 0; t < config.tuples; t++) {
        bool matched = false;
        for (size_t r = 0; r < config.rules_per_tuple && !matched; r++) {
          const auto &value = RuleValue(t, r);
          bool equal = true;
          for (size_t b = 0; b < config.key_bytes && equal; b++) {
            const uint8_t masked =
                std::to_integer<uint8_t>(key[b]) &
                std::to_integer<uint8_t>(tuple_masks[t][b]);
            equal = masked == std::to_integer<uint8_t>(value[b]);
          }
          matched = equal;
        }
        total += matched ? 1 : 0;
      }
    }
    return static_cast<double>(total) / static_cast<double>(config.batch);
  }

  std::vector<RuntimeMaskedRule<Result>> Rules() const {
    std::vector<RuntimeMaskedRule<Result>> rules;
    rules.reserve(config.tuples * config.rules_per_tuple);
    for (size_t t = 0; t < config.tuples; t++) {
      for (size_t r = 0; r < config.rules_per_tuple; r++) {
        rules.push_back(RuntimeMaskedRule<Result>{
            .value = ConstBytes(RuleValue(t, r)),
            .mask = ConstBytes(tuple_masks[t]),
            .priority = static_cast<int64_t>(r),
            .result = static_cast<Result>(t * 1000 + r),
        });
      }
    }
    return rules;
  }
};

// ---------------------------------------------------------------------------
// Rung 1: naive linear rule scan
// ---------------------------------------------------------------------------

void BM_Masked_Naive(benchmark::State &state) {
  MaskedWorkload workload;
  workload.config = MaskedConfig::FromArgs(state);
  if (!workload.Build()) {
    state.SkipWithError("workload build failed");
    return;
  }
  const auto rules = workload.Rules();
  const MaskedConfig &config = workload.config;
  std::array<Result, kMaxBatch> results{};

  for (auto _ : state) {
    uint64_t hits = 0;
    for (size_t i = 0; i < config.batch; i++) {
      const std::byte *key = workload.packed_keys.data() + i * config.key_bytes;
      const RuntimeMaskedRule<Result> *best = nullptr;
      for (const auto &rule : rules) {
        bool equal = true;
        for (size_t b = 0; b < config.key_bytes && equal; b++) {
          const uint8_t masked =
              std::to_integer<uint8_t>(key[b]) &
              std::to_integer<uint8_t>(rule.mask[b]);
          equal = masked == std::to_integer<uint8_t>(rule.value[b]);
        }
        if (equal && (best == nullptr || rule.priority > best->priority)) {
          best = &rule;
        }
      }
      if (best != nullptr) {
        results[i] = best->result;
        hits |= (uint64_t{1} << i);
      }
    }
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.counters["avg_tuple_matches"] =
      benchmark::Counter(workload.avg_tuple_matches);
  state.SetItemsProcessed(state.iterations() * config.batch);
}

// ---------------------------------------------------------------------------
// Rung 2: legacy tuple-space (per-tuple CuckooMap over 8-byte-word keys)
// ---------------------------------------------------------------------------

struct LegacyKey {
  uint64_t words[kLegacyWords];
};

struct LegacyHash {
  size_t words = 1;

  HashResult operator()(const LegacyKey &key) const noexcept {
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

struct LegacyEqual {
  size_t words = 1;

  bool operator()(const LegacyKey &lhs, const LegacyKey &rhs) const noexcept {
    for (size_t i = 0; i < words; i++) {
      if (lhs.words[i] != rhs.words[i]) {
        return false;
      }
    }
    return true;
  }
};

struct LegacyTuple {
  LegacyKey mask{};
  CuckooMap<LegacyKey, Ranked, LegacyHash, LegacyEqual> table;
};

// Copy the leading key bytes into the 8-byte-word key shape, zero-padding the
// tail exactly like the module's key construction.
void LegacyFromBytes(ConstBytes key, size_t words, LegacyKey &out) {
  std::memset(&out, 0, sizeof(out));
  std::memcpy(&out, key.data(), key.size());
  (void)words;
}

// Masked 8-byte-word load, matching the module's packet path shape: the key is
// copied into a word array and masked there.
void LegacyMaskKey(ConstBytes key, const LegacyKey &mask, size_t words,
                   LegacyKey &out) {
  LegacyFromBytes(key, words, out);
  for (size_t i = 0; i < words; i++) {
    out.words[i] &= mask.words[i];
  }
}

void BM_Masked_Legacy(benchmark::State &state) {
  MaskedWorkload workload;
  workload.config = MaskedConfig::FromArgs(state);
  if (!workload.Build()) {
    state.SkipWithError("workload build failed");
    return;
  }
  const MaskedConfig &config = workload.config;
  const size_t words = (config.key_bytes + 7) / 8;

  std::vector<LegacyTuple> tuples(config.tuples);
  for (size_t t = 0; t < config.tuples; t++) {
    LegacyFromBytes(ConstBytes(workload.tuple_masks[t]), words, tuples[t].mask);
    for (size_t r = 0; r < config.rules_per_tuple; r++) {
      LegacyKey key{};
      LegacyFromBytes(ConstBytes(workload.RuleValue(t, r)), words, key);
      const Ranked ranked{.priority = static_cast<int64_t>(r),
                          .ordinal = r,
                          .result = static_cast<Result>(t * 1000 + r)};
      if (tuples[t].table.Insert(key, ranked, LegacyHash{words},
                                 LegacyEqual{words}) == nullptr) {
        state.SkipWithError("legacy table insertion failed");
        return;
      }
    }
  }

  std::array<Result, kMaxBatch> results{};
  std::array<Ranked, kMaxBatch> best{};
  for (auto _ : state) {
    uint64_t matched = 0;
    for (size_t i = 0; i < config.batch; i++) {
      const ConstBytes key(workload.packed_keys.data() + i * config.key_bytes,
                           config.key_bytes);
      for (const LegacyTuple &tuple : tuples) {
        LegacyKey masked{};
        LegacyMaskKey(key, tuple.mask, words, masked);
        const auto *entry =
            tuple.table.Find(masked, LegacyHash{words}, LegacyEqual{words});
        if (entry == nullptr) {
          continue;
        }
        const uint64_t bit = uint64_t{1} << i;
        if ((matched & bit) == 0 || entry->second.priority > best[i].priority) {
          best[i] = entry->second;
          results[i] = entry->second.result;
          matched |= bit;
        }
      }
    }
    benchmark::DoNotOptimize(matched);
    benchmark::DoNotOptimize(results);
  }
  state.counters["avg_tuple_matches"] =
      benchmark::Counter(workload.avg_tuple_matches);
  state.SetItemsProcessed(state.iterations() * config.batch);
}

// ---------------------------------------------------------------------------
// Rung 3: RuntimeMaskedBackend
// ---------------------------------------------------------------------------

void BM_Masked_Substrate(benchmark::State &state) {
  MaskedWorkload workload;
  workload.config = MaskedConfig::FromArgs(state);
  if (!workload.Build()) {
    state.SkipWithError("workload build failed");
    return;
  }
  const MaskedConfig &config = workload.config;
  const auto rules = workload.Rules();
  auto built = Masked::Build(config.key_bytes, rules);
  if (!built) {
    state.SkipWithError(built.error().message.c_str());
    return;
  }
  const Masked &backend = *built;

  std::array<Result, kMaxBatch> results{};
  for (auto _ : state) {
    uint64_t hits = backend.lookup_batch(
        ConstBytes(workload.packed_keys.data(),
                   config.batch * config.key_bytes),
        config.key_bytes, std::span<Result>(results).first(config.batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.counters["tuples"] = benchmark::Counter(
      static_cast<double>(backend.tuple_count()));
  state.counters["avg_tuple_matches"] =
      benchmark::Counter(workload.avg_tuple_matches);
  state.SetItemsProcessed(state.iterations() * config.batch);
}

// ---------------------------------------------------------------------------
// Isolation: masking cost and exact-lookup cost inside the substrate
// ---------------------------------------------------------------------------

void BM_Masked_MaskOnly(benchmark::State &state) {
  MaskedWorkload workload;
  workload.config = MaskedConfig::FromArgs(state);
  if (!workload.Build()) {
    state.SkipWithError("workload build failed");
    return;
  }
  const MaskedConfig &config = workload.config;
  const auto &mask = workload.tuple_masks[0];
  std::array<Byte, kMaxBatch * kMaxKeyBytes> scratch{};

  for (auto _ : state) {
    for (size_t i = 0; i < config.batch; i++) {
      const std::byte *src =
          workload.packed_keys.data() + i * config.key_bytes;
      std::byte *dst = scratch.data() + i * config.key_bytes;
      for (size_t b = 0; b < config.key_bytes; b++) {
        dst[b] = src[b] & mask[b];
      }
    }
    benchmark::DoNotOptimize(scratch);
  }
  state.SetItemsProcessed(state.iterations() * config.batch);
}

// Isolation: the exact lookup the substrate pays once per tuple, on keys that
// were already masked at setup. BM_Masked_Substrate at one tuple should sit
// close to BM_Masked_MaskOnly plus this number.
void BM_Masked_ExactOnly(benchmark::State &state) {
  MaskedWorkload workload;
  workload.config = MaskedConfig::FromArgs(state);
  if (!workload.Build()) {
    state.SkipWithError("workload build failed");
    return;
  }
  const MaskedConfig &config = workload.config;
  const auto &mask = workload.tuple_masks[0];

  std::vector<Byte> masked_keys(config.batch * config.key_bytes);
  for (size_t i = 0; i < config.batch; i++) {
    for (size_t b = 0; b < config.key_bytes; b++) {
      masked_keys[i * config.key_bytes + b] =
          workload.packed_keys[i * config.key_bytes + b] & mask[b];
    }
  }

  std::vector<bess::classifier::RuntimeExactRule<Ranked>> exact_rules;
  exact_rules.reserve(config.rules_per_tuple);
  for (size_t r = 0; r < config.rules_per_tuple; r++) {
    exact_rules.push_back(bess::classifier::RuntimeExactRule<Ranked>{
        .key = ConstBytes(workload.RuleValue(0, r)),
        .result = Ranked{.priority = static_cast<int64_t>(r),
                         .ordinal = r,
                         .result = static_cast<Result>(r)},
    });
  }
  auto built = bess::classifier::BuildRuntimeCuckooBackend<Ranked>(
      config.key_bytes, exact_rules);
  if (!built) {
    state.SkipWithError(built.error().message.c_str());
    return;
  }
  auto backend = std::move(*built);
  std::array<Ranked, kMaxBatch> candidates{};

  for (auto _ : state) {
    uint64_t hits = backend.lookup_batch(
        ConstBytes(masked_keys.data(), config.batch * config.key_bytes),
        config.key_bytes, std::span<Ranked>(candidates).first(config.batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(candidates);
  }
  state.SetItemsProcessed(state.iterations() * config.batch);
}

// ---------------------------------------------------------------------------
// Registration: one factor at a time around a base configuration
// ---------------------------------------------------------------------------

struct SweepArgs {
  size_t key_bytes;
  size_t tuples;
  size_t rules;
  int density;
  int diverse;
  size_t batch;
  int dist;
};

using BenchFn = void (*)(benchmark::State &);

void RegisterMasked(const char *name, BenchFn fn, const SweepArgs &args) {
  benchmark::RegisterBenchmark(name, fn)
      ->Args({static_cast<int64_t>(args.key_bytes),
              static_cast<int64_t>(args.tuples),
              static_cast<int64_t>(args.rules),
              static_cast<int64_t>(args.density),
              static_cast<int64_t>(args.diverse),
              static_cast<int64_t>(args.batch),
              static_cast<int64_t>(args.dist)});
}

[[maybe_unused]] const bool kMaskedBenchmarksRegistered = [] {
  // Density 75 with diverse masks: distinct masks per tuple, so the tuple-count
  // axis actually varies the tuple space. At density 100 every mask is all-ones
  // and the substrate collapses the tuples by construction, which the density
  // sweep reports rather than hides.
  const SweepArgs base{.key_bytes = 8,
                       .tuples = 4,
                       .rules = 16,
                       .density = 75,
                       .diverse = 1,
                       .batch = 8,
                       .dist = 0};

  std::vector<SweepArgs> points;
  auto add = [&points](SweepArgs args) { points.push_back(args); };

  // Key width.
  for (size_t key_bytes : {size_t{4}, size_t{8}, size_t{16}, size_t{32},
                           size_t{64}}) {
    add(SweepArgs{key_bytes, base.tuples, base.rules, base.density,
                  base.diverse, base.batch, base.dist});
  }
  // Tuple count.
  for (size_t tuples : {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{16},
                        size_t{32}}) {
    add(SweepArgs{base.key_bytes, tuples, base.rules, base.density,
                  base.diverse, base.batch, base.dist});
  }
  // Rules per tuple.
  for (size_t rules : {size_t{4}, size_t{16}, size_t{64}}) {
    add(SweepArgs{base.key_bytes, base.tuples, rules, base.density,
                  base.diverse, base.batch, base.dist});
  }
  // Mask density and identical-vs-diverse masks.
  for (int density : {50, 75, 100}) {
    for (int diverse : {0, 1}) {
      add(SweepArgs{base.key_bytes, base.tuples, base.rules, density, diverse,
                    base.batch, base.dist});
    }
  }
  // Batch.
  for (size_t batch : {size_t{1}, size_t{8}, size_t{16}, size_t{32}}) {
    add(SweepArgs{base.key_bytes, base.tuples, base.rules, base.density,
                  base.diverse, batch, base.dist});
  }
  // Hit distribution.
  for (int dist : {0, 1, 2}) {
    add(SweepArgs{base.key_bytes, base.tuples, base.rules, base.density,
                  base.diverse, base.batch, dist});
  }
  // Combined points: wide keys with many tuples, and a dense multi-match case.
  add(SweepArgs{32, 16, 16, 50, 1, 32, 1});
  add(SweepArgs{64, 8, 64, 75, 1, 16, 2});
  add(SweepArgs{4, 32, 4, 100, 0, 32, 0});

  for (const SweepArgs &args : points) {
    RegisterMasked("BM_Masked_Naive", &BM_Masked_Naive, args);
    RegisterMasked("BM_Masked_Legacy", &BM_Masked_Legacy, args);
    RegisterMasked("BM_Masked_Substrate", &BM_Masked_Substrate, args);
    RegisterMasked("BM_Masked_MaskOnly", &BM_Masked_MaskOnly, args);
    RegisterMasked("BM_Masked_ExactOnly", &BM_Masked_ExactOnly, args);
  }
  return true;
}();

}  // namespace
