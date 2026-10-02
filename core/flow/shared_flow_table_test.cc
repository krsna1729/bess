// SPDX-License-Identifier: BSD-3-Clause

#include "flow/shared_flow_table.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
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

// -- placement and publication ------------------------------------------------
//
// The directory is an rte_hash. Free capacity does not promise that one given
// key fits: keys that share a cuckoo bucket pair fill it at 16 entries however
// empty the table is. These tests craft such keys (the technique of
// TransactionEngineTest.KeysThatCannotBePlacedRejectCleanly) and check that a
// refusal is ordinary and leaves nothing behind.

// A State that counts constructions and destructions. `in_constructor` runs
// before a construction completes, that is, while a flow's keys are in the
// directory and the flow is not yet published; it may throw.
struct Counted {
  static inline int built = 0;
  static inline int destroyed = 0;
  static inline std::function<void()> in_constructor;
  explicit Counted(uint64_t v) : value(v) {
    if (in_constructor) in_constructor();
    built++;
  }
  Counted(const Counted &) = delete;
  Counted &operator=(const Counted &) = delete;
  ~Counted() { destroyed++; }
  uint64_t value;
};

struct CountedTraits : DefaultSharedFlowTableTraits {
  static constexpr size_t kAliases = 1;
  using Observer = FlowCounters;
};
struct CountedLifoTraits : CountedTraits {
  static constexpr SlotReuse kReuse = SlotReuse::kLifo;
};
using CountedTable = SharedFlowTable<uint64_t, Counted, CountedTraits>;
using CountedLifoTable = SharedFlowTable<uint64_t, Counted, CountedLifoTraits>;

classifier::ConstBytes KeyBytes(const uint64_t &key) {
  return classifier::ConstBytes(reinterpret_cast<const std::byte *>(&key),
                                sizeof(key));
}

// The two rte_hash buckets `key` may live in: primary = hash & mask, alternate
// = (primary ^ (hash >> 16)) & mask, with eight entries per bucket.
struct Buckets {
  uint32_t primary, alternate;
  bool operator==(const Buckets &) const = default;
};
Buckets BucketsOf(const classifier::ConcurrentExactTable &directory,
                  uint64_t key) {
  const uint32_t mask = std::bit_ceil(directory.capacity()) / 8 - 1;
  const uint32_t hash = directory.DpdkHash(KeyBytes(key));
  const uint32_t primary = hash & mask;
  return {primary, (primary ^ (hash >> 16)) & mask};
}

// `crowd` keys that all share one bucket pair (two distinct buckets: sixteen
// of them fit, not more), and `elsewhere` keys that touch neither bucket.
struct KeyGroups {
  std::vector<uint64_t> crowd, elsewhere;
};
KeyGroups MakeKeyGroups(const classifier::ConcurrentExactTable &directory,
                        size_t crowd, size_t elsewhere) {
  KeyGroups groups;
  Buckets target{};
  bool chosen = false;
  for (uint64_t key = 1;
       groups.crowd.size() < crowd || groups.elsewhere.size() < elsewhere;
       key++) {
    const Buckets b = BucketsOf(directory, key);
    if (!chosen) {
      if (b.primary == b.alternate) continue;
      target = b;
      chosen = true;
    }
    if (b == target) {
      if (groups.crowd.size() < crowd) groups.crowd.push_back(key);
    } else if (b.primary != target.primary && b.primary != target.alternate &&
               b.alternate != target.primary &&
               b.alternate != target.alternate) {
      if (groups.elsewhere.size() < elsewhere) groups.elsewhere.push_back(key);
    }
  }
  return groups;
}

// Whether the directory itself, bypassing the table's validation, holds `key`.
bool InDirectory(const classifier::ConcurrentExactTable &directory,
                 uint64_t key, FlowHandle *handle = nullptr) {
  uint64_t value = 0;
  const bool hit =
      directory.LookupBatch(KeyBytes(key), sizeof(key), &value, 1) != 0;
  if (hit && handle != nullptr) *handle = std::bit_cast<FlowHandle>(value);
  return hit;
}

class SharedFlowTablePlacementTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Counted::built = 0;
    Counted::destroyed = 0;
    Counted::in_constructor = nullptr;
  }
  void TearDown() override {
    Counted::in_constructor = nullptr;
    EXPECT_EQ(Counted::built, Counted::destroyed)
        << "a State was leaked or destroyed twice";
  }

  template <typename T>
  std::unique_ptr<T> Make(size_t capacity) {
    auto table = T::Create(capacity, domain_);
    EXPECT_TRUE(table.has_value());
    return std::move(*table);
  }

  rcu::RcuDomain domain_{16};
};

TEST_F(SharedFlowTablePlacementTest, AKeyTheDirectoryCannotPlaceIsRefusedAndChangesNothing) {
  constexpr size_t kCapacity = 64;
  auto t = Make<CountedTable>(kCapacity);
  const KeyGroups keys = MakeKeyGroups(t->directory(), 40, 80);

  // Fill the crowd's bucket pair while 48 or more slots stay free.
  EmplaceResult<Counted> refused;
  size_t fit = 0;
  for (; fit < keys.crowd.size(); fit++) {
    const auto r = t->Emplace(keys.crowd[fit], fit);
    if (!r.created()) {
      refused = r;
      break;
    }
  }
  ASSERT_GE(fit, 9u) << "two buckets of eight";
  ASSERT_LT(fit, keys.crowd.size()) << "the crowd never filled its buckets";
  const uint64_t refused_key = keys.crowd[fit];

  EXPECT_EQ(EmplaceStatus::kPlacementFailed, refused.status);
  EXPECT_EQ(nullptr, refused.state);
  EXPECT_EQ(kNoFlow, refused.handle);
  EXPECT_FALSE(refused);
  EXPECT_EQ(fit, t->size());
  EXPECT_EQ(fit, t->directory().size());
  EXPECT_GE(kCapacity - fit, 40u) << "free slots remained: this was placement";
  EXPECT_EQ(static_cast<int>(fit), Counted::built)
      << "a refused create built a State";
  EXPECT_EQ(0, Counted::destroyed);
  EXPECT_EQ(fit, t->observer().created) << "OnCreate fired for a refusal";
  EXPECT_EQ(1u, t->observer().rejected_full);
  EXPECT_EQ(0u, t->observer().erased);
  EXPECT_EQ(nullptr, t->Find(refused_key));
  EXPECT_EQ(nullptr, t->Peek(refused_key));
  EXPECT_FALSE(InDirectory(t->directory(), refused_key));

  // The same key is refused the same way, and nothing accumulates.
  EXPECT_EQ(EmplaceStatus::kPlacementFailed, t->Emplace(refused_key, 0).status);
  EXPECT_EQ(2u, t->observer().rejected_full);
  EXPECT_EQ(fit, t->directory().size());
  EXPECT_EQ(static_cast<int>(fit), Counted::built);

  // Room in the crowded pair makes the refused key fit, and only one more.
  ASSERT_TRUE(t->Erase(keys.crowd[0]));
  const auto fits = t->Emplace(refused_key, 99);
  ASSERT_TRUE(fits.created());
  EXPECT_EQ(fits.state, t->Find(refused_key));
  EXPECT_EQ(EmplaceStatus::kPlacementFailed,
            t->Emplace(keys.crowd[fit + 1], 0).status);

  // Every slot is still usable: the table fills to its capacity with other
  // keys, and only then is it full.
  size_t next = 0;
  while (t->size() < kCapacity) {
    ASSERT_LT(next, keys.elsewhere.size());
    ASSERT_TRUE(t->Emplace(keys.elsewhere[next++], 0).created())
        << "the table lost a slot to the refused creates";
  }
  EXPECT_EQ(EmplaceStatus::kFull, t->Emplace(keys.elsewhere[next], 0).status);
  EXPECT_EQ(kCapacity, t->directory().size());

  // Slots 0..fit-1 took the crowd and the refusals tried slot `fit` (FIFO, a
  // fresh table): its generation moved past the handle that attempt used, so
  // the flow that finally lands there has generation 3, an untouched slot 1.
  std::map<uint32_t, uint32_t> generation_of_id;
  t->ForEach([&](FlowHandle handle, const uint64_t &, const Counted &) {
    generation_of_id[handle.id.value()] = handle.generation;
  });
  ASSERT_EQ(kCapacity, generation_of_id.size()) << "every slot holds a flow";
  EXPECT_EQ(3u, generation_of_id.at(static_cast<uint32_t>(fit) + 1));
  EXPECT_EQ(1u, generation_of_id.at(2));
}

TEST_F(SharedFlowTablePlacementTest, ARefusedCreateMovesTheStackSlotToANewGeneration) {
  auto t = Make<CountedLifoTable>(64);
  const KeyGroups keys = MakeKeyGroups(t->directory(), 40, 4);
  size_t fit = 0;
  while (fit < keys.crowd.size() && t->Emplace(keys.crowd[fit], fit).created()) {
    fit++;
  }
  ASSERT_LT(fit, keys.crowd.size());
  // The refused attempt tried the slot on top of the stack; it stays on top,
  // one generation on, so the next flow to take it cannot be named by the
  // handle the refused attempt used.
  const auto next = t->Emplace(keys.elsewhere[0], 0);
  ASSERT_TRUE(next.created());
  EXPECT_EQ(fit + 1, next.handle.id.value());
  EXPECT_EQ(3u, next.handle.generation);
}

TEST_F(SharedFlowTablePlacementTest, AnAliasedCreateThatCannotBePlacedTakesBackWhatItInserted) {
  auto t = Make<CountedTable>(64);
  const KeyGroups keys = MakeKeyGroups(t->directory(), 40, 80);
  size_t used = 0;
  auto elsewhere = [&] { return keys.elsewhere[used++]; };

  // Aliased flows whose primary keys crowd one pair and whose aliases do not.
  size_t fit = 0;
  uint64_t orphan = 0;
  EmplaceResult<Counted> refused;
  for (; fit < keys.crowd.size(); fit++) {
    orphan = elsewhere();
    const auto r = t->EmplaceAliased(keys.crowd[fit], orphan, fit);
    if (!r.created()) {
      refused = r;
      break;
    }
  }
  ASSERT_GE(fit, 9u);
  ASSERT_LT(fit, keys.crowd.size());

  // The primary could not be placed after the alias had been: the alias is
  // taken back out of the directory, not left behind.
  EXPECT_EQ(EmplaceStatus::kPlacementFailed, refused.status);
  EXPECT_EQ(nullptr, refused.state);
  EXPECT_EQ(fit, t->size());
  EXPECT_EQ(2 * fit, t->directory().size()) << "the alias entry leaked";
  EXPECT_FALSE(InDirectory(t->directory(), orphan));
  EXPECT_FALSE(InDirectory(t->directory(), keys.crowd[fit]));
  EXPECT_EQ(static_cast<int>(fit), Counted::built);
  EXPECT_EQ(1u, t->observer().rejected_full);
  EXPECT_EQ(fit, t->observer().created);
  // The orphaned key is free for anyone: here, as a flow of its own.
  const auto reused = t->Emplace(orphan, 1000);
  ASSERT_TRUE(reused.created());
  EXPECT_EQ(reused.state, t->Find(orphan));
  ASSERT_TRUE(t->Erase(orphan));

  // The alias could not be placed: nothing was inserted, nothing to take back.
  const uint64_t primary = elsewhere();
  const auto alias_refused = t->EmplaceAliased(primary, keys.crowd[fit], 7);
  EXPECT_EQ(EmplaceStatus::kPlacementFailed, alias_refused.status);
  EXPECT_EQ(fit, t->size());
  EXPECT_EQ(2 * fit, t->directory().size());
  EXPECT_FALSE(InDirectory(t->directory(), primary));
  EXPECT_EQ(nullptr, t->Find(primary));
  EXPECT_EQ(2u, t->observer().rejected_full) << "one OnFull per refusal";
  const auto lone = t->Emplace(primary, 8);
  ASSERT_TRUE(lone.created());
  ASSERT_TRUE(t->Erase(primary));

  // Once the pair has room the refused alias fits, and the flow answers to
  // both keys.
  ASSERT_TRUE(t->Erase(keys.crowd[0]));
  const uint64_t other_primary = elsewhere();
  const auto ok = t->EmplaceAliased(other_primary, keys.crowd[fit], 9);
  ASSERT_TRUE(ok.created());
  EXPECT_EQ(ok.state, t->Find(other_primary));
  EXPECT_EQ(ok.state, t->Find(keys.crowd[fit]));
  EXPECT_TRUE(t->FindHandle(keys.crowd[fit]).via_alias);
  EXPECT_EQ(ok.handle, t->FindHandle(other_primary).handle);
}

TEST_F(SharedFlowTablePlacementTest, AnAliasThatCannotBePlacedLeavesTheFlowAsItWas) {
  auto t = Make<CountedTable>(64);
  const KeyGroups keys = MakeKeyGroups(t->directory(), 40, 8);
  const auto flow = t->Emplace(keys.elsewhere[0], 7);
  ASSERT_TRUE(flow.created());
  size_t fit = 0;
  while (fit < keys.crowd.size() && t->Emplace(keys.crowd[fit], fit).created()) {
    fit++;
  }
  ASSERT_GE(fit, 9u);
  ASSERT_LT(fit, keys.crowd.size());
  const size_t flows = t->size();
  const size_t entries = t->directory().size();
  const uint64_t refusals = t->observer().rejected_full;

  EXPECT_EQ(AliasStatus::kPlacementFailed,
            t->AddAlias(flow.handle, keys.crowd[fit]));
  EXPECT_EQ(flows, t->size());
  EXPECT_EQ(entries, t->directory().size());
  EXPECT_EQ(nullptr, t->Find(keys.crowd[fit]));
  EXPECT_FALSE(InDirectory(t->directory(), keys.crowd[fit]));
  EXPECT_FALSE(t->RemoveAlias(keys.crowd[fit]));
  EXPECT_EQ(refusals, t->observer().rejected_full)
      << "OnFull is for refused creates";
  EXPECT_EQ(flow.state, t->Lookup(flow.handle));

  // The flow still has its alias position: the refusal did not take it.
  EXPECT_EQ(AliasStatus::kAdded, t->AddAlias(flow.handle, keys.elsewhere[1]));
  EXPECT_EQ(flow.state, t->Find(keys.elsewhere[1]));
  EXPECT_EQ(AliasStatus::kNoRoom, t->AddAlias(flow.handle, keys.elsewhere[2]));

  // The key that was refused fits once the pair has room.
  ASSERT_TRUE(t->RemoveAlias(keys.elsewhere[1]));
  ASSERT_TRUE(t->Erase(keys.crowd[0]));
  EXPECT_EQ(AliasStatus::kAdded, t->AddAlias(flow.handle, keys.crowd[fit]));
  EXPECT_EQ(flow.state, t->Find(keys.crowd[fit]));

  // Erasing the flow takes every key it had, and no other.
  ASSERT_TRUE(t->Erase(flow.handle));
  EXPECT_EQ(fit - 1, t->directory().size());
  EXPECT_EQ(nullptr, t->Find(keys.crowd[fit]));
}

TEST_F(SharedFlowTablePlacementTest, NoKeyOfAFlowResolvesBeforeAllOfThemDo) {
  auto t = Make<CountedTable>(16);
  constexpr uint64_t kKey = 100, kAlias = 200;
  int probes = 0;
  // From inside the State's constructor: the flow's keys are in the directory
  // and the flow is not published. Nothing may resolve through any reader path
  // (a reader that did would hold a State that is not built).
  Counted::in_constructor = [&] {
    probes++;
    EXPECT_TRUE(InDirectory(t->directory(), kKey)) << "the probe is not in the window";
    EXPECT_TRUE(InDirectory(t->directory(), kAlias)) << "the probe is not in the window";
    for (uint64_t k : {kKey, kAlias}) {
      EXPECT_EQ(nullptr, t->Peek(k));
      EXPECT_EQ(nullptr, t->Find(k));
      EXPECT_EQ(kNoFlow, t->FindHandle(k).handle);
    }
    const uint64_t both[2] = {kKey, kAlias};
    const Counted *peeked[2] = {nullptr, nullptr};
    EXPECT_EQ(0u, t->PeekBatch(both, peeked));
    EXPECT_EQ(nullptr, peeked[0]);
    EXPECT_EQ(nullptr, peeked[1]);
    Counted *found[2] = {nullptr, nullptr};
    EXPECT_EQ(0u, t->FindBatch(both, found));
    EXPECT_EQ(nullptr, found[0]);
    EXPECT_EQ(nullptr, found[1]);
  };
  const auto made = t->EmplaceAliased(kKey, kAlias, 5);
  Counted::in_constructor = nullptr;
  ASSERT_TRUE(made.created());
  ASSERT_EQ(1, probes);

  // Published: both keys name the flow, through every path, at once.
  EXPECT_EQ(made.state, t->Find(kKey));
  EXPECT_EQ(made.state, t->Find(kAlias));
  EXPECT_EQ(made.handle, t->FindHandle(kKey).handle);
  EXPECT_EQ(made.handle, t->FindHandle(kAlias).handle);
  EXPECT_FALSE(t->FindHandle(kKey).via_alias);
  EXPECT_TRUE(t->FindHandle(kAlias).via_alias);
  const uint64_t both[2] = {kKey, kAlias};
  Counted *found[2] = {nullptr, nullptr};
  EXPECT_EQ(3u, t->FindBatch(both, found));
  EXPECT_EQ(made.state, found[0]);
  EXPECT_EQ(made.state, found[1]);
}

template <typename Table>
void ExpectThrowingConstructorTakesKeysBack(rcu::RcuDomain &domain,
                                            bool same_slot_next) {
  auto created = Table::Create(16, domain);
  ASSERT_TRUE(created.has_value());
  auto &t = **created;
  FlowHandle seen{};  // what the directory held for the key during the build
  Counted::in_constructor = [&] {
    EXPECT_TRUE(InDirectory(t.directory(), 100, &seen));
    EXPECT_TRUE(InDirectory(t.directory(), 200));
    throw std::runtime_error("State refused");
  };
  EXPECT_THROW(t.EmplaceAliased(100, 200, 1), std::runtime_error);
  Counted::in_constructor = nullptr;

  EXPECT_EQ(0u, t.size());
  EXPECT_EQ(0u, t.directory().size()) << "a key stayed in the directory";
  EXPECT_FALSE(InDirectory(t.directory(), 100));
  EXPECT_FALSE(InDirectory(t.directory(), 200));
  EXPECT_EQ(0, Counted::built);
  EXPECT_EQ(0u, t.observer().created);
  EXPECT_EQ(0u, t.observer().rejected_full) << "a throw is not a refusal";
  ASSERT_NE(0u, seen.id.value());

  // The same keys create the flow now; the handle the failed attempt used
  // names nothing, even if the new flow took the same slot.
  const auto made = t.EmplaceAliased(100, 200, 2);
  ASSERT_TRUE(made.created());
  EXPECT_EQ(made.state, t.Find(100));
  EXPECT_EQ(made.state, t.Find(200));
  if (same_slot_next) {
    EXPECT_EQ(seen.id, made.handle.id) << "the test no longer reuses the slot";
  }
  EXPECT_NE(seen, made.handle);
  EXPECT_EQ(nullptr, t.Peek(seen));
  EXPECT_EQ(nullptr, t.KeyOf(seen));
  EXPECT_FALSE(t.Alive(seen));
  EXPECT_FALSE(t.Erase(seen));
  EXPECT_EQ(made.state, t.Peek(made.handle));
}

TEST_F(SharedFlowTablePlacementTest, AThrowingConstructorTakesTheKeysBack) {
  ExpectThrowingConstructorTakesKeysBack<CountedTable>(domain_, false);
}

TEST_F(SharedFlowTablePlacementTest, AHandleOfAnAbandonedCreateNeverNamesALaterFlow) {
  // A stack hands the same slot to the next create: the only way the stale
  // handle could collide with the new flow.
  ExpectThrowingConstructorTakesKeysBack<CountedLifoTable>(domain_, true);
}

TEST_F(SharedFlowTablePlacementTest, ARefusedCreateRetiresASlotWhoseGenerationWouldWrap) {
  constexpr size_t kCapacity = 32;
  auto t = Make<CountedTable>(kCapacity);
  const KeyGroups keys = MakeKeyGroups(t->directory(), 40, 40);
  size_t fit = 0;
  while (fit < keys.crowd.size() && t->Emplace(keys.crowd[fit], fit).created()) {
    fit++;
  }
  ASSERT_GE(fit, 9u);
  ASSERT_LT(fit, keys.crowd.size());
  // That refusal tried slot `fit` (FIFO, a fresh table) and sent it to the
  // back of the free list, so the next refusal tries slot `fit + 1`. Put that
  // slot's generation where one more lifetime would wrap it.
  const uint32_t victim = static_cast<uint32_t>(fit) + 1;
  t->SetFreeSlotGenerationForTesting(victim, 0xfffffffeu);
  EXPECT_EQ(EmplaceStatus::kPlacementFailed,
            t->Emplace(keys.crowd[fit], 0).status);
  EXPECT_EQ(1u, t->quarantined_slots());

  // The slot is gone for good, as an erased slot at the wrap would be; every
  // other slot works.
  size_t next = 0;
  while (t->size() < kCapacity - 1) {
    ASSERT_LT(next, keys.elsewhere.size());
    const auto r = t->Emplace(keys.elsewhere[next++], 0);
    ASSERT_TRUE(r.created());
    EXPECT_NE(victim + 1, r.handle.id.value()) << "the retired slot was reused";
  }
  EXPECT_EQ(EmplaceStatus::kFull, t->Emplace(keys.elsewhere[next], 0).status);
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

// An aliased flow appears, and disappears, as one to a concurrent reader: a
// reader that found one of its keys and then looks for the other finds it with
// the same handle, unless the flow is gone by then. The writer creates and
// erases flows of four fixed key pairs, so keys are reused by flows of
// different generations; the loops are bounded by counts, not by time. A
// reader that finds a key and misses its partner while the flow is still
// alive has seen half of a flow.
TEST_F(SharedFlowTableTest, AliasedFlowAppearsAndVanishesAsOneToConcurrentReaders) {
  constexpr uint64_t kPairs = 4;     // pair p: primary 2p, alias 2p+1, State p
  constexpr int kReaders = 3;
  constexpr int kCycles = 30000;     // writer operations
  auto t = Make<AliasTable>(8);
  // One pair is permanent, so that every reader meets a whole flow at least
  // once whatever the scheduling.
  ASSERT_TRUE(t->EmplaceAliased(2 * kPairs, 2 * kPairs + 1, kPairs).created());

  std::atomic<bool> writer_done{false};
  std::atomic<uint64_t> whole{0}, permanent_whole{0}, split{0}, wrong{0},
      poisoned{0}, stalled{0};

  // Joins on every exit path, after telling the readers to stop.
  struct Threads {
    std::atomic<bool> &stop;
    std::vector<std::thread> threads;
    ~Threads() {
      stop.store(true, std::memory_order_release);
      for (auto &th : threads) {
        if (th.joinable()) th.join();
      }
    }
  } pool{writer_done, {}};

  for (int r = 0; r < kReaders; r++) {
    const rcu::ReaderId id = static_cast<rcu::ReaderId>(r);
    ASSERT_TRUE(domain_.Register(id).has_value());
    domain_.Online(id);
    pool.threads.emplace_back([&, r, id] {
      std::mt19937_64 rng(1000 + r);
      uint64_t round = 0;
      do {
        const uint64_t pair = round == 0 ? kPairs : rng() % kPairs;
        const bool alias_first = (rng() & 1) != 0;
        const uint64_t first = 2 * pair + (alias_first ? 1 : 0);
        const uint64_t second = 2 * pair + (alias_first ? 0 : 1);
        const auto a = t->FindHandle(first);
        if (a.handle.id.value() != 0) {
          if (a.via_alias != alias_first) wrong++;
          if (const Big *state = t->Peek(a.handle)) {
            if (state->magic.load() != Big::kAlive) poisoned++;
            if (state->value != pair || state->echo != pair * 7) wrong++;
          }
          const auto b = t->FindHandle(second);
          if (b.handle == a.handle) {
            whole++;
            if (round == 0) permanent_whole++;
            if (b.via_alias == alias_first) wrong++;
          } else if (t->Alive(a.handle)) {
            split++;  // the partner is missing, or another flow's, while this one lives
          }
        }
        round++;
        domain_.Quiescent(id);  // between "task invocations"
        // Let the writer in even when every thread shares one CPU, so that a
        // reader is not off-CPU, mid-grace-period, for a whole time slice.
        std::this_thread::yield();
      } while (!writer_done.load(std::memory_order_acquire));
      domain_.Offline(id);
      domain_.Unregister(id);
    });
  }
  pool.threads.emplace_back([&] {
    struct Finish {
      std::atomic<bool> &flag;
      ~Finish() { flag.store(true, std::memory_order_release); }
    } finish{writer_done};
    std::mt19937_64 rng(7);
    bool present[kPairs] = {};
    for (int cycle = 0; cycle < kCycles; cycle++) {
      const uint64_t p = rng() % kPairs;
      if (present[p]) {
        t->Erase(rng() & 1 ? 2 * p : 2 * p + 1);
        present[p] = false;
      } else {
        // Erased slots come back when every reader has passed a quiescent
        // state; if they have not yet, give them the CPU and try again.
        auto made = t->EmplaceAliased(2 * p, 2 * p + 1, p);
        for (int spin = 0;
             made.status == EmplaceStatus::kFull && spin < 1'000'000; spin++) {
          std::this_thread::yield();
          made = t->EmplaceAliased(2 * p, 2 * p + 1, p);
        }
        if (made.created()) {
          present[p] = true;
        } else {
          stalled++;
          break;
        }
      }
      if (cycle % 16 == 0) t->Reclaim();
    }
  });
  for (auto &th : pool.threads) th.join();

  EXPECT_EQ(0u, stalled.load()) << "the readers never let a slot come back";
  EXPECT_EQ(0u, split.load()) << "a reader found one key of a flow and not the other";
  EXPECT_EQ(0u, wrong.load());
  EXPECT_EQ(0u, poisoned.load()) << "a reader met a State after its destructor";
  EXPECT_EQ(static_cast<uint64_t>(kReaders), permanent_whole.load());
  EXPECT_GE(whole.load(), static_cast<uint64_t>(kReaders));

  t->Reclaim();
  EXPECT_EQ(0u, t->pending_reclaim());
  EXPECT_EQ(2 * t->size(), t->directory().size()) << "a key outlived its flow";
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
      ASSERT_EQ(by_key.size(), t->directory().size())
          << "the directory holds exactly the keys of the live flows";
      check(key);
      if (step % 101 == 0) {
        for (uint64_t k = 0; k < 150; k++) check(k);
      }
    }
    ASSERT_EQ(flows.size(), static_cast<size_t>(Big::alive.load()) - t->pending_reclaim());
    t.reset();
  }
}

// The same model against a directory that cannot hold the keys: every key
// shares one bucket pair, which fits `fit` of them (measured on a table of the
// same geometry), so creates and aliases are refused by placement, and aliased
// creates insert one key and take it back when the other does not fit. The
// model predicts every status exactly.
TEST_F(SharedFlowTablePlacementTest, CrowdedDirectoryMatchesAModelOfItsBucketCapacity) {
  constexpr size_t kCapacity = 12;
  constexpr size_t kUniverse = 30;
  for (uint32_t seed = 1; seed <= 3; seed++) {
    KeyGroups groups;
    size_t fit = 0;
    uint32_t directory_capacity = 0;
    {
      auto probe = Make<CountedTable>(64);
      directory_capacity = probe->directory().capacity();
      groups = MakeKeyGroups(probe->directory(), kUniverse, 0);
      while (fit < groups.crowd.size() &&
             probe->Emplace(groups.crowd[fit], 0).created()) {
        fit++;
      }
      ASSERT_GE(fit, 9u);
      ASSERT_LT(fit, groups.crowd.size());
    }
    auto t = Make<CountedTable>(kCapacity);
    ASSERT_EQ(directory_capacity, t->directory().capacity()) << "geometry differs";
    const std::vector<uint64_t> &universe = groups.crowd;

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
    uint64_t created = 0, refused = 0, placement_refusals = 0, rollbacks = 0,
             alias_refusals = 0;
    std::mt19937_64 rng(seed);

    auto check = [&](uint64_t key) {
      auto it = by_key.find(key);
      const Counted *state = t->Peek(key);
      if (it == by_key.end()) {
        ASSERT_EQ(nullptr, state) << "key " << key;
        ASSERT_FALSE(InDirectory(t->directory(), key)) << "key " << key;
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
    auto pick = [&] { return universe[rng() % kUniverse]; };
    auto forget = [&](uint64_t s) {
      dead.push_back(flows.at(s).handle);
      for (uint64_t k : flows.at(s).keys) by_key.erase(k);
      flows.erase(s);
    };

    for (int step = 0; step < 20000; step++) {
      const uint64_t key = pick();
      switch (rng() % 8) {
        case 0: {
          const auto r = t->Emplace(key, value);
          if (by_key.contains(key)) {
            ASSERT_EQ(EmplaceStatus::kExists, r.status);
          } else if (flows.size() >= kCapacity) {
            ASSERT_EQ(EmplaceStatus::kFull, r.status);
            refused++;
          } else if (by_key.size() + 1 > fit) {
            ASSERT_EQ(EmplaceStatus::kPlacementFailed, r.status);
            ASSERT_EQ(nullptr, r.state);
            refused++;
            placement_refusals++;
          } else {
            ASSERT_EQ(EmplaceStatus::kCreated, r.status);
            flows.emplace(serial, ModelFlow{value, {key}, key, r.handle});
            by_key[key] = serial++;
            value++;
            created++;
          }
          break;
        }
        case 1:
        case 2: {
          const uint64_t alias = pick();
          const auto r = t->EmplaceAliased(key, alias, value);
          if (by_key.contains(key)) {
            ASSERT_EQ(EmplaceStatus::kExists, r.status);
          } else if (alias == key || by_key.contains(alias)) {
            ASSERT_EQ(EmplaceStatus::kAliasExists, r.status);
          } else if (flows.size() >= kCapacity) {
            ASSERT_EQ(EmplaceStatus::kFull, r.status);
            refused++;
          } else if (by_key.size() + 2 > fit) {
            ASSERT_EQ(EmplaceStatus::kPlacementFailed, r.status);
            ASSERT_EQ(nullptr, r.state);
            refused++;
            placement_refusals++;
            if (by_key.size() + 1 <= fit) rollbacks++;  // the alias fitted first
          } else {
            ASSERT_EQ(EmplaceStatus::kCreated, r.status);
            flows.emplace(serial, ModelFlow{value, {key, alias}, key, r.handle});
            by_key[key] = serial;
            by_key[alias] = serial++;
            value++;
            created++;
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
          } else if (by_key.size() + 1 > fit) {
            ASSERT_EQ(AliasStatus::kPlacementFailed, got);
            alias_refusals++;
          } else {
            ASSERT_EQ(AliasStatus::kAdded, got);
            it->second.keys.insert(key);
            by_key[key] = it->first;
          }
          break;
        }
        case 4: {
          const bool present = by_key.contains(key);
          ASSERT_EQ(present, t->Erase(key));
          if (present) forget(by_key[key]);
          break;
        }
        case 5: {
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
        case 6: {
          for (int i = 0; i < 3 && !dead.empty(); i++) {
            const FlowHandle h = dead[rng() % dead.size()];
            ASSERT_EQ(nullptr, t->Peek(h)) << "a dead handle resolved";
            ASSERT_FALSE(t->Erase(h));
            ASSERT_FALSE(t->Alive(h));
          }
          break;
        }
        default:
          t->Reclaim();
      }
      ASSERT_EQ(flows.size(), t->size());
      ASSERT_EQ(by_key.size(), t->directory().size())
          << "the directory holds exactly the keys of the live flows";
      check(key);
      if (step % 101 == 0) {
        for (uint64_t k : universe) check(k);
      }
    }
    EXPECT_EQ(created, t->observer().created);
    EXPECT_EQ(refused, t->observer().rejected_full);
    EXPECT_GT(placement_refusals, 0u) << "the model never reached the directory's limit";
    EXPECT_GT(rollbacks, 0u) << "no aliased create inserted one key and lost the other";
    EXPECT_GT(alias_refusals, 0u);
    ASSERT_EQ(flows.size(), static_cast<size_t>(Counted::built - Counted::destroyed) -
                                t->pending_reclaim());
    t.reset();
  }
}

}  // namespace
}  // namespace bess::flow
