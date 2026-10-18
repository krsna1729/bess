// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/range_backend.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
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
  // Keys are built from the rules, not at random. A random key against a random
  // mask matches with probability around 2^-32, so a random stream only shows
  // that both sides miss - a MatchFast that always returned false would pass
  // it. Each constructible rule gets a key that takes its masked value, fills
  // the unmasked bits at random and puts both ports inside its ranges, so the
  // hit path and the result are exercised; the remaining keys are one flipped
  // masked bit or one port at high + 1 away from such a key, so near-misses
  // are too. Rules whose mask selects port bytes cannot be hit this way and are
  // left to the near-miss and random keys.
  std::mt19937 rng(42);
  std::uniform_int_distribution<uint16_t> port_dist(1, 60000);
  std::uniform_int_distribution<int64_t> prio_dist(1, 1000);
  std::uniform_int_distribution<int> byte_dist(0, 255);

  constexpr size_t kBatchSize = 32;
  constexpr size_t kRules = 24;

  uint64_t total_fast_hits = 0;
  uint64_t total_scalar_hits = 0;

  for (const size_t key_size : {size_t{4}, size_t{8}, size_t{12}, size_t{13}}) {
    for (const size_t mask_len : {key_size - 1, key_size, key_size + 3}) {
      const size_t src_off = 4;
      const size_t dst_off = 6;
      const bool ports_fit = dst_off + sizeof(uint16_t) <= key_size;

      std::vector<RangeRule<uint32_t>> rules;
      std::vector<std::vector<std::byte>> rule_keys;  // hit keys, per rule

      for (uint32_t i = 0; i < kRules; i++) {
        // Half the rules leave the port bytes unmasked, so a key can satisfy
        // the mask and land inside both ranges. The whole window has to be
        // inside the mask, or a chosen port would violate the mask instead.
        const bool ports_free =
            ports_fit && (i % 2 == 0) &&
            src_off + sizeof(uint16_t) <= mask_len &&
            dst_off + sizeof(uint16_t) <= mask_len;

        std::vector<std::byte> value(mask_len);
        std::vector<std::byte> mask(mask_len);
        for (size_t j = 0; j < mask_len; j++) {
          mask[j] = static_cast<std::byte>(byte_dist(rng));
          // A rule can only ever match if its value carries no bits outside its
          // mask, so the constructible rules are built that way. The others
          // carry such bits on purpose and can never match: they still
          // exercise the rejection path.
          value[j] = static_cast<std::byte>(
              (ports_free || j % 2 == 0)
                  ? (byte_dist(rng) & static_cast<int>(mask[j]))
                  : byte_dist(rng));
        }
        if (ports_free) {
          for (size_t j = 0; j < sizeof(uint16_t); j++) {
            if (src_off + j < mask_len) {
              mask[src_off + j] = std::byte{0};
              value[src_off + j] = std::byte{0};
            }
            if (dst_off + j < mask_len) {
              mask[dst_off + j] = std::byte{0};
              value[dst_off + j] = std::byte{0};
            }
          }
        }

        const uint16_t s_lo = port_dist(rng);
        const uint16_t s_hi = s_lo + (port_dist(rng) % 1000);
        const uint16_t d_lo = port_dist(rng);
        const uint16_t d_hi = d_lo + (port_dist(rng) % 1000);

        // Every rule gets a key, so every configuration is exercised. The key
        // matches the value/mask by construction; when the mask leaves the
        // ports free they must be inside the rule's range, or the rule's own
        // range check rejects the key.
        std::vector<std::byte> key(key_size);
        for (size_t j = 0; j < key_size; j++) {
          const std::byte m = j < mask.size() ? mask[j] : std::byte{0};
          const std::byte v = j < value.size() ? value[j] : std::byte{0};
          key[j] = (v & m) | (static_cast<std::byte>(byte_dist(rng)) &
                              static_cast<std::byte>(~static_cast<int>(m)));
        }
        if (ports_free) {
          const be16_t src_port{static_cast<uint16_t>(
              s_lo + (port_dist(rng) % (s_hi - s_lo + 1)))};
          const be16_t dst_port{static_cast<uint16_t>(
              d_lo + (port_dist(rng) % (d_hi - d_lo + 1)))};
          std::memcpy(key.data() + src_off, &src_port, sizeof(src_port));
          std::memcpy(key.data() + dst_off, &dst_port, sizeof(dst_port));
        }
        rule_keys.push_back(std::move(key));

        rules.push_back({
            .value = std::move(value),
            .mask = std::move(mask),
            .src_range = PortRange{.low = s_lo, .high = s_hi},
            .dst_range = PortRange{.low = d_lo, .high = d_hi},
            .src_port_offset = src_off,
            .dst_port_offset = dst_off,
            .priority = prio_dist(rng),
            .result = i + 1,
        });
      }

      RangeClassifier<uint32_t> fast_classifier(rules);
      // A copy, not a move: the key loop below still reads rules[i].
      ScalarRangeBackend<uint32_t> scalar_ref(rules);

      // Per rule: its hit key, the same key with one masked bit flipped, and
      // the same key with one port stepped past its range's end.
      std::vector<std::vector<std::byte>> keys;
      for (size_t i = 0; i < rules.size(); i++) {
        const auto &hit = rule_keys[i];
        if (hit.empty()) {
          continue;
        }
        keys.push_back(hit);

        auto flipped = hit;
        for (size_t j = 0; j < mask_len && j < key_size; j++) {
          if (rules[i].mask[j] != std::byte{0}) {
            flipped[j] ^= static_cast<std::byte>(
                byte_dist(rng) & static_cast<int>(rules[i].mask[j]));
            break;
          }
        }
        keys.push_back(std::move(flipped));

        if (ports_fit) {
          auto past = hit;
          // One past the range's end: a near-miss, not a hit.
          const be16_t src_hi{
              static_cast<uint16_t>(rules[i].src_range.high + 1)};
          const be16_t dst_hi{
              static_cast<uint16_t>(rules[i].dst_range.high + 1)};
          std::memcpy(past.data() + src_off, &src_hi, sizeof(src_hi));
          std::memcpy(past.data() + dst_off, &dst_hi, sizeof(dst_hi));
          keys.push_back(std::move(past));
        }

        auto random_key = hit;
        for (auto &b : random_key) {
          b = static_cast<std::byte>(byte_dist(rng));
        }
        keys.push_back(std::move(random_key));
      }
      // A configuration whose rules yield no keys would pass vacuously.
      ASSERT_GT(keys.size(), 0u)
          << "no keys for key size " << key_size << ", mask length "
          << mask_len;

      for (size_t b = 0; b < keys.size(); b += kBatchSize) {
        const size_t count = std::min(kBatchSize, keys.size() - b);
        std::array<uint32_t, kBatchSize> fast_results{};
        std::array<uint32_t, kBatchSize> scalar_results{};

        std::vector<std::byte> flat(count * key_size);
        for (size_t i = 0; i < count; i++) {
          std::memcpy(flat.data() + i * key_size, keys[b + i].data(), key_size);
        }

        ConstBytes batch_keys(flat.data(), flat.size());
        // Only `count` keys are in the buffer: a full 32-element span would
        // make LookupBatch read past it.
        uint64_t fast_hits = fast_classifier.LookupBatch(
            batch_keys, key_size, key_size,
            std::span<uint32_t>(fast_results).first(count));
        uint64_t scalar_hits = scalar_ref.LookupBatch(
            batch_keys, key_size, key_size,
            std::span<uint32_t>(scalar_results).first(count));

        total_fast_hits += std::popcount(fast_hits);
        total_scalar_hits += std::popcount(scalar_hits);

        EXPECT_EQ(fast_hits, scalar_hits)
            << "Hit mask mismatch at batch " << b << ", key size " << key_size
            << ", mask length " << mask_len;
        for (size_t i = 0; i < count; i++) {
          if (fast_hits & (uint64_t{1} << i)) {
            EXPECT_EQ(fast_results[i], scalar_results[i])
                << "Result mismatch at packet " << (b + i);
          }
        }
      }
    }
  }

  // Without this the whole comparison could be vacuous: a stream that never
  // hits exercises only the rejection path, and an always-false implementation
  // would pass it.
  EXPECT_GT(total_scalar_hits, 0u);
  EXPECT_EQ(total_fast_hits, total_scalar_hits);
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
