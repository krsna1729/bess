// SPDX-License-Identifier: BSD-3-Clause

#include "flow/shared_flow_table.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rcu/rcu_domain.h"

namespace bess::flow {
namespace {

// More than rte_hash's eight-byte value, poisoned on destruction so a reader
// that meets a State after its destructor ran is caught.
struct Big {
  static constexpr uint64_t kAlive = 0xa11ce;
  static constexpr uint64_t kDead = 0xdead;
  static inline std::atomic<int> alive{0};
  explicit Big(uint64_t v) : value(v), echo(v * 7), magic(kAlive) {
    alive.fetch_add(1);
  }
  Big(const Big &) = delete;
  Big &operator=(const Big &) = delete;
  ~Big() {
    magic.store(kDead);
    alive.fetch_sub(1);
  }
  uint64_t value;
  uint64_t echo;
  uint64_t payload[14] = {};
  std::atomic<uint64_t> magic;
};

struct AliasTraits : DefaultSharedFlowTableTraits {
  static constexpr size_t kAliases = 1;
};

using Table = SharedFlowTable<uint64_t, Big>;
using AliasTable = SharedFlowTable<uint64_t, Big, AliasTraits>;

class SharedFlowTableTest : public ::testing::Test {
 protected:
  void SetUp() override { Big::alive = 0; }
  void TearDown() override {
    EXPECT_EQ(0, Big::alive.load()) << "a State was leaked or destroyed twice";
  }

  template <typename T>
  std::unique_ptr<T> Make(size_t capacity) {
    auto table = T::Create(capacity, domain_);
    EXPECT_TRUE(table.has_value());
    return std::move(*table);
  }

  rcu::RcuDomain domain_{16};
};

TEST_F(SharedFlowTableTest, CreateFindEraseWithStateLargerThanTheBackendValue) {
  static_assert(sizeof(Big) > sizeof(uint64_t));
  auto t = Make<Table>(100);
  EXPECT_EQ(nullptr, t->Find(5));
  const auto created = t->Emplace(5, 50);
  ASSERT_TRUE(created.created());
  EXPECT_EQ(50u, created.state->value);
  EXPECT_EQ(created.state, t->Find(5));
  EXPECT_EQ(created.state, t->Peek(5));
  EXPECT_EQ(created.state, t->Lookup(created.handle));

  const auto again = t->Emplace(5, 99);
  EXPECT_EQ(EmplaceStatus::kExists, again.status);
  EXPECT_EQ(created.state, again.state);
  EXPECT_EQ(created.handle, again.handle);
  EXPECT_EQ(1u, t->size());

  EXPECT_TRUE(t->Erase(5));
  EXPECT_FALSE(t->Erase(5));
  EXPECT_EQ(nullptr, t->Find(5));
  EXPECT_EQ(nullptr, t->Lookup(created.handle));
  EXPECT_EQ(0u, t->size());
}

TEST_F(SharedFlowTableTest, CapacityExhaustionAndRecovery) {
  auto t = Make<Table>(20);
  for (uint64_t k = 1; k <= 20; k++) ASSERT_TRUE(t->Emplace(k, k).created());
  const int built = Big::alive;
  const auto refused = t->Emplace(21, 21);
  EXPECT_EQ(EmplaceStatus::kFull, refused.status);
  EXPECT_EQ(nullptr, refused.state);
  EXPECT_EQ(built, Big::alive);
  EXPECT_EQ(nullptr, t->Find(21));
  EXPECT_EQ(EmplaceStatus::kExists, t->Emplace(7, 0).status);

  ASSERT_TRUE(t->Erase(7));
  // No reader is online, so the grace period is already over: the next create
  // reclaims the slot on its own.
  EXPECT_TRUE(t->Emplace(21, 21).created());
  EXPECT_EQ(EmplaceStatus::kFull, t->Emplace(22, 22).status);
  EXPECT_EQ(0u, t->pending_reclaim());
}

// The same erase, with a reader that has not reported quiescence: the State
// stays alive and intact, the slot stays taken, and everything is released the
// moment the reader passes a quiescent state.
TEST_F(SharedFlowTableTest, StalledReaderHoldsStateAndSlotUntilQuiescent) {
  constexpr rcu::ReaderId kReader = 3;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  auto t = Make<Table>(8);
  std::vector<Big *> found;
  for (uint64_t k = 1; k <= 8; k++) ASSERT_TRUE(t->Emplace(k, k).created());

  domain_.Online(kReader);
  // The reader finds every flow, then the writer erases them all.
  for (uint64_t k = 1; k <= 8; k++) found.push_back(t->Find(k));
  for (uint64_t k = 1; k <= 8; k++) ASSERT_TRUE(t->Erase(k));
  EXPECT_EQ(0u, t->size());
  EXPECT_EQ(8u, t->pending_reclaim());
  EXPECT_EQ(0u, t->Reclaim()) << "freed a slot a reader may still be using";
  EXPECT_EQ(8, Big::alive.load()) << "destroyed a State a reader may be using";
  for (Big *state : found) {
    EXPECT_EQ(Big::kAlive, state->magic.load());
    EXPECT_EQ(state->value * 7, state->echo);
  }
  // Nothing can be created: every slot is held back.
  EXPECT_EQ(EmplaceStatus::kFull, t->Emplace(100, 1).status);
  EXPECT_EQ(8u, t->pending_reclaim());

  domain_.Quiescent(kReader);
  EXPECT_EQ(8u, t->Reclaim());
  EXPECT_EQ(0, Big::alive.load());
  EXPECT_EQ(0u, t->pending_reclaim());
  EXPECT_TRUE(t->Emplace(100, 1).created());

  domain_.Offline(kReader);
  domain_.Unregister(kReader);
}

// Every Reclaim() that finds new erasures starts a grace period for them. With
// a reader that never quiesces those pile up, past the table's fixed number of
// outstanding batches; the overflow must fold into the newest batch (a later
// grace period than strictly needed), not allocate and not lose or double-free
// a State.
TEST_F(SharedFlowTableTest, MoreOutstandingBatchesThanTheTableTracksAreMerged) {
  constexpr rcu::ReaderId kReader = 5;
  constexpr uint64_t kFlows = 700;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  auto t = Make<Table>(kFlows);
  for (uint64_t k = 0; k < kFlows; k++) ASSERT_TRUE(t->Emplace(k, k).created());
  domain_.Online(kReader);
  for (uint64_t k = 0; k < kFlows; k++) {
    ASSERT_TRUE(t->Erase(k));
    EXPECT_EQ(0u, t->Reclaim());  // one batch per erase: 700 > 256
  }
  EXPECT_EQ(kFlows, t->pending_reclaim());
  EXPECT_EQ(static_cast<int>(kFlows), Big::alive.load());

  domain_.Quiescent(kReader);
  EXPECT_EQ(kFlows, t->Reclaim());
  EXPECT_EQ(0, Big::alive.load());
  EXPECT_EQ(0u, t->pending_reclaim());
  for (uint64_t k = 0; k < kFlows; k++) ASSERT_TRUE(t->Emplace(k, k).created());
  EXPECT_EQ(EmplaceStatus::kFull, t->Emplace(kFlows, 0).status);
  domain_.Offline(kReader);
  domain_.Unregister(kReader);
}

TEST_F(SharedFlowTableTest, StaleHandleCannotReachAFlowThatReusedItsSlot) {
  auto t = Make<Table>(2);
  const auto first = t->Emplace(1, 1);
  ASSERT_TRUE(first.created());
  ASSERT_TRUE(t->Erase(1));
  EXPECT_EQ(nullptr, t->Lookup(first.handle)) << "dead the moment it is erased";
  EXPECT_FALSE(t->Alive(first.handle));

  // FIFO reuse: walk until the first slot comes round again.
  EmplaceResult<Big> again;
  for (uint64_t k = 2; k < 10 && !again; k++) {
    const auto made = t->Emplace(k, k);
    ASSERT_TRUE(made.created());
    if (made.handle.id == first.handle.id) {
      again = made;
    } else {
      ASSERT_TRUE(t->Erase(k));
    }
  }
  ASSERT_TRUE(again) << "the slot was never reused";
  EXPECT_NE(first.handle.generation, again.handle.generation);
  EXPECT_EQ(again.state, t->Lookup(again.handle));
  EXPECT_EQ(nullptr, t->Lookup(first.handle)) << "stale handle reached the new flow";
  EXPECT_EQ(nullptr, t->KeyOf(first.handle));
  EXPECT_FALSE(t->Erase(first.handle)) << "stale handle erased the new flow";
  EXPECT_EQ(again.state, t->Lookup(again.handle));
  EXPECT_EQ(nullptr, t->Lookup(kNoFlow));
  EXPECT_EQ(nullptr, t->Lookup(FlowHandle{FlowId(77), 1}));
}

TEST_F(SharedFlowTableTest, ExhaustedGenerationRetiresTheSlot) {
  auto t = Make<Table>(2);
  t->SetFreeSlotGenerationForTesting(0, 0xfffffffeu);  // FIFO hands slot 0 out first
  const auto a = t->Emplace(1, 1);
  ASSERT_TRUE(a.created());
  EXPECT_EQ(1u, a.handle.id.value());
  EXPECT_EQ(0xffffffffu, a.handle.generation);
  ASSERT_TRUE(t->Erase(1));
  EXPECT_EQ(1u, t->Reclaim() + 1u) << "a retired slot is destroyed but not freed";
  EXPECT_EQ(1u, t->quarantined_slots());
  EXPECT_EQ(nullptr, t->Lookup(a.handle));
  EXPECT_EQ(0, Big::alive.load());
  // One usable slot is left.
  ASSERT_TRUE(t->Emplace(2, 2).created());
  EXPECT_EQ(EmplaceStatus::kFull, t->Emplace(3, 3).status);
}

TEST_F(SharedFlowTableTest, AliasesNameOneFlowAndLeaveWithIt) {
  auto t = Make<AliasTable>(8);
  const auto made = t->EmplaceAliased(10, 20, 7);
  ASSERT_TRUE(made.created());
  EXPECT_EQ(made.state, t->Find(10));
  EXPECT_EQ(made.state, t->Find(20));
  EXPECT_FALSE(t->FindHandle(10).via_alias);
  const auto via = t->FindHandle(20);
  EXPECT_TRUE(via.via_alias);
  EXPECT_EQ(made.handle, via.handle);
  EXPECT_EQ(1u, t->size());

  EXPECT_EQ(AliasStatus::kNoRoom, t->AddAlias(made.handle, 30));
  const auto other = t->Emplace(50, 5);
  EXPECT_EQ(AliasStatus::kExists, t->AddAlias(other.handle, 20));

  EXPECT_FALSE(t->RemoveAlias(10)) << "a primary key is not an alias";
  EXPECT_TRUE(t->RemoveAlias(20));
  EXPECT_EQ(nullptr, t->Find(20));
  EXPECT_EQ(made.state, t->Find(10));
  EXPECT_EQ(AliasStatus::kAdded, t->AddAlias(made.handle, 30));
  EXPECT_EQ(made.state, t->Find(30));

  // Erasing through the alias removes every key at once.
  ASSERT_TRUE(t->Erase(30));
  for (uint64_t k : {10, 20, 30}) {
    EXPECT_EQ(nullptr, t->Find(k)) << "alias " << k << " outlived its flow";
  }
  EXPECT_EQ(AliasStatus::kStale, t->AddAlias(made.handle, 40));
  EXPECT_EQ(other.state, t->Find(50));

  // The keys are free again.
  const auto next = t->EmplaceAliased(20, 10, 9);
  ASSERT_TRUE(next.created());
  EXPECT_EQ(next.state, t->Find(10));

  // All or nothing.
  const int built = Big::alive;
  EXPECT_EQ(EmplaceStatus::kExists, t->EmplaceAliased(20, 99, 0).status);
  EXPECT_EQ(EmplaceStatus::kAliasExists, t->EmplaceAliased(60, 10, 0).status);
  EXPECT_EQ(EmplaceStatus::kAliasExists, t->EmplaceAliased(61, 61, 0).status);
  EXPECT_EQ(nullptr, t->Find(60));
  EXPECT_EQ(nullptr, t->Find(61));
  EXPECT_EQ(nullptr, t->Find(99));
  EXPECT_EQ(built, Big::alive);
}

TEST_F(SharedFlowTableTest, FindBatchEqualsScalarFind) {
  auto t = Make<Table>(1000);
  for (uint64_t k = 0; k < 1000; k += 2) ASSERT_TRUE(t->Emplace(k, k).created());
  std::mt19937_64 rng(9);
  for (int round = 0; round < 200; round++) {
    const size_t n = rng() % 65;
    std::vector<uint64_t> keys(n);
    for (auto &k : keys) k = rng() % 1100;
    std::vector<Big *> out(n, reinterpret_cast<Big *>(1));
    const uint64_t hits = t->FindBatch(keys, out);
    for (size_t i = 0; i < n; i++) {
      ASSERT_EQ(t->Find(keys[i]), out[i]);
      ASSERT_EQ(out[i] != nullptr, (hits >> i & 1) != 0);
    }
  }
}

// -- state sharing ------------------------------------------------------------

struct FakeOwner {
  static constexpr bool kChecked = true;
  static inline OwnerToken current = 1;
  static OwnerToken Current() noexcept { return current; }
};
struct OwnedTraits : DefaultSharedFlowTableTraits {
  static constexpr StateSharing kSharing = StateSharing::kOwnedByCreator;
  using Owner = FakeOwner;
};
using OwnedTable = SharedFlowTable<uint64_t, Big, OwnedTraits>;

TEST_F(SharedFlowTableTest, OwnedStateIsMutableOnlyByItsCreator) {
  FakeOwner::current = 1;
  auto t = Make<OwnedTable>(8);
  const auto made = t->Emplace(5, 5);
  ASSERT_TRUE(made.created());
  EXPECT_EQ(made.state, t->FindOwned(5));
  EXPECT_EQ(made.state, t->LookupOwned(made.handle));

  // Any worker may classify with the shared directory and read the State...
  FakeOwner::current = 2;
  EXPECT_EQ(made.state, t->Peek(5));
  EXPECT_EQ(made.state, t->Peek(made.handle));
  EXPECT_EQ(5u, t->Peek(5)->value);
  // ...but not mutate it, and a create that finds an existing flow does not
  // hand its State to a stranger.
  const auto dup = t->Emplace(5, 6);
  EXPECT_EQ(EmplaceStatus::kExists, dup.status);
  EXPECT_EQ(nullptr, dup.state);
  EXPECT_EQ(made.handle, dup.handle);
  EXPECT_DEATH(t->FindOwned(5), "ownership violation in FindOwned");
  EXPECT_DEATH(t->LookupOwned(made.handle), "ownership violation in LookupOwned");
  // A miss is not a violation.
  EXPECT_EQ(nullptr, t->FindOwned(404));

  FakeOwner::current = 1;
  EXPECT_EQ(made.state, t->Emplace(5, 6).state);
  EXPECT_TRUE(t->Erase(5));
}

// -- failure ------------------------------------------------------------------

struct FailingAllocator {
  static inline int fail_at = -1;
  static inline int count = 0;
  static inline int live = 0;
  static void Reset(int fail) {
    fail_at = fail;
    count = 0;
  }
  static void *Allocate(size_t bytes, size_t align) noexcept {
    if (count++ == fail_at) return nullptr;
    live++;
    return ::operator new(bytes, std::align_val_t(align), std::nothrow);
  }
  static void Deallocate(void *p, size_t, size_t align) noexcept {
    live--;
    ::operator delete(p, std::align_val_t(align));
  }
};
struct FailingTraits : DefaultSharedFlowTableTraits {
  using Allocator = FailingAllocator;
};
using FailingTable = SharedFlowTable<uint64_t, Big, FailingTraits>;

TEST_F(SharedFlowTableTest, ConstructionFailureLeavesNothingAllocated) {
  FailingAllocator::live = 0;
  for (int fail = 0; fail < 3; fail++) {
    FailingAllocator::Reset(fail);
    auto table = FailingTable::Create(1000, domain_);
    ASSERT_FALSE(table.has_value()) << "allocation " << fail;
    EXPECT_EQ(FlowTableError::kOutOfMemory, table.error());
    EXPECT_EQ(0, FailingAllocator::live) << "allocation " << fail << " leaked";
  }
  FailingAllocator::Reset(-1);
  {
    auto table = FailingTable::Create(1000, domain_);
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ(3, FailingAllocator::live);
    ASSERT_TRUE((*table)->Emplace(1, 1).created());
  }
  EXPECT_EQ(0, FailingAllocator::live);
}

TEST_F(SharedFlowTableTest, CreateRejectsWhatTheDirectoryCannotHold) {
  EXPECT_EQ(FlowTableError::kInvalidCapacity, Table::Create(0, domain_).error());
  EXPECT_EQ(FlowTableError::kInvalidCapacity,
            Table::Create(size_t{1} << 31, domain_).error());
  // rte_hash refuses more than 2^30 key slots; the table says so without having
  // allocated its slot array first.
  FailingAllocator::Reset(-1);
  FailingAllocator::live = 0;
  EXPECT_EQ(FlowTableError::kBackendFailed,
            FailingTable::Create(850'000'000, domain_).error());
  EXPECT_EQ(0, FailingAllocator::count) << "the slot array was allocated first";
}

// -- concurrency --------------------------------------------------------------

// Several writers create the same keys at once. Exactly one wins each key and
// every loser is told about the winner's flow (D-028's insert-if-absent).
TEST_F(SharedFlowTableTest, RacingCreatorsMakeEachFlowOnce) {
  constexpr uint64_t kKeys = 3000;
  constexpr int kThreads = 4;
  auto t = Make<Table>(kKeys);
  std::atomic<uint64_t> created{0};
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int id = 0; id < kThreads; id++) {
    threads.emplace_back([&, id] {
      std::mt19937_64 rng(id);
      std::vector<uint64_t> order(kKeys);
      for (uint64_t i = 0; i < kKeys; i++) order[i] = i;
      std::shuffle(order.begin(), order.end(), rng);
      for (uint64_t key : order) {
        const auto r = t->Emplace(key, key);
        if (r.created()) created++;
        if (!r || r.state->value != key) wrong++;
      }
    });
  }
  for (auto &th : threads) th.join();
  EXPECT_EQ(kKeys, created.load());
  EXPECT_EQ(0, wrong.load());
  EXPECT_EQ(kKeys, t->size());
  EXPECT_EQ(static_cast<int>(kKeys), Big::alive.load());
  for (uint64_t key = 0; key < kKeys; key++) ASSERT_NE(nullptr, t->Find(key));
}

// Readers look flows up in batches while writers create and erase them, and a
// reader takes a State it found and uses it until its next quiescent state.
// A State seen after its destructor ran, or a value that does not belong to
// its key, fails the test.
TEST_F(SharedFlowTableTest, ConcurrentLookupWithCreateAndErase) {
  constexpr uint64_t kKeysPerWriter = 400;
  constexpr int kWriters = 2;
  constexpr int kReaders = 3;
  constexpr uint64_t kKeys = kKeysPerWriter * kWriters;
  auto t = Make<Table>(kKeys);  // exactly enough: slots are recycled

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> poisoned{0}, misattributed{0}, reads{0}, hits_seen{0};
  std::vector<std::thread> threads;

  for (int r = 0; r < kReaders; r++) {
    const rcu::ReaderId id = static_cast<rcu::ReaderId>(r);
    ASSERT_TRUE(domain_.Register(id).has_value());
    domain_.Online(id);
    threads.emplace_back([&, id] {
      std::mt19937_64 rng(100 + id);
      uint64_t keys[32];
      Big *out[32];
      while (!stop.load(std::memory_order_relaxed)) {
        for (auto &k : keys) k = rng() % kKeys;
        const uint64_t hits = t->FindBatch(keys, out);
        for (int i = 0; i < 32; i++) {
          if (!(hits >> i & 1)) continue;
          hits_seen++;
          // Use the State: it must be alive and be this key's.
          if (out[i]->magic.load() != Big::kAlive) poisoned++;
          if (out[i]->value != keys[i] || out[i]->echo != keys[i] * 7) {
            misattributed++;
          }
        }
        reads += 32;
        domain_.Quiescent(id);  // between "task invocations"
      }
      domain_.Offline(id);
      domain_.Unregister(id);
    });
  }
  for (int w = 0; w < kWriters; w++) {
    threads.emplace_back([&, w] {
      std::mt19937_64 rng(w);
      const uint64_t base = w * kKeysPerWriter;
      while (!stop.load(std::memory_order_relaxed)) {
        const uint64_t key = base + rng() % kKeysPerWriter;
        if (rng() % 2) {
          t->Emplace(key, key);
        } else {
          t->Erase(key);
        }
      }
    });
  }
  // A control thread reclaims, as the docs advise, keeping destructors off
  // the writers' hot path.
  threads.emplace_back([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      t->Reclaim();
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  stop = true;
  for (auto &th : threads) th.join();

  EXPECT_EQ(0u, poisoned.load()) << "a reader met a State after its destructor";
  EXPECT_EQ(0u, misattributed.load());
  EXPECT_GT(hits_seen.load(), 0u) << "the readers never found a flow";

  t->Reclaim();
  EXPECT_EQ(0u, t->pending_reclaim()) << "backlog did not drain";
  EXPECT_EQ(static_cast<int>(t->size()), Big::alive.load());
  size_t live = 0;
  for (uint64_t key = 0; key < kKeys; key++) {
    if (const Big *s = t->Peek(key)) {
      live++;
      ASSERT_EQ(key, s->value);
    }
  }
  EXPECT_EQ(t->size(), live);
}

// -- reference model ----------------------------------------------------------

TEST_F(SharedFlowTableTest, RandomOperationsMatchAStdModel) {
  for (uint32_t seed = 1; seed <= 3; seed++) {
    auto t = Make<AliasTable>(48);
    struct ModelFlow {
      uint64_t value;
      std::set<uint64_t> keys;
      uint64_t primary;
      FlowHandle handle;
    };
    std::map<uint64_t, ModelFlow> flows;
    std::unordered_map<uint64_t, uint64_t> by_key;
    std::vector<FlowHandle> dead;
    uint64_t serial = 1, value = 1;
    std::mt19937_64 rng(seed);

    auto check = [&](uint64_t key) {
      auto it = by_key.find(key);
      const Big *state = t->Peek(key);
      if (it == by_key.end()) {
        ASSERT_EQ(nullptr, state) << "key " << key;
        return;
      }
      const ModelFlow &flow = flows.at(it->second);
      ASSERT_NE(nullptr, state) << "key " << key;
      ASSERT_EQ(flow.value, state->value);
      const auto ref = t->FindHandle(key);
      ASSERT_EQ(flow.handle, ref.handle);
      ASSERT_EQ(key != flow.primary, ref.via_alias);
      ASSERT_EQ(state, t->Peek(flow.handle));
    };

    for (int step = 0; step < 20000; step++) {
      const uint64_t key = rng() % 150;
      switch (rng() % 7) {
        case 0:
        case 1: {
          const auto r = t->Emplace(key, value);
          if (by_key.contains(key)) {
            ASSERT_EQ(EmplaceStatus::kExists, r.status);
          } else if (flows.size() >= 48) {
            ASSERT_EQ(EmplaceStatus::kFull, r.status);
          } else {
            ASSERT_EQ(EmplaceStatus::kCreated, r.status);
            flows.emplace(serial, ModelFlow{value, {key}, key, r.handle});
            by_key[key] = serial++;
            value++;
          }
          break;
        }
        case 2: {
          const bool present = by_key.contains(key);
          ASSERT_EQ(present, t->Erase(key));
          if (present) {
            const uint64_t s = by_key[key];
            dead.push_back(flows.at(s).handle);
            for (uint64_t k : flows.at(s).keys) by_key.erase(k);
            flows.erase(s);
          }
          break;
        }
        case 3: {
          if (flows.empty()) break;
          auto it = flows.begin();
          std::advance(it, rng() % flows.size());
          const AliasStatus got = t->AddAlias(it->second.handle, key);
          if (by_key.contains(key)) {
            ASSERT_EQ(AliasStatus::kExists, got);
          } else if (it->second.keys.size() >= 2) {
            ASSERT_EQ(AliasStatus::kNoRoom, got);
          } else {
            ASSERT_EQ(AliasStatus::kAdded, got);
            it->second.keys.insert(key);
            by_key[key] = it->first;
          }
          break;
        }
        case 4: {
          auto it = by_key.find(key);
          const bool is_alias =
              it != by_key.end() && flows.at(it->second).primary != key;
          ASSERT_EQ(is_alias, t->RemoveAlias(key));
          if (is_alias) {
            flows.at(it->second).keys.erase(key);
            by_key.erase(it);
          }
          break;
        }
        case 5: {
          for (int i = 0; i < 3 && !dead.empty(); i++) {
            const FlowHandle h = dead[rng() % dead.size()];
            ASSERT_EQ(nullptr, t->Peek(h)) << "a dead handle resolved";
            ASSERT_FALSE(t->Erase(h));
          }
          break;
        }
        default:
          t->Reclaim();
      }
      ASSERT_EQ(flows.size(), t->size());
      check(key);
      if (step % 101 == 0) {
        for (uint64_t k = 0; k < 150; k++) check(k);
      }
    }
    ASSERT_EQ(flows.size(), static_cast<size_t>(Big::alive.load()) - t->pending_reclaim());
    t.reset();
  }
}

}  // namespace
}  // namespace bess::flow
