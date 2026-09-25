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
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
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

// L2Forward's MAC table: every entry that was added must be found again, in
// every bucket slot, and nothing may be added twice.

#include "modules/l2_table.h"

#include <gtest/gtest.h>

#include <random>
#include <vector>

namespace {

class L2TableTest : public ::testing::TestWithParam<int> {};

TEST_P(L2TableTest, EveryAddedEntryIsFound) {
  const int bucket = GetParam();
  l2_table table = {};
  ASSERT_EQ(0, l2_init(&table, 1024, bucket));

  std::mt19937_64 rng(0x12);
  std::vector<std::pair<uint64_t, gate_idx_t>> added;
  for (int i = 0; i < 1024 * bucket; i++) {
    const uint64_t mac = rng() & 0xffffffffffffull;
    const auto gate = static_cast<gate_idx_t>(i % 4096);
    if (l2_add_entry(&table, mac, gate) == 0) {
      added.emplace_back(mac, gate);
    }
  }
  ASSERT_GT(added.size(), 1024u * bucket / 2) << "table filled too little";

  size_t missing = 0, wrong = 0, duplicate_adds = 0;
  for (const auto &[mac, gate] : added) {
    gate_idx_t found = 0;
    if (l2_find(&table, mac, &found) != 0) {
      missing++;
    } else if (found != gate) {
      wrong++;
    }
    if (l2_add_entry(&table, mac, 1) == 0) {
      duplicate_adds++;
    }
  }
  EXPECT_EQ(0u, missing) << "of " << added.size();
  EXPECT_EQ(0u, wrong);
  EXPECT_EQ(0u, duplicate_adds);
  l2_deinit(&table);
}

INSTANTIATE_TEST_SUITE_P(Buckets, L2TableTest, ::testing::Values(1, 2, 4));

// l2_find_batch agrees with l2_find for hits and misses, whichever body the
// table uses.
TEST(L2TableBatchTest, MatchesSingleLookupsUnderBothBodies) {
  l2_table table = {};
  ASSERT_EQ(0, l2_init(&table, 1024, 4));
  std::mt19937_64 rng(0x34);
  std::vector<uint64_t> keys;
  for (int i = 0; i < 2000; i++) {
    const uint64_t mac = rng() & 0xffffffffffffull;
    if (l2_add_entry(&table, mac, static_cast<gate_idx_t>(i % 512)) == 0) {
      keys.push_back(mac);
    }
    keys.push_back(rng() & 0xffffffffffffull);  // mostly misses
  }
  for (auto body : {bess::dataplane::LookupBody::kPlain,
                    bess::dataplane::LookupBody::kStaged}) {
    table.lookup_body = body;
    for (size_t base = 0; base + 32 <= keys.size(); base += 32) {
      gate_idx_t gates[32];
      const uint64_t hits = l2_find_batch(&table, &keys[base], gates, 32);
      for (size_t i = 0; i < 32; i++) {
        gate_idx_t expected;
        const bool hit = l2_find(&table, keys[base + i], &expected) == 0;
        ASSERT_EQ(hit, ((hits >> i) & 1) != 0) << i;
        if (hit) {
          ASSERT_EQ(expected, gates[i]);
        }
      }
    }
  }
  l2_deinit(&table);
}

}  // namespace
