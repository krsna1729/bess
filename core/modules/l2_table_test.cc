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

#include <xmmintrin.h>

#include <atomic>
#include <chrono>
#include <random>
#include <thread>
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

// The former AVX probe compared slots as doubles (_mm256_cmp_pd): an empty
// slot (all zeros, +0.0) compared equal to the key for MAC 0 (occupied bit
// set: -0.0), so MAC 0 "hit" an empty table.
TEST(L2TableCompareTest, MacZeroMissesOnAnEmptyTable) {
  l2_table table = {};
  ASSERT_EQ(0, l2_init(&table, 1024, 4));
  gate_idx_t gate = 0;
  EXPECT_EQ(-ENOENT, l2_find(&table, 0, &gate));
  ASSERT_EQ(0, l2_add_entry(&table, 0, 7));
  ASSERT_EQ(0, l2_find(&table, 0, &gate));
  EXPECT_EQ(7, gate);
  l2_deinit(&table);
}

// ...and with denormals-are-zero set in MXCSR (some libraries and
// -ffast-math code set it), every masked slot compared equal to every key.
TEST(L2TableCompareTest, DenormalsAreZeroDoesNotMatchEverything) {
  l2_table table = {};
  ASSERT_EQ(0, l2_init(&table, 1024, 4));
  for (uint64_t mac = 1; mac <= 500; mac++) {
    ASSERT_EQ(0, l2_add_entry(&table, mac * 0x10001, 1));
  }
  const unsigned saved = _mm_getcsr();
  _mm_setcsr(saved | 0x8040);  // DAZ | FTZ
  size_t false_hits = 0;
  for (uint64_t mac = 1; mac <= 500; mac++) {
    gate_idx_t gate;
    false_hits += l2_find(&table, mac * 0x10001 + 3, &gate) == 0;
  }
  _mm_setcsr(saved);
  EXPECT_EQ(0u, false_hits);
  l2_deinit(&table);
}

// The move-order contract, deterministically: a reader that looks up an
// entry while it is being moved (after the alternate slot is written, before
// the primary is cleared) finds it with its own gate. Writing the alternate
// first is what makes primary-then-alternate probing never miss a moving
// entry; a stress test alone cannot see this (the window is two stores).
TEST(L2TableConcurrencyTest, EntryIsFoundInTheMiddleOfItsMove) {
  l2_table table = {};
  ASSERT_EQ(0, l2_init(&table, 16, 4));
  struct Probe {
    l2_table *table;
    int moves = 0, found = 0;
  } probe{&table};
  table.move_hook_ctx = &probe;
  table.move_hook = [](void *ctx, uint64_t word) {
    auto *p = static_cast<Probe *>(ctx);
    l2_entry e;
    e.entry = word;
    gate_idx_t gate = 0;
    p->moves++;
    if (l2_find(p->table, e.addr, &gate) == 0 && gate == e.gate) {
      p->found++;
    }
  };
  std::mt19937_64 rng(0x7);
  for (int i = 0; i < 2000 && probe.moves < 50; i++) {
    l2_add_entry(&table, rng() & 0xffffffffffffull,
                 static_cast<gate_idx_t>(i % 4096));
  }
  ASSERT_GT(probe.moves, 0) << "no insert displaced an entry";
  EXPECT_EQ(probe.moves, probe.found)
      << "a moving entry was invisible to readers mid-move";
  l2_deinit(&table);
}

// G1.2 mode C: readers look up stable MACs without locks while one writer
// churns other MACs through a small, nearly full table, so inserts keep
// displacing entries (primary -> alternate moves) and slots keep being
// reused. A reader must always find every stable MAC, with its own gate.
TEST(L2TableConcurrencyTest, ReadersNeverMissAStableEntryDuringChurn) {
  l2_table table = {};
  ASSERT_EQ(0, l2_init(&table, 64, 4));  // 256 slots
  std::mt19937_64 rng(0x42);
  std::vector<uint64_t> stable;
  while (stable.size() < 96) {
    const uint64_t mac = rng() & 0xffffffffffffull;
    if (l2_add_entry(&table, mac, static_cast<gate_idx_t>(stable.size())) ==
        0) {
      stable.push_back(mac);
    }
  }

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> missed{0}, wrong{0}, lookups{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 2; r++) {
    readers.emplace_back([&] {
      uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        for (size_t i = 0; i < stable.size(); i++) {
          gate_idx_t gate = 0;
          if (l2_find(&table, stable[i], &gate) != 0) {
            missed++;
          } else if (gate != static_cast<gate_idx_t>(i)) {
            wrong++;
          }
        }
        local += stable.size();
      }
      lookups += local;
    });
  }

  std::vector<uint64_t> churn;
  uint64_t ops = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!churn.empty() && (churn.size() > 120 || rng() % 2)) {
      const size_t at = rng() % churn.size();
      ASSERT_EQ(0, l2_del_entry(&table, churn[at]));
      churn[at] = churn.back();
      churn.pop_back();
    } else {
      const uint64_t mac = rng() & 0xffffffffffffull;
      // A full table (-ENOMEM) is fine: the churn continues with deletes.
      if (l2_add_entry(&table, mac, 0x7000) == 0) {
        churn.push_back(mac);
      }
    }
    ops++;
  }
  stop = true;
  for (auto &t : readers) t.join();
  EXPECT_EQ(0u, missed.load()) << "of " << lookups.load() << " lookups, "
                               << ops << " ops";
  EXPECT_EQ(0u, wrong.load());
  EXPECT_GT(ops, 100000u);
  l2_deinit(&table);
}

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
