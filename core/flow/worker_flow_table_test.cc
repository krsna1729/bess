// SPDX-License-Identifier: BSD-3-Clause

#include "flow/worker_flow_table.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bess::flow {
namespace {

// A key that is not a five-tuple: BESS does not care what a flow is.
struct FiveTuple {
  uint32_t src;
  uint32_t dst;
  uint16_t sport;
  uint16_t dport;
  uint8_t proto;
  uint8_t pad[3];  // explicit and always zero, so the bytes are canonical
};
static_assert(ByteHashableFlowKey<FiveTuple>);

// Counts constructions and destructions, cannot be copied or moved: the table
// must build it where it stays.
struct Counted {
  static inline int alive = 0;
  static inline int constructed = 0;
  explicit Counted(uint64_t v, uint64_t tag = 0) : value(v), tag(tag) {
    if (v == ~uint64_t{0}) throw std::runtime_error("State constructor failed");
    alive++;
    constructed++;
  }
  Counted(const Counted &) = delete;
  Counted &operator=(const Counted &) = delete;
  ~Counted() { alive--; }
  uint64_t value;
  uint64_t tag;
};

using Table = WorkerFlowTable<uint64_t, Counted>;

struct AliasTraits : DefaultFlowTableTraits {
  static constexpr size_t kAliases = 2;
};
using AliasTable = WorkerFlowTable<uint64_t, Counted, DefaultFlowHash<uint64_t>,
                                   DefaultFlowEqual<uint64_t>, AliasTraits>;

struct FifoTraits : DefaultFlowTableTraits {
  static constexpr SlotReuse kReuse = SlotReuse::kFifo;
};
using FifoTable = WorkerFlowTable<uint64_t, Counted, DefaultFlowHash<uint64_t>,
                                  DefaultFlowEqual<uint64_t>, FifoTraits>;

template <typename T>
std::unique_ptr<T> Make(size_t capacity) {
  auto table = T::Create(capacity);
  EXPECT_TRUE(table.has_value());
  return std::move(*table);
}

class WorkerFlowTableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Counted::alive = 0;
    Counted::constructed = 0;
  }
  void TearDown() override {
    EXPECT_EQ(0, Counted::alive) << "a State was leaked or destroyed twice";
  }
};

TEST_F(WorkerFlowTableTest, CreateFindEraseAndDuplicate) {
  auto t = Make<Table>(8);
  EXPECT_EQ(nullptr, t->Find(5));

  auto created = t->Emplace(5, 50);
  ASSERT_TRUE(created.created());
  EXPECT_EQ(50u, created.state->value);
  EXPECT_EQ(created.state, t->Find(5));
  EXPECT_EQ(1u, t->size());

  // A duplicate returns the existing flow and does not build a State.
  const int built = Counted::constructed;
  auto again = t->Emplace(5, 99);
  EXPECT_EQ(EmplaceStatus::kExists, again.status);
  EXPECT_EQ(created.state, again.state);
  EXPECT_EQ(created.handle, again.handle);
  EXPECT_EQ(50u, again.state->value);
  EXPECT_EQ(built, Counted::constructed);
  EXPECT_EQ(1u, t->size());

  EXPECT_TRUE(t->Erase(5));
  EXPECT_FALSE(t->Erase(5)) << "erasing an absent key reports it";
  EXPECT_EQ(nullptr, t->Find(5));
  EXPECT_EQ(0u, t->size());
  EXPECT_EQ(0, Counted::alive);
}

TEST_F(WorkerFlowTableTest, CapacityExhaustionIsDefinedAndRecoverable) {
  struct Traits : DefaultFlowTableTraits {
    using Observer = FlowCounters;
  };
  using T = WorkerFlowTable<uint64_t, Counted, DefaultFlowHash<uint64_t>,
                            DefaultFlowEqual<uint64_t>, Traits>;
  auto t = Make<T>(10);
  for (uint64_t k = 1; k <= 10; k++) {
    ASSERT_TRUE(t->Emplace(k, k).created()) << k;
  }
  EXPECT_TRUE(t->full());

  const int built = Counted::constructed;
  auto refused = t->Emplace(11, 11);
  EXPECT_EQ(EmplaceStatus::kFull, refused.status);
  EXPECT_EQ(nullptr, refused.state);
  EXPECT_EQ(built, Counted::constructed) << "a refused create built a State";
  EXPECT_EQ(10u, t->size());
  EXPECT_EQ(nullptr, t->Find(11));
  EXPECT_EQ(1u, t->observer().rejected_full);

  // Existing keys still work, and a duplicate is `exists`, not `full`.
  for (uint64_t k = 1; k <= 10; k++) ASSERT_EQ(k, t->Find(k)->value);
  EXPECT_EQ(EmplaceStatus::kExists, t->Emplace(3, 0).status);

  // One erase frees exactly one slot.
  ASSERT_TRUE(t->Erase(4));
  EXPECT_TRUE(t->Emplace(11, 11).created());
  EXPECT_EQ(EmplaceStatus::kFull, t->Emplace(12, 12).status);
  EXPECT_EQ(2u, t->observer().rejected_full);
  EXPECT_EQ(11u, t->observer().created);
  EXPECT_EQ(1u, t->observer().erased);
}

TEST_F(WorkerFlowTableTest, StateIsBuiltInPlaceAndKeepsItsAddress) {
  auto t = Make<Table>(256);
  std::vector<std::pair<uint64_t, Counted *>> seen;
  for (uint64_t k = 1; k <= 200; k++) {
    seen.emplace_back(k, t->Emplace(k, k * 3).state);
  }
  // Creating and erasing other flows never moves a live one.
  for (uint64_t k = 201; k <= 256; k++) t->Emplace(k, k);
  for (uint64_t k = 201; k <= 256; k++) ASSERT_TRUE(t->Erase(k));
  for (auto &[k, state] : seen) {
    ASSERT_EQ(state, t->Find(k));
    ASSERT_EQ(k * 3, state->value);
  }
  EXPECT_EQ(200, Counted::alive);
  t.reset();
  EXPECT_EQ(0, Counted::alive) << "the table must destroy live State";
}

TEST_F(WorkerFlowTableTest, ThrowingStateConstructorChangesNothing) {
  auto t = Make<Table>(4);
  ASSERT_TRUE(t->Emplace(1, 1).created());
  EXPECT_THROW(t->Emplace(2, ~uint64_t{0}), std::runtime_error);
  EXPECT_EQ(1u, t->size());
  EXPECT_EQ(nullptr, t->Find(2));
  // The slot it would have used is still free: the table fills completely.
  for (uint64_t k = 3; k <= 5; k++) ASSERT_TRUE(t->Emplace(k, k).created());
  EXPECT_TRUE(t->full());
}

// Architecture section 7, rule 1: a handle must fail closed once its slot has
// been reused by a different flow.
TEST_F(WorkerFlowTableTest, StaleHandleCannotReachAFlowThatReusedItsSlot) {
  auto t = Make<Table>(4);
  const auto a = t->Emplace(100, 1);
  ASSERT_TRUE(a.created());
  ASSERT_EQ(a.state, t->Lookup(a.handle));
  ASSERT_TRUE(t->Erase(100));
  EXPECT_EQ(nullptr, t->Lookup(a.handle));
  EXPECT_FALSE(t->Alive(a.handle));

  // LIFO reuse: the very slot just freed is the next one handed out.
  const auto b = t->Emplace(200, 2);
  ASSERT_TRUE(b.created());
  EXPECT_EQ(a.handle.id, b.handle.id) << "the test needs the slot to be reused";
  EXPECT_NE(a.handle.generation, b.handle.generation);
  EXPECT_EQ(a.state, b.state) << "same slot, same State storage";

  EXPECT_EQ(nullptr, t->Lookup(a.handle)) << "stale handle reached the new flow";
  EXPECT_EQ(nullptr, t->KeyOf(a.handle));
  EXPECT_FALSE(t->Erase(a.handle)) << "a stale handle erased the new flow";
  EXPECT_EQ(b.state, t->Find(200));
  EXPECT_EQ(b.state, t->Lookup(b.handle));
  EXPECT_EQ(200u, *t->KeyOf(b.handle));

  // Handles that were never issued resolve to nothing.
  EXPECT_EQ(nullptr, t->Lookup(kNoFlow));
  EXPECT_EQ(nullptr, t->Lookup(FlowHandle{FlowId(99), 1}));
  EXPECT_EQ(nullptr, t->Lookup(FlowHandle{b.handle.id, b.handle.generation + 1}));
  // The generation of a free slot is even; a forged handle carrying it must not
  // resolve the dead flow.
  ASSERT_TRUE(t->Erase(200));
  EXPECT_EQ(nullptr, t->Lookup(FlowHandle{b.handle.id, b.handle.generation + 1}));
}

TEST_F(WorkerFlowTableTest, ReusePolicyDecidesHowSoonASlotIsReused) {
  auto lifo = Make<Table>(4);
  auto fifo = Make<FifoTable>(4);
  std::set<uint32_t> lifo_slots, fifo_slots;
  for (uint64_t k = 1; k <= 4; k++) {
    lifo_slots.insert(lifo->Emplace(k, k).handle.id.value());
    ASSERT_TRUE(lifo->Erase(k));
    fifo_slots.insert(fifo->Emplace(k, k).handle.id.value());
    ASSERT_TRUE(fifo->Erase(k));
  }
  EXPECT_EQ(1u, lifo_slots.size()) << "LIFO reuses the slot it just freed";
  EXPECT_EQ(4u, fifo_slots.size()) << "FIFO visits every slot before repeating";
}

// A slot whose generation counter is exhausted is retired, not reused: its
// next lifetime would otherwise issue handles equal to its first.
TEST_F(WorkerFlowTableTest, ExhaustedGenerationRetiresTheSlot) {
  auto t = Make<Table>(2);
  // Slot 0 (the first one handed out) is about to use its last generation.
  t->SetFreeSlotGenerationForTesting(0, 0xfffffffeu);
  const auto a = t->Emplace(1, 1);
  ASSERT_TRUE(a.created());
  EXPECT_EQ(1u, a.handle.id.value());
  EXPECT_EQ(0xffffffffu, a.handle.generation);
  ASSERT_TRUE(t->Erase(1));
  EXPECT_EQ(1u, t->quarantined_slots());
  EXPECT_EQ(1u, t->usable_capacity());
  EXPECT_EQ(nullptr, t->Lookup(a.handle));

  // Only the other slot can be used now.
  const auto b = t->Emplace(2, 2);
  EXPECT_EQ(2u, b.handle.id.value());
  EXPECT_EQ(EmplaceStatus::kFull, t->Emplace(3, 3).status);
  ASSERT_TRUE(t->Erase(2));
  const auto c = t->Emplace(4, 4);
  EXPECT_EQ(2u, c.handle.id.value()) << "the retired slot came back";
  EXPECT_EQ(nullptr, t->Lookup(a.handle));
}

TEST_F(WorkerFlowTableTest, AliasesNameOneFlowAndLeaveWithIt) {
  auto t = Make<AliasTable>(8);
  auto made = t->EmplaceAliased(10, 20, 7);
  ASSERT_TRUE(made.created());
  EXPECT_EQ(made.state, t->Find(10));
  EXPECT_EQ(made.state, t->Find(20));
  EXPECT_FALSE(t->FindRef(10).via_alias);
  const auto via = t->FindRef(20);
  EXPECT_TRUE(via.via_alias);
  EXPECT_EQ(made.handle, via.handle);
  EXPECT_EQ(1u, t->size()) << "two keys, one flow";

  // A third key joins later; a fourth finds no room (kAliases == 2).
  EXPECT_EQ(AliasStatus::kAdded, t->AddAlias(made.handle, 30));
  EXPECT_EQ(made.state, t->Find(30));
  EXPECT_EQ(AliasStatus::kNoRoom, t->AddAlias(made.handle, 40));
  EXPECT_EQ(nullptr, t->Find(40));
  // A key that is taken cannot be taken again, by this flow or another.
  auto other = t->Emplace(50, 5);
  EXPECT_EQ(AliasStatus::kExists, t->AddAlias(other.handle, 20));
  EXPECT_EQ(AliasStatus::kExists, t->AddAlias(other.handle, 10));
  EXPECT_EQ(AliasStatus::kExists, t->AddAlias(made.handle, 20));

  // Removing one alias keeps the flow and its other keys.
  EXPECT_TRUE(t->RemoveAlias(20));
  EXPECT_EQ(nullptr, t->Find(20));
  EXPECT_EQ(made.state, t->Find(10));
  EXPECT_EQ(made.state, t->Find(30));
  EXPECT_FALSE(t->RemoveAlias(20)) << "already gone";
  EXPECT_FALSE(t->RemoveAlias(10)) << "a primary key is not an alias";
  EXPECT_EQ(made.state, t->Find(10));
  EXPECT_FALSE(t->RemoveAlias(777));
  // The freed alias position is reusable.
  EXPECT_EQ(AliasStatus::kAdded, t->AddAlias(made.handle, 40));

  // Erasing through an alias erases the flow and every key it had.
  ASSERT_TRUE(t->Erase(30));
  for (uint64_t k : {10, 20, 30, 40}) {
    EXPECT_EQ(nullptr, t->Find(k)) << "alias " << k << " outlived its flow";
  }
  EXPECT_EQ(AliasStatus::kStale, t->AddAlias(made.handle, 60));
  EXPECT_EQ(other.state, t->Find(50));
  EXPECT_EQ(1u, t->size());

  // None of the freed keys is still held in the index: another flow can use
  // each of them.
  auto next = t->EmplaceAliased(20, 10, 9);
  ASSERT_TRUE(next.created());
  EXPECT_EQ(AliasStatus::kAdded, t->AddAlias(next.handle, 30));
  EXPECT_EQ(AliasStatus::kNoRoom, t->AddAlias(next.handle, 40));
}

TEST_F(WorkerFlowTableTest, AliasedCreateIsAllOrNothing) {
  auto t = Make<AliasTable>(3);
  ASSERT_TRUE(t->Emplace(1, 1).created());
  ASSERT_TRUE(t->Emplace(2, 2).created());
  const int built = Counted::constructed;

  EXPECT_EQ(EmplaceStatus::kExists, t->EmplaceAliased(1, 9, 0).status);
  EXPECT_EQ(EmplaceStatus::kAliasExists, t->EmplaceAliased(5, 2, 0).status);
  EXPECT_EQ(EmplaceStatus::kAliasExists, t->EmplaceAliased(5, 5, 0).status)
      << "an alias equal to the key would name the flow twice";
  EXPECT_EQ(nullptr, t->Find(5));
  EXPECT_EQ(nullptr, t->Find(9));
  EXPECT_EQ(built, Counted::constructed);
  EXPECT_EQ(2u, t->size());

  ASSERT_TRUE(t->EmplaceAliased(3, 4, 0).created());
  EXPECT_EQ(EmplaceStatus::kFull, t->EmplaceAliased(6, 7, 0).status);
  EXPECT_EQ(nullptr, t->Find(6));
  EXPECT_EQ(nullptr, t->Find(7));
}

// Keys with the same low bits, a constant hash and a 5-tuple: the table does
// not care what the key means.
TEST_F(WorkerFlowTableTest, WorksForAnyFixedKeyAndAnyHash) {
  struct ConstantHash {
    uint64_t operator()(const FiveTuple &) const noexcept { return 42; }
  };
  using Tuples = WorkerFlowTable<FiveTuple, Counted>;
  using Colliding = WorkerFlowTable<FiveTuple, Counted, ConstantHash,
                                    DefaultFlowEqual<FiveTuple>>;
  auto tuples = Make<Tuples>(500);
  auto colliding = Make<Colliding>(500);
  std::mt19937 rng(5);
  std::vector<FiveTuple> keys;
  for (int i = 0; i < 500; i++) {
    FiveTuple k{static_cast<uint32_t>(rng()), static_cast<uint32_t>(rng()),
                static_cast<uint16_t>(rng()), static_cast<uint16_t>(rng()),
                6,
                {}};
    keys.push_back(k);
    ASSERT_TRUE(tuples->Emplace(k, static_cast<uint64_t>(i)).created());
    ASSERT_TRUE(colliding->Emplace(k, static_cast<uint64_t>(i)).created());
  }
  for (int i = 0; i < 500; i++) {
    ASSERT_EQ(static_cast<uint64_t>(i), tuples->Find(keys[i])->value);
    ASSERT_EQ(static_cast<uint64_t>(i), colliding->Find(keys[i])->value)
        << "every key hashes alike; equality must decide";
  }
  FiveTuple absent = keys[0];
  absent.dport ^= 1;
  EXPECT_EQ(nullptr, tuples->Find(absent));
  EXPECT_EQ(nullptr, colliding->Find(absent));
}

TEST_F(WorkerFlowTableTest, FindBatchEqualsScalarFind) {
  auto t = Make<Table>(1000);
  for (uint64_t k = 0; k < 1000; k += 2) ASSERT_TRUE(t->Emplace(k, k).created());

  std::mt19937_64 rng(9);
  for (int round = 0; round < 200; round++) {
    const size_t n = rng() % 65;
    std::vector<uint64_t> keys(n);
    for (auto &k : keys) k = rng() % 1100;  // hits, misses, duplicates
    std::vector<Counted *> out(n, reinterpret_cast<Counted *>(1));
    const uint64_t hits = t->FindBatch(keys, out);
    for (size_t i = 0; i < n; i++) {
      Counted *scalar = t->Find(keys[i]);
      ASSERT_EQ(scalar, out[i]) << "key " << keys[i];
      ASSERT_EQ(scalar != nullptr, (hits >> i & 1) != 0);
    }
  }
}

TEST_F(WorkerFlowTableTest, ForEachVisitsExactlyTheLiveFlows) {
  auto t = Make<AliasTable>(50);
  std::set<uint64_t> live;
  for (uint64_t k = 1; k <= 50; k++) {
    ASSERT_TRUE(t->EmplaceAliased(k, k + 1000, k).created());
    live.insert(k);
  }
  for (uint64_t k = 1; k <= 50; k += 3) {
    ASSERT_TRUE(t->Erase(k + 1000));
    live.erase(k);
  }
  std::set<uint64_t> visited;
  t->ForEach([&](FlowHandle h, const uint64_t &key, Counted &state) {
    EXPECT_EQ(key, state.value);
    EXPECT_EQ(&state, t->Lookup(h));
    visited.insert(key);
  });
  EXPECT_EQ(live, visited);
}

// bytes per flow is part of the contract: the table reports what it holds.
TEST_F(WorkerFlowTableTest, ReportsMemoryPerFlow) {
  auto t = Make<Table>(100000);
  const double per_flow =
      static_cast<double>(t->memory_bytes()) / static_cast<double>(t->capacity());
  const double slot_and_free = Table::slot_bytes() + sizeof(uint32_t);
  EXPECT_GT(per_flow, slot_and_free);
  // The directory costs one 64-byte bucket per four keys.
  EXPECT_LT(per_flow, slot_and_free + 64.0 / 4.0 + 1.0);
}

// -- construction failure -----------------------------------------------------

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
struct FailingTraits : DefaultFlowTableTraits {
  using Allocator = FailingAllocator;
};
using FailingTable = WorkerFlowTable<uint64_t, Counted, DefaultFlowHash<uint64_t>,
                                     DefaultFlowEqual<uint64_t>, FailingTraits>;

TEST_F(WorkerFlowTableTest, ConstructionFailureLeavesNothingAllocated) {
  FailingAllocator::live = 0;
  // The factory makes three allocations; whichever one fails, the ones made
  // before it are released and the caller gets an error, not a half table.
  for (int fail = 0; fail < 3; fail++) {
    FailingAllocator::Reset(fail);
    auto table = FailingTable::Create(1000);
    ASSERT_FALSE(table.has_value()) << "allocation " << fail;
    EXPECT_EQ(FlowTableError::kOutOfMemory, table.error());
    EXPECT_EQ(0, FailingAllocator::live) << "allocation " << fail << " leaked";
  }
  FailingAllocator::Reset(-1);
  {
    auto table = FailingTable::Create(1000);
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ(3, FailingAllocator::live);
    ASSERT_TRUE((*table)->Emplace(1, 1).created());
  }
  EXPECT_EQ(0, FailingAllocator::live) << "destroying the table must free it";
}

TEST_F(WorkerFlowTableTest, CreateRejectsCapacitiesItCannotHonour) {
  EXPECT_EQ(FlowTableError::kInvalidCapacity, Table::Create(0).error());
  EXPECT_EQ(FlowTableError::kInvalidCapacity,
            Table::Create(size_t{1} << 33).error())
      << "more flows than 32-bit ids can name";
  EXPECT_EQ(FlowTableError::kInvalidCapacity,
            AliasTable::Create(0x60000000u).error())
      << "capacity * keys per flow must fit 32 bits";
  // Slots too big for the byte count to be represented.
  struct Huge {
    std::byte data[size_t{1} << 40];
  };
  EXPECT_EQ(FlowTableError::kTooLarge,
            (WorkerFlowTable<uint64_t, Huge>::Create(size_t{1} << 24).error()));
}

// -- ownership ----------------------------------------------------------------

struct FakeOwner {
  static constexpr bool kChecked = true;
  static inline OwnerToken current = 1;
  static OwnerToken Current() noexcept { return current; }
};
struct OwnedTraits : DefaultFlowTableTraits {
  using Owner = FakeOwner;
};
using OwnedTable = WorkerFlowTable<uint64_t, Counted, DefaultFlowHash<uint64_t>,
                                   DefaultFlowEqual<uint64_t>, OwnedTraits>;

static_assert(std::is_empty_v<OwnerGuard<UncheckedOwner>>,
              "an unchecked owner policy must cost no storage");

TEST_F(WorkerFlowTableTest, FirstCallerOwnsAndOthersAreReported) {
  FakeOwner::current = 7;
  auto t = Make<OwnedTable>(8);
  EXPECT_EQ(kNoOwner, t->owner());
  ASSERT_TRUE(t->Emplace(1, 1).created());
  EXPECT_EQ(7u, t->owner());
  EXPECT_NE(nullptr, t->Find(1));

  FakeOwner::current = 8;
  EXPECT_DEATH(t->Find(1), "ownership violation in Find");
  EXPECT_DEATH(t->Emplace(2, 2), "ownership violation in Emplace");
  EXPECT_DEATH(t->Erase(1), "ownership violation in Erase");
  EXPECT_DEATH(t->Lookup(FlowHandle{FlowId(1), 1}), "ownership violation");
  EXPECT_EQ(7u, t->owner()) << "a violation must not change the owner";

  // Hand-off: the owner releases, and the next worker to call becomes owner.
  FakeOwner::current = 7;
  t->ReleaseOwner();
  FakeOwner::current = 8;
  EXPECT_NE(nullptr, t->Find(1));
  EXPECT_EQ(8u, t->owner());
  FakeOwner::current = 7;
  EXPECT_DEATH(t->ReleaseOwner(), "ownership violation in ReleaseOwner");
  FakeOwner::current = 1;
}

TEST_F(WorkerFlowTableTest, ThreadOwnerTellsThreadsApart) {
  EXPECT_NE(kNoOwner, ThreadOwner::Current());
  const OwnerToken mine = ThreadOwner::Current();
  OwnerToken theirs = kNoOwner;
  std::thread([&] { theirs = ThreadOwner::Current(); }).join();
  EXPECT_NE(kNoOwner, theirs);
  EXPECT_NE(mine, theirs);
  EXPECT_EQ(mine, ThreadOwner::Current());
  EXPECT_NE(TokenOf(dataplane::WorkerId(0)), kNoOwner);
}

// -- the expiry seam ----------------------------------------------------------

// What the timer substrate of M10 will be to a flow table, in miniature: it
// learns of flows from the observer hooks, keeps only handles, and expires by
// calling Erase(handle). No timer logic belongs in the table.
struct FakeExpiry {
  std::multimap<uint64_t, FlowHandle> deadlines;  // deadline -> flow
  uint64_t now = 0;
  int creates = 0, erases = 0, fulls = 0;

  void OnCreate(FlowHandle h, Counted &state) noexcept {
    creates++;
    deadlines.emplace(now + state.value, h);  // value = idle timeout
  }
  void OnErase(FlowHandle h, Counted &) noexcept {
    erases++;
    for (auto it = deadlines.begin(); it != deadlines.end(); ++it) {
      if (it->second == h) {
        deadlines.erase(it);
        break;
      }
    }
  }
  void OnFull() noexcept { fulls++; }
};
struct ExpiryTraits : DefaultFlowTableTraits {
  using Observer = FakeExpiry;
};
using ExpiryTable = WorkerFlowTable<uint64_t, Counted, DefaultFlowHash<uint64_t>,
                                    DefaultFlowEqual<uint64_t>, ExpiryTraits>;

TEST_F(WorkerFlowTableTest, ExpiryEngineDrivesTheTableThroughHandles) {
  using T = ExpiryTable;
  auto t = Make<T>(8);
  static_assert(FlowObserver<FakeExpiry, Counted>);
  FakeExpiry &expiry = t->observer();

  expiry.now = 100;
  const auto a = t->Emplace(1, 10);  // expires at 110
  const auto b = t->Emplace(2, 50);  // expires at 150
  const auto c = t->Emplace(3, 10);  // expires at 110
  ASSERT_EQ(3, expiry.creates);
  ASSERT_EQ(3u, expiry.deadlines.size());

  auto tick = [&](uint64_t now) {
    expiry.now = now;
    std::vector<FlowHandle> due;
    for (auto &[deadline, handle] : expiry.deadlines) {
      if (deadline <= now) due.push_back(handle);
    }
    size_t expired = 0;
    for (FlowHandle h : due) expired += t->Erase(h) ? 1 : 0;
    return expired;
  };
  EXPECT_EQ(0u, tick(109));
  // The application erases c itself before its deadline: the hook cancels
  // the record.
  ASSERT_TRUE(t->Erase(3));
  EXPECT_EQ(1, expiry.erases);
  EXPECT_EQ(2u, expiry.deadlines.size());
  EXPECT_EQ(1u, tick(110));
  EXPECT_EQ(nullptr, t->Lookup(a.handle));
  EXPECT_NE(nullptr, t->Lookup(b.handle));
  EXPECT_EQ(2, expiry.erases);
  EXPECT_EQ(1u, tick(1000));
  EXPECT_EQ(0u, t->size());
  EXPECT_EQ(0u, expiry.deadlines.size());
  (void)c;
}

// M10's exit criterion in advance: a timer record that outlived its flow
// cannot expire the flow that reused the slot.
TEST_F(WorkerFlowTableTest, StaleExpiryRecordCannotKillTheNewFlow) {
  using T = ExpiryTable;
  auto t = Make<T>(1);
  const auto old_flow = t->Emplace(1, 10);
  const FlowHandle stale_record = old_flow.handle;
  ASSERT_TRUE(t->Erase(1));
  const auto new_flow = t->Emplace(2, 10);
  ASSERT_EQ(old_flow.handle.id, new_flow.handle.id);

  EXPECT_FALSE(t->Erase(stale_record)) << "expired the wrong generation";
  EXPECT_EQ(new_flow.state, t->Find(2));
  EXPECT_EQ(1u, t->size());
  EXPECT_TRUE(t->Erase(new_flow.handle));
}

TEST_F(WorkerFlowTableTest, FullTableReportsThroughTheObserver) {
  using T = ExpiryTable;
  auto t = Make<T>(2);
  t->Emplace(1, 1);
  t->Emplace(2, 1);
  EXPECT_EQ(EmplaceStatus::kFull, t->Emplace(3, 1).status);
  EXPECT_EQ(1, t->observer().fulls);
  EXPECT_EQ(2, t->observer().creates);
}

// -- reference model ----------------------------------------------------------

// Drives random operations against both the table and plain std containers
// and compares everything observable after each step. Small key domain, a
// weak hash and a tiny capacity force collisions, chains, duplicates and
// full tables all the time.
struct WeakHash {
  uint64_t operator()(const uint64_t &k) const noexcept { return k % 13; }
};

template <typename T>
void RunModel(uint32_t seed, size_t capacity, uint64_t key_domain, int steps) {
  auto table_or = T::Create(capacity);
  ASSERT_TRUE(table_or.has_value());
  auto &t = **table_or;

  struct ModelFlow {
    uint64_t value;
    std::set<uint64_t> keys;  // primary first inserted; the set holds all
    uint64_t primary;
    FlowHandle handle;
  };
  std::map<uint64_t, ModelFlow> flows;        // flow serial -> flow
  std::unordered_map<uint64_t, uint64_t> by_key;  // key -> flow serial
  std::vector<FlowHandle> dead_handles;
  uint64_t next_serial = 1, next_value = 1;
  std::mt19937_64 rng(seed);
  constexpr size_t kAlias = T::kAliases;

  auto check_one = [&](uint64_t key) {
    auto it = by_key.find(key);
    Counted *state = t.Find(key);
    if (it == by_key.end()) {
      ASSERT_EQ(nullptr, state) << "key " << key << " must be absent";
      return;
    }
    const ModelFlow &flow = flows.at(it->second);
    ASSERT_NE(nullptr, state) << "key " << key << " lost";
    ASSERT_EQ(flow.value, state->value);
    const auto ref = t.FindRef(key);
    ASSERT_EQ(flow.handle, ref.handle);
    ASSERT_EQ(key != flow.primary, ref.via_alias);
    ASSERT_EQ(state, t.Lookup(flow.handle));
  };

  for (int step = 0; step < steps; step++) {
    const uint64_t key = rng() % key_domain;
    switch (rng() % 8) {
      case 0:
      case 1: {  // create
        auto result = t.Emplace(key, next_value);
        const bool present = by_key.contains(key);
        if (present) {
          ASSERT_EQ(EmplaceStatus::kExists, result.status);
        } else if (flows.size() >= capacity) {
          ASSERT_EQ(EmplaceStatus::kFull, result.status);
          ASSERT_EQ(nullptr, result.state);
        } else {
          ASSERT_EQ(EmplaceStatus::kCreated, result.status);
          ModelFlow flow{next_value, {key}, key, result.handle};
          flows.emplace(next_serial, flow);
          by_key[key] = next_serial++;
          next_value++;
        }
        break;
      }
      case 2: {  // erase by key (primary or alias)
        const bool present = by_key.contains(key);
        ASSERT_EQ(present, t.Erase(key));
        if (present) {
          const uint64_t serial = by_key[key];
          dead_handles.push_back(flows.at(serial).handle);
          for (uint64_t k : flows.at(serial).keys) by_key.erase(k);
          flows.erase(serial);
        }
        break;
      }
      case 3: {  // erase by handle: live or stale
        if (!flows.empty() && rng() % 2) {
          auto it = flows.begin();
          std::advance(it, rng() % flows.size());
          ASSERT_TRUE(t.Erase(it->second.handle));
          dead_handles.push_back(it->second.handle);
          for (uint64_t k : it->second.keys) by_key.erase(k);
          flows.erase(it);
        } else if (!dead_handles.empty()) {
          const FlowHandle h = dead_handles[rng() % dead_handles.size()];
          ASSERT_FALSE(t.Erase(h)) << "a dead handle erased something";
        }
        break;
      }
      case 4: {  // add alias to a random live flow
        if constexpr (kAlias > 0) {
          if (flows.empty()) break;
          auto it = flows.begin();
          std::advance(it, rng() % flows.size());
          ModelFlow &flow = it->second;
          const AliasStatus got = t.AddAlias(flow.handle, key);
          if (by_key.contains(key)) {
            ASSERT_EQ(AliasStatus::kExists, got);
          } else if (flow.keys.size() >= 1 + kAlias) {
            ASSERT_EQ(AliasStatus::kNoRoom, got);
          } else {
            ASSERT_EQ(AliasStatus::kAdded, got);
            flow.keys.insert(key);
            by_key[key] = it->first;
          }
        }
        break;
      }
      case 5: {  // remove an alias
        if constexpr (kAlias > 0) {
          auto it = by_key.find(key);
          const bool is_alias =
              it != by_key.end() && flows.at(it->second).primary != key;
          ASSERT_EQ(is_alias, t.RemoveAlias(key));
          if (is_alias) {
            flows.at(it->second).keys.erase(key);
            by_key.erase(it);
          }
        }
        break;
      }
      case 6: {  // stale handles never resolve
        for (int i = 0; i < 4 && !dead_handles.empty(); i++) {
          const FlowHandle h = dead_handles[rng() % dead_handles.size()];
          ASSERT_EQ(nullptr, t.Lookup(h)) << "a dead handle resolved";
          ASSERT_EQ(nullptr, t.KeyOf(h));
        }
        break;
      }
      default: {  // batch lookup against the model
        uint64_t keys[32];
        Counted *out[32];
        for (auto &k : keys) k = rng() % key_domain;
        const uint64_t hits = t.FindBatch(std::span<const uint64_t>(keys, 32),
                                          std::span<Counted *>(out, 32));
        for (int i = 0; i < 32; i++) {
          ASSERT_EQ(by_key.contains(keys[i]), (hits >> i & 1) != 0);
          if (by_key.contains(keys[i])) {
            ASSERT_EQ(flows.at(by_key[keys[i]]).value, out[i]->value);
          }
        }
        break;
      }
    }
    ASSERT_EQ(flows.size(), t.size());
    ASSERT_EQ(flows.size() == capacity, t.full());
    check_one(key);
    if (step % 97 == 0) {
      for (uint64_t k = 0; k < key_domain; k++) check_one(k);
      size_t visited = 0;
      t.ForEach([&](FlowHandle, const uint64_t &, Counted &) { visited++; });
      ASSERT_EQ(flows.size(), visited);
    }
  }
  ASSERT_EQ(static_cast<int>(flows.size()), Counted::alive);
}

TEST_F(WorkerFlowTableTest, RandomOperationsMatchAStdModel) {
  for (uint32_t seed = 1; seed <= 6; seed++) {
    RunModel<Table>(seed, 64, 200, 40000);
    if (HasFatalFailure()) return;
    RunModel<AliasTable>(seed, 48, 300, 40000);
    if (HasFatalFailure()) return;
    RunModel<FifoTable>(seed, 37, 120, 40000);
    if (HasFatalFailure()) return;
  }
  using WeakTable = WorkerFlowTable<uint64_t, Counted, WeakHash,
                                    DefaultFlowEqual<uint64_t>, AliasTraits>;
  for (uint32_t seed = 11; seed <= 13; seed++) {
    RunModel<WeakTable>(seed, 100, 250, 40000);
    if (HasFatalFailure()) return;
  }
  // A one-flow table is the edge of the sizing arithmetic.
  RunModel<Table>(99, 1, 4, 20000);
}

}  // namespace
}  // namespace bess::flow
