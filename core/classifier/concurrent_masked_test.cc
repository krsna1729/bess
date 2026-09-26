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


#include "classifier/concurrent_masked.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "control/runtime_state.h"

namespace bess::classifier {
namespace {

constexpr uint32_t kKeyLen = 8;
using Bytes = std::array<std::byte, kKeyLen>;
using R = ConcurrentMaskedTable::UpsertResult;

Bytes B(uint64_t v) {
  Bytes b;
  std::memcpy(b.data(), &v, kKeyLen);
  return b;
}
ConstBytes C(const Bytes &b) { return ConstBytes(b.data(), b.size()); }

// Masks covering the low 8, 4, 2 or 1 bytes (little-endian u64 layout).
const Bytes kFull = B(~uint64_t{0});
const Bytes kLow4 = B(0x00000000ffffffffull);
const Bytes kLow2 = B(0x000000000000ffffull);
const Bytes kLow1 = B(0x00000000000000ffull);

std::unique_ptr<ConcurrentMaskedTable> MakeTable(size_t max_tuples = 8) {
  auto t = ConcurrentMaskedTable::Create(kKeyLen, max_tuples,
                                         control::runtime().rcu());
  EXPECT_TRUE(t.has_value()) << t.error();
  return std::move(*t);
}

// The result for one key, or -1 on a miss.
int Classify(const ConcurrentMaskedTable &t, uint64_t key) {
  const Bytes k = B(key);
  uint16_t result = 0;
  return t.LookupBatch(C(k), kKeyLen, &result, 1) ? result : -1;
}

TEST(ConcurrentMaskedTableTest, HigherPriorityThenLaterCommandWins) {
  auto t = MakeTable();
  const uint64_t key = 0x1122334455667788ull;
  ASSERT_EQ(R::kInserted, t->Upsert(C(kLow1), C(B(key & 0xff)), 1, 10));
  ASSERT_EQ(R::kInserted, t->Upsert(C(kFull), C(B(key)), 9, 20));
  EXPECT_EQ(20, Classify(*t, key)) << "higher priority wins";
  EXPECT_EQ(10, Classify(*t, key ^ 0xff00)) << "only the wide rule matches";
  EXPECT_EQ(-1, Classify(*t, 0x99));

  // Equal priority: the later command wins, whichever mask it is on.
  ASSERT_EQ(R::kInserted, t->Upsert(C(kLow2), C(B(key & 0xffff)), 9, 30));
  EXPECT_EQ(30, Classify(*t, key));
  // Re-adding an existing (mask, value) replaces it and counts as later.
  ASSERT_EQ(R::kUpdated, t->Upsert(C(kFull), C(B(key)), 9, 21));
  EXPECT_EQ(21, Classify(*t, key));
  EXPECT_EQ(3u, t->size());
  EXPECT_EQ(3u, t->tuple_count());
}

TEST(ConcurrentMaskedTableTest, TupleCeilingCountsActiveMasks) {
  auto t = MakeTable(/*max_tuples=*/2);
  ASSERT_EQ(R::kInserted, t->Upsert(C(kFull), C(B(1)), 0, 1));
  ASSERT_EQ(R::kInserted, t->Upsert(C(kLow4), C(B(2)), 0, 2));
  EXPECT_EQ(R::kTooManyTuples, t->Upsert(C(kLow2), C(B(3)), 0, 3));
  EXPECT_EQ(2u, t->size());
  ASSERT_TRUE(t->Erase(C(kLow4), C(B(2))));
  EXPECT_EQ(1u, t->tuple_count()) << "the mask's last rule took the tuple";
  EXPECT_EQ(R::kInserted, t->Upsert(C(kLow2), C(B(3)), 0, 3));
  EXPECT_FALSE(t->Erase(C(kLow2), C(B(4))));
  EXPECT_FALSE(t->Erase(C(kLow1), C(B(3))));
  t->Clear();
  EXPECT_EQ(0u, t->size());
  EXPECT_EQ(0u, t->tuple_count());
  EXPECT_EQ(-1, Classify(*t, 1));
  EXPECT_EQ(R::kInserted, t->Upsert(C(kLow1), C(B(5)), 0, 5));
}

TEST(ConcurrentMaskedTableTest, RejectsNonCanonicalRules) {
  auto t = MakeTable();
  EXPECT_EQ(R::kNotCanonical, t->Upsert(C(kLow1), C(B(0x1ff)), 0, 1));
  EXPECT_EQ(0u, t->tuple_count());
}

TEST(ConcurrentMaskedTableTest, GrowsWithoutLosingRules) {
  auto t = MakeTable();
  constexpr uint64_t kRules = 5000;
  for (uint64_t i = 0; i < kRules; i++) {
    ASSERT_EQ(R::kInserted,
              t->Upsert(C(kLow4), C(B(i)), 0, static_cast<uint16_t>(i)));
  }
  for (uint64_t i = 0; i < kRules; i++) {
    ASSERT_EQ(static_cast<int>(i & 0xffff), Classify(*t, i | (7ull << 40)));
  }
  size_t seen = 0;
  t->ForEach([&](ConstBytes mask, ConstBytes, const auto &) {
    EXPECT_EQ(0, std::memcmp(mask.data(), kLow4.data(), kKeyLen));
    seen++;
  });
  EXPECT_EQ(kRules, seen);
}

// The grace-period contract for rule records, deterministically: with a
// reader online since before a delete or an update, the retired rule id is
// not reused -- that reader may have loaded it and be about to read its
// record. After the reader is quiescent the id comes back.
TEST(ConcurrentMaskedTableTest, RetiredRuleIdWaitsForOnlineReaders) {
  rcu::RcuDomain &domain = control::runtime().rcu();
  auto t = MakeTable();
  ASSERT_EQ(R::kInserted, t->Upsert(C(kFull), C(B(1)), 0, 1));
  ASSERT_EQ(R::kInserted, t->Upsert(C(kFull), C(B(2)), 0, 2));

  constexpr uint32_t kReader = 25;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  domain.Online(kReader);
  ASSERT_TRUE(t->Erase(C(kFull), C(B(1))));
  ASSERT_EQ(R::kUpdated, t->Upsert(C(kFull), C(B(2)), 0, 3));
  ASSERT_EQ(R::kInserted, t->Upsert(C(kFull), C(B(4)), 0, 4));
  EXPECT_EQ(2u, t->retiring_ids())
      << "an id was recycled while a reader was mid-grace-period";
  EXPECT_EQ(3, Classify(*t, 2));
  EXPECT_EQ(4, Classify(*t, 4));

  domain.Quiescent(kReader);
  ASSERT_EQ(R::kInserted, t->Upsert(C(kFull), C(B(5)), 0, 5));
  EXPECT_EQ(0u, t->retiring_ids()) << "retired ids never came back";
  domain.Offline(kReader);
  domain.Unregister(kReader);
}

// Stable rules at the highest priority must always win while other masks
// churn underneath them: rules come and go on three masks (so tuples are
// created and dropped, publishing new tuple lists), tables grow, and rule
// ids are recycled. A reader must never miss a stable key or see another
// rule's result for it.
TEST(ConcurrentMaskedTableTest, ConcurrentReadersAlwaysSeeTheStableWinner) {
  rcu::RcuDomain &domain = control::runtime().rcu();
  auto t = MakeTable();
  std::vector<uint64_t> keys;
  for (uint64_t i = 0; i < 64; i++) {
    keys.push_back(0x5a00000000000000ull | (i * 0x01010101ull));
    ASSERT_EQ(R::kInserted, t->Upsert(C(kFull), C(B(keys.back())), 100, 7));
  }
  std::vector<std::byte> packed(keys.size() * kKeyLen);
  std::memcpy(packed.data(), keys.data(), packed.size());

  constexpr int kReaders = 2;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> wrong{0}, batches{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; r++) {
    const uint32_t reader_id = 26 + r;
    ASSERT_TRUE(domain.Register(reader_id).has_value());
    readers.emplace_back([&, reader_id] {
      domain.Online(reader_id);
      uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        for (size_t base = 0; base < keys.size(); base += 32) {
          uint16_t results[32];
          const uint64_t hits = t->LookupBatch(
              ConstBytes(packed.data() + base * kKeyLen, 32 * kKeyLen),
              kKeyLen, results, 32);
          if (hits != 0xffffffffull) {
            wrong++;
            continue;
          }
          for (int i = 0; i < 32; i++) {
            wrong += results[i] != 7;
          }
        }
        local++;
        domain.Quiescent(reader_id);
      }
      batches += local;
      domain.Offline(reader_id);
    });
  }

  const Bytes masks[] = {kLow4, kLow2, kLow1};
  const uint64_t mask_bits[] = {0xffffffffull, 0xffffull, 0xffull};
  std::mt19937_64 rng(0x7e57);
  std::vector<std::pair<int, uint64_t>> live;
  uint64_t ops = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!live.empty() && (live.size() > 3000 || rng() % 2)) {
      const size_t at = rng() % live.size();
      const auto [m, v] = live[at];
      ASSERT_TRUE(t->Erase(C(masks[m]), C(B(v))));
      live[at] = live.back();
      live.pop_back();
    } else {
      const int m = static_cast<int>(rng() % 3);
      // Half the churn rules match stable keys (at lower priority).
      const uint64_t v = (rng() % 2 ? keys[rng() % keys.size()] : rng()) &
                         mask_bits[m];
      const auto r = t->Upsert(C(masks[m]), C(B(v)),
                               static_cast<int64_t>(rng() % 100), 9);
      ASSERT_TRUE(r == R::kInserted || r == R::kUpdated);
      if (r == R::kInserted) live.emplace_back(m, v);
    }
    ops++;
  }
  stop = true;
  for (auto &th : readers) th.join();
  for (int r = 0; r < kReaders; r++) domain.Unregister(26 + r);

  EXPECT_EQ(0u, wrong.load()) << "of " << batches.load() << " batch rounds, "
                              << ops << " ops";
  EXPECT_GT(ops, 10000u);
}

}  // namespace
}  // namespace bess::classifier
