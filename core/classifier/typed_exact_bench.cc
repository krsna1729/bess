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

// K3.4 typed-author benchmark: three rungs over the same logical workload.
//
//   Direct  : author-written loop calling backend.lookup() per packet
//   Table   : the same backend behind ExactTable<Key, Result, Backend>
//   Runtime : the runtime-generic equivalent (packed keys, ExtractPlan where a
//             packet is parsed, RuntimeExactBackend)
//
// Direct and Table must agree; they differ only by the table wrapper. Runtime
// is not expected to match: it measures the cost of runtime genericity. No
// universal winner is declared from these numbers.
//
// Both sides hash with DPDK CRC32C, so a rung difference is framework cost and
// not a hash-choice artifact.
//
// Workloads are generic, not UPF-shaped: byte keys of 4/8/16/32 bytes, a
// scalar uint16_t domain for Direct/Small (the only domains where those
// backends are semantically valid), a natural 13-byte flow struct parsed from
// packet bytes, results of 2/4/8/16/32 bytes, rule counts 4/16/64/1K/10K/100K,
// batch 1/8/16/32, and hit/miss/alternating/hot-four distributions.

#include <benchmark/benchmark.h>

#include <rte_hash_crc.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "classifier/cuckoo_exact.h"
#include "classifier/direct_exact.h"
#include "classifier/extract_plan.h"
#include "classifier/runtime_schema.h"
#include "classifier/small_exact.h"
#include "classifier/typed_exact.h"

namespace {

using bess::classifier::BoundsPolicy;
using bess::classifier::BuildRuntimeCuckooBackend;
using bess::classifier::Byte;
using bess::classifier::ConstBytes;
using bess::classifier::CuckooExactBackend;
using bess::classifier::DirectExactBackend;
using bess::classifier::ExactTable;
using bess::classifier::ExtractPlan;
using bess::classifier::MutableBytes;
using bess::classifier::RuntimeClassifierSchema;
using bess::classifier::RuntimeExactBackend;
using bess::classifier::RuntimeExactRule;
using bess::classifier::RuntimeKeyField;
using bess::classifier::RuntimeResultField;
using bess::classifier::SmallExactBackend;
using bess::classifier::SourceKind;
using bess::classifier::SourceView;

constexpr size_t kK34Batch = 32;

// K3.4 intentionally uses a typed key with explicit operations rather than a
// KeyTraits registration. This is the author-facing case that must not be
// forced through ByteKey or a runtime schema.
template <size_t KeyBytes>
struct K34ByteKey {
  std::array<std::byte, KeyBytes> bytes{};

  friend bool operator==(const K34ByteKey &, const K34ByteKey &) = default;
};

// Both sides of every rung comparison hash with DPDK CRC32C: the runtime
// backend uses it internally, and these author hashes call the same
// primitives. That keeps the three-rung comparison about framework cost
// rather than about which side got the faster hash.
template <size_t KeyBytes>
struct K34ByteHash {
  size_t operator()(const K34ByteKey<KeyBytes> &key) const noexcept {
    if constexpr (KeyBytes == 1) {
      return rte_hash_crc_1byte(std::to_integer<uint8_t>(key.bytes[0]), 0);
    } else if constexpr (KeyBytes == 2) {
      uint16_t value;
      std::memcpy(&value, key.bytes.data(), sizeof(value));
      return rte_hash_crc_2byte(value, 0);
    } else if constexpr (KeyBytes == 4) {
      uint32_t value;
      std::memcpy(&value, key.bytes.data(), sizeof(value));
      return rte_hash_crc_4byte(value, 0);
    } else if constexpr (KeyBytes == 8) {
      uint64_t value;
      std::memcpy(&value, key.bytes.data(), sizeof(value));
      return rte_hash_crc_8byte(value, 0);
    } else {
      return static_cast<size_t>(rte_hash_crc(key.bytes.data(), KeyBytes, 0));
    }
  }
};

template <size_t KeyBytes>
struct K34ByteEqual {
  bool operator()(const K34ByteKey<KeyBytes> &lhs,
                  const K34ByteKey<KeyBytes> &rhs) const noexcept {
    return lhs == rhs;
  }
};

template <size_t ResultBytes>
struct K34Result {
  std::array<std::byte, ResultBytes> bytes{};
};

template <size_t KeyBytes, size_t ResultBytes>
using K34Cuckoo =
    CuckooExactBackend<K34ByteKey<KeyBytes>, K34Result<ResultBytes>,
                       K34ByteHash<KeyBytes>, K34ByteEqual<KeyBytes>>;

template <size_t KeyBytes, size_t ResultBytes>
using K34Small =
    SmallExactBackend<K34ByteKey<KeyBytes>, K34Result<ResultBytes>,
                      K34ByteEqual<KeyBytes>>;

template <size_t Bytes>
K34ByteKey<Bytes> MakeK34Key(size_t rule) {
  K34ByteKey<Bytes> key;
  const uint64_t index = rule;
  for (size_t i = 0; i < Bytes; i++) {
    // The low bytes carry the rule index verbatim, so keys stay distinct for
    // every rule count; higher bytes still mix the index so wide keys exercise
    // all their bytes.
    const uint8_t value =
        i < sizeof(index)
            ? static_cast<uint8_t>((index >> (i * 8)) & 0xff)
            : static_cast<uint8_t>((index * 131 + i * 17 + 3) & 0xff);
    key.bytes[i] = static_cast<std::byte>(value);
  }
  return key;
}

template <size_t Bytes>
K34ByteKey<Bytes> MakeK34MissKey() {
  K34ByteKey<Bytes> key;
  key.bytes.fill(static_cast<std::byte>(0xa5));
  return key;
}

template <size_t Bytes>
K34Result<Bytes> MakeK34Result(size_t rule) {
  K34Result<Bytes> result;
  for (size_t i = 0; i < Bytes; i++) {
    result.bytes[i] =
        static_cast<std::byte>((rule * 29 + i * 7 + 11) & 0xff);
  }
  return result;
}

struct K34InputChoice {
  bool hit;
  size_t rule;
};

K34InputChoice SelectK34Input(size_t index, size_t rules, int distribution) {
  switch (distribution) {
    case 0:  // all hit
      return {true, index % rules};
    case 1:  // all miss
      return {false, 0};
    case 2:  // alternating hit/miss
      return {index % 2 == 0, index % rules};
    case 3:  // hot four-rule working set
      return {true, index % std::min<size_t>(rules, 4)};
    default:
      return {false, 0};
  }
}

template <size_t KeyBytes>
void FillK34ByteKeys(size_t rules, size_t batch, int distribution,
                     std::array<K34ByteKey<KeyBytes>, kK34Batch> &keys) {
  for (size_t i = 0; i < batch; i++) {
    const K34InputChoice choice = SelectK34Input(i, rules, distribution);
    keys[i] = choice.hit ? MakeK34Key<KeyBytes>(choice.rule)
                         : MakeK34MissKey<KeyBytes>();
  }
}

template <size_t KeyBytes, size_t ResultBytes, typename Backend>
std::optional<Backend> BuildK34ByteBackend(size_t rules) {
  Backend backend;
  for (size_t rule = 0; rule < rules; rule++) {
    if (!backend.insert(MakeK34Key<KeyBytes>(rule),
                        MakeK34Result<ResultBytes>(rule))) {
      return std::nullopt;
    }
  }
  return backend;
}

template <size_t KeyBytes, size_t ResultBytes>
std::optional<RuntimeExactBackend<K34Result<ResultBytes>>>
BuildK34ByteRuntimeBackend(size_t rules) {
  using Result = K34Result<ResultBytes>;
  std::vector<std::byte> storage(rules * KeyBytes);
  std::vector<RuntimeExactRule<Result>> runtime_rules;
  runtime_rules.reserve(rules);
  for (size_t rule = 0; rule < rules; rule++) {
    const auto key = MakeK34Key<KeyBytes>(rule);
    std::memcpy(storage.data() + rule * KeyBytes, key.bytes.data(), KeyBytes);
    runtime_rules.push_back(RuntimeExactRule<Result>{
        .key = ConstBytes(storage.data() + rule * KeyBytes, KeyBytes),
        .result = MakeK34Result<ResultBytes>(rule),
    });
  }
  auto backend = BuildRuntimeCuckooBackend<Result>(KeyBytes, runtime_rules);
  if (!backend) {
    return std::nullopt;
  }
  return std::move(*backend);
}

template <size_t KeyBytes, size_t ResultBytes, typename Backend>
void BM_K34ByteDirect(benchmark::State &state) {
  using Key = K34ByteKey<KeyBytes>;
  using Result = K34Result<ResultBytes>;
  const size_t rules = static_cast<size_t>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int distribution = static_cast<int>(state.range(2));

  auto backend = BuildK34ByteBackend<KeyBytes, ResultBytes, Backend>(rules);
  if (!backend) {
    state.SkipWithError("typed backend construction failed");
    return;
  }
  std::array<Key, kK34Batch> keys{};
  FillK34ByteKeys(rules, batch, distribution, keys);
  std::array<Result, kK34Batch> results{};

  for (auto _ : state) {
    uint64_t hits = 0;
    for (size_t i = 0; i < batch; i++) {
      const Result *result = backend->lookup(keys[i]);
      if (result != nullptr) {
        results[i] = *result;
        hits |= (uint64_t{1} << i);
      }
    }
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

template <size_t KeyBytes, size_t ResultBytes, typename Backend>
void BM_K34ByteTable(benchmark::State &state) {
  using Key = K34ByteKey<KeyBytes>;
  using Result = K34Result<ResultBytes>;
  using Table = ExactTable<Key, Result, Backend>;
  const size_t rules = static_cast<size_t>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int distribution = static_cast<int>(state.range(2));

  auto backend = BuildK34ByteBackend<KeyBytes, ResultBytes, Backend>(rules);
  if (!backend) {
    state.SkipWithError("typed backend construction failed");
    return;
  }
  Table table{std::move(*backend)};
  std::array<Key, kK34Batch> keys{};
  FillK34ByteKeys(rules, batch, distribution, keys);
  std::array<Result, kK34Batch> results{};

  for (auto _ : state) {
    uint64_t hits = table.lookup_batch(
        std::span<const Key>(keys).first(batch),
        std::span<Result>(results).first(batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

template <size_t KeyBytes, size_t ResultBytes>
void BM_K34ByteRuntime(benchmark::State &state) {
  using Key = K34ByteKey<KeyBytes>;
  using Result = K34Result<ResultBytes>;
  const size_t rules = static_cast<size_t>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int distribution = static_cast<int>(state.range(2));

  auto backend = BuildK34ByteRuntimeBackend<KeyBytes, ResultBytes>(rules);
  if (!backend) {
    state.SkipWithError("runtime backend construction failed");
    return;
  }
  std::array<Key, kK34Batch> typed_keys{};
  FillK34ByteKeys(rules, batch, distribution, typed_keys);
  std::array<std::byte, kK34Batch * KeyBytes> packed_keys{};
  for (size_t i = 0; i < batch; i++) {
    std::memcpy(packed_keys.data() + i * KeyBytes, typed_keys[i].bytes.data(),
                KeyBytes);
  }
  std::array<Result, kK34Batch> results{};

  for (auto _ : state) {
    uint64_t hits = backend->lookup_batch(
        ConstBytes(packed_keys.data(), batch * KeyBytes), KeyBytes,
        std::span<Result>(results).first(batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

// Scalar integer key domain. DirectExactBackend is semantically valid here;
// its uint16_t domain supports every registered rule count except 100K.
template <size_t ResultBytes>
using K34ScalarResult = K34Result<ResultBytes>;

template <size_t ResultBytes, typename Backend>
std::optional<Backend> BuildK34ScalarBackend(size_t rules) {
  Backend backend;
  for (size_t rule = 0; rule < rules; rule++) {
    const uint16_t key = static_cast<uint16_t>(rule * 3 + 1);
    if (!backend.insert(key, MakeK34Result<ResultBytes>(rule))) {
      return std::nullopt;
    }
  }
  return backend;
}

uint16_t MakeK34ScalarKey(size_t rule) {
  return static_cast<uint16_t>(rule * 3 + 1);
}

template <size_t ResultBytes>
void FillK34ScalarKeys(size_t rules, size_t batch, int distribution,
                       std::array<uint16_t, kK34Batch> &keys) {
  for (size_t i = 0; i < batch; i++) {
    const K34InputChoice choice = SelectK34Input(i, rules, distribution);
    keys[i] = choice.hit ? MakeK34ScalarKey(choice.rule) : uint16_t{0xffff};
  }
}

template <size_t ResultBytes>
void BM_K34ScalarDirect(benchmark::State &state) {
  using Result = K34ScalarResult<ResultBytes>;
  using Backend = DirectExactBackend<uint16_t, Result>;
  const size_t rules = static_cast<size_t>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int distribution = static_cast<int>(state.range(2));

  auto backend = BuildK34ScalarBackend<ResultBytes, Backend>(rules);
  if (!backend) {
    state.SkipWithError("direct backend construction failed");
    return;
  }
  std::array<uint16_t, kK34Batch> keys{};
  FillK34ScalarKeys<ResultBytes>(rules, batch, distribution, keys);
  std::array<Result, kK34Batch> results{};

  for (auto _ : state) {
    uint64_t hits = 0;
    for (size_t i = 0; i < batch; i++) {
      const Result *result = backend->lookup(keys[i]);
      if (result != nullptr) {
        results[i] = *result;
        hits |= (uint64_t{1} << i);
      }
    }
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

template <size_t ResultBytes>
void BM_K34ScalarTable(benchmark::State &state) {
  using Key = uint16_t;
  using Result = K34ScalarResult<ResultBytes>;
  using Backend = DirectExactBackend<Key, Result>;
  using Table = ExactTable<Key, Result, Backend>;
  const size_t rules = static_cast<size_t>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int distribution = static_cast<int>(state.range(2));

  auto backend = BuildK34ScalarBackend<ResultBytes, Backend>(rules);
  if (!backend) {
    state.SkipWithError("direct backend construction failed");
    return;
  }
  Table table{std::move(*backend)};
  std::array<Key, kK34Batch> keys{};
  FillK34ScalarKeys<ResultBytes>(rules, batch, distribution, keys);
  std::array<Result, kK34Batch> results{};

  for (auto _ : state) {
    uint64_t hits = table.lookup_batch(
        std::span<const Key>(keys).first(batch),
        std::span<Result>(results).first(batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

template <size_t ResultBytes>
void BM_K34ScalarRuntime(benchmark::State &state) {
  using Result = K34ScalarResult<ResultBytes>;
  const size_t rules = static_cast<size_t>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int distribution = static_cast<int>(state.range(2));

  std::vector<std::byte> storage(rules * sizeof(uint16_t));
  std::vector<RuntimeExactRule<Result>> runtime_rules;
  runtime_rules.reserve(rules);
  for (size_t rule = 0; rule < rules; rule++) {
    const uint16_t key = MakeK34ScalarKey(rule);
    std::memcpy(storage.data() + rule * sizeof(key), &key, sizeof(key));
    runtime_rules.push_back(RuntimeExactRule<Result>{
        .key = ConstBytes(storage.data() + rule * sizeof(key), sizeof(key)),
        .result = MakeK34Result<ResultBytes>(rule),
    });
  }
  auto built = BuildRuntimeCuckooBackend<Result>(sizeof(uint16_t), runtime_rules);
  if (!built) {
    state.SkipWithError("runtime backend construction failed");
    return;
  }
  auto backend = std::move(*built);

  std::array<uint16_t, kK34Batch> typed_keys{};
  FillK34ScalarKeys<ResultBytes>(rules, batch, distribution, typed_keys);
  std::array<std::byte, kK34Batch * sizeof(uint16_t)> packed_keys{};
  for (size_t i = 0; i < batch; i++) {
    std::memcpy(packed_keys.data() + i * sizeof(uint16_t), &typed_keys[i],
                sizeof(uint16_t));
  }
  std::array<Result, kK34Batch> results{};

  for (auto _ : state) {
    uint64_t hits = backend.lookup_batch(
        ConstBytes(packed_keys.data(), batch * sizeof(uint16_t)),
        sizeof(uint16_t), std::span<Result>(results).first(batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

// Natural structured key with an authored packet parser. The runtime side
// extracts the same five fields into canonical bytes; neither side imitates
// the other's key representation.
struct K34FlowKey {
  uint32_t src;
  uint32_t dst;
  uint16_t sport;
  uint16_t dport;
  uint8_t proto;

  friend bool operator==(const K34FlowKey &, const K34FlowKey &) = default;
};

// Field-wise CRC32C over the natural struct: no serialization, no padding
// bytes hashed, and the same primitive family the runtime backend uses.
struct K34FlowHash {
  size_t operator()(const K34FlowKey &key) const noexcept {
    uint32_t crc = rte_hash_crc_4byte(key.src, 0);
    crc = rte_hash_crc_4byte(key.dst, crc);
    crc = rte_hash_crc_2byte(key.sport, crc);
    crc = rte_hash_crc_2byte(key.dport, crc);
    crc = rte_hash_crc_1byte(key.proto, crc);
    return static_cast<size_t>(crc);
  }
};

struct K34FlowEqual {
  bool operator()(const K34FlowKey &lhs,
                  const K34FlowKey &rhs) const noexcept {
    return lhs == rhs;
  }
};

using K34FlowResult = K34Result<8>;
using K34FlowBackend =
    CuckooExactBackend<K34FlowKey, K34FlowResult, K34FlowHash, K34FlowEqual>;

K34FlowKey MakeK34FlowKey(size_t rule) {
  return K34FlowKey{
      .src = 0x0a000001u + static_cast<uint32_t>(rule),
      .dst = 0x0a010001u + static_cast<uint32_t>(rule * 3),
      .sport = static_cast<uint16_t>(1000 + rule),
      .dport = static_cast<uint16_t>(2000 + rule * 2),
      .proto = static_cast<uint8_t>(rule % 3 + 6),
  };
}

void EncodeK34FlowKey(const K34FlowKey &key, std::span<std::byte> output) {
  std::memcpy(output.data() + 0, &key.src, sizeof(key.src));
  std::memcpy(output.data() + 4, &key.dst, sizeof(key.dst));
  std::memcpy(output.data() + 8, &key.sport, sizeof(key.sport));
  std::memcpy(output.data() + 10, &key.dport, sizeof(key.dport));
  output[12] = static_cast<std::byte>(key.proto);
}

K34FlowKey ParseK34FlowPacket(const std::array<std::byte, 32> &packet) {
  K34FlowKey key{};
  std::memcpy(&key.src, packet.data() + 0, sizeof(key.src));
  std::memcpy(&key.dst, packet.data() + 4, sizeof(key.dst));
  std::memcpy(&key.sport, packet.data() + 8, sizeof(key.sport));
  std::memcpy(&key.dport, packet.data() + 10, sizeof(key.dport));
  key.proto = std::to_integer<uint8_t>(packet[12]);
  return key;
}

struct K34FlowInputs {
  std::array<std::array<std::byte, 32>, kK34Batch> packets{};
  std::array<SourceView, kK34Batch> sources{};

  void Fill(size_t rules, size_t batch, int distribution) {
    for (size_t i = 0; i < batch; i++) {
      const K34InputChoice choice = SelectK34Input(i, rules, distribution);
      if (choice.hit) {
        EncodeK34FlowKey(MakeK34FlowKey(choice.rule), packets[i]);
      } else {
        packets[i].fill(static_cast<std::byte>(0xa5));
      }
      sources[i].packet = ConstBytes(packets[i]);
    }
  }
};

RuntimeClassifierSchema K34FlowSchema() {
  return RuntimeClassifierSchema{
      .key_size = 13,
      .bounds = BoundsPolicy::kAssumeAvailable,
      .key_fields = {
          RuntimeKeyField{SourceKind::kPacket, 0, 0, 4},
          RuntimeKeyField{SourceKind::kPacket, 4, 4, 4},
          RuntimeKeyField{SourceKind::kPacket, 8, 8, 2},
          RuntimeKeyField{SourceKind::kPacket, 10, 10, 2},
          RuntimeKeyField{SourceKind::kPacket, 12, 12, 1},
      },
  };
}

std::optional<K34FlowBackend> BuildK34FlowBackend(size_t rules) {
  K34FlowBackend backend;
  for (size_t rule = 0; rule < rules; rule++) {
    if (!backend.insert(MakeK34FlowKey(rule), MakeK34Result<8>(rule))) {
      return std::nullopt;
    }
  }
  return backend;
}

std::optional<RuntimeExactBackend<K34FlowResult>> BuildK34FlowRuntimeBackend(
    size_t rules) {
  constexpr size_t kKeyBytes = 13;
  std::vector<std::byte> storage(rules * kKeyBytes);
  std::vector<RuntimeExactRule<K34FlowResult>> runtime_rules;
  runtime_rules.reserve(rules);
  for (size_t rule = 0; rule < rules; rule++) {
    std::span<std::byte, kKeyBytes> key_bytes(
        storage.data() + rule * kKeyBytes, kKeyBytes);
    EncodeK34FlowKey(MakeK34FlowKey(rule), key_bytes);
    runtime_rules.push_back(RuntimeExactRule<K34FlowResult>{
        .key = ConstBytes(key_bytes.data(), kKeyBytes),
        .result = MakeK34Result<8>(rule),
    });
  }
  auto backend = BuildRuntimeCuckooBackend<K34FlowResult>(kKeyBytes,
                                                            runtime_rules);
  if (!backend) {
    return std::nullopt;
  }
  return std::move(*backend);
}

void BM_K34FlowDirect(benchmark::State &state) {
  constexpr size_t kRules = 64;
  const size_t batch = static_cast<size_t>(state.range(0));
  const int distribution = static_cast<int>(state.range(1));
  auto backend = BuildK34FlowBackend(kRules);
  if (!backend) {
    state.SkipWithError("typed flow backend construction failed");
    return;
  }
  K34FlowInputs inputs;
  inputs.Fill(kRules, batch, distribution);
  std::array<K34FlowResult, kK34Batch> results{};

  for (auto _ : state) {
    uint64_t hits = 0;
    for (size_t i = 0; i < batch; i++) {
      const K34FlowKey key = ParseK34FlowPacket(inputs.packets[i]);
      const K34FlowResult *result = backend->lookup(key);
      if (result != nullptr) {
        results[i] = *result;
        hits |= (uint64_t{1} << i);
      }
    }
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_K34FlowTable(benchmark::State &state) {
  constexpr size_t kRules = 64;
  const size_t batch = static_cast<size_t>(state.range(0));
  const int distribution = static_cast<int>(state.range(1));
  auto backend = BuildK34FlowBackend(kRules);
  if (!backend) {
    state.SkipWithError("typed flow backend construction failed");
    return;
  }
  ExactTable<K34FlowKey, K34FlowResult, K34FlowBackend> table{
      std::move(*backend)};
  K34FlowInputs inputs;
  inputs.Fill(kRules, batch, distribution);
  std::array<K34FlowKey, kK34Batch> keys{};
  std::array<K34FlowResult, kK34Batch> results{};

  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      keys[i] = ParseK34FlowPacket(inputs.packets[i]);
    }
    uint64_t hits = table.lookup_batch(
        std::span<const K34FlowKey>(keys).first(batch),
        std::span<K34FlowResult>(results).first(batch));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

void BM_K34FlowRuntime(benchmark::State &state) {
  constexpr size_t kRules = 64;
  constexpr size_t kKeyBytes = 13;
  const size_t batch = static_cast<size_t>(state.range(0));
  const int distribution = static_cast<int>(state.range(1));
  auto backend = BuildK34FlowRuntimeBackend(kRules);
  if (!backend) {
    state.SkipWithError("runtime flow backend construction failed");
    return;
  }
  auto plan = ExtractPlan::Compile(K34FlowSchema());
  if (!plan) {
    state.SkipWithError("flow extraction plan construction failed");
    return;
  }
  K34FlowInputs inputs;
  inputs.Fill(kRules, batch, distribution);
  std::array<std::byte, kK34Batch * kKeyBytes> packed_keys{};
  std::array<K34FlowResult, kK34Batch> results{};

  for (auto _ : state) {
    uint64_t valid = plan->ExecuteBatch(
        std::span<const SourceView>(inputs.sources).first(batch),
        MutableBytes(packed_keys).first(batch * kKeyBytes), kKeyBytes);
    uint64_t hits = backend->lookup_batch(
        ConstBytes(packed_keys.data(), batch * kKeyBytes), kKeyBytes,
        std::span<K34FlowResult>(results).first(batch));
    benchmark::DoNotOptimize(valid);
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

using BenchFn = void (*)(benchmark::State &);

std::string K34Name(const char *family, const char *rung, size_t key_bytes,
                    size_t result_bytes) {
  return std::string("BM_K34_") + family + "_" + rung + "_K" +
         std::to_string(key_bytes) + "_R" + std::to_string(result_bytes);
}

template <size_t KeyBytes, size_t ResultBytes>
void RegisterK34BytePair(const char *family, BenchFn direct, BenchFn table,
                         BenchFn runtime,
                         std::initializer_list<size_t> rule_counts) {
  for (size_t rules : rule_counts) {
    for (size_t batch : {size_t{1}, size_t{8}, size_t{16}, size_t{32}}) {
      for (int distribution = 0; distribution < 4; distribution++) {
        const auto args =
            {static_cast<int64_t>(rules), static_cast<int64_t>(batch),
             static_cast<int64_t>(distribution)};
        const std::string direct_name =
            K34Name(family, "Direct", KeyBytes, ResultBytes);
        const std::string table_name =
            K34Name(family, "Table", KeyBytes, ResultBytes);
        const std::string runtime_name =
            K34Name(family, "Runtime", KeyBytes, ResultBytes);
        benchmark::RegisterBenchmark(direct_name.c_str(), direct)->Args(args);
        benchmark::RegisterBenchmark(table_name.c_str(), table)->Args(args);
        benchmark::RegisterBenchmark(runtime_name.c_str(), runtime)->Args(args);
      }
    }
  }
}

template <size_t ResultBytes>
void RegisterK34ScalarPair() {
  for (size_t rules : {size_t{4}, size_t{16}, size_t{64}, size_t{1024},
                        size_t{10000}}) {
    for (size_t batch : {size_t{1}, size_t{8}, size_t{16}, size_t{32}}) {
      for (int distribution = 0; distribution < 4; distribution++) {
        const auto args =
            {static_cast<int64_t>(rules), static_cast<int64_t>(batch),
             static_cast<int64_t>(distribution)};
        const std::string direct_name =
            std::string("BM_K34_Scalar_Direct_R") +
            std::to_string(ResultBytes);
        const std::string table_name =
            std::string("BM_K34_Scalar_Table_R") +
            std::to_string(ResultBytes);
        const std::string runtime_name =
            std::string("BM_K34_Scalar_Runtime_R") +
            std::to_string(ResultBytes);
        benchmark::RegisterBenchmark(
            direct_name.c_str(), &BM_K34ScalarDirect<ResultBytes>)->Args(args);
        benchmark::RegisterBenchmark(
            table_name.c_str(), &BM_K34ScalarTable<ResultBytes>)->Args(args);
        benchmark::RegisterBenchmark(
            runtime_name.c_str(), &BM_K34ScalarRuntime<ResultBytes>)->Args(args);
      }
    }
  }
}

[[maybe_unused]] const bool kK34BenchmarksRegistered = [] {
  RegisterK34BytePair<4, 2>(
      "Cuckoo", &BM_K34ByteDirect<4, 2, K34Cuckoo<4, 2>>,
      &BM_K34ByteTable<4, 2, K34Cuckoo<4, 2>>, &BM_K34ByteRuntime<4, 2>,
      {4, 16, 64});
  RegisterK34BytePair<8, 4>(
      "Cuckoo", &BM_K34ByteDirect<8, 4, K34Cuckoo<8, 4>>,
      &BM_K34ByteTable<8, 4, K34Cuckoo<8, 4>>, &BM_K34ByteRuntime<8, 4>,
      {4, 16, 64});
  RegisterK34BytePair<16, 8>(
      "Cuckoo", &BM_K34ByteDirect<16, 8, K34Cuckoo<16, 8>>,
      &BM_K34ByteTable<16, 8, K34Cuckoo<16, 8>>,
      &BM_K34ByteRuntime<16, 8>, {16, 64, 1024});
  RegisterK34BytePair<32, 16>(
      "Cuckoo", &BM_K34ByteDirect<32, 16, K34Cuckoo<32, 16>>,
      &BM_K34ByteTable<32, 16, K34Cuckoo<32, 16>>,
      &BM_K34ByteRuntime<32, 16>, {64, 1024, 10000});
  RegisterK34BytePair<32, 32>(
      "Cuckoo", &BM_K34ByteDirect<32, 32, K34Cuckoo<32, 32>>,
      &BM_K34ByteTable<32, 32, K34Cuckoo<32, 32>>,
      &BM_K34ByteRuntime<32, 32>, {1024, 10000, 100000});

  RegisterK34BytePair<4, 2>(
      "Small", &BM_K34ByteDirect<4, 2, K34Small<4, 2>>,
      &BM_K34ByteTable<4, 2, K34Small<4, 2>>, &BM_K34ByteRuntime<4, 2>,
      {4, 16, 64});

  RegisterK34ScalarPair<2>();
  RegisterK34ScalarPair<4>();
  RegisterK34ScalarPair<8>();
  RegisterK34ScalarPair<16>();
  RegisterK34ScalarPair<32>();

  for (size_t batch : {size_t{1}, size_t{8}, size_t{16}, size_t{32}}) {
    for (int distribution = 0; distribution < 4; distribution++) {
      const auto args = {static_cast<int64_t>(batch),
                         static_cast<int64_t>(distribution)};
      benchmark::RegisterBenchmark("BM_K34FlowDirect", &BM_K34FlowDirect)
          ->Args(args);
      benchmark::RegisterBenchmark("BM_K34FlowTable", &BM_K34FlowTable)
          ->Args(args);
      benchmark::RegisterBenchmark("BM_K34FlowRuntime", &BM_K34FlowRuntime)
          ->Args(args);
    }
  }
  return true;
}();

}  // namespace
