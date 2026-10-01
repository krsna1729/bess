// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/range_backend.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "utils/endian.h"

namespace {

using bess::classifier::ConstBytes;
using bess::classifier::PortRange;
using bess::classifier::RangeClassifier;
using bess::classifier::RangeRule;
using bess::utils::be16_t;

struct PacketKey {
  uint32_t src_ip = 0x0a000001;
  uint32_t dst_ip = 0x0a000002;
  be16_t src_port{1234};
  be16_t dst_port{8500};
};

std::vector<std::byte> ToBytes(const PacketKey &k) {
  std::vector<std::byte> b(sizeof(PacketKey));
  std::memcpy(b.data(), &k, sizeof(PacketKey));
  return b;
}

// Benchmark K3.8 Range Matching (1 rule with [8000, 9000] interval)
void BM_K38RangeLookupBatch(benchmark::State &state) {
  const size_t batch_size = static_cast<size_t>(state.range(0));

  std::vector<RangeRule<uint32_t>> rules;
  PacketKey k{};
  PacketKey m{.src_ip = 0xffffffff, .dst_ip = 0xffffffff};

  rules.push_back({
      .value = ToBytes(k),
      .mask = ToBytes(m),
      .src_range = PortRange{.low = 1000, .high = 2000},
      .dst_range = PortRange{.low = 8000, .high = 9000},
      .src_port_offset = 8,
      .dst_port_offset = 10,
      .priority = 100,
      .result = 1,
  });

  RangeClassifier<uint32_t> classifier(std::move(rules));

  std::vector<PacketKey> keys(batch_size);
  for (size_t i = 0; i < batch_size; i++) {
    keys[i].src_port = be16_t(1500);
    keys[i].dst_port = be16_t(8500);
  }

  std::vector<std::byte> raw_keys(sizeof(PacketKey) * batch_size);
  std::memcpy(raw_keys.data(), keys.data(), raw_keys.size());

  std::vector<uint32_t> results(batch_size);

  for (auto _ : state) {
    uint64_t hits = classifier.LookupBatch(
        ConstBytes(raw_keys.data(), raw_keys.size()), sizeof(PacketKey),
        sizeof(PacketKey), std::span<uint32_t>(results));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }

  state.SetItemsProcessed(state.iterations() * batch_size);
}
BENCHMARK(BM_K38RangeLookupBatch)->Arg(1)->Arg(8)->Arg(16)->Arg(32);

// Benchmark Equivalent Cartesian Expanded Ternary Masks (25 rules)
void BM_CartesianTernaryLookupBatch(benchmark::State &state) {
  const size_t batch_size = static_cast<size_t>(state.range(0));

  // Simulating Cartesian product of 5 src masks x 5 dst masks = 25 rules
  std::vector<RangeRule<uint32_t>> cartesian_rules;
  for (uint16_t s = 0; s < 5; s++) {
    for (uint16_t d = 0; d < 5; d++) {
      PacketKey k{};
      PacketKey m{.src_ip = 0xffffffff, .dst_ip = 0xffffffff};
      cartesian_rules.push_back({
          .value = ToBytes(k),
          .mask = ToBytes(m),
          .src_range = PortRange{.low = static_cast<uint16_t>(1000 + s * 200),
                                 .high = static_cast<uint16_t>(1199 + s * 200)},
          .dst_range = PortRange{.low = static_cast<uint16_t>(8000 + d * 200),
                                 .high = static_cast<uint16_t>(8199 + d * 200)},
          .src_port_offset = 8,
          .dst_port_offset = 10,
          .priority = static_cast<int64_t>(100 - (s + d)),
          .result = static_cast<uint32_t>(s * 5 + d + 1),
      });
    }
  }

  RangeClassifier<uint32_t> classifier(std::move(cartesian_rules));

  std::vector<PacketKey> keys(batch_size);
  for (size_t i = 0; i < batch_size; i++) {
    keys[i].src_port = be16_t(1500);
    keys[i].dst_port = be16_t(8500);
  }

  std::vector<std::byte> raw_keys(sizeof(PacketKey) * batch_size);
  std::memcpy(raw_keys.data(), keys.data(), raw_keys.size());

  std::vector<uint32_t> results(batch_size);

  for (auto _ : state) {
    uint64_t hits = classifier.LookupBatch(
        ConstBytes(raw_keys.data(), raw_keys.size()), sizeof(PacketKey),
        sizeof(PacketKey), std::span<uint32_t>(results));
    benchmark::DoNotOptimize(hits);
    benchmark::DoNotOptimize(results);
  }

  state.SetItemsProcessed(state.iterations() * batch_size);
}
BENCHMARK(BM_CartesianTernaryLookupBatch)->Arg(1)->Arg(8)->Arg(16)->Arg(32);

}  // namespace

BENCHMARK_MAIN();
