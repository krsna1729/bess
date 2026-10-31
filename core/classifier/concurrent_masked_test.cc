// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/concurrent_masked.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <tuple>
#include <thread>
#include <vector>

#include "classifier/range_backend.h"
#include "framework/memory_scope.h"
#include "runtime/runtime_state.h"
#include "utils/dpdk_memory.h"
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
                                         bess::runtime::runtime().rcu());
  EXPECT_TRUE(t.has_value()) << t.error();
  return std::move(*t);
}

// The gate of one key, or -1 on a miss. The rule id a Match also carries is
// what a module with an action resource reads its record for (D-141).
int Classify(const ConcurrentMaskedTable &t, uint64_t key) {
  const Bytes k = B(key);
  ConcurrentMaskedTable::Match m{};
  return t.LookupBatch(C(k), kKeyLen, &m, 1) ? m.gate : -1;
}

// LookupBatch that keeps the gates and drops the rule ids: what these tests
// assert on. n must be <= kMaxBatch.
uint64_t LookupGates(const ConcurrentMaskedTable &t, ConstBytes keys,
                     size_t stride, uint16_t *out, size_t n) {
  std::array<ConcurrentMaskedTable::Match, ConcurrentMaskedTable::kMaxBatch> m;
  const uint64_t hits = t.LookupBatch(keys, stride, m.data(), n);
  for (size_t i = 0; i < n; i++) {
    if ((hits >> i) & 1) {
      out[i] = m[i].gate;
    }
  }
  return hits;
}

// A walk resumed one rule at a time returns every rule present throughout,
// though a higher-priority rule reorders the tuple list and a new mask adds
// a tuple between the steps: the walk goes by mask, not list position.
TEST(ConcurrentMaskedTableTest, ResumedWalkReturnsEveryStableRule) {
  auto t = MakeTable();
  const std::array<const Bytes *, 3> masks{&kFull, &kLow4, &kLow2};
  std::map<std::pair<uint64_t, uint64_t>, int> seen;  // (mask, value) -> visits
  for (const Bytes *mask : masks) {
    uint64_t m;
    std::memcpy(&m, mask->data(), kKeyLen);
    for (uint64_t v = 1; v <= 100; v++) {
      ASSERT_EQ(R::kInserted, t->Upsert(C(*mask), C(B(v & m)), 1, 1));
      seen[{m, v & m}] = 0;
    }
  }
  ConcurrentMaskedTable::WalkCursor cursor;
  int step = 0;
  bool more = true;
  while (more) {
    more = t->VisitFrom(&cursor, [&](ConstBytes mask, ConstBytes value,
                                     const ConcurrentMaskedTable::Rule &,
                                     ConcurrentMaskedTable::Ranges) {
      uint64_t m;
      uint64_t v;
      std::memcpy(&m, mask.data(), kKeyLen);
      std::memcpy(&v, value.data(), kKeyLen);
      seen[{m, v}]++;
      return false;
    });
    if (++step == 150) {
      ASSERT_EQ(R::kInserted, t->Upsert(C(kLow2), C(B(0xbeef)), 99, 2));  // reorders
      ASSERT_EQ(R::kInserted, t->Upsert(C(kLow1), C(B(0x7)), 1, 3));      // a new tuple
    }
  }
  for (const auto &[rule, visits] : seen) {
    EXPECT_GE(visits, 1) << std::hex << rule.first << "/" << rule.second;
  }
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

// The lookup skips a tuple only when every packet of the batch already holds a
// strictly higher-priority match (D-098): an equal priority in a later-visited
// mask still wins as the later command, a higher one in a mask with a lower
// ceiling still wins, priorities beyond the packed range rank with packed ones,
// and one unmatched packet keeps every tuple looked up.
TEST(ConcurrentMaskedTableTest, SkipsOnlyTuplesThatCannotWin) {
  auto t = MakeTable();
  // Batch of 4 keys sharing the low byte 0x07.
  const std::array<uint64_t, 4> keys = {0x1107, 0x2207, 0x3307, 0x4407};
  ASSERT_EQ(R::kInserted, t->Upsert(C(kLow1), C(B(0x07)), 50, 1));  // matches all: ceiling 50
  auto classify_all = [&](std::array<uint16_t, 4> &out) {
    std::array<Bytes, 4> k;
    for (size_t i = 0; i < 4; i++) k[i] = B(keys[i]);
    return LookupGates(*t, ConstBytes(k[0].data(), 4 * kKeyLen), kKeyLen,
                       out.data(), 4);
  };
  std::array<uint16_t, 4> out{};
  // A lower-priority, more specific rule cannot win: skipping it is correct.
  ASSERT_EQ(R::kInserted, t->Upsert(C(kLow2), C(B(0x1107)), 10, 2));
  ASSERT_EQ(0xfu, classify_all(out));
  EXPECT_EQ(1, out[0]);
  // An equal priority added later, in another mask, wins for its key.
  ASSERT_EQ(R::kInserted, t->Upsert(C(kLow4), C(B(0x2207)), 50, 3));
  ASSERT_EQ(0xfu, classify_all(out));
  EXPECT_EQ(3, out[1]);
  EXPECT_EQ(1, out[0]);
  // A higher priority beyond the packed range wins over a packed 50.
  ASSERT_EQ(R::kInserted, t->Upsert(C(kFull), C(B(0x3307)), 1'000'000, 4));
  ASSERT_EQ(0xfu, classify_all(out));
  EXPECT_EQ(4, out[2]);
  // A packed priority above an unpacked negative one.
  ASSERT_EQ(R::kUpdated, t->Upsert(C(kLow1), C(B(0x07)), -1'000'000, 1));
  ASSERT_EQ(R::kInserted, t->Upsert(C(kLow2), C(B(0x4407)), -5, 5));
  ASSERT_EQ(0xfu, classify_all(out));
  EXPECT_EQ(5, out[3]);
  EXPECT_EQ(2, out[0]) << "priority 10 beats the now -1,000,000 wide rule";
}

// Against a brute-force model: random masks, values, priorities (inside and
// outside the packed range, with ties), updates and deletes, classified in
// batches of up to 64.
TEST(ConcurrentMaskedTableTest, RandomRulesMatchABruteForceModel) {
  for (uint32_t seed = 1; seed <= 6; seed++) {
    std::mt19937_64 rng(seed);
    auto t = MakeTable(8);
    struct ModelRule { uint64_t mask, value; int64_t priority; uint64_t seq; uint16_t result; };
    std::vector<ModelRule> model;
    uint64_t seq = 0;
    const std::array<uint64_t, 6> masks = {~uint64_t{0}, 0xffffffffull, 0xffffull, 0xffull,
                                           0xff00ffull, 0xf0f0ull};
    for (int step = 0; step < 400; step++) {
      const uint64_t mask = masks[rng() % masks.size()];
      const uint64_t value = (rng() % 4) * 0x0101010101010101ull & mask;
      if (rng() % 5 == 0) {
        auto it = std::find_if(model.begin(), model.end(),
                               [&](const ModelRule &r) { return r.mask == mask && r.value == value; });
        EXPECT_EQ(it != model.end(), t->Erase(C(B(mask)), C(B(value))));
        if (it != model.end()) model.erase(it);
        continue;
      }
      const int64_t priority = rng() % 3 == 0 ? static_cast<int64_t>(rng() % 200000) - 100000
                                              : static_cast<int64_t>(rng() % 8);
      const uint16_t result = static_cast<uint16_t>(rng() % 1000);
      const auto r = t->Upsert(C(B(mask)), C(B(value)), priority, result);
      ASSERT_TRUE(r == R::kInserted || r == R::kUpdated);
      auto it = std::find_if(model.begin(), model.end(),
                             [&](const ModelRule &m) { return m.mask == mask && m.value == value; });
      if (it != model.end()) model.erase(it);
      model.push_back({mask, value, priority, ++seq, result});
      // A batch of keys built from the rules' values (to hit) and random ones.
      const size_t n = 1 + rng() % 64;
      std::vector<Bytes> keys(n);
      for (size_t i = 0; i < n; i++) {
        keys[i] = B((rng() % 4) * 0x0101010101010101ull ^ (rng() % 3 == 0 ? rng() : 0));
      }
      std::vector<uint16_t> out(n, 0xEEEE);
      const uint64_t hits = LookupGates(*t, ConstBytes(keys[0].data(), n * kKeyLen), kKeyLen,
                                        out.data(), n);
      for (size_t i = 0; i < n; i++) {
        uint64_t key;
        std::memcpy(&key, keys[i].data(), kKeyLen);
        const ModelRule *best = nullptr;
        for (const ModelRule &m : model) {
          if ((key & m.mask) == m.value &&
              (best == nullptr || m.priority > best->priority ||
               (m.priority == best->priority && m.seq > best->seq))) {
            best = &m;
          }
        }
        ASSERT_EQ(best != nullptr, ((hits >> i) & 1) != 0) << "seed " << seed << " step " << step;
        if (best != nullptr) {
          ASSERT_EQ(best->result, out[i]) << "seed " << seed << " step " << step << " key " << i;
        }
      }
    }
  }
}

// Against a brute-force model at key widths covering every tail of the
// fixed-width mask and hash kernels (8-, 4-, 2- and 1-byte steps; D-124): up
// to 12 masks; priorities with ties and beyond the packed range;
// replacements, deletes, and a pending (prepared, uncommitted) rule; batches
// of 1-64 keys, some at a stride wider than the key, in buffers ending
// exactly at the last key (an over-read is an ASan error).
TEST(ConcurrentMaskedTableTest, EveryKeyWidthMatchesABruteForceModel) {
  using Key = std::vector<std::byte>;
  struct ModelRule {
    Key mask, value;
    int64_t priority;
    uint64_t seq;
    uint16_t result;
  };
  static constexpr std::array<uint8_t, 4> kMaskBytes = {0x00, 0xff, 0xf0, 0x3c};
  for (const size_t width : std::array<size_t, 14>{1, 2, 3, 4, 5, 6, 7, 8, 13,
                                                   16, 17, 24, 37, 64}) {
    for (uint32_t seed = 1; seed <= 3; seed++) {
      std::mt19937_64 rng(seed * 1000 + width);
      auto created = ConcurrentMaskedTable::Create(
          static_cast<uint32_t>(width), 12, bess::runtime::runtime().rcu());
      ASSERT_TRUE(created.has_value()) << created.error();
      ConcurrentMaskedTable &t = **created;

      std::vector<Key> masks(12, Key(width));
      for (Key &m : masks) {
        for (std::byte &b : m) b = std::byte{kMaskBytes[rng() % 4]};
      }
      auto value_byte = [&] {
        return std::byte{static_cast<uint8_t>((rng() % 3) * 0x55)};
      };
      std::vector<ModelRule> model;
      uint64_t seq = 0;
      auto find = [&](const Key &mask, const Key &value) {
        return std::find_if(model.begin(), model.end(), [&](const ModelRule &r) {
          return r.mask == mask && r.value == value;
        });
      };
      for (int step = 0; step < 300; step++) {
        const Key &mask = masks[rng() % masks.size()];
        Key value(width);
        for (size_t b = 0; b < width; b++) value[b] = value_byte() & mask[b];
        const ConstBytes cm(mask.data(), width), cv(value.data(), width);
        if (rng() % 5 == 0) {
          auto it = find(mask, value);
          ASSERT_EQ(it != model.end(), t.Erase(cm, cv));
          if (it != model.end()) model.erase(it);
        } else {
          const int64_t priority =
              rng() % 3 == 0 ? static_cast<int64_t>(rng() % 200000) - 100000
                             : static_cast<int64_t>(rng() % 8);
          const auto result = static_cast<uint16_t>(rng() % 1000);
          const auto r = t.Upsert(cm, cv, priority, result);
          ASSERT_TRUE(r == R::kInserted || r == R::kUpdated);
          auto it = find(mask, value);
          if (it != model.end()) model.erase(it);
          model.push_back({mask, value, priority, ++seq, result});
        }
        // Sometimes a prepared new rule: present in its tuple, never an
        // answer.
        std::optional<ConcurrentMaskedTable::Prepared> pending;
        if (rng() % 4 == 0) {
          Key pv(width);
          for (size_t b = 0; b < width; b++) pv[b] = value_byte() & mask[b];
          if (find(mask, pv) == model.end()) {
            auto p = t.PrepareUpsert(cm, ConstBytes(pv.data(), width),
                                     1'000'000, 7);
            ASSERT_TRUE(p.has_value());
            pending = std::move(*p);
          }
        }

        const size_t n = 1 + rng() % 64;
        const size_t stride = width + rng() % 3;
        std::vector<std::byte> keys((n - 1) * stride + width);
        for (std::byte &b : keys) {
          b = rng() % 8 == 0 ? static_cast<std::byte>(rng()) : value_byte();
        }
        std::vector<uint16_t> out(n, 0xEEEE);
        const uint64_t hits = LookupGates(t, ConstBytes(keys.data(), keys.size()),
                                          stride, out.data(), n);

        for (size_t i = 0; i < n; i++) {
          const std::byte *key = keys.data() + i * stride;
          const ModelRule *best = nullptr;
          for (const ModelRule &m : model) {
            bool match = true;
            for (size_t b = 0; b < width; b++) {
              match = match && (key[b] & m.mask[b]) == m.value[b];
            }
            if (match && (best == nullptr || m.priority > best->priority ||
                          (m.priority == best->priority && m.seq > best->seq))) {
              best = &m;
            }
          }
          ASSERT_EQ(best != nullptr, ((hits >> i) & 1) != 0)
              << "width " << width << " seed " << seed << " step " << step;
          if (best != nullptr) {
            ASSERT_EQ(best->result, out[i])
                << "width " << width << " seed " << seed << " step " << step;
          } else {
            ASSERT_EQ(0xEEEE, out[i]) << "a miss leaves its slot untouched";
          }
        }
        if (pending) t.Cancel(*pending);
      }
    }
  }
}

// The transactional split (D-024): a prepared new rule is present but loses
// to every rule and alone reads as a miss; a prepared replacement leaves the
// old rule answering; commit switches in one store; cancel undoes; the ids a
// commit frees come back through Recycle(), which also drops emptied masks.
TEST(ConcurrentMaskedTableTest, PrepareCommitCancelRecycle) {
  auto t = MakeTable();
  ASSERT_EQ(R::kInserted, t->Upsert(C(kLow1), C(B(0x07)), 1, 10));
  const size_t base_ids = t->ids_in_use();

  // A new, higher-priority rule for 0x0707 in a new mask: prepared, it
  // neither outranks the existing low-priority match nor answers alone.
  auto wide = t->PrepareUpsert(C(kLow2), C(B(0x0707)), 100, 20);
  auto alone = t->PrepareUpsert(C(kLow2), C(B(0x0909)), 5, 30);
  ASSERT_TRUE(wide && alone);
  EXPECT_EQ(Classify(*t, 0x0707), 10);
  EXPECT_EQ(Classify(*t, 0x0909), -1);
  EXPECT_EQ(t->size(), 1u);
  EXPECT_FALSE(t->FindRule(C(kLow2), C(B(0x0707))).has_value());

  EXPECT_EQ(t->Commit(*wide), 0u);
  EXPECT_EQ(Classify(*t, 0x0707), 20);
  t->Cancel(*alone);
  EXPECT_EQ(Classify(*t, 0x0909), -1);
  EXPECT_EQ(t->size(), 2u);

  // A replacement: the old rule answers until the commit, which frees its id.
  auto regate = t->PrepareUpsert(C(kLow2), C(B(0x0707)), 100, 21);
  ASSERT_TRUE(regate);
  EXPECT_EQ(Classify(*t, 0x0707), 20);
  const auto freed = t->Commit(*regate);
  EXPECT_NE(freed, 0u);
  EXPECT_EQ(Classify(*t, 0x0707), 21);
  t->Recycle(freed);

  // Many replace cycles do not grow the ids in use.
  const size_t steady = t->ids_in_use();
  for (int i = 0; i < 3000; i++) {
    auto p = t->PrepareUpsert(C(kLow2), C(B(0x0707)), 100,
                              static_cast<uint16_t>(i % 50));
    ASSERT_TRUE(p);
    t->Recycle(t->Commit(*p));
  }
  EXPECT_EQ(t->ids_in_use(), steady);

  // Erasing the mask's last rule leaves an empty tuple until Recycle().
  auto erase = t->PrepareErase(C(kLow2), C(B(0x0707)));
  ASSERT_TRUE(erase);
  EXPECT_FALSE(t->PrepareErase(C(kLow2), C(B(0x0808))).has_value());
  EXPECT_EQ(t->tuple_count(), 2u);
  const auto erased = t->Commit(*erase);
  EXPECT_EQ(Classify(*t, 0x0707), 10);
  EXPECT_EQ(t->tuple_count(), 2u);
  t->Recycle(erased);
  EXPECT_EQ(t->tuple_count(), 1u);
  EXPECT_EQ(t->size(), 1u);
  EXPECT_EQ(t->ids_in_use(), base_ids + 1);  // + the pending record
  EXPECT_EQ(R::kReservedResult,
            t->Upsert(C(kLow1), C(B(0x08)), 1,
                      ConcurrentMaskedTable::kPendingResult));
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
  t->ForEach([&](ConstBytes mask, ConstBytes, const auto &, auto) {
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
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
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
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
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
          const uint64_t hits = LookupGates(
              *t, ConstBytes(packed.data() + base * kKeyLen, 32 * kKeyLen),
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
  // At least 500 ms and 10000 operations; see concurrent_exact_test.cc.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline || ops <= 10000u) {
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


// -- ranges (D-186) -----------------------------------------------------------
//
// These keys are WildcardMatch's packing of fields of 4, 2 and 2 bytes: an
// address, then two big-endian ports. Prefixes are on the address, ranges on
// the ports.

using Range = ConcurrentMaskedTable::KeyRange;
using RangeList = std::vector<Range>;

Bytes Key(uint32_t addr, uint16_t sport, uint16_t dport) {
  Bytes k{};
  for (int b = 0; b < 4; b++) {
    k[b] = std::byte{static_cast<uint8_t>(addr >> (24 - 8 * b))};
  }
  k[4] = std::byte{static_cast<uint8_t>(sport >> 8)};
  k[5] = std::byte{static_cast<uint8_t>(sport)};
  k[6] = std::byte{static_cast<uint8_t>(dport >> 8)};
  k[7] = std::byte{static_cast<uint8_t>(dport)};
  return k;
}
Bytes Prefix(int len) {
  return Key(len == 0 ? 0 : ~uint32_t{0} << (32 - len), 0, 0);
}
Range Sport(uint64_t low, uint64_t high) { return {4, 2, low, high}; }
Range Dport(uint64_t low, uint64_t high) { return {6, 2, low, high}; }

int ClassifyKey(const ConcurrentMaskedTable &t, const Bytes &k) {
  ConcurrentMaskedTable::Match m{};
  return t.LookupBatch(C(k), kKeyLen, &m, 1) ? m.gate : -1;
}

// Rules that share a (mask, value) and differ in their ranges are distinct
// rules under one key: inside their ranges they rank like any rules (priority,
// then the later command), outside them they do not match, an unranged rule of
// the same key is one more of them, and replacing, erasing and erasing the last
// rule behave as they do for masks alone.
TEST(ConcurrentMaskedTableRangeTest, RulesOfOneKeyRankByPriorityThenCommand) {
  auto t = MakeTable();
  const Bytes m24 = Prefix(24), net = Key(0x0a000100, 0, 0);
  const auto at = [&](uint16_t dport) {
    return ClassifyKey(*t, Key(0x0a000105, 1000, dport));
  };
  const RangeList low = {Dport(100, 199)}, high = {Dport(150, 299)};
  ASSERT_EQ(R::kInserted, t->Upsert(C(m24), C(net), 5, 1, 0, low));
  ASSERT_EQ(R::kInserted, t->Upsert(C(m24), C(net), 5, 2, 0, high));
  EXPECT_EQ(-1, at(99));
  EXPECT_EQ(1, at(100));
  EXPECT_EQ(1, at(149));
  EXPECT_EQ(2, at(150)) << "overlap at equal priority: the later command";
  EXPECT_EQ(2, at(299));
  EXPECT_EQ(-1, at(300));
  EXPECT_EQ(-1, ClassifyKey(*t, Key(0x0a000205, 1000, 120))) << "outside the prefix";

  const RangeList narrow = {Dport(160, 170)};
  ASSERT_EQ(R::kInserted, t->Upsert(C(m24), C(net), 9, 3, 0, narrow));
  EXPECT_EQ(2, at(159));
  EXPECT_EQ(3, at(160));
  EXPECT_EQ(3, at(170));
  EXPECT_EQ(2, at(171));

  ASSERT_EQ(R::kInserted, t->Upsert(C(m24), C(net), 1, 4));
  EXPECT_EQ(4, at(99)) << "the key's unranged rule";
  EXPECT_EQ(1, at(120));
  EXPECT_EQ(4u, t->size());
  EXPECT_EQ(1u, t->tuple_count()) << "a range costs no tuple";

  // The same (mask, value, ranges) again replaces the rule, as the later
  // command.
  ASSERT_EQ(R::kUpdated, t->Upsert(C(m24), C(net), 5, 5, 0, low));
  EXPECT_EQ(5, at(150));
  EXPECT_EQ(5, at(120));
  EXPECT_EQ(2, at(200));
  EXPECT_EQ(4u, t->size());
  ASSERT_TRUE(t->FindRule(C(m24), C(net), low).has_value());
  EXPECT_EQ(5, t->FindRule(C(m24), C(net), low)->gate);
  EXPECT_EQ(4, t->FindRule(C(m24), C(net))->gate);
  EXPECT_FALSE(t->FindRule(C(m24), C(net), RangeList{Dport(100, 198)}).has_value());

  EXPECT_FALSE(t->Erase(C(m24), C(net), RangeList{Dport(100, 198)}));
  ASSERT_TRUE(t->Erase(C(m24), C(net), high));
  EXPECT_EQ(4, at(250));
  EXPECT_EQ(5, at(150));
  EXPECT_EQ(3, at(165));
  ASSERT_TRUE(t->Erase(C(m24), C(net), low));
  ASSERT_TRUE(t->Erase(C(m24), C(net), narrow));
  EXPECT_EQ(4, at(165));
  EXPECT_EQ(1u, t->size());
  ASSERT_TRUE(t->Erase(C(m24), C(net)));
  EXPECT_EQ(-1, at(99));
  EXPECT_EQ(0u, t->size());
  EXPECT_EQ(0u, t->tuple_count()) << "the key's last rule took the tuple";

  // A key whose last rule is ranged goes the same way.
  ASSERT_EQ(R::kInserted, t->Upsert(C(m24), C(net), 5, 1, 0, low));
  ASSERT_TRUE(t->Erase(C(m24), C(net), low));
  EXPECT_EQ(-1, at(120));
  EXPECT_EQ(0u, t->size());
  EXPECT_EQ(0u, t->tuple_count());
}

// Every range of a rule must hold, each read from its own bytes, and the
// bounds are inclusive.
TEST(ConcurrentMaskedTableRangeTest, EveryRangeOfARuleMustHold) {
  auto t = MakeTable();
  const RangeList both = {Sport(1000, 1999), Dport(80, 80)};
  ASSERT_EQ(R::kInserted, t->Upsert(C(Prefix(0)), C(Prefix(0)), 1, 1, 0, both));
  EXPECT_EQ(1, ClassifyKey(*t, Key(1, 1000, 80)));
  EXPECT_EQ(1, ClassifyKey(*t, Key(2, 1999, 80)));
  EXPECT_EQ(-1, ClassifyKey(*t, Key(1, 999, 80)));
  EXPECT_EQ(-1, ClassifyKey(*t, Key(1, 2000, 80)));
  EXPECT_EQ(-1, ClassifyKey(*t, Key(1, 1500, 79)));
  EXPECT_EQ(-1, ClassifyKey(*t, Key(1, 1500, 81)));
}

// A ranged rule competes with other tuples as any rule does: a better rule in
// another mask wins, a worse one loses inside the range and answers outside it
// (a key whose ranges all miss contributes nothing from its tuple), in one
// batch.
TEST(ConcurrentMaskedTableRangeTest, RangedRuleRanksAcrossTuples) {
  auto t = MakeTable();
  const Bytes host = Key(0x0a000105, 0, 0), net16 = Key(0x0a000000, 0, 0);
  ASSERT_EQ(R::kInserted,
            t->Upsert(C(Prefix(32)), C(host), 5, 1, 0, RangeList{Dport(100, 199)}));
  ASSERT_EQ(R::kInserted, t->Upsert(C(Prefix(16)), C(net16), 10, 2));
  EXPECT_EQ(2, ClassifyKey(*t, Key(0x0a000105, 7, 150)));
  ASSERT_EQ(R::kUpdated, t->Upsert(C(Prefix(16)), C(net16), 1, 2));

  const std::array<Bytes, 4> keys = {Key(0x0a000105, 7, 150), Key(0x0a000105, 7, 250),
                                     Key(0x0a000201, 7, 150), Key(0x0b000105, 7, 150)};
  std::array<uint16_t, 4> out{};
  EXPECT_EQ(0x7u, LookupGates(*t, ConstBytes(keys[0].data(), 4 * kKeyLen), kKeyLen,
                              out.data(), 4));
  EXPECT_EQ(1, out[0]) << "inside the range the ranged rule outranks the /16";
  EXPECT_EQ(2, out[1]) << "outside it the /16 answers";
  EXPECT_EQ(2, out[2]);
}

// A range the table cannot hold exactly is refused, not widened, and leaves
// nothing behind; one value (low == high) is a range.
TEST(ConcurrentMaskedTableRangeTest, RefusesNonCanonicalRanges) {
  auto t = MakeTable();
  const Bytes m24 = Prefix(24), net = Key(0x0a000100, 0, 0);
  const std::vector<RangeList> bad = {
      {Range{6, 0, 1, 2}},               // no width
      {Range{0, 9, 1, 2}},               // wider than 8 bytes
      {Range{7, 2, 1, 2}},               // past the key
      {Dport(200, 100)},                 // empty
      {Range{6, 2, 1, 0x10000}},         // beyond the field
      {Dport(0, 0xffff)},                // the whole field: no range
      {Range{2, 2, 1, 2}},               // under the mask
      {Dport(1, 2), Sport(1, 2)},        // out of order
      {Range{4, 3, 1, 2}, Dport(1, 2)},  // overlapping
  };
  for (const RangeList &ranges : bad) {
    EXPECT_EQ(R::kBadRange, t->Upsert(C(m24), C(net), 1, 1, 0, ranges));
    EXPECT_FALSE(t->PrepareUpsert(C(m24), C(net), 1, 1, 0, ranges).has_value());
  }
  EXPECT_EQ(0u, t->size());
  EXPECT_EQ(0u, t->tuple_count());
  ASSERT_EQ(R::kInserted, t->Upsert(C(m24), C(net), 1, 1, 0, RangeList{Dport(80, 80)}));
  EXPECT_EQ(1, ClassifyKey(*t, Key(0x0a000101, 0, 80)));
  EXPECT_EQ(-1, ClassifyKey(*t, Key(0x0a000101, 0, 81)));
}

// A replaced chain is freed only after a grace period: with a reader online
// since before the swaps, every replaced chain is still held; once it is
// quiescent, the next write frees them. A key back to one unranged rule holds
// no chain, so replacing that rule retires none.
TEST(ConcurrentMaskedTableRangeTest, ReplacedChainWaitsForOnlineReaders) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  auto t = MakeTable();
  const Bytes m24 = Prefix(24), net = Key(0x0a000100, 0, 0);
  const RangeList a = {Dport(1, 10)}, b = {Dport(5, 20)};

  constexpr uint32_t kReader = 28;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  domain.Online(kReader);
  ASSERT_EQ(R::kInserted, t->Upsert(C(m24), C(net), 1, 1));
  ASSERT_EQ(R::kUpdated, t->Upsert(C(m24), C(net), 1, 2));
  EXPECT_EQ(0u, t->retiring_chains()) << "one unranged rule is no chain";
  ASSERT_EQ(R::kInserted, t->Upsert(C(m24), C(net), 2, 3, 0, a));
  EXPECT_EQ(0u, t->retiring_chains());
  ASSERT_EQ(R::kInserted, t->Upsert(C(m24), C(net), 3, 4, 0, b));
  EXPECT_EQ(1u, t->retiring_chains());
  ASSERT_TRUE(t->Erase(C(m24), C(net), a));
  ASSERT_TRUE(t->Erase(C(m24), C(net), b));
  EXPECT_EQ(3u, t->retiring_chains())
      << "a chain was freed while a reader was mid-grace-period";
  ASSERT_EQ(R::kUpdated, t->Upsert(C(m24), C(net), 1, 5));
  EXPECT_EQ(3u, t->retiring_chains()) << "back to one unranged rule: no chain";
  EXPECT_EQ(5, ClassifyKey(*t, Key(0x0a000101, 0, 7)));

  domain.Quiescent(kReader);
  ASSERT_EQ(R::kInserted, t->Upsert(C(Prefix(32)), C(Key(1, 0, 0)), 0, 6));
  EXPECT_EQ(0u, t->retiring_chains()) << "retired chains never came back";
  domain.Offline(kReader);
  domain.Unregister(kReader);
}

// Readers never miss a stable ranged rule while the chain of its own key is
// rewritten underneath them: lower-priority rules with overlapping ranges come
// and go on the same key (each change swaps in a new chain and retires the
// old one), directly and through prepare/commit, and the key's unranged rule
// is added and erased.
TEST(ConcurrentMaskedTableRangeTest, ConcurrentReadersSeeTheStableRangedWinner) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  auto t = MakeTable();
  const Bytes m24 = Prefix(24), net = Key(0x0a000100, 0, 0);
  const RangeList stable = {Dport(1000, 1999)};
  ASSERT_EQ(R::kInserted, t->Upsert(C(m24), C(net), 100, 7, 0, stable));
  std::vector<Bytes> keys;
  for (uint16_t i = 0; i < 32; i++) {
    keys.push_back(Key(0x0a000100 | i, i, static_cast<uint16_t>(1000 + i * 31)));
  }

  constexpr int kReaders = 2;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> wrong{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; r++) {
    const uint32_t reader_id = 29 + r;
    ASSERT_TRUE(domain.Register(reader_id).has_value());
    readers.emplace_back([&, reader_id] {
      domain.Online(reader_id);
      while (!stop.load(std::memory_order_relaxed)) {
        uint16_t results[32];
        const uint64_t hits =
            LookupGates(*t, ConstBytes(keys[0].data(), 32 * kKeyLen), kKeyLen, results, 32);
        if (hits != 0xffffffffull) {
          wrong++;
        } else {
          for (const uint16_t g : results) wrong += g != 7;
        }
        domain.Quiescent(reader_id);
      }
      domain.Offline(reader_id);
    });
  }

  std::mt19937_64 rng(0x7a9e);
  std::vector<RangeList> live;
  uint64_t ops = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline || ops <= 10000u) {
    const uint64_t low = rng() % 2500;
    const RangeList ranges = {Dport(low, low + rng() % 600)};
    const bool present = std::find(live.begin(), live.end(), ranges) != live.end();
    switch (rng() % 4) {
      case 0:
        if (!live.empty()) {
          const size_t at = rng() % live.size();
          ASSERT_TRUE(t->Erase(C(m24), C(net), live[at]));
          live[at] = live.back();
          live.pop_back();
        }
        break;
      case 1: {
        // Through the transactional split, one commit per change.
        auto p = t->PrepareUpsert(C(m24), C(net), static_cast<int64_t>(rng() % 100), 9, 0,
                                  ranges);
        ASSERT_TRUE(p.has_value());
        if (const auto freed = t->Commit(*p); freed != 0) t->Recycle(freed);
        if (!present) live.push_back(ranges);
        break;
      }
      case 2:
        if (t->FindRule(C(m24), C(net)).has_value()) {
          ASSERT_TRUE(t->Erase(C(m24), C(net)));
        } else {
          ASSERT_EQ(R::kInserted, t->Upsert(C(m24), C(net), 50, 9));
        }
        break;
      default:
        ASSERT_TRUE(R::kInserted ==
                        t->Upsert(C(m24), C(net), static_cast<int64_t>(rng() % 100), 9, 0,
                                  ranges) ||
                    present);
        if (!present) live.push_back(ranges);
        break;
    }
    ops++;
  }
  stop = true;
  for (auto &th : readers) th.join();
  for (int r = 0; r < kReaders; r++) domain.Unregister(29 + r);
  EXPECT_EQ(0u, wrong.load()) << ops << " ops";
}

// -- transactions over one key (D-024, D-186) ----------------------------------

struct Change {
  bool erase;
  Bytes mask, value;
  RangeList ranges;
  int64_t priority = 0;
  uint16_t gate = 0;
};
using Identity = std::tuple<Bytes, Bytes, std::vector<std::array<uint64_t, 4>>>;
Identity IdentityOf(const Bytes &mask, const Bytes &value, const RangeList &ranges) {
  std::vector<std::array<uint64_t, 4>> r;
  for (const Range &range : ranges) {
    r.push_back({range.offset, range.width, range.low, range.high});
  }
  return {mask, value, r};
}
struct ModelRule {
  RangeList ranges;
  int64_t priority;
  uint64_t seq;
  uint16_t gate;
};
using Model = std::map<Identity, ModelRule>;

bool RuleMatches(const Identity &id, const ModelRule &rule, const Bytes &key) {
  for (size_t b = 0; b < kKeyLen; b++) {
    if ((key[b] & std::get<0>(id)[b]) != std::get<1>(id)[b]) {
      return false;
    }
  }
  for (const Range &r : rule.ranges) {
    uint64_t v = 0;
    for (uint32_t b = 0; b < r.width; b++) {
      v = (v << 8) | static_cast<uint8_t>(key[r.offset + b]);
    }
    if (v < r.low || v > r.high) {
      return false;
    }
  }
  return true;
}
int ModelClassify(const Model &model, const Bytes &key) {
  const ModelRule *best = nullptr;
  for (const auto &[id, rule] : model) {
    if (RuleMatches(id, rule, key) &&
        (best == nullptr || rule.priority > best->priority ||
         (rule.priority == best->priority && rule.seq > best->seq))) {
      best = &rule;
    }
  }
  return best == nullptr ? -1 : best->gate;
}
// Every rule the table visits, as (identity, priority, gate).
std::vector<std::tuple<Identity, int64_t, uint16_t>> Snapshot(const ConcurrentMaskedTable &t) {
  std::vector<std::tuple<Identity, int64_t, uint16_t>> out;
  t.ForEach([&](ConstBytes mask, ConstBytes value, const ConcurrentMaskedTable::Rule &rule,
                ConcurrentMaskedTable::Ranges ranges) {
    Bytes m, v;
    std::memcpy(m.data(), mask.data(), kKeyLen);
    std::memcpy(v.data(), value.data(), kKeyLen);
    out.emplace_back(IdentityOf(m, v, RangeList(ranges.begin(), ranges.end())), rule.priority,
                     rule.gate);
  });
  std::sort(out.begin(), out.end());
  return out;
}

// Changes to one key prepared together (new ranged rules, a replacement, an
// erase of its unranged rule, of every rule) committed in every order, with
// every subset cancelled: prepared rules never answer, the table ends as if
// only the committed changes were applied whatever the order, and cancelling
// every change leaves the table exactly as it was.
void CommitInEveryOrder(const std::vector<Change> &base, const std::vector<Change> &changes,
                        const std::vector<Bytes> &probes) {
  const size_t n = changes.size();
  for (uint32_t cancelled = 0; cancelled < (1u << n); cancelled++) {
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    do {
      SCOPED_TRACE(::testing::Message() << "cancelled " << cancelled << " order "
                                        << ::testing::PrintToString(order));
      auto t = MakeTable();
      Model model;
      uint64_t seq = 0;
      for (const Change &c : base) {
        ASSERT_EQ(R::kInserted,
                  t->Upsert(C(c.mask), C(c.value), c.priority, c.gate, 0, c.ranges));
        model[IdentityOf(c.mask, c.value, c.ranges)] = {c.ranges, c.priority, ++seq, c.gate};
      }
      const auto before = Snapshot(*t);
      std::vector<ConcurrentMaskedTable::Prepared> prepared;
      std::vector<uint64_t> seqs;
      for (const Change &c : changes) {
        if (c.erase) {
          auto p = t->PrepareErase(C(c.mask), C(c.value), c.ranges);
          ASSERT_TRUE(p.has_value());
          prepared.push_back(std::move(*p));
          seqs.push_back(0);
        } else {
          auto p = t->PrepareUpsert(C(c.mask), C(c.value), c.priority, c.gate, 0, c.ranges);
          ASSERT_TRUE(p.has_value());
          prepared.push_back(std::move(*p));
          seqs.push_back(++seq);
        }
      }
      for (const Bytes &k : probes) {
        ASSERT_EQ(ModelClassify(model, k), ClassifyKey(*t, k)) << "a prepared change answered";
      }
      std::vector<ConcurrentMaskedTable::RuleId> freed;
      for (const size_t i : order) {
        if ((cancelled >> i) & 1) {
          t->Cancel(prepared[i]);
          continue;
        }
        freed.push_back(t->Commit(prepared[i]));
        const Change &c = changes[i];
        const Identity id = IdentityOf(c.mask, c.value, c.ranges);
        if (c.erase) {
          model.erase(id);
        } else {
          model[id] = {c.ranges, c.priority, seqs[i], c.gate};
        }
        for (const Bytes &k : probes) {
          ASSERT_EQ(ModelClassify(model, k), ClassifyKey(*t, k));
        }
      }
      for (const ConcurrentMaskedTable::RuleId id : freed) {
        if (id != 0) {
          t->Recycle(id);
        }
      }
      EXPECT_EQ(model.size(), t->size());
      for (const auto &[id, rule] : model) {
        const auto found = t->FindRule(C(std::get<0>(id)), C(std::get<1>(id)), rule.ranges);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(rule.gate, found->gate);
      }
      if (cancelled == (1u << n) - 1) {
        EXPECT_EQ(before, Snapshot(*t)) << "cancelling every change changed the table";
      }
      EXPECT_EQ(model.size(), Snapshot(*t).size()) << "a pending entry was left behind";
    } while (std::next_permutation(order.begin(), order.end()));
  }
}

std::vector<Bytes> PortProbes(uint32_t addr) {
  std::vector<Bytes> probes;
  for (const uint16_t dport : {0, 1, 99, 100, 149, 150, 199, 200, 249, 250}) {
    for (const uint16_t sport : {9, 10, 20, 21}) {
      probes.push_back(Key(addr, sport, dport));
    }
  }
  probes.push_back(Key(addr ^ 0x01000000, 10, 150));
  return probes;
}

TEST(ConcurrentMaskedTableRangeTest, ChangesToOneKeyCommitInAnyOrder) {
  const Bytes m24 = Prefix(24), net = Key(0x0a000100, 0, 0);
  CommitInEveryOrder(
      {{false, m24, net, {}, 1, 1}, {false, m24, net, {Dport(100, 199)}, 5, 2}},
      {{false, m24, net, {Dport(150, 249)}, 7, 3},
       {false, m24, net, {Dport(100, 199)}, 5, 4},
       {true, m24, net, {}},
       {false, m24, net, {Sport(10, 20)}, 2, 5}},
      PortProbes(0x0a000107));
}

TEST(ConcurrentMaskedTableRangeTest, NewKeyChangesCommitInAnyOrder) {
  const Bytes m24 = Prefix(24), net = Key(0x0a000100, 0, 0);
  CommitInEveryOrder({{false, Prefix(16), Key(0x0a000000, 0, 0), {}, 0, 9}},
                     {{false, m24, net, {Dport(1, 150)}, 3, 6},
                      {false, m24, net, {}, 0, 7},
                      {false, m24, net, {Sport(10, 10)}, 3, 8}},
                     PortProbes(0x0a000107));
}

TEST(ConcurrentMaskedTableRangeTest, ErasingEveryRuleOfAKeyCommitsInAnyOrder) {
  const Bytes m24 = Prefix(24), net = Key(0x0a000100, 0, 0);
  CommitInEveryOrder(
      {{false, m24, net, {Dport(100, 199)}, 5, 2}, {false, m24, net, {Dport(150, 249)}, 6, 3}},
      {{true, m24, net, {Dport(100, 199)}},
       {true, m24, net, {Dport(150, 249)}},
       {false, m24, net, {}, 0, 4}},
      PortProbes(0x0a000107));
}

// Against the scalar oracle (D-038's ScalarRangeBackend): random rules over
// five prefix lengths with source and destination port ranges that overlap,
// many sharing a (mask, value), priorities with ties and beyond the packed
// range; random upserts, replacements and erases, directly and as
// transactions that commit in random order and cancel some changes; random
// batches checked after every step, and while a transaction is prepared.
TEST(ConcurrentMaskedTableRangeTest, RandomRangedRulesMatchTheScalarOracle) {
  for (uint32_t seed = 1; seed <= 4; seed++) {
    SCOPED_TRACE(::testing::Message() << "seed " << seed);
    std::mt19937_64 rng(seed);
    auto t = MakeTable(8);
    Model model;
    uint64_t seq = 0;
    constexpr std::array<int, 5> kLens = {0, 8, 16, 24, 32};
    const auto random_identity = [&] {
      const Bytes mask = Prefix(kLens[rng() % kLens.size()]);
      Bytes value = Key(0x0a000000 | static_cast<uint32_t>(rng() % 3) << 8 |
                            static_cast<uint32_t>(rng() % 2),
                        0, 0);
      for (size_t b = 0; b < kKeyLen; b++) value[b] &= mask[b];
      RangeList ranges;
      const auto random_range = [&](Range (*make)(uint64_t, uint64_t)) {
        const uint64_t low = rng() % 60;
        ranges.push_back(make(low, low + rng() % 20));
      };
      const int shape = static_cast<int>(rng() % 4);
      if (shape & 1) random_range(Sport);
      if (shape & 2) random_range(Dport);
      return std::tuple{mask, value, ranges};
    };
    const auto random_priority = [&] {
      return rng() % 3 == 0 ? static_cast<int64_t>(rng() % 200000) - 100000
                            : static_cast<int64_t>(rng() % 8);
    };
    const auto check = [&](const Model &expected, int step) {
      std::vector<RangeRule<uint16_t>> rules;
      std::vector<std::pair<const Identity *, const ModelRule *>> by_seq;
      for (const auto &[id, rule] : expected) by_seq.emplace_back(&id, &rule);
      // Latest first: the oracle's stable priority sort then gives ties to
      // the later command, as the table does.
      std::sort(by_seq.begin(), by_seq.end(),
                [](const auto &a, const auto &b) { return a.second->seq > b.second->seq; });
      for (const auto &[id, rule] : by_seq) {
        RangeRule<uint16_t> r;
        r.mask.assign(std::get<0>(*id).begin(), std::get<0>(*id).end());
        r.value.assign(std::get<1>(*id).begin(), std::get<1>(*id).end());
        r.src_port_offset = 4;
        r.dst_port_offset = 6;
        for (const Range &range : rule->ranges) {
          (range.offset == 4 ? r.src_range : r.dst_range) =
              PortRange{static_cast<uint16_t>(range.low), static_cast<uint16_t>(range.high)};
        }
        r.priority = rule->priority;
        r.result = rule->gate;
        rules.push_back(std::move(r));
      }
      const ScalarRangeBackend<uint16_t> oracle(std::move(rules));
      const size_t n = 1 + rng() % 64;
      std::vector<Bytes> keys(n);
      for (Bytes &k : keys) {
        k = Key(0x0a000000 | static_cast<uint32_t>(rng() % 3) << 8 |
                    static_cast<uint32_t>(rng() % 3),
                static_cast<uint16_t>(rng() % 85), static_cast<uint16_t>(rng() % 85));
      }
      std::vector<uint16_t> want(n, 0), got(n, 0);
      const uint64_t want_hits = oracle.LookupBatch(
          ConstBytes(keys[0].data(), n * kKeyLen), kKeyLen, kKeyLen, std::span<uint16_t>(want));
      const uint64_t got_hits =
          LookupGates(*t, ConstBytes(keys[0].data(), n * kKeyLen), kKeyLen, got.data(), n);
      ASSERT_EQ(want_hits, got_hits) << "step " << step;
      for (size_t i = 0; i < n; i++) {
        if ((want_hits >> i) & 1) {
          ASSERT_EQ(want[i], got[i]) << "step " << step << " key " << i;
        }
      }
    };

    for (int step = 0; step < 400; step++) {
      const uint64_t op = rng() % 10;
      if (op < 2 && !model.empty()) {
        auto it = model.begin();
        std::advance(it, static_cast<ptrdiff_t>(rng() % model.size()));
        ASSERT_TRUE(t->Erase(C(std::get<0>(it->first)), C(std::get<1>(it->first)),
                             it->second.ranges));
        model.erase(it);
      } else if (op < 5) {
        // A transaction of up to five distinct rules, mostly on one key.
        std::vector<Change> changes;
        std::vector<Identity> ids;
        const auto [mask, value, unused] = random_identity();
        for (int k = 0, want = 1 + static_cast<int>(rng() % 5); k < want; k++) {
          auto [m, v, ranges] = random_identity();
          if (rng() % 4 != 0) {
            m = mask;
            v = value;
          }
          const Identity id = IdentityOf(m, v, ranges);
          if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
          ids.push_back(id);
          const bool erase = model.contains(id) && rng() % 3 == 0;
          changes.push_back({erase, m, v, ranges, random_priority(),
                             static_cast<uint16_t>(rng() % 1000)});
        }
        std::vector<ConcurrentMaskedTable::Prepared> prepared;
        std::vector<uint64_t> seqs;
        for (const Change &c : changes) {
          if (c.erase) {
            auto p = t->PrepareErase(C(c.mask), C(c.value), c.ranges);
            ASSERT_TRUE(p.has_value());
            prepared.push_back(std::move(*p));
            seqs.push_back(0);
          } else {
            auto p = t->PrepareUpsert(C(c.mask), C(c.value), c.priority, c.gate, 0, c.ranges);
            ASSERT_TRUE(p.has_value());
            prepared.push_back(std::move(*p));
            seqs.push_back(++seq);
          }
        }
        check(model, step);  // prepared changes never answer
        std::vector<size_t> order(changes.size());
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), rng);
        for (const size_t i : order) {
          if (rng() % 4 == 0) {
            t->Cancel(prepared[i]);
            continue;
          }
          const ConcurrentMaskedTable::RuleId freed = t->Commit(prepared[i]);
          const Change &c = changes[i];
          if (c.erase) {
            model.erase(IdentityOf(c.mask, c.value, c.ranges));
          } else {
            model[IdentityOf(c.mask, c.value, c.ranges)] = {c.ranges, c.priority, seqs[i],
                                                            c.gate};
          }
          if (freed != 0) t->Recycle(freed);
        }
      } else {
        const auto [mask, value, ranges] = random_identity();
        const int64_t priority = random_priority();
        const auto gate = static_cast<uint16_t>(rng() % 1000);
        const auto r = t->Upsert(C(mask), C(value), priority, gate, 0, ranges);
        const Identity id = IdentityOf(mask, value, ranges);
        ASSERT_EQ(model.contains(id) ? R::kUpdated : R::kInserted, r);
        model[id] = {ranges, priority, ++seq, gate};
      }
      ASSERT_EQ(model.size(), t->size()) << "step " << step;
      check(model, step);
    }
    // Every rule erased: no rule id, chain or tuple is left.
    const size_t base_ids = 1;  // the pending record
    for (const auto &[id, rule] : model) {
      ASSERT_TRUE(t->Erase(C(std::get<0>(id)), C(std::get<1>(id)), rule.ranges));
    }
    EXPECT_EQ(0u, t->size());
    EXPECT_EQ(0u, t->tuple_count());
    ASSERT_EQ(R::kInserted, t->Upsert(C(Prefix(32)), C(Key(1, 0, 0)), 0, 1));
    ASSERT_TRUE(t->Erase(C(Prefix(32)), C(Key(1, 0, 0))));
    EXPECT_EQ(0u, t->retiring_chains());
    EXPECT_LE(t->ids_in_use(), base_ids + t->retiring_ids());
  }
}

// -- memory (D-198) -----------------------------------------------------------

TEST(ConcurrentMaskedTableMemoryTest, BytesMatchesCreate) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  for (const size_t max_tuples : {size_t{1}, size_t{8}, size_t{64}}) {
    auto t = ConcurrentMaskedTable::Create(kKeyLen, max_tuples, domain);
    ASSERT_TRUE(t.has_value()) << t.error();
    EXPECT_EQ(bess::framework::MemoryCost<ConcurrentMaskedTable>::Bytes(
                  kKeyLen, max_tuples, domain),
              (*t)->bytes());
  }
}

// Admits every growth, records it, and refuses once `allow` growths passed.
struct RecordingGrowth final : bess::utils::MemoryGrowth {
  std::vector<size_t> grown;
  size_t net = 0, glibc_net = 0;
  size_t allow = SIZE_MAX;
  void Grow(size_t bytes, size_t glibc_bytes) override {
    if (grown.size() >= allow) {
      throw bess::utils::MemoryRefused(
          bess::utils::MemoryRefused::Reason::kNormalLimit, bytes, "refused");
    }
    grown.push_back(bytes);
    net += bytes;
    glibc_net += glibc_bytes;
  }
  void Shrink(size_t bytes, size_t glibc_bytes) noexcept override {
    net -= bytes;
    glibc_net -= glibc_bytes;
  }
};

TEST(ConcurrentMaskedTableMemoryTest, GrowthAdmitsTuplesChunksAndChains) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  auto t = MakeTable();
  RecordingGrowth growth;
  t->set_memory_growth(&growth);
  const size_t initial = t->bytes();

  // A new mask: its tuple table first, then the first record chunk.
  ASSERT_EQ(R::kInserted, t->Upsert(C(kLow4), C(B(1)), 1, 1));
  ASSERT_EQ(2u, growth.grown.size());
  EXPECT_EQ(ConcurrentExactTable::Bytes(
                kKeyLen, ConcurrentExactTable::CapacityFor(1), domain),
            growth.grown[0]);
  EXPECT_EQ(RecordArena::ChunkBytes(sizeof(ConcurrentMaskedTable::Rule)),
            growth.grown[1]);
  EXPECT_EQ(initial + growth.net, t->bytes());
  EXPECT_EQ(0u, growth.glibc_net);

  // A ranged rule under another key: its chain is glibc.
  const ConcurrentMaskedTable::KeyRange range{
      .offset = 4, .width = 2, .low = 10, .high = 20};
  ASSERT_EQ(R::kInserted,
            t->Upsert(C(kLow4), C(B(2)), 1, 2, 0,
                      ConcurrentMaskedTable::Ranges(&range, 1)));
  EXPECT_GT(growth.glibc_net, 0u);
  EXPECT_EQ(initial + growth.net, t->bytes());

  // Erasing every rule drops the tuple: its table is given back once the
  // tuple list holding it is reclaimed (no reader is online here).
  ASSERT_TRUE(t->Erase(C(kLow4), C(B(1))));
  ASSERT_TRUE(t->Erase(C(kLow4), C(B(2)),
                       ConcurrentMaskedTable::Ranges(&range, 1)));
  domain.Synchronize();
  domain.ReclaimReady();
  t->Recycle(0);  // frees the retired chain
  EXPECT_EQ(0u, t->tuple_count());
  EXPECT_EQ(0u, growth.glibc_net);
  EXPECT_EQ(RecordArena::ChunkBytes(sizeof(ConcurrentMaskedTable::Rule)),
            growth.net);  // record chunks stay with the table
  EXPECT_EQ(initial + growth.net, t->bytes());
}

TEST(ConcurrentMaskedTableMemoryTest, RefusedGrowthLeavesTheTableUnchanged) {
  auto t = MakeTable();
  RecordingGrowth growth;
  t->set_memory_growth(&growth);
  const size_t initial = t->bytes();

  // The tuple table refused.
  growth.allow = 0;
  EXPECT_THROW(t->Upsert(C(kLow4), C(B(1)), 1, 1), bess::utils::MemoryRefused);
  // The tuple table admitted, the record chunk refused: the table is given
  // back with nothing published.
  growth.allow = 1;
  EXPECT_THROW(t->Upsert(C(kLow4), C(B(1)), 1, 1), bess::utils::MemoryRefused);
  EXPECT_EQ(1u, growth.grown.size());
  EXPECT_EQ(0u, growth.net);
  EXPECT_EQ(initial, t->bytes());
  EXPECT_EQ(0u, t->size());
  EXPECT_EQ(0u, t->tuple_count());
  EXPECT_EQ(0u, t->ids_in_use());
  EXPECT_EQ(-1, Classify(*t, 1));

  // Admitted, it works as before.
  growth.allow = SIZE_MAX;
  ASSERT_EQ(R::kInserted, t->Upsert(C(kLow4), C(B(1)), 1, 1));
  EXPECT_EQ(1, Classify(*t, 1));
  EXPECT_EQ(initial + growth.net, t->bytes());
}

}  // namespace
}  // namespace bess::classifier
