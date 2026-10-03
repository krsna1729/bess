// SPDX-License-Identifier: BSD-3-Clause

// Member-selection algorithms (M16, D-066).

#include "dataplane/member_select.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <numeric>
#include <random>
#include <vector>

namespace bess::dataplane {
namespace {

// HashLB's mapping before M16 (modules/hash_lb.cc, hash_range).
uint16_t FloatRange(uint32_t hashval, uint16_t range) {
  uint64_t i = 0x3ff0000000000000ull | (static_cast<uint64_t>(hashval) << 20);
  double d;
  std::memcpy(&d, &i, sizeof(d));
  return static_cast<uint16_t>((d - 1.0) * range);
}

TEST(MemberSelectTest, RangeSelectEqualsHashLbsFloatMapping) {
  std::mt19937 rng(16);
  const uint32_t edges[] = {0, 1, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff};
  for (uint32_t n = 1; n <= 65535; n += (n < 300 ? 1 : 97)) {
    for (const uint32_t h : edges) {
      ASSERT_EQ(FloatRange(h, static_cast<uint16_t>(n)), RangeSelect(h, n)) << h << " " << n;
    }
    for (int k = 0; k < 64; k++) {
      const uint32_t h = rng();
      ASSERT_EQ(FloatRange(h, static_cast<uint16_t>(n)), RangeSelect(h, n)) << h << " " << n;
    }
  }
  EXPECT_EQ(FloatRange(0xffffffff, 65535), RangeSelect(0xffffffff, 65535));
}

// The probability each member gets, read off the alias table exactly.
std::vector<double> AliasProbabilities(const WeightedSelector &s) {
  const size_t n = s.size();
  std::vector<double> p(n, 0.0);
  for (size_t i = 0; i < n; i++) {
    const double keep = s.alias(i) == i ? 1.0 : s.threshold(i) / 4294967296.0;
    p[i] += keep / n;
    p[s.alias(i)] += (1.0 - keep) / n;
  }
  return p;
}

TEST(MemberSelectTest, WeightedTableMatchesTheWeightsExactly) {
  std::mt19937 rng(17);
  for (int trial = 0; trial < 200; trial++) {
    const size_t n = 1 + rng() % 130;
    std::vector<uint32_t> w(n);
    for (auto &x : w) {
      x = rng() % 5 == 0 ? 0 : rng() % (trial % 2 ? 1000 : 0xffffffffu);
    }
    w[rng() % n] = 1 + rng() % 1000;  // at least one positive
    auto s = WeightedSelector::Build(w);
    ASSERT_TRUE(s.has_value());
    const double total = std::accumulate(w.begin(), w.end(), 0.0);
    const auto p = AliasProbabilities(*s);
    for (size_t i = 0; i < n; i++) {
      ASSERT_NEAR(w[i] / total, p[i], 1e-8) << "trial " << trial << " member " << i;
      if (w[i] == 0) {
        ASSERT_EQ(0.0, p[i]);
      }
    }
  }
  EXPECT_FALSE(WeightedSelector::Build(std::vector<uint32_t>{}).has_value());
  EXPECT_FALSE(WeightedSelector::Build(std::vector<uint32_t>{0, 0}).has_value());
}

TEST(MemberSelectTest, WeightedSamplingFollowsTheWeights) {
  const std::vector<uint32_t> w = {1, 0, 3, 6};
  auto s = WeightedSelector::Build(w).value();
  std::vector<int> count(4, 0);
  std::mt19937 rng(18);
  constexpr int kDraws = 200000;
  for (int i = 0; i < kDraws; i++) {
    count[s(rng())]++;
  }
  EXPECT_EQ(0, count[1]);
  EXPECT_NEAR(0.1, count[0] / double(kDraws), 0.005);
  EXPECT_NEAR(0.3, count[2] / double(kDraws), 0.005);
  EXPECT_NEAR(0.6, count[3] / double(kDraws), 0.005);
}

std::vector<uint64_t> Keys(size_t n, uint64_t base = 1000) {
  std::vector<uint64_t> k(n);
  std::iota(k.begin(), k.end(), base);
  return k;
}

TEST(MemberSelectTest, MaglevBalancesAndMovesFewFlowsOnRemoval) {
  for (const size_t n : {2u, 5u, 32u, 128u}) {
    const auto keys = Keys(n);
    auto m = MaglevSelector::Build(keys).value();
    std::vector<int> entries(n, 0);
    for (size_t i = 0; i < m.table_size(); i++) {
      entries[m.entry(i)]++;
    }
    const auto [lo, hi] = std::minmax_element(entries.begin(), entries.end());
    EXPECT_LE(*hi - *lo, 1 + static_cast<int>(m.table_size() / n / 50)) << n << " members";

    // Remove member n/2: its flows move, and few of the others' do.
    std::vector<uint64_t> fewer = keys;
    fewer.erase(fewer.begin() + n / 2);
    auto m2 = MaglevSelector::Build(fewer).value();
    size_t moved_others = 0, others = 0;
    for (uint32_t h = 0; h < 200000; h++) {
      const uint32_t hash = h * 2654435761u;
      const uint64_t before = keys[m(hash)];
      const uint64_t after = fewer[m2(hash)];
      if (before != keys[n / 2]) {
        others++;
        moved_others += before != after;
      }
    }
    // The paper reports a few percent; Range would move about half.
    EXPECT_LT(double(moved_others) / others, 0.05) << n << " members";
  }
  EXPECT_FALSE(MaglevSelector::Build(Keys(3), 65536).has_value()) << "not prime";
  EXPECT_FALSE(MaglevSelector::Build(Keys(7), 7).has_value()) << "not larger than n";
  EXPECT_FALSE(MaglevSelector::Build(std::vector<uint64_t>{1, 2, 1}).has_value()) << "duplicate";
  EXPECT_FALSE(MaglevSelector::Build(std::vector<uint64_t>{}).has_value());
}

TEST(MemberSelectTest, RendezvousMovesOnlyTheRemovedMembersFlows) {
  const auto keys = Keys(10);
  auto r = RendezvousSelector::Build(keys).value();
  std::vector<uint64_t> fewer = keys;
  fewer.erase(fewer.begin() + 3);
  auto r2 = RendezvousSelector::Build(fewer).value();
  std::vector<int> count(10, 0);
  for (uint32_t h = 0; h < 100000; h++) {
    const uint32_t hash = h * 2654435761u;
    const uint64_t before = keys[r(hash)];
    count[r(hash)]++;
    if (before != keys[3]) {
      ASSERT_EQ(before, fewer[r2(hash)]);
    }
  }
  for (int c : count) {
    EXPECT_NEAR(10000, c, 600);
  }
}

TEST(MemberSelectTest, RangeSpreadsEvenly) {
  std::vector<int> count(7, 0);
  for (uint32_t h = 0; h < 70000; h++) {
    count[RangeSelect(h * 2654435761u, 7)]++;
  }
  for (int c : count) {
    EXPECT_NEAR(10000, c, 300);
  }
}

TEST(MemberSelectTest, RoundRobinCyclesAndShrinks) {
  RoundRobinCursor rr;
  std::vector<uint32_t> seq;
  for (int i = 0; i < 7; i++) {
    seq.push_back(rr.Next(3));
  }
  EXPECT_EQ((std::vector<uint32_t>{0, 1, 2, 0, 1, 2, 0}), seq);
  EXPECT_EQ(1u, rr.Next(3));
  EXPECT_EQ(0u, rr.Next(2)) << "cursor 2 restarts when the group shrinks to 2";
  EXPECT_EQ(1u, rr.Next(2));
  RoundRobinCursor at3;
  for (int i = 0; i < 3; i++) {
    at3.Next(4);
  }
  EXPECT_EQ(0u, at3.Next(2)) << "cursor 3 restarts at 0, not 3 mod 2";
  EXPECT_EQ(0u, rr.Next(0));
}

TEST(MemberSelectTest, AnySelectorBatchMatchesTheSelector) {
  const std::vector<uint32_t> w = {5, 1, 1, 9};
  std::vector<AnySelector> all;
  all.emplace_back(RangeSelector{4});
  all.emplace_back(WeightedSelector::Build(w).value());
  all.emplace_back(MaglevSelector::Build(Keys(4), 251).value());
  all.emplace_back(RendezvousSelector::Build(Keys(4)).value());
  const SelectorKind kinds[] = {SelectorKind::kRange, SelectorKind::kWeighted,
                                SelectorKind::kMaglev, SelectorKind::kRendezvous};
  std::mt19937 rng(19);
  std::vector<uint32_t> hashes(64), out(64);
  for (auto &h : hashes) {
    h = rng();
  }
  for (size_t k = 0; k < all.size(); k++) {
    EXPECT_EQ(kinds[k], all[k].kind());
    EXPECT_EQ(4u, all[k].size());
    all[k].SelectBatch(hashes, out);
    for (size_t i = 0; i < hashes.size(); i++) {
      ASSERT_EQ(all[k].Select(hashes[i]), out[i]);
      ASSERT_LT(out[i], 4u);
    }
  }
}

}  // namespace
}  // namespace bess::dataplane
