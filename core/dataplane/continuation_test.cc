// SPDX-License-Identifier: BSD-3-Clause

#include "dataplane/continuation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "dataplane/expiry_wheel.h"
#include "dataplane/interface_id.h"

// Allocation counting and failure injection for the table's one allocation
// (the technique of expiry_wheel_test.cc, with a refusal added): while a
// window is open every allocation is counted, and the Nth can be made to fail.
// The nothrow forms the table uses fall back on these throwing ones.
namespace {
std::atomic<bool> g_window_open{false};
std::atomic<size_t> g_allocations{0};
std::atomic<size_t> g_frees{0};
std::atomic<int64_t> g_fail_at{-1};

class AllocationWindow {
 public:
  // fail_at: the 0-based index of the allocation to refuse, or -1.
  explicit AllocationWindow(int64_t fail_at = -1) {
    g_allocations = 0;
    g_frees = 0;
    g_fail_at = fail_at;
    g_window_open = true;
  }
  ~AllocationWindow() { g_window_open = false; }
  size_t allocations() const { return g_allocations.load(); }
  size_t frees() const { return g_frees.load(); }
};

void *Allocate(std::size_t n, std::size_t align) {
  if (g_window_open.load(std::memory_order_relaxed)) {
    const size_t index = g_allocations++;
    if (static_cast<int64_t>(index) == g_fail_at.load()) {
      throw std::bad_alloc();
    }
  }
  if (align <= alignof(std::max_align_t)) {
    if (void *p = std::malloc(n == 0 ? 1 : n)) {
      return p;
    }
  } else if (void *p = std::aligned_alloc(align, (n + align - 1) / align * align)) {
    return p;
  }
  throw std::bad_alloc();
}
void Release(void *p) noexcept {
  if (p != nullptr && g_window_open.load(std::memory_order_relaxed)) {
    g_frees++;
  }
  std::free(p);
}
}  // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void *operator new(std::size_t n) { return Allocate(n, 0); }
void *operator new[](std::size_t n) { return Allocate(n, 0); }
void *operator new(std::size_t n, std::align_val_t a) {
  return Allocate(n, static_cast<std::size_t>(a));
}
void *operator new[](std::size_t n, std::align_val_t a) {
  return Allocate(n, static_cast<std::size_t>(a));
}
void operator delete(void *p) noexcept { Release(p); }
void operator delete[](void *p) noexcept { Release(p); }
void operator delete(void *p, std::size_t) noexcept { Release(p); }
void operator delete[](void *p, std::size_t) noexcept { Release(p); }
void operator delete(void *p, std::align_val_t) noexcept { Release(p); }
void operator delete[](void *p, std::align_val_t) noexcept { Release(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept {
  Release(p);
}
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept {
  Release(p);
}
#pragma GCC diagnostic pop

namespace bess::dataplane {
namespace {

struct Hop {
  uint32_t interface;
  uint32_t stage;
  friend bool operator==(const Hop &, const Hop &) = default;
};

using Table = ContinuationTable<Hop>;

std::unique_ptr<Table> Make(size_t capacity) {
  auto table = Table::Create(capacity);
  EXPECT_TRUE(table.has_value());
  return std::move(table).value();
}

// Threads of a concurrency test. Whatever way the test body exits -- a failed
// ASSERT that returns early included -- they are told to stop and joined, so a
// failure is reported instead of terminating the process with joinable threads.
class Threads {
 public:
  Threads() = default;
  Threads(const Threads &) = delete;
  Threads &operator=(const Threads &) = delete;
  ~Threads() { Join(); }

  std::atomic<bool> &stop() { return stop_; }
  void Start(std::function<void()> body) {
    threads_.emplace_back(std::move(body));
  }
  void Join() {
    stop_ = true;
    for (auto &thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    threads_.clear();
  }

 private:
  std::atomic<bool> stop_{false};
  std::vector<std::thread> threads_;
};

// -- the handle type ----------------------------------------------------------------

TEST(ContinuationHandleTest, IsOneEightByteWordOfItsOwnIdType) {
  static_assert(sizeof(ContinuationHandle) == 8);
  static_assert(std::is_same_v<ContinuationHandle,
                               GenerationHandle<ContinuationId>>);
  static_assert(!std::is_same_v<ContinuationId, InterfaceId>);
  static_assert(!std::is_same_v<ContinuationId, ExpiryId>);
  static_assert(!std::is_same_v<ContinuationHandle, ExpiryHandle>,
                "a handle of one kind must not be passed where another is meant");
  static_assert(!std::is_convertible_v<InterfaceId, ContinuationId>);
  EXPECT_EQ(kNoContinuation, ContinuationHandle{});
  EXPECT_EQ(0u, kNoContinuation.id.value());
  EXPECT_EQ(0u, kNoContinuation.generation);
}

TEST(ContinuationTableTest, CreateRefusesBadCapacities) {
  EXPECT_EQ(ContinuationError::kInvalidCapacity, Table::Create(0).error());
  EXPECT_EQ(ContinuationError::kInvalidCapacity,
            Table::Create(std::numeric_limits<uint32_t>::max()).error());
  EXPECT_EQ(ContinuationError::kInvalidCapacity,
            Table::Create(std::numeric_limits<size_t>::max()).error());
  auto one = Table::Create(1);
  ASSERT_TRUE(one.has_value());
  EXPECT_EQ(1u, (*one)->capacity());
  EXPECT_EQ(0u, (*one)->size());
  EXPECT_NE(std::string(ToString(ContinuationError::kOutOfMemory)),
            std::string(ToString(ContinuationError::kInvalidCapacity)));
}

// -- lifecycle ----------------------------------------------------------------------

TEST(ContinuationTableTest, IssueResolveRetire) {
  auto table = Make(4);
  const ContinuationHandle a = table->Issue({1, 10});
  const ContinuationHandle b = table->Issue({2, 20});
  ASSERT_NE(a, kNoContinuation);
  ASSERT_NE(b, kNoContinuation);
  EXPECT_NE(a, b);
  EXPECT_NE(a.id, b.id);
  EXPECT_EQ(1u, a.generation & 1u) << "a live handle's generation is odd";
  EXPECT_EQ(2u, table->size());

  EXPECT_EQ((Hop{1, 10}), table->Resolve(a));
  EXPECT_EQ((Hop{2, 20}), table->Resolve(b));

  EXPECT_TRUE(table->Retire(a));
  EXPECT_EQ(1u, table->size());
  EXPECT_EQ(std::nullopt, table->Resolve(a));
  EXPECT_EQ((Hop{2, 20}), table->Resolve(b)) << "b is not affected";
  EXPECT_FALSE(table->Retire(a)) << "a handle retires once";
  EXPECT_EQ(1u, table->size());
  EXPECT_TRUE(table->Retire(b));
  EXPECT_EQ(0u, table->size());
}

TEST(ContinuationTableTest, FullTableRefusesAndChangesNothing) {
  auto table = Make(3);
  std::vector<ContinuationHandle> live;
  for (uint32_t i = 0; i < 3; i++) {
    live.push_back(table->Issue({i, 0}));
    ASSERT_NE(live.back(), kNoContinuation);
  }
  EXPECT_TRUE(table->full());
  EXPECT_EQ(kNoContinuation, table->Issue({9, 9}));
  EXPECT_EQ(kNoContinuation, table->Issue({9, 9}));
  EXPECT_EQ(3u, table->size());
  for (uint32_t i = 0; i < 3; i++) {
    EXPECT_EQ((Hop{i, 0}), table->Resolve(live[i])) << "unchanged by refusals";
  }
  ASSERT_TRUE(table->Retire(live[1]));
  EXPECT_FALSE(table->full());
  const ContinuationHandle again = table->Issue({7, 7});
  EXPECT_NE(again, kNoContinuation);
  EXPECT_EQ((Hop{7, 7}), table->Resolve(again));
}

// architecture.md section 7: a handle that outlived its object resolves to
// nothing even after its slot holds another one, and cannot retire it.
TEST(ContinuationTableTest, StaleHandleCannotReachAContinuationThatReusedItsSlot) {
  auto table = Make(1);
  const ContinuationHandle first = table->Issue({1, 1});
  ASSERT_TRUE(table->Retire(first));
  const ContinuationHandle second = table->Issue({2, 2});
  ASSERT_NE(second, kNoContinuation);
  ASSERT_EQ(first.id, second.id) << "a one-slot table must reuse its slot";
  ASSERT_NE(first.generation, second.generation);

  EXPECT_EQ(std::nullopt, table->Resolve(first));
  EXPECT_FALSE(table->Retire(first)) << "a stale handle retires nothing";
  EXPECT_EQ((Hop{2, 2}), table->Resolve(second)) << "the new one is untouched";
  EXPECT_EQ(1u, table->size());
  EXPECT_TRUE(table->Retire(second));
  EXPECT_EQ(std::nullopt, table->Resolve(second));
}

// A slot freed longest ago is reused first, so a handle parked in a queue sees
// the longest possible time before its slot can hold anything else.
TEST(ContinuationTableTest, SlotsAreReusedOldestFreedFirst) {
  auto table = Make(4);
  std::vector<ContinuationHandle> h;
  for (uint32_t i = 0; i < 4; i++) {
    h.push_back(table->Issue({i, 0}));
  }
  ASSERT_TRUE(table->Retire(h[2]));
  ASSERT_TRUE(table->Retire(h[0]));
  ASSERT_TRUE(table->Retire(h[3]));
  EXPECT_EQ(h[2].id, table->Issue({10, 0}).id);
  EXPECT_EQ(h[0].id, table->Issue({11, 0}).id);
  EXPECT_EQ(h[3].id, table->Issue({12, 0}).id);
  EXPECT_EQ(kNoContinuation, table->Issue({13, 0}));

  // Churn one continuation at a time through a four-slot table: the slots
  // take turns, so each one's generation advances every fourth time.
  auto churn = Make(4);
  for (uint32_t i = 0; i < 40; i++) {
    const ContinuationHandle handle = churn->Issue({i, 0});
    EXPECT_EQ(1 + (i % 4), handle.id.value()) << i;
    EXPECT_EQ(1u + 2 * (i / 4), handle.generation) << i;
    ASSERT_TRUE(churn->Retire(handle));
  }
}

// -- forged handles -----------------------------------------------------------------

TEST(ContinuationTableTest, ForgedHandlesFailClosed) {
  auto table = Make(3);
  const ContinuationHandle live = table->Issue({5, 5});
  const ContinuationHandle dead = table->Issue({6, 6});
  ASSERT_TRUE(table->Retire(dead));
  ASSERT_NE(live, kNoContinuation);

  std::vector<ContinuationHandle> forged = {
      kNoContinuation,
      {ContinuationId(0), 1},                  // id zero is nobody
      {ContinuationId(0), live.generation},
      {ContinuationId(4), 1},                  // one past the capacity
      {ContinuationId(1u << 20), 1},
      {ContinuationId(0xFFFFFFFFu), 1},
      {ContinuationId(0xFFFFFFFFu), 0xFFFFFFFFu},
      {live.id, 0},                            // even generations are never live
      {live.id, live.generation + 1},
      {live.id, live.generation + 2},          // the next life of the slot
      {live.id, live.generation - 1},
      {live.id, 0xFFFFFFFFu},
      {dead.id, dead.generation},              // retired
      {dead.id, dead.generation + 1},          // the even value it holds now
      {dead.id, dead.generation + 2},
      {ContinuationId(3), 1},                  // a free slot, never issued
  };
  for (const ContinuationHandle &handle : forged) {
    SCOPED_TRACE(::testing::Message()
                 << "id " << handle.id.value() << " generation " << handle.generation);
    EXPECT_EQ(std::nullopt, table->Resolve(handle));
    EXPECT_FALSE(table->Retire(handle));
  }
  EXPECT_EQ((Hop{5, 5}), table->Resolve(live)) << "forgeries disturbed nothing";
  EXPECT_EQ(1u, table->size());
  // The slot a forged handle named is still usable.
  EXPECT_NE(kNoContinuation, table->Issue({7, 7}));
  EXPECT_NE(kNoContinuation, table->Issue({8, 8}));
  EXPECT_EQ(kNoContinuation, table->Issue({9, 9}));
}

// -- generation exhaustion ------------------------------------------------------------

TEST(ContinuationTableTest, ASlotWhoseGenerationWouldWrapIsRetiredForGood) {
  auto table = Make(2);
  table->SetNextSlotGenerationForTesting(0xFFFFFFFCu);
  const ContinuationHandle a = table->Issue({1, 1});
  EXPECT_EQ(0xFFFFFFFDu, a.generation);
  ASSERT_TRUE(table->Retire(a));
  EXPECT_EQ(0u, table->quarantined());

  // The other slot, then the first slot again: its last life.
  const ContinuationHandle b = table->Issue({2, 2});
  EXPECT_NE(a.id, b.id);
  const ContinuationHandle c = table->Issue({3, 3});
  EXPECT_EQ(a.id, c.id);
  EXPECT_EQ(0xFFFFFFFFu, c.generation);
  EXPECT_EQ((Hop{3, 3}), table->Resolve(c));
  EXPECT_EQ(std::nullopt, table->Resolve(a)) << "the earlier life stays dead";

  ASSERT_TRUE(table->Retire(c));
  EXPECT_EQ(1u, table->quarantined()) << "the next generation would be zero";
  EXPECT_EQ(std::nullopt, table->Resolve(c));
  EXPECT_FALSE(table->Retire(c));

  // The retired slot is never handed out again; the other still is.
  EXPECT_EQ(kNoContinuation, table->Issue({4, 4})) << "b still holds the other";
  ASSERT_TRUE(table->Retire(b));
  const ContinuationHandle d = table->Issue({5, 5});
  EXPECT_EQ(b.id, d.id);
  EXPECT_EQ(kNoContinuation, table->Issue({6, 6}));
  EXPECT_EQ(std::nullopt, table->Resolve(a));
  EXPECT_EQ(std::nullopt, table->Resolve(c));
  EXPECT_EQ(1u, table->size());
  EXPECT_EQ(1u, table->quarantined());
}

// -- the target ------------------------------------------------------------------------

template <size_t N>
struct Bytes {
  std::array<uint8_t, N> bytes;
  friend bool operator==(const Bytes &, const Bytes &) = default;
};

template <size_t N>
void CheckTargetOfSize() {
  using T = ContinuationTable<Bytes<N>>;
  auto table = T::Create(8);
  ASSERT_TRUE(table.has_value()) << N;
  std::mt19937 rng(static_cast<uint32_t>(N));
  std::vector<std::pair<ContinuationHandle, Bytes<N>>> issued;
  for (int i = 0; i < 8; i++) {
    Bytes<N> value;
    for (auto &byte : value.bytes) {
      byte = static_cast<uint8_t>(rng());
    }
    const ContinuationHandle handle = (*table)->Issue(value);
    ASSERT_NE(handle, kNoContinuation);
    issued.emplace_back(handle, value);
  }
  for (const auto &[handle, value] : issued) {
    const auto got = (*table)->Resolve(handle);
    ASSERT_TRUE(got.has_value()) << N;
    EXPECT_EQ(0, std::memcmp(got->bytes.data(), value.bytes.data(), N))
        << "a " << N << "-byte target did not survive the table";
  }
}

TEST(ContinuationTableTest, TargetsOfEverySizeRoundTripByteForByte) {
  CheckTargetOfSize<1>();
  CheckTargetOfSize<3>();
  CheckTargetOfSize<7>();
  CheckTargetOfSize<8>();
  CheckTargetOfSize<9>();
  CheckTargetOfSize<12>();
  CheckTargetOfSize<24>();
  CheckTargetOfSize<33>();
  CheckTargetOfSize<64>();
  static_assert(Table::slot_bytes() == 16, "a two-word target costs 16 B/slot");
}

// -- against a model ------------------------------------------------------------------

// Everything the table promises, predicted exactly: FIFO slot reuse, the
// generation each slot issues next, which handles are live, and the answer for
// every handle in a grid around them (live, stale, free, forged).
TEST(ContinuationTableTest, RandomOperationsMatchAModel) {
  for (uint32_t seed = 0; seed < 300; seed++) {
    std::mt19937 rng(seed);
    const uint32_t capacity = 1 + rng() % 6;
    auto table = Make(capacity);
    // Model.
    std::deque<uint32_t> free_slots;
    for (uint32_t i = 1; i <= capacity; i++) {
      free_slots.push_back(i);
    }
    std::vector<uint32_t> generation(capacity + 1, 0);
    std::map<std::pair<uint32_t, uint32_t>, Hop> live;
    std::vector<ContinuationHandle> history;

    for (int step = 0; step < 400; step++) {
      const uint32_t op = rng() % 3;
      if (op == 0) {
        const Hop hop{static_cast<uint32_t>(rng()), static_cast<uint32_t>(rng())};
        const ContinuationHandle handle = table->Issue(hop);
        if (free_slots.empty()) {
          ASSERT_EQ(kNoContinuation, handle) << seed << ":" << step;
        } else {
          const uint32_t slot = free_slots.front();
          free_slots.pop_front();
          generation[slot]++;
          ASSERT_EQ(slot, handle.id.value()) << seed << ":" << step;
          ASSERT_EQ(generation[slot], handle.generation) << seed << ":" << step;
          live[{slot, generation[slot]}] = hop;
          history.push_back(handle);
        }
      } else if (op == 1 && !history.empty()) {
        const ContinuationHandle handle = history[rng() % history.size()];
        const bool expect = live.erase({handle.id.value(), handle.generation}) == 1;
        ASSERT_EQ(expect, table->Retire(handle)) << seed << ":" << step;
        if (expect) {
          generation[handle.id.value()]++;
          free_slots.push_back(handle.id.value());
        }
      }
      ASSERT_EQ(live.size(), table->size());
      ASSERT_EQ(free_slots.empty(), table->full());
      // The grid: every id up to two past the capacity, every generation up to
      // a little past the highest any slot has reached.
      const uint32_t top = *std::max_element(generation.begin(), generation.end());
      for (uint32_t id = 0; id <= capacity + 2; id++) {
        for (uint32_t gen = 0; gen <= top + 2; gen++) {
          const auto it = live.find({id, gen});
          const std::optional<Hop> expect =
              it == live.end() ? std::nullopt : std::optional<Hop>(it->second);
          ASSERT_EQ(expect, table->Resolve({ContinuationId(id), gen}))
              << "seed " << seed << " step " << step << " id " << id
              << " generation " << gen;
        }
      }
    }
  }
}

// -- allocation -----------------------------------------------------------------------

TEST(ContinuationTableTest, NothingAllocatesAfterCreate) {
  auto table = Make(64);
  std::vector<ContinuationHandle> handles(64);
  const AllocationWindow window;
  for (int round = 0; round < 50; round++) {
    for (auto &h : handles) {
      h = table->Issue({1, 2});
    }
    EXPECT_EQ(kNoContinuation, table->Issue({3, 4}));
    for (auto &h : handles) {
      EXPECT_TRUE(table->Resolve(h).has_value());
      EXPECT_TRUE(table->Retire(h));
      EXPECT_FALSE(table->Resolve(h).has_value());
    }
  }
  EXPECT_EQ(0u, window.allocations());
}

// The allocator refusing at setup is an error, never a crash, and takes nothing
// with it.
TEST(ContinuationTableTest, AllocatorRefusalAtCreateIsAnErrorAndLeaksNothing) {
  {
    const AllocationWindow window(0);  // the slot array
    const auto table = Table::Create(16);
    ASSERT_FALSE(table.has_value());
    EXPECT_EQ(ContinuationError::kOutOfMemory, table.error());
    EXPECT_EQ(window.allocations(), window.frees() + 1)
        << "the refused allocation is the only one not freed";
  }
  {
    const AllocationWindow window(1);  // the table object, after its slots
    const auto table = Table::Create(16);
    ASSERT_FALSE(table.has_value());
    EXPECT_EQ(ContinuationError::kOutOfMemory, table.error());
    EXPECT_EQ(2u, window.allocations());
    EXPECT_EQ(1u, window.frees()) << "the slot array was released";
  }
  {
    const AllocationWindow window;
    {
      auto table = Table::Create(16);
      ASSERT_TRUE(table.has_value());
      EXPECT_NE(kNoContinuation, (*table)->Issue({1, 1}));
    }
    EXPECT_EQ(window.allocations(), window.frees());
  }
}

// -- threads --------------------------------------------------------------------------

struct Probe {
  uint64_t seq;
  uint64_t inverted;
  uint64_t mixed;
};
Probe ProbeFor(uint64_t seq) { return {seq, ~seq, seq * 0x9E3779B97F4A7C15ull}; }
bool Coherent(const Probe &p) {
  return p.inverted == ~p.seq && p.mixed == p.seq * 0x9E3779B97F4A7C15ull;
}

// One thread issues and retires; others resolve concurrently, as a punting
// worker and the service threads that hold its handles do. A resolved target
// must be whole (never a mix of two lives), must be the one issued for exactly
// that handle, and a handle whose retirement has completed must never resolve.
// The volume (the owner's operations, each resolver's resolves) is the loop
// condition, not a time window; a yield every few hundred operations keeps the
// threads interleaving on one CPU.
TEST(ContinuationTableTest, ConcurrentResolversNeverSeeATornOrStaleTarget) {
  constexpr uint32_t kCapacity = 32;
  constexpr uint64_t kOwnerOps = 150000;
  constexpr uint64_t kResolvesEach = 150000;
  constexpr int kResolvers = 3;
  using ProbeTable = ContinuationTable<Probe>;
  auto created = ProbeTable::Create(kCapacity);
  ASSERT_TRUE(created.has_value());
  ProbeTable &table = **created;

  // Handles by issue sequence, append-only: [seq] is valid once seq <
  // `published`. `retired[seq]` is set after Retire(handle of seq) returned.
  std::vector<std::atomic<uint64_t>> handle_of(kOwnerOps + 1);
  std::vector<std::atomic<uint8_t>> retired(kOwnerOps + 1);
  std::atomic<uint64_t> published{0};
  std::atomic<bool> owner_done{false};
  std::atomic<uint64_t> torn{0}, wrong{0}, zombies{0}, resolved{0}, missed{0};

  Threads threads;
  for (int r = 0; r < kResolvers; r++) {
    threads.Start([&, r] {
      std::mt19937_64 rng(1000 + r);
      uint64_t done = 0;
      while (!threads.stop().load(std::memory_order_relaxed) &&
             !(owner_done.load(std::memory_order_acquire) &&
               done >= kResolvesEach)) {
        const uint64_t count = published.load(std::memory_order_acquire);
        if (count == 0) {
          std::this_thread::yield();
          continue;
        }
        // Mostly the recent past (live or just retired), sometimes anything.
        const uint64_t seq = (rng() % 4 != 0 && count > 64)
                                 ? count - 1 - rng() % 64
                                 : rng() % count;
        const bool was_retired =
            retired[seq].load(std::memory_order_acquire) != 0;
        const ContinuationHandle handle = std::bit_cast<ContinuationHandle>(
            handle_of[seq].load(std::memory_order_acquire));
        const std::optional<Probe> got = table.Resolve(handle);
        if (got.has_value()) {
          if (!Coherent(*got)) {
            torn++;
          } else if (got->seq != seq) {
            wrong++;
          }
          if (was_retired) {
            zombies++;
          }
          resolved++;
        } else {
          missed++;
        }
        if ((++done & 0xFF) == 0) {
          std::this_thread::yield();
        }
      }
    });
  }

  std::mt19937_64 rng(7);
  std::vector<uint64_t> live;  // sequences currently live
  uint64_t next = 0;
  for (uint64_t op = 0; op < kOwnerOps; op++) {
    const bool issue = live.empty() || (live.size() < kCapacity - 4 && rng() % 2 == 0);
    if (issue) {
      const ContinuationHandle handle = table.Issue(ProbeFor(next));
      ASSERT_NE(handle, kNoContinuation);
      handle_of[next].store(std::bit_cast<uint64_t>(handle),
                            std::memory_order_release);
      published.store(next + 1, std::memory_order_release);
      live.push_back(next);
      next++;
    } else {
      const size_t at = rng() % live.size();
      const uint64_t seq = live[at];
      live[at] = live.back();
      live.pop_back();
      ASSERT_TRUE(table.Retire(std::bit_cast<ContinuationHandle>(
          handle_of[seq].load(std::memory_order_relaxed))));
      retired[seq].store(1, std::memory_order_release);
    }
    if ((op & 0xFF) == 0) {
      std::this_thread::yield();
    }
  }
  // One more that stays live to the end, so the resolvers always have a live
  // handle to find once the owner has stopped.
  const ContinuationHandle last = table.Issue(ProbeFor(next));
  ASSERT_NE(last, kNoContinuation);
  handle_of[next].store(std::bit_cast<uint64_t>(last), std::memory_order_release);
  published.store(next + 1, std::memory_order_release);
  owner_done.store(true, std::memory_order_release);
  threads.Join();

  EXPECT_EQ(0u, torn.load()) << "a target mixed two continuations";
  EXPECT_EQ(0u, wrong.load()) << "a handle resolved to another continuation's target";
  EXPECT_EQ(0u, zombies.load()) << "a retired handle resolved";
  EXPECT_GT(resolved.load(), 0u) << "the resolvers never found a live handle";
  EXPECT_GT(missed.load(), 0u) << "the resolvers never met a retired handle";
  const std::optional<Probe> final_target = table.Resolve(last);
  ASSERT_TRUE(final_target.has_value());
  EXPECT_EQ(next, final_target->seq);
}

}  // namespace
}  // namespace bess::dataplane
