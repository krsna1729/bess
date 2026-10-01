// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/range_backend.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace bess::classifier {
namespace {

using bess::utils::be16_t;

// Test packet key layout (8 bytes):
// [0..3]: IPv4 Src IP
// [4..5]: L4 Src Port
// [6..7]: L4 Dst Port
struct TestKey {
  uint32_t src_ip = 0;
  be16_t src_port{0};
  be16_t dst_port{0};
};

std::vector<std::byte> ToBytes(const TestKey &k) {
  std::vector<std::byte> b(sizeof(TestKey));
  std::memcpy(b.data(), &k, sizeof(TestKey));
  return b;
}

TEST(RangeBackendTest, ExactAndWildcardRanges) {
  std::vector<RangeRule<uint32_t>> rules;

  // Rule 1: Src IP 10.0.0.1, Dst Port 80 (exact)
  TestKey k1{.src_ip = 0x0a000001, .src_port = be16_t(0), .dst_port = be16_t(0)};
  TestKey m1{.src_ip = 0xffffffff, .src_port = be16_t(0), .dst_port = be16_t(0)};
  rules.push_back({
      .value = ToBytes(k1),
      .mask = ToBytes(m1),
      .src_range = PortRange{.low = 0, .high = 0xffff},  // wildcard
      .dst_range = PortRange{.low = 80, .high = 80},     // exact
      .src_port_offset = 4,
      .dst_port_offset = 6,
      .priority = 10,
      .result = 100,
  });

  RangeClassifier<uint32_t> classifier(std::move(rules));

  // Match: Dst port 80
  TestKey match_pkt{.src_ip = 0x0a000001, .src_port = be16_t(1234), .dst_port = be16_t(80)};
  auto match_bytes = ToBytes(match_pkt);

  // Miss: Dst port 81
  TestKey miss_pkt{.src_ip = 0x0a000001, .src_port = be16_t(1234), .dst_port = be16_t(81)};
  auto miss_bytes = ToBytes(miss_pkt);

  std::vector<std::byte> keys(sizeof(TestKey) * 2);
  std::memcpy(keys.data(), match_bytes.data(), sizeof(TestKey));
  std::memcpy(keys.data() + sizeof(TestKey), miss_bytes.data(), sizeof(TestKey));

  std::array<uint32_t, 2> results{};
  uint64_t hits = classifier.LookupBatch(
      ConstBytes(keys.data(), keys.size()), sizeof(TestKey), sizeof(TestKey),
      std::span<uint32_t>(results));

  EXPECT_EQ(hits, 0b01u);
  EXPECT_EQ(results[0], 100u);
}

TEST(RangeBackendTest, NonPowerOfTwoAndSimultaneousRanges) {
  // PDR rule with non-power-of-two port ranges:
  // Src Port in [1000, 2000] AND Dst Port in [8000, 9000]
  std::vector<RangeRule<uint32_t>> rules;

  TestKey k{};
  TestKey m{};  // wildcard IP
  rules.push_back({
      .value = ToBytes(k),
      .mask = ToBytes(m),
      .src_range = PortRange{.low = 1000, .high = 2000},
      .dst_range = PortRange{.low = 8000, .high = 9000},
      .src_port_offset = 4,
      .dst_port_offset = 6,
      .priority = 50,
      .result = 777,
  });

  RangeClassifier<uint32_t> classifier(std::move(rules));

  // 1. Both inside range: Src 1500, Dst 8500 -> MATCH
  TestKey p1{.src_port = be16_t(1500), .dst_port = be16_t(8500)};
  // 2. Src inside, Dst outside: Src 1500, Dst 7999 -> MISS
  TestKey p2{.src_port = be16_t(1500), .dst_port = be16_t(7999)};
  // 3. Src outside, Dst inside: Src 999, Dst 8500 -> MISS
  TestKey p3{.src_port = be16_t(999), .dst_port = be16_t(8500)};
  // 4. Exact boundaries: Src 1000, Dst 9000 -> MATCH
  TestKey p4{.src_port = be16_t(1000), .dst_port = be16_t(9000)};

  std::vector<std::byte> keys(sizeof(TestKey) * 4);
  auto b1 = ToBytes(p1); auto b2 = ToBytes(p2); auto b3 = ToBytes(p3); auto b4 = ToBytes(p4);
  std::memcpy(keys.data() + 0 * sizeof(TestKey), b1.data(), sizeof(TestKey));
  std::memcpy(keys.data() + 1 * sizeof(TestKey), b2.data(), sizeof(TestKey));
  std::memcpy(keys.data() + 2 * sizeof(TestKey), b3.data(), sizeof(TestKey));
  std::memcpy(keys.data() + 3 * sizeof(TestKey), b4.data(), sizeof(TestKey));

  std::array<uint32_t, 4> results{};
  uint64_t hits = classifier.LookupBatch(
      ConstBytes(keys.data(), keys.size()), sizeof(TestKey), sizeof(TestKey),
      std::span<uint32_t>(results));

  EXPECT_EQ(hits, 0b1001u);
  EXPECT_EQ(results[0], 777u);
  EXPECT_EQ(results[3], 777u);
}

TEST(RangeBackendTest, PrecedenceAndOverlappingRanges) {
  // Rule 1: Dst port [1000, 3000], priority 10 -> Result 1
  // Rule 2: Dst port [2000, 4000], priority 20 -> Result 2 (higher priority)
  std::vector<RangeRule<uint32_t>> rules;

  TestKey k{};
  TestKey m{};
  rules.push_back({
      .value = ToBytes(k),
      .mask = ToBytes(m),
      .src_range = PortRange{},
      .dst_range = PortRange{.low = 1000, .high = 3000},
      .src_port_offset = 4,
      .dst_port_offset = 6,
      .priority = 10,
      .result = 1,
  });
  rules.push_back({
      .value = ToBytes(k),
      .mask = ToBytes(m),
      .src_range = PortRange{},
      .dst_range = PortRange{.low = 2000, .high = 4000},
      .src_port_offset = 4,
      .dst_port_offset = 6,
      .priority = 20,
      .result = 2,
  });

  RangeClassifier<uint32_t> classifier(std::move(rules));

  // Dst port 1500 matches only Rule 1 -> Result 1
  TestKey p1{.dst_port = be16_t(1500)};
  // Dst port 2500 matches BOTH Rule 1 and Rule 2 -> Result 2 (priority 20 wins!)
  TestKey p2{.dst_port = be16_t(2500)};
  // Dst port 3500 matches only Rule 2 -> Result 2
  TestKey p3{.dst_port = be16_t(3500)};

  std::vector<std::byte> keys(sizeof(TestKey) * 3);
  auto b1 = ToBytes(p1); auto b2 = ToBytes(p2); auto b3 = ToBytes(p3);
  std::memcpy(keys.data() + 0 * sizeof(TestKey), b1.data(), sizeof(TestKey));
  std::memcpy(keys.data() + 1 * sizeof(TestKey), b2.data(), sizeof(TestKey));
  std::memcpy(keys.data() + 2 * sizeof(TestKey), b3.data(), sizeof(TestKey));

  std::array<uint32_t, 3> results{};
  uint64_t hits = classifier.LookupBatch(
      ConstBytes(keys.data(), keys.size()), sizeof(TestKey), sizeof(TestKey),
      std::span<uint32_t>(results));

  EXPECT_EQ(hits, 0b111u);
  EXPECT_EQ(results[0], 1u);
  EXPECT_EQ(results[1], 2u);
  EXPECT_EQ(results[2], 2u);
}

TEST(RangeBackendTest, DifferentialFuzzAgainstScalarReference) {
  // Create 50 random rules with overlapping port ranges and priorities
  std::mt19937 rng(42);
  std::uniform_int_distribution<uint16_t> port_dist(1, 60000);
  std::uniform_int_distribution<int64_t> prio_dist(1, 1000);

  std::vector<RangeRule<uint32_t>> rules;
  for (uint32_t i = 0; i < 50; i++) {
    uint16_t s_lo = port_dist(rng);
    uint16_t s_hi = s_lo + (port_dist(rng) % 1000);
    uint16_t d_lo = port_dist(rng);
    uint16_t d_hi = d_lo + (port_dist(rng) % 1000);

    TestKey k{};
    TestKey m{};
    rules.push_back({
        .value = ToBytes(k),
        .mask = ToBytes(m),
        .src_range = PortRange{.low = s_lo, .high = s_hi},
        .dst_range = PortRange{.low = d_lo, .high = d_hi},
        .src_port_offset = 4,
        .dst_port_offset = 6,
        .priority = prio_dist(rng),
        .result = i + 1,
    });
  }

  RangeClassifier<uint32_t> fast_classifier(rules);
  ScalarRangeBackend<uint32_t> scalar_ref(std::move(rules));

  // Run 1,000 random packet lookups in batches of 32
  constexpr size_t kBatchSize = 32;
  constexpr size_t kTotalPackets = 1024;
  std::vector<std::byte> keys(sizeof(TestKey) * kTotalPackets);

  for (size_t i = 0; i < kTotalPackets; i++) {
    TestKey pkt{.src_port = be16_t(port_dist(rng)), .dst_port = be16_t(port_dist(rng))};
    std::memcpy(keys.data() + i * sizeof(TestKey), &pkt, sizeof(TestKey));
  }

  for (size_t b = 0; b < kTotalPackets; b += kBatchSize) {
    std::array<uint32_t, kBatchSize> fast_results{};
    std::array<uint32_t, kBatchSize> scalar_results{};

    ConstBytes batch_keys(keys.data() + b * sizeof(TestKey), kBatchSize * sizeof(TestKey));

    uint64_t fast_hits = fast_classifier.LookupBatch(
        batch_keys, sizeof(TestKey), sizeof(TestKey),
        std::span<uint32_t>(fast_results));

    uint64_t scalar_hits = scalar_ref.LookupBatch(
        batch_keys, sizeof(TestKey), sizeof(TestKey),
        std::span<uint32_t>(scalar_results));

    EXPECT_EQ(fast_hits, scalar_hits) << "Hit mask mismatch at batch " << b;
    for (size_t i = 0; i < kBatchSize; i++) {
      if (fast_hits & (uint64_t{1} << i)) {
        EXPECT_EQ(fast_results[i], scalar_results[i])
            << "Result mismatch at packet " << (b + i);
      }
    }
  }
}

TEST(RangeBackendTest, CartesianExplosionElimination) {
  // A single range [1000, 1020] would require ~5 ternary prefix rules.
  // Simultaneous [1000, 1020] and [8000, 8020] requires 5 x 5 = 25 rules.
  // With K3.8 RangeRule, it requires exactly 1 rule!
  std::vector<RangeRule<uint32_t>> rules;
  TestKey k{};
  TestKey m{};
  rules.push_back({
      .value = ToBytes(k),
      .mask = ToBytes(m),
      .src_range = PortRange{.low = 1000, .high = 1020},
      .dst_range = PortRange{.low = 8000, .high = 8020},
      .src_port_offset = 4,
      .dst_port_offset = 6,
      .priority = 1,
      .result = 42,
  });

  RangeClassifier<uint32_t> classifier(std::move(rules));
  EXPECT_EQ(classifier.size(), 1u);
}

}  // namespace
}  // namespace bess::classifier
