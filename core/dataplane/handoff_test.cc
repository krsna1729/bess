// SPDX-License-Identifier: BSD-3-Clause

#include "dataplane/handoff.h"

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <cstdint>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "dataplane/continuation.h"
#include "packet.h"
#include "packet_pool.h"

// Allocation counting for the packet-path calls: every allocation made while
// the window is open is counted. (The technique of expiry_wheel_test.cc.)
namespace {
std::atomic<bool> g_window_open{false};
std::atomic<size_t> g_allocations{0};

class AllocationWindow {
 public:
  AllocationWindow() {
    g_allocations = 0;
    g_window_open = true;
  }
  ~AllocationWindow() { g_window_open = false; }
  size_t allocations() const { return g_allocations.load(); }
};

void *Allocate(std::size_t n, std::size_t align) {
  if (g_window_open.load(std::memory_order_relaxed)) {
    g_allocations++;
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
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
#pragma GCC diagnostic pop

namespace bess::dataplane {
namespace {

constexpr HandoffTopology kSpSc = HandoffTopology::kSpSc;
constexpr HandoffTopology kMpSc = HandoffTopology::kMpSc;
constexpr HandoffTopology kMpMc = HandoffTopology::kMpMc;

// -- contexts of every size the ring has to carry ------------------------------------

struct Ctx8 {  // item 16 bytes
  uint64_t id;
};
struct Ctx16 {  // item 24 bytes: not a power of two, a continuation inside
  uint32_t reason;
  uint32_t seq;
  ContinuationHandle continuation;
};
struct Ctx24 {  // item 32 bytes
  uint64_t a, b, c;
};
struct Ctx56 {  // item 64 bytes: the largest there is
  uint64_t w[7];
};

static_assert(sizeof(PuntItem<NoContext>) == 8, "no context: the pointer alone");
static_assert(sizeof(PuntItem<Ctx8>) == 16);
static_assert(sizeof(PuntItem<Ctx16>) == 24);
static_assert(sizeof(PuntItem<Ctx24>) == 32);
static_assert(sizeof(PuntItem<Ctx56>) == 64);
static_assert(HandoffChannel<NoContext>::kItemBytes == 8);
static_assert(HandoffChannel<Ctx56>::kItemBytes == HandoffChannel<Ctx56>::kMaxItemBytes);
static_assert(std::is_trivially_copyable_v<PuntItem<Ctx16>>);

template <typename C>
C MakeContext(uint64_t id);
template <>
NoContext MakeContext<NoContext>(uint64_t) {
  return {};
}
template <>
Ctx8 MakeContext<Ctx8>(uint64_t id) {
  return {id};
}
template <>
Ctx16 MakeContext<Ctx16>(uint64_t id) {
  return {static_cast<uint32_t>(id), static_cast<uint32_t>(id * 7),
          ContinuationHandle{ContinuationId(static_cast<uint32_t>(id + 1)),
                             static_cast<uint32_t>(2 * id + 1)}};
}
template <>
Ctx24 MakeContext<Ctx24>(uint64_t id) {
  return {id, ~id, id * 0x9E3779B97F4A7C15ull};
}
template <>
Ctx56 MakeContext<Ctx56>(uint64_t id) {
  Ctx56 c;
  for (int i = 0; i < 7; i++) {
    c.w[i] = id * 31 + static_cast<uint64_t>(i);
  }
  return c;
}

template <typename C>
bool IsContextOf(const C &context, uint64_t id) {
  const C expect = MakeContext<C>(id);
  return std::memcmp(&context, &expect, sizeof(C)) == 0;
}
template <>
bool IsContextOf<NoContext>(const NoContext &, uint64_t) {
  return true;
}

// A packet handle that points at nothing: the channel never dereferences a
// packet except to free it, so tests of the queue itself use these and drain
// before the channel dies.
PacketHandle Fake(uint64_t id) {
  return reinterpret_cast<PacketHandle>(uintptr_t{0x10000} + id * 64);
}
uint64_t IdOf(PacketHandle packet) {
  return (reinterpret_cast<uintptr_t>(packet) - uintptr_t{0x10000}) / 64;
}

// A real packet carries its identity in its first payload word.
void SetTag(PacketHandle packet, uint64_t id) {
  std::memcpy(rte_pktmbuf_mtod(packet, void *), &id, sizeof(id));
}
uint64_t TagOf(PacketHandle packet) {
  uint64_t id;
  std::memcpy(&id, rte_pktmbuf_mtod(packet, void *), sizeof(id));
  return id;
}

// -- an allocator the tests control -----------------------------------------------------

struct TestAllocator {
  static inline int allocations = 0;
  static inline int deallocations = 0;
  static inline int last_socket = -2;
  static inline size_t last_bytes = 0;
  static inline bool refuse = false;
  static inline int placed = 0;

  static void Reset() {
    allocations = deallocations = 0;
    last_socket = -2;
    last_bytes = 0;
    refuse = false;
    placed = 0;
  }
  static void *Allocate(size_t bytes, size_t align, int socket,
                        int *placed_socket) noexcept {
    allocations++;
    last_socket = socket;
    last_bytes = bytes;
    if (refuse) {
      return nullptr;
    }
    *placed_socket = placed;
    return std::aligned_alloc(align, (bytes + align - 1) / align * align);
  }
  static void Deallocate(void *block) noexcept {
    deallocations++;
    std::free(block);
  }
  static const HandoffAllocator &Get() {
    static const HandoffAllocator allocator{Allocate, Deallocate};
    return allocator;
  }
};

// A channel of fake packets: it is drained, never freed through, when the test
// ends, however it ends.
template <typename C, HandoffTopology T>
class FakeChannel {
 public:
  using Channel = HandoffChannel<C, T>;
  using Item = typename Channel::Item;

  explicit FakeChannel(size_t capacity) {
    HandoffConfig config;
    config.capacity = capacity;
    config.allocator = &TestAllocator::Get();
    auto created = Channel::Create(config);
    CHECK(created.has_value());
    channel_ = std::move(created).value();
  }
  FakeChannel(const FakeChannel &) = delete;
  FakeChannel &operator=(const FakeChannel &) = delete;
  ~FakeChannel() {
    channel_->Drain([](Item &) noexcept {});
  }
  Channel *operator->() { return channel_.get(); }
  Channel &operator*() { return *channel_; }

 private:
  typename Channel::Ptr channel_;
};

template <typename C>
PuntItem<C> MakeItem(uint64_t id) {
  return {Fake(id), MakeContext<C>(id)};
}

template <HandoffTopology T>
using Topo = std::integral_constant<HandoffTopology, T>;
using AllTopologies = ::testing::Types<Topo<kSpSc>, Topo<kMpSc>, Topo<kMpMc>>;

struct TopologyName {
  template <typename T>
  static std::string GetName(int) {
    switch (T::value) {
      case HandoffTopology::kSpSc:
        return "SpSc";
      case HandoffTopology::kMpSc:
        return "MpSc";
      case HandoffTopology::kMpMc:
        return "MpMc";
    }
    return "?";
  }
};

template <typename T>
class HandoffTest : public ::testing::Test {
 public:
  static constexpr HandoffTopology kTopology = T::value;
};
TYPED_TEST_SUITE(HandoffTest, AllTopologies, TopologyName);

// -- ownership ----------------------------------------------------------------------

// After a punt the producer owns exactly the items the channel did not take:
// the taken ones are moved-from (null), the rest are as they were.
TYPED_TEST(HandoffTest, AcceptedItemsAreMovedAndRefusedItemsStayWithTheCaller) {
  FakeChannel<Ctx8, TestFixture::kTopology> ch(5);
  using Item = PuntItem<Ctx8>;
  std::array<Item, 8> items;
  for (uint64_t i = 0; i < items.size(); i++) {
    items[i] = MakeItem<Ctx8>(i);
  }
  const auto original = items;

  const PuntResult result = ch->TryPuntBurst(items);
  EXPECT_EQ(5u, result.accepted);
  ASSERT_TRUE(result.refused.has_value());
  EXPECT_EQ(HandoffError::kFull, *result.refused);
  EXPECT_FALSE(static_cast<bool>(result));
  for (size_t i = 0; i < 5; i++) {
    EXPECT_EQ(nullptr, items[i].packet) << i << " was handed over: not ours now";
  }
  for (size_t i = 5; i < 8; i++) {
    EXPECT_EQ(original[i].packet, items[i].packet) << i << " was refused: still ours";
    EXPECT_EQ(original[i].context.id, items[i].context.id);
  }
  HandoffStats stats = ch->stats();
  EXPECT_EQ(5u, stats.enqueued);
  EXPECT_EQ(3u, stats.refused_full);
  EXPECT_EQ(0u, stats.refused_closed);
  EXPECT_EQ(5u, stats.occupancy);
  EXPECT_TRUE(ch->full());
  EXPECT_EQ(0u, ch->free_space());

  // The consumer gets them in order, with their contexts, and they are its.
  std::array<Item, 3> out;
  EXPECT_EQ(3u, ch->Dequeue(out));
  for (uint64_t i = 0; i < 3; i++) {
    EXPECT_EQ(Fake(i), out[i].packet);
    EXPECT_EQ(i, out[i].context.id);
  }
  EXPECT_EQ(2u, ch->size());
  EXPECT_EQ(3u, ch->free_space());

  // The caller retries the refused tail now that there is room: all taken.
  const PuntResult again =
      ch->TryPuntBurst(std::span<Item>(items).subspan(result.accepted));
  EXPECT_EQ(3u, again.accepted);
  EXPECT_TRUE(static_cast<bool>(again));
  EXPECT_FALSE(again.refused.has_value());
  for (size_t i = 5; i < 8; i++) {
    EXPECT_EQ(nullptr, items[i].packet);
  }
  std::array<Item, 8> rest;
  ASSERT_EQ(5u, ch->Dequeue(rest));
  const uint64_t expect_ids[] = {3, 4, 5, 6, 7};
  for (size_t i = 0; i < 5; i++) {
    EXPECT_EQ(Fake(expect_ids[i]), rest[i].packet);
    EXPECT_EQ(expect_ids[i], rest[i].context.id);
  }
  stats = ch->stats();
  EXPECT_EQ(8u, stats.enqueued);
  EXPECT_EQ(8u, stats.dequeued);
  EXPECT_EQ(3u, stats.refused_full);
  EXPECT_EQ(0u, stats.occupancy);
  EXPECT_TRUE(ch->empty());
}

TYPED_TEST(HandoffTest, TryPuntMovesOneOrRefusesAndSaysWhy) {
  FakeChannel<Ctx8, TestFixture::kTopology> ch(1);
  PacketHandle first = Fake(1), second = Fake(2);
  ASSERT_TRUE(ch->TryPunt(first, Ctx8{11}).has_value());
  EXPECT_EQ(nullptr, first) << "handed over: the caller's handle is moved-from";

  const auto full = ch->TryPunt(second, Ctx8{22});
  ASSERT_FALSE(full.has_value());
  EXPECT_EQ(HandoffError::kFull, full.error());
  EXPECT_EQ(Fake(2), second) << "refused: still the caller's, untouched";

  PuntItem<Ctx8> out;
  ASSERT_EQ(1u, ch->Dequeue(std::span<PuntItem<Ctx8>>(&out, 1)));
  EXPECT_EQ(Fake(1), out.packet);
  EXPECT_EQ(11u, out.context.id);

  ch->Close();
  const auto closed = ch->TryPunt(second, Ctx8{22});
  ASSERT_FALSE(closed.has_value());
  EXPECT_EQ(HandoffError::kClosed, closed.error());
  EXPECT_EQ(Fake(2), second);
  const HandoffStats stats = ch->stats();
  EXPECT_EQ(1u, stats.refused_full);
  EXPECT_EQ(1u, stats.refused_closed);
}

TYPED_TEST(HandoffTest, ZeroLengthCallsAreNoOps) {
  FakeChannel<Ctx8, TestFixture::kTopology> ch(4);
  const PuntResult result = ch->TryPuntBurst({});
  EXPECT_EQ(0u, result.accepted);
  EXPECT_FALSE(result.refused.has_value());
  EXPECT_EQ(0u, ch->Dequeue({}));
  ch->Close();
  EXPECT_FALSE(ch->TryPuntBurst({}).refused.has_value())
      << "nothing offered, nothing refused, even when closed";
  const HandoffStats stats = ch->stats();
  EXPECT_EQ(0u, stats.enqueued + stats.dequeued + stats.refused_full +
                    stats.refused_closed + stats.discarded);
}

TYPED_TEST(HandoffTest, ClosedRefusesEverythingKeepsQueuedItemsAndIsIdempotent) {
  FakeChannel<Ctx8, TestFixture::kTopology> ch(8);
  using Item = PuntItem<Ctx8>;
  std::array<Item, 3> queued{MakeItem<Ctx8>(1), MakeItem<Ctx8>(2), MakeItem<Ctx8>(3)};
  ASSERT_EQ(3u, ch->TryPuntBurst(queued).accepted);
  EXPECT_FALSE(ch->closed());
  ch->Close();
  ch->Close();
  EXPECT_TRUE(ch->closed());

  std::array<Item, 4> offered{MakeItem<Ctx8>(10), MakeItem<Ctx8>(11),
                              MakeItem<Ctx8>(12), MakeItem<Ctx8>(13)};
  const PuntResult result = ch->TryPuntBurst(offered);
  EXPECT_EQ(0u, result.accepted);
  ASSERT_TRUE(result.refused.has_value());
  EXPECT_EQ(HandoffError::kClosed, *result.refused);
  for (uint64_t i = 0; i < 4; i++) {
    EXPECT_EQ(Fake(10 + i), offered[i].packet) << "a closed channel took nothing";
  }
  EXPECT_EQ(3u, ch->size()) << "closing loses nothing already queued";

  // The consumer can still take what is queued.
  std::array<Item, 8> out;
  ASSERT_EQ(3u, ch->Dequeue(out));
  EXPECT_EQ(Fake(1), out[0].packet);
  EXPECT_EQ(Fake(3), out[2].packet);
  const HandoffStats stats = ch->stats();
  EXPECT_EQ(3u, stats.enqueued);
  EXPECT_EQ(3u, stats.dequeued);
  EXPECT_EQ(4u, stats.refused_closed);
  EXPECT_EQ(0u, stats.refused_full);
}

TYPED_TEST(HandoffTest, CapacityIsExactForAnyRequestedSize) {
  for (const size_t capacity : {1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33, 100}) {
    FakeChannel<Ctx8, TestFixture::kTopology> ch(capacity);
    ASSERT_EQ(capacity, ch->capacity());
    size_t taken = 0;
    for (;;) {
      PacketHandle packet = Fake(taken);
      if (!ch->TryPunt(packet, Ctx8{taken}).has_value()) {
        EXPECT_NE(nullptr, packet) << "a refused packet stays with the caller";
        break;
      }
      taken++;
      ASSERT_LE(taken, capacity) << "the channel took more than its capacity";
    }
    EXPECT_EQ(capacity, taken) << "capacity " << capacity;
    EXPECT_TRUE(ch->full());
    EXPECT_EQ(0u, ch->free_space());
    // One whole-burst refusal, then it drains to the same items it took.
    std::vector<PuntItem<Ctx8>> burst = {MakeItem<Ctx8>(9999)};
    EXPECT_EQ(0u, ch->TryPuntBurst(burst).accepted);
    std::vector<PuntItem<Ctx8>> out(capacity + 3);
    ASSERT_EQ(capacity, ch->Dequeue(out));
    for (size_t i = 0; i < capacity; i++) {
      ASSERT_EQ(i, out[i].context.id);
    }
  }
}

TYPED_TEST(HandoffTest, DrainHandsEveryQueuedItemToTheCallerAndCountsItDiscarded) {
  FakeChannel<Ctx24, TestFixture::kTopology> ch(40);
  std::vector<PuntItem<Ctx24>> items;
  for (uint64_t i = 0; i < 37; i++) {
    items.push_back(MakeItem<Ctx24>(i));
  }
  ASSERT_EQ(37u, ch->TryPuntBurst(items).accepted);
  std::array<PuntItem<Ctx24>, 5> taken;
  ASSERT_EQ(5u, ch->Dequeue(taken));

  std::vector<uint64_t> drained;
  const size_t removed = ch->Drain([&](PuntItem<Ctx24> &item) noexcept {
    EXPECT_TRUE(IsContextOf(item.context, IdOf(item.packet)));
    drained.push_back(IdOf(item.packet));
  });
  EXPECT_EQ(32u, removed);
  ASSERT_EQ(32u, drained.size());
  for (uint64_t i = 0; i < 32; i++) {
    EXPECT_EQ(5 + i, drained[i]) << "in order, each exactly once";
  }
  const HandoffStats stats = ch->stats();
  EXPECT_EQ(37u, stats.enqueued);
  EXPECT_EQ(5u, stats.dequeued);
  EXPECT_EQ(32u, stats.discarded);
  EXPECT_EQ(0u, stats.occupancy);
  EXPECT_EQ(stats.enqueued, stats.dequeued + stats.discarded + stats.occupancy);
  EXPECT_EQ(0u, ch->Drain([](PuntItem<Ctx24> &) noexcept {})) << "nothing left";
}

// -- the indices ----------------------------------------------------------------------

// Every sequence of enqueue and dequeue bursts of 1..capacity+1 items up to a
// depth, from every ring position that matters (all alignments to the ring
// size, either side of the 2^32 wrap of the free-running indices, and the 2^31
// boundary), checked step by step against a queue. Between sequences the
// channel is put back in the position the sequence started from by moving the
// indices (`SetIndicesForTesting`) and re-queueing, not by replaying.
template <HandoffTopology T>
class ExhaustiveWalk {
 public:
  using Item = PuntItem<Ctx8>;

  ExhaustiveWalk(size_t capacity, int depth)
      : capacity_(capacity), depth_(depth), channel_(capacity) {}

  // Runs every sequence from ring position `start`; returns the nodes visited.
  uint64_t Run(uint32_t start) {
    channel_->SetIndicesForTesting(start);
    queue_n_ = 0;
    cons_tail_ = start;
    next_id_ = 1;
    start_ = start;
    nodes_ = 0;
    Visit(0);
    return nodes_;
  }
  bool failed() const { return failed_; }

 private:
  struct State {
    std::array<uint64_t, 8> queue;
    size_t queue_n;
    uint32_t cons_tail;
    uint64_t next_id;
  };

  State Save() const { return {queue_, queue_n_, cons_tail_, next_id_}; }

  void Restore(const State &saved) {
    Item scratch[16];
    while (channel_->Dequeue(scratch) != 0) {
    }
    channel_->SetIndicesForTesting(saved.cons_tail);
    Item items[8];
    for (size_t i = 0; i < saved.queue_n; i++) {
      items[i] = MakeItem<Ctx8>(saved.queue[i]);
    }
    if (saved.queue_n != 0) {
      const PuntResult r =
          channel_->TryPuntBurst(std::span<Item>(items, saved.queue_n));
      CHECK_EQ(saved.queue_n, r.accepted);
    }
    queue_ = saved.queue;
    queue_n_ = saved.queue_n;
    cons_tail_ = saved.cons_tail;
    next_id_ = saved.next_id;
  }

  void Fail(const char *what, size_t burst, int depth) {
    if (!failed_) {
      failed_ = true;
      ADD_FAILURE() << what << ": capacity " << capacity_ << ", start index "
                    << start_ << ", depth " << depth << ", burst " << burst
                    << ", occupancy " << queue_n_;
    }
  }

  void Visit(int depth) {
    if (depth == depth_ || failed_) {
      return;
    }
    const size_t max_burst = capacity_ + 1;
    for (size_t b = 1; b <= max_burst && !failed_; b++) {
      const State saved = Save();
      Enqueue(b, depth);
      Visit(depth + 1);
      Restore(saved);
    }
    for (size_t b = 1; b <= max_burst && !failed_; b++) {
      const State saved = Save();
      Dequeue(b, depth);
      Visit(depth + 1);
      Restore(saved);
    }
  }

  void Enqueue(size_t b, int depth) {
    nodes_++;
    Item items[8];
    for (size_t i = 0; i < b; i++) {
      items[i] = MakeItem<Ctx8>(next_id_ + i);
    }
    const size_t expect = std::min(b, capacity_ - queue_n_);
    const PuntResult r = channel_->TryPuntBurst(std::span<Item>(items, b));
    if (r.accepted != expect) {
      Fail("wrong number accepted", b, depth);
    }
    if (r.refused.has_value() != (expect < b) ||
        (expect < b && *r.refused != HandoffError::kFull)) {
      Fail("wrong refusal", b, depth);
    }
    for (size_t i = 0; i < b; i++) {
      if ((i < expect) != (items[i].packet == nullptr)) {
        Fail("ownership: wrong items moved-from", b, depth);
      }
    }
    for (size_t i = 0; i < expect; i++) {
      queue_[queue_n_++] = next_id_ + i;
    }
    next_id_ += b;
    CheckOccupancy(b, depth);
  }

  void Dequeue(size_t b, int depth) {
    nodes_++;
    Item out[8];
    const size_t expect = std::min(b, queue_n_);
    const size_t got = channel_->Dequeue(std::span<Item>(out, b));
    if (got != expect) {
      Fail("wrong number dequeued", b, depth);
      return;
    }
    for (size_t i = 0; i < got; i++) {
      if (out[i].packet != Fake(queue_[i]) || out[i].context.id != queue_[i]) {
        Fail("wrong item dequeued", b, depth);
      }
    }
    for (size_t i = got; i < queue_n_; i++) {
      queue_[i - got] = queue_[i];
    }
    queue_n_ -= got;
    cons_tail_ += static_cast<uint32_t>(got);
    CheckOccupancy(b, depth);
  }

  void CheckOccupancy(size_t b, int depth) {
    if (channel_->size() != queue_n_ ||
        channel_->free_space() != capacity_ - queue_n_ ||
        channel_->empty() != (queue_n_ == 0) ||
        channel_->full() != (queue_n_ == capacity_)) {
      Fail("occupancy disagrees with the model", b, depth);
    }
  }

  const size_t capacity_;
  const int depth_;
  FakeChannel<Ctx8, T> channel_;
  std::array<uint64_t, 8> queue_{};
  size_t queue_n_ = 0;
  uint32_t cons_tail_ = 0;
  uint64_t next_id_ = 1;
  uint32_t start_ = 0;
  uint64_t nodes_ = 0;
  bool failed_ = false;
};

TYPED_TEST(HandoffTest, IndexWraparoundIsExhaustivelyConsistent) {
  // {capacity, depth}: the deeper the smaller the channel, so that each sequence
  // set stays in the hundreds of thousands.
  struct Shape {
    size_t capacity;
    int depth;
  };
  const Shape shapes[] = {{1, 8}, {2, 6}, {3, 5}, {4, 4}, {5, 4}};
  uint64_t nodes = 0;
  for (const Shape &shape : shapes) {
    const uint32_t ring = static_cast<uint32_t>(std::bit_ceil(shape.capacity + 1));
    ExhaustiveWalk<TestFixture::kTopology> walk(shape.capacity, shape.depth);
    std::vector<uint32_t> starts;
    for (uint32_t k = 0; k < ring; k++) {
      starts.push_back(k);  // every alignment to the ring from zero
    }
    for (uint32_t k = 0; k < 3 * ring; k++) {
      starts.push_back(0u - k);  // and the same running up to, over, the wrap
    }
    for (uint32_t k = 0; k < 3; k++) {
      starts.push_back(0x7FFFFFFFu + k);  // the signed boundary
    }
    for (const uint32_t start : starts) {
      nodes += walk.Run(start);
      ASSERT_FALSE(walk.failed());
    }
  }
  // The walk must have been broad: this guards against the shapes quietly
  // shrinking to nothing, not against machine speed.
  EXPECT_GT(nodes, 1000000u);
}

// -- against a model, across contexts and topologies ----------------------------------

template <typename C, HandoffTopology T>
void RunDifferential(uint32_t seed) {
  std::mt19937_64 rng(seed);
  const size_t capacity = 1 + rng() % 40;
  FakeChannel<C, T> ch(capacity);
  using Item = PuntItem<C>;
  // Half the runs start with the indices near the 32-bit wrap.
  const uint32_t start = (rng() % 2) ? static_cast<uint32_t>(rng())
                                     : static_cast<uint32_t>(0u - rng() % 100);
  ch->SetIndicesForTesting(start);

  std::deque<uint64_t> queue;
  bool closed = false;
  uint64_t enqueued = 0, dequeued = 0, refused_full = 0, refused_closed = 0,
           discarded = 0, next_id = 1;

  auto check_state = [&](int step) {
    const HandoffStats s = ch->stats();
    ASSERT_EQ(queue.size(), ch->size()) << seed << ":" << step;
    ASSERT_EQ(enqueued, s.enqueued) << seed << ":" << step;
    ASSERT_EQ(dequeued, s.dequeued) << seed << ":" << step;
    ASSERT_EQ(refused_full, s.refused_full) << seed << ":" << step;
    ASSERT_EQ(refused_closed, s.refused_closed) << seed << ":" << step;
    ASSERT_EQ(discarded, s.discarded) << seed << ":" << step;
    ASSERT_EQ(s.enqueued, s.dequeued + s.discarded + s.occupancy);
    ASSERT_EQ(capacity - queue.size(), ch->free_space());
    ASSERT_EQ(ch->closed(), closed);
  };

  for (int step = 0; step < 1500; step++) {
    const uint64_t dice = rng() % 100;
    if (dice < 44) {
      const size_t n = rng() % (capacity + 4);
      std::vector<Item> items;
      for (size_t i = 0; i < n; i++) {
        items.push_back({Fake(next_id + i), MakeContext<C>(next_id + i)});
      }
      const PuntResult r = ch->TryPuntBurst(items);
      size_t expect = 0;
      if (n != 0 && !closed) {
        expect = std::min(n, capacity - queue.size());
      }
      ASSERT_EQ(expect, r.accepted) << seed << ":" << step;
      if (n == 0) {
        ASSERT_FALSE(r.refused.has_value());
      } else if (closed) {
        ASSERT_EQ(HandoffError::kClosed, r.refused);
        refused_closed += n;
      } else if (expect < n) {
        ASSERT_EQ(HandoffError::kFull, r.refused);
        refused_full += n - expect;
      } else {
        ASSERT_FALSE(r.refused.has_value());
      }
      for (size_t i = 0; i < n; i++) {
        ASSERT_EQ(i < expect, items[i].packet == nullptr) << seed << ":" << step;
        if (i >= expect) {
          ASSERT_EQ(Fake(next_id + i), items[i].packet);
          ASSERT_TRUE(IsContextOf(items[i].context, next_id + i));
        }
      }
      for (size_t i = 0; i < expect; i++) {
        queue.push_back(next_id + i);
      }
      enqueued += expect;
      next_id += n;
    } else if (dice < 88) {
      const size_t n = rng() % (capacity + 4);
      std::vector<Item> out(n);
      const size_t got = ch->Dequeue(out);
      ASSERT_EQ(std::min(n, queue.size()), got) << seed << ":" << step;
      for (size_t i = 0; i < got; i++) {
        ASSERT_EQ(Fake(queue.front()), out[i].packet) << seed << ":" << step;
        ASSERT_TRUE(IsContextOf(out[i].context, queue.front()));
        queue.pop_front();
      }
      dequeued += got;
    } else if (dice < 91) {
      ch->Close();
      closed = true;
    } else if (dice < 93) {
      std::vector<uint64_t> drained;
      const size_t removed = ch->Drain([&](Item &item) noexcept {
        drained.push_back(IdOf(item.packet));
      });
      ASSERT_EQ(queue.size(), removed);
      for (uint64_t id : drained) {
        ASSERT_EQ(queue.front(), id);
        queue.pop_front();
      }
      discarded += removed;
    } else {
      PacketHandle packet = Fake(next_id);
      const auto r = ch->TryPunt(packet, MakeContext<C>(next_id));
      if (closed) {
        ASSERT_FALSE(r.has_value());
        ASSERT_EQ(HandoffError::kClosed, r.error());
        refused_closed++;
      } else if (queue.size() == capacity) {
        ASSERT_FALSE(r.has_value());
        ASSERT_EQ(HandoffError::kFull, r.error());
        refused_full++;
      } else {
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(nullptr, packet);
        queue.push_back(next_id);
        enqueued++;
      }
      if (!r.has_value()) {
        ASSERT_EQ(Fake(next_id), packet);
      }
      next_id++;
    }
    check_state(step);
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
  }
}

TYPED_TEST(HandoffTest, RandomOperationsMatchAModelForEveryContextSize) {
  constexpr HandoffTopology T = TestFixture::kTopology;
  for (uint32_t seed = 0; seed < 60; seed++) {
    RunDifferential<NoContext, T>(seed);
    RunDifferential<Ctx8, T>(seed + 1000);
    RunDifferential<Ctx16, T>(seed + 2000);
    RunDifferential<Ctx24, T>(seed + 3000);
    RunDifferential<Ctx56, T>(seed + 4000);
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
  }
}

// -- setup, memory, placement --------------------------------------------------------

TEST(HandoffCreateTest, RefusesBadCapacitiesBeforeAllocatingAnything) {
  TestAllocator::Reset();
  HandoffConfig config;
  config.allocator = &TestAllocator::Get();
  for (const size_t capacity :
       {size_t{0}, HandoffChannel<Ctx8>::kMaxCapacity + 1,
        std::numeric_limits<size_t>::max()}) {
    config.capacity = capacity;
    const auto created = HandoffChannel<Ctx8>::Create(config);
    ASSERT_FALSE(created.has_value()) << capacity;
    EXPECT_EQ(HandoffCreateError::kInvalidCapacity, created.error());
  }
  EXPECT_EQ(0, TestAllocator::allocations);
  EXPECT_NE(std::string(ToString(HandoffCreateError::kOutOfMemory)),
            std::string(ToString(HandoffCreateError::kInvalidCapacity)));
  EXPECT_NE(std::string(ToString(HandoffError::kFull)),
            std::string(ToString(HandoffError::kClosed)));
}

// The allocator is the one seam for setup memory: a refusal is an error and
// nothing is left behind; success allocates once and frees once.
TEST(HandoffCreateTest, AllocatorRefusalIsAnErrorAndAllocationIsOneBlock) {
  TestAllocator::Reset();
  HandoffConfig config;
  config.capacity = 64;
  config.allocator = &TestAllocator::Get();

  TestAllocator::refuse = true;
  const auto refused = HandoffChannel<Ctx8, kMpSc>::Create(config);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(HandoffCreateError::kOutOfMemory, refused.error());
  EXPECT_EQ(1, TestAllocator::allocations);
  EXPECT_EQ(0, TestAllocator::deallocations);

  TestAllocator::refuse = false;
  {
    auto created = HandoffChannel<Ctx8, kMpSc>::Create(config);
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(2, TestAllocator::allocations) << "one block per channel";
    EXPECT_EQ(0, TestAllocator::deallocations);
    EXPECT_EQ(TestAllocator::last_bytes, (*created)->memory_bytes());
    EXPECT_GE((*created)->memory_bytes(),
              size_t{64} * (HandoffChannel<Ctx8, kMpSc>::kItemBytes));
    EXPECT_EQ(64u, (*created)->capacity());
  }
  EXPECT_EQ(2, TestAllocator::allocations);
  EXPECT_EQ(1, TestAllocator::deallocations) << "one block freed, none leaked";
}

// Placement diagnostics: what was asked for, where it landed, and whether they
// differ (a cross-NUMA channel must not look local).
TEST(HandoffCreateTest, PlacementReportsWhereTheMemoryLanded) {
  TestAllocator::Reset();
  HandoffConfig config;
  config.capacity = 8;
  config.allocator = &TestAllocator::Get();

  config.socket = 1;
  TestAllocator::placed = 1;
  auto on_one = HandoffChannel<Ctx8>::Create(config);
  ASSERT_TRUE(on_one.has_value());
  EXPECT_EQ(1, TestAllocator::last_socket);
  EXPECT_EQ(1, (*on_one)->placement().requested_socket);
  EXPECT_EQ(1, (*on_one)->placement().placed_socket);
  EXPECT_FALSE((*on_one)->placement().misplaced());

  TestAllocator::placed = 0;  // asked for node 1, got node 0
  auto elsewhere = HandoffChannel<Ctx8>::Create(config);
  ASSERT_TRUE(elsewhere.has_value());
  EXPECT_TRUE((*elsewhere)->placement().misplaced());

  TestAllocator::placed = -1;  // the allocator cannot tell
  auto unknown = HandoffChannel<Ctx8>::Create(config);
  ASSERT_TRUE(unknown.has_value());
  EXPECT_TRUE((*unknown)->placement().misplaced())
      << "a requested node that cannot be confirmed is reported, not assumed";

  config.socket = SOCKET_ID_ANY;
  auto any = HandoffChannel<Ctx8>::Create(config);
  ASSERT_TRUE(any.has_value());
  EXPECT_EQ(-1, (*any)->placement().requested_socket);
  EXPECT_FALSE((*any)->placement().misplaced());
  EXPECT_EQ((*any)->memory_bytes(), (*any)->placement().bytes);
}

TEST(HandoffCreateTest, DefaultAllocatorPlacesOnTheDpdkHeapAndAdmitsAFallback) {
  HandoffConfig config;
  config.capacity = 256;
  auto local = HandoffChannel<Ctx16, kMpSc>::Create(config);
  ASSERT_TRUE(local.has_value());
  EXPECT_GE((*local)->placement().placed_socket, 0)
      << "DPDK's heap knows which node it gave";
  EXPECT_FALSE((*local)->placement().misplaced());
  EXPECT_GE((*local)->memory_bytes(), 256 * sizeof(PuntItem<Ctx16>));

  // This machine has one node: a request for another one is served from the
  // heap that exists, and the channel says it was not placed as asked.
  config.socket = 7;
  auto remote = HandoffChannel<Ctx16, kMpSc>::Create(config);
  ASSERT_TRUE(remote.has_value());
  EXPECT_NE(7, (*remote)->placement().placed_socket);
  EXPECT_TRUE((*remote)->placement().misplaced());

  // It works.
  std::array<PuntItem<Ctx16>, 4> items;
  for (uint64_t i = 0; i < 4; i++) {
    items[i] = MakeItem<Ctx16>(i);
  }
  ASSERT_EQ(4u, (*remote)->TryPuntBurst(items).accepted);
  std::array<PuntItem<Ctx16>, 4> out;
  ASSERT_EQ(4u, (*remote)->Dequeue(out));
  EXPECT_TRUE(IsContextOf(out[3].context, 3));
  EXPECT_EQ(Fake(3), out[3].packet);
}

TEST(HandoffCreateTest, NothingAllocatesAfterCreate) {
  FakeChannel<Ctx16, kMpMc> ch(64);
  std::array<PuntItem<Ctx16>, 32> items, out;
  const AllocationWindow window;
  for (int round = 0; round < 200; round++) {
    for (uint64_t i = 0; i < items.size(); i++) {
      items[i] = MakeItem<Ctx16>(i);
    }
    (void)ch->TryPuntBurst(items);
    (void)ch->TryPuntBurst(items);
    (void)ch->TryPuntBurst(items);  // full by now: a refusal
    PacketHandle one = Fake(1);
    (void)ch->TryPunt(one, Ctx16{});
    while (ch->Dequeue(out) != 0) {
    }
    (void)ch->stats();
    (void)ch->placement();
    ch->Close();
    (void)ch->TryPuntBurst(items);
    (void)ch->Drain([](PuntItem<Ctx16> &) noexcept {});
  }
  EXPECT_EQ(0u, window.allocations());
}

// -- real packets: the pool counts them ------------------------------------------------

class PacketHandoffTest : public ::testing::Test {
 protected:
  static constexpr size_t kPackets = 512;

  PacketHandle Alloc(uint64_t id) {
    PacketHandle packet = pool_.Alloc(64);
    EXPECT_NE(nullptr, packet);
    if (packet != nullptr) {
      SetTag(packet, id);
    }
    return packet;
  }
  // Packets taken from the pool and not given back: the exact ownership count.
  size_t Outstanding() const { return pool_.Capacity() - pool_.Size(); }

  // Queues 100 packets on a fresh channel of topology T, takes 7 out and frees
  // them, and lets the channel go with 93 still inside.
  template <HandoffTopology T>
  void QueueThenDestroy() {
    ASSERT_EQ(0u, Outstanding());
    {
      HandoffConfig config;
      config.capacity = 128;
      auto created = HandoffChannel<Ctx8, T>::Create(config);
      ASSERT_TRUE(created.has_value());
      std::vector<PuntItem<Ctx8>> items;
      for (uint64_t i = 0; i < 100; i++) {
        items.push_back({Alloc(i), Ctx8{i}});
      }
      ASSERT_EQ(100u, (*created)->TryPuntBurst(items).accepted);
      for (const auto &item : items) {
        EXPECT_EQ(nullptr, item.packet);
      }
      std::array<PuntItem<Ctx8>, 7> taken;
      ASSERT_EQ(7u, (*created)->Dequeue(taken));
      for (auto &item : taken) {
        bess::PacketFree(item.packet);
      }
      EXPECT_EQ(93u, Outstanding()) << "93 packets are in the channel";
    }
    EXPECT_EQ(0u, Outstanding()) << "destroying the channel freed what it held";
  }

  bess::PlainPacketPool pool_{kPackets};
};

TEST_F(PacketHandoffTest, DestroyingAChannelWithQueuedPacketsFreesThemAll) {
  QueueThenDestroy<kSpSc>();
  QueueThenDestroy<kMpSc>();
  QueueThenDestroy<kMpMc>();
}

TEST_F(PacketHandoffTest, DrainDeliversEveryQueuedPacketExactlyOnce) {
  HandoffConfig config;
  config.capacity = 64;
  auto created = HandoffChannel<Ctx8, kMpSc>::Create(config);
  ASSERT_TRUE(created.has_value());
  auto &ch = **created;
  std::vector<PuntItem<Ctx8>> items;
  for (uint64_t i = 0; i < 50; i++) {
    items.push_back({Alloc(i), Ctx8{i}});
  }
  ASSERT_EQ(50u, ch.TryPuntBurst(items).accepted);
  ch.Close();

  std::set<uint64_t> seen;
  const size_t removed = ch.Drain([&](PuntItem<Ctx8> &item) noexcept {
    EXPECT_TRUE(seen.insert(TagOf(item.packet)).second) << "delivered twice";
    EXPECT_EQ(TagOf(item.packet), item.context.id) << "context follows its packet";
    bess::PacketFree(item.packet);
  });
  EXPECT_EQ(50u, removed);
  EXPECT_EQ(50u, seen.size());
  EXPECT_EQ(0u, Outstanding());
  const HandoffStats stats = ch.stats();
  EXPECT_EQ(50u, stats.discarded);
  EXPECT_EQ(stats.enqueued, stats.dequeued + stats.discarded + stats.occupancy);
}

TEST_F(PacketHandoffTest, RefusedPacketsAreStillTheCallersToFreeOrRetry) {
  HandoffConfig config;
  config.capacity = 4;
  auto created = HandoffChannel<Ctx8>::Create(config);
  ASSERT_TRUE(created.has_value());
  auto &ch = **created;
  std::vector<PuntItem<Ctx8>> items;
  for (uint64_t i = 0; i < 10; i++) {
    items.push_back({Alloc(i), Ctx8{i}});
  }
  const PuntResult result = ch.TryPuntBurst(items);
  ASSERT_EQ(4u, result.accepted);
  EXPECT_EQ(10u, Outstanding());
  // The fail/drop policy: free the refused tail and count it.
  for (size_t i = result.accepted; i < items.size(); i++) {
    ASSERT_NE(nullptr, items[i].packet);
    EXPECT_EQ(i, TagOf(items[i].packet)) << "the refused packet is intact";
    bess::PacketFree(items[i].packet);
  }
  EXPECT_EQ(4u, Outstanding());
  std::array<PuntItem<Ctx8>, 8> out;
  ASSERT_EQ(4u, ch.Dequeue(out));
  for (size_t i = 0; i < 4; i++) {
    EXPECT_EQ(i, TagOf(out[i].packet));
    bess::PacketFree(out[i].packet);
  }
  EXPECT_EQ(0u, Outstanding());
}

TEST_F(PacketHandoffTest, ClosedChannelLeavesEveryPacketWithItsProducer) {
  HandoffConfig config;
  config.capacity = 16;
  auto created = HandoffChannel<Ctx8>::Create(config);
  ASSERT_TRUE(created.has_value());
  auto &ch = **created;
  ch.Close();
  std::vector<PuntItem<Ctx8>> items;
  for (uint64_t i = 0; i < 6; i++) {
    items.push_back({Alloc(i), Ctx8{i}});
  }
  const PuntResult result = ch.TryPuntBurst(items);
  EXPECT_EQ(0u, result.accepted);
  EXPECT_EQ(6u, Outstanding());
  for (auto &item : items) {
    ASSERT_NE(nullptr, item.packet);
    bess::PacketFree(item.packet);
  }
  EXPECT_EQ(0u, Outstanding());
}

// Packet in, packet out, twice over: a punt, the service's dequeue, its reply on
// a second channel, the worker's dequeue. Same packets, same contexts, nothing
// duplicated, nothing outstanding at the end.
TEST_F(PacketHandoffTest, PuntAndResumeThroughTwoChannelsKeepsEveryPacket) {
  struct Reply {
    ContinuationHandle continuation;
    uint32_t verdict;
    uint32_t seq;
  };
  HandoffConfig config;
  config.capacity = 32;
  auto punt = HandoffChannel<Ctx16>::Create(config);
  auto resume = HandoffChannel<Reply>::Create(config);
  ASSERT_TRUE(punt.has_value());
  ASSERT_TRUE(resume.has_value());
  auto continuations = ContinuationTable<uint32_t>::Create(32);
  ASSERT_TRUE(continuations.has_value());

  std::vector<PuntItem<Ctx16>> items;
  for (uint32_t i = 0; i < 20; i++) {
    const ContinuationHandle handle = (*continuations)->Issue(1000 + i);
    ASSERT_NE(kNoContinuation, handle);
    items.push_back({Alloc(i), Ctx16{7, i, handle}});
  }
  ASSERT_EQ(20u, (*punt)->TryPuntBurst(items).accepted);

  // The service: take everything, answer each, and hand it back.
  std::array<PuntItem<Ctx16>, 32> got;
  const size_t n = (*punt)->Dequeue(got);
  ASSERT_EQ(20u, n);
  std::vector<PuntItem<Reply>> replies;
  for (size_t i = 0; i < n; i++) {
    replies.push_back({got[i].packet,
                       Reply{got[i].context.continuation, 42u + got[i].context.seq,
                             got[i].context.seq}});
  }
  ASSERT_EQ(n, (*resume)->TryPuntBurst(replies).accepted);

  // The worker resumes them: each continuation resolves to what was issued.
  std::array<PuntItem<Reply>, 32> back;
  ASSERT_EQ(20u, (*resume)->Dequeue(back));
  for (size_t i = 0; i < 20; i++) {
    EXPECT_EQ(i, TagOf(back[i].packet));
    EXPECT_EQ(42u + i, back[i].context.verdict);
    EXPECT_EQ(1000u + i, (*continuations)->Resolve(back[i].context.continuation));
    EXPECT_TRUE((*continuations)->Retire(back[i].context.continuation));
    bess::PacketFree(back[i].packet);
  }
  EXPECT_EQ(0u, Outstanding());
  EXPECT_EQ(0u, (*continuations)->size());
}

TEST_F(PacketHandoffTest, RepeatedCreateAndDestroyWithPacketsInFlightLeaksNothing) {
  TestAllocator::Reset();
  std::mt19937 rng(11);
  for (int round = 0; round < 300; round++) {
    HandoffConfig config;
    config.capacity = 1 + rng() % 64;
    config.allocator = &TestAllocator::Get();
    auto created = HandoffChannel<Ctx8, kMpMc>::Create(config);
    ASSERT_TRUE(created.has_value());
    auto &ch = **created;
    const size_t want = rng() % (config.capacity + 8);
    std::vector<PuntItem<Ctx8>> items;
    for (size_t i = 0; i < want; i++) {
      items.push_back({Alloc(i), Ctx8{i}});
    }
    const PuntResult result = ch.TryPuntBurst(items);
    for (size_t i = result.accepted; i < items.size(); i++) {
      bess::PacketFree(items[i].packet);  // the refused tail is ours
    }
    std::array<PuntItem<Ctx8>, 16> out;
    const size_t taken = ch.Dequeue(std::span<PuntItem<Ctx8>>(out).first(rng() % 16));
    for (size_t i = 0; i < taken; i++) {
      bess::PacketFree(out[i].packet);
    }
    if (rng() % 3 == 0) {
      ch.Close();
    }
    EXPECT_EQ(result.accepted - taken, Outstanding());
    created->reset();  // frees the packets still queued
    ASSERT_EQ(0u, Outstanding()) << "round " << round;
  }
  EXPECT_EQ(300, TestAllocator::allocations);
  EXPECT_EQ(300, TestAllocator::deallocations);
}

}  // namespace
}  // namespace bess::dataplane
