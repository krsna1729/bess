// SPDX-License-Identifier: BSD-3-Clause

// dataplane::PacketStore (D-201): the store alone, without a module - FIFO
// order, caps, the bound, release/drop/clear, expiry, packet ownership (every
// packet the store takes is held, handed back or freed exactly once), and the
// flows resource driven through the transaction engine.

#include "dataplane/packet_store.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "framework/memory_scope.h"
#include "framework/packet_store_codec.h"
#include "dataplane/transaction_engine.h"
#include "packet.h"
#include "packet_pool.h"
#include "runtime/runtime_state.h"
#include "testing/allocation_faults.h"

namespace bess::dataplane {
namespace {

// A limits table for flow ids 1..16 holding `entries`, as a flows resource
// publishes them.
std::unique_ptr<PacketStoreLimitsTable> Limits(
    std::initializer_list<std::pair<uint32_t, PacketStoreLimits>> entries =
        {}) {
  auto table = std::make_unique<PacketStoreLimitsTable>(16);
  for (const auto &[id, limits] : entries) {
    (void)table->Publish(PacketStoreFlowId(id), table->Make(limits));
  }
  return table;
}

// A table with no entries: every flow takes the store's numbers. Never
// freed, so the DPDK heap outlives it.
const PacketStoreLimitsTable &NoLimits() {
  static const PacketStoreLimitsTable *const table = Limits().release();
  return *table;
}

constexpr uint64_t kSecond = 1'000'000'000ULL;

// Fixed storage: neither the observer nor its context allocates. Inspect the
// packet in the notification to check that the observer runs before free.
struct DiscardLog {
  struct Event {
    uint64_t tag;
    PacketStoreDiscardReason reason;
    uint16_t refcount;
  };
  std::array<Event, 256> events{};
  size_t size = 0;
  bool overflow = false;

  static void Observe(void *opaque, PacketHandle handle,
                      PacketStoreDiscardReason reason) noexcept {
    auto &log = *static_cast<DiscardLog *>(opaque);
    if (log.size == log.events.size()) {
      log.overflow = true;
      return;
    }
    uint64_t tag;
    std::memcpy(&tag, rte_pktmbuf_mtod(handle, void *), sizeof(tag));
    log.events[log.size++] = {tag, reason, rte_mbuf_refcnt_read(handle)};
  }
};

class PacketStoreTest : public ::testing::Test {
 protected:
  static constexpr size_t kPackets = 2048;

  void TearDown() override { EXPECT_EQ(0u, Outstanding()) << "a packet leaked"; }

  PacketHandle Alloc(uint64_t tag) {
    PacketHandle packet = pool_.Alloc(64);
    EXPECT_NE(nullptr, packet);
    std::memcpy(rte_pktmbuf_mtod(packet, void *), &tag, sizeof(tag));
    return packet;
  }
  static uint64_t TagOf(PacketHandle packet) {
    uint64_t tag;
    std::memcpy(&tag, rte_pktmbuf_mtod(packet, void *), sizeof(tag));
    return tag;
  }
  // Packets taken from the pool and not given back.
  size_t Outstanding() const { return pool_.Capacity() - pool_.Size(); }

  static std::unique_ptr<PacketStore> Make(uint32_t max, uint32_t per_flow,
                                           uint64_t timeout_ns = 5 * kSecond) {
    auto made = PacketStore::Create({.max_packets = max,
                                     .max_packets_per_flow = per_flow,
                                     .timeout_ns = timeout_ns});
    EXPECT_TRUE(made.has_value());
    return made.has_value() ? std::move(*made) : nullptr;
  }

  // Stores one packet per flow id, tagged with `first_tag + i`; returns the
  // tags `evict` received (and frees those packets).
  std::vector<uint64_t> Store(
      PacketStore &store, std::vector<uint32_t> ids, uint64_t now_ns,
      uint64_t first_tag, const PacketStoreLimitsTable &limits = NoLimits()) {
    std::vector<PacketHandle> handles;
    for (size_t i = 0; i < ids.size(); i++) {
      handles.push_back(Alloc(first_tag + i));
    }
    std::vector<uint64_t> evicted;
    store.Store(handles.data(), ids.data(), ids.size(), limits, now_ns,
                [&](PacketHandle h) {
                  evicted.push_back(TagOf(h));
                  PacketFree(h);
                });
    return evicted;
  }

  std::vector<uint64_t> Release(PacketStore &store, uint32_t id) {
    std::vector<uint64_t> tags;
    const size_t n = store.Release(id, [&](PacketHandle h) {
      tags.push_back(TagOf(h));
      PacketFree(h);
    });
    EXPECT_EQ(n, tags.size());
    return tags;
  }

  PlainPacketPool pool_{kPackets};
};

// A bad config is refused before anything is allocated; what Create
// allocates is Bytes(config), all of it DPDK memory (D-206: workers read
// and write it per packet), and bytes() reports the same.
TEST_F(PacketStoreTest, CreateRefusesBadConfigsAndCostsWhatBytesSays) {
  EXPECT_FALSE(PacketStore::Create({.max_packets = 0}).has_value());
  EXPECT_FALSE(PacketStore::Create({.max_packets_per_flow = 0}).has_value());
  EXPECT_FALSE(
      PacketStore::Create({.max_packets = (uint32_t{1} << 30) + 1}).has_value());

  EXPECT_EQ(16u, PacketStore::FlowCapacity(1));
  EXPECT_EQ(256u, PacketStore::FlowCapacity(100));
  EXPECT_EQ(256u, PacketStore::FlowCapacity(128));

  const PacketStoreConfig config{.max_packets = 100};
  const size_t dpdk_before = utils::GetMemoryStatus().dpdk_heap_alloc_bytes;
  auto store = PacketStore::Create(config);
  ASSERT_TRUE(store.has_value() && *store != nullptr);
  EXPECT_GE(utils::GetMemoryStatus().dpdk_heap_alloc_bytes - dpdk_before,
            PacketStore::Bytes(config))
      << "the store did not come from DPDK's heap";
  EXPECT_EQ(PacketStore::Bytes(config), (*store)->bytes());
  EXPECT_EQ(0u, framework::MemoryCost<PacketStore>::GlibcBytes(config));
  EXPECT_GT(PacketStore::Bytes({.max_packets = 200}), PacketStore::Bytes(config));
  EXPECT_EQ(0u, (*store)->packets());
  EXPECT_EQ(0u, (*store)->flows());
}

// A release hands a flow's packets back oldest first and forgets the flow;
// other flows are untouched; a flow that holds nothing releases nothing.
TEST_F(PacketStoreTest, ReleaseIsFifoPerFlow) {
  auto store = Make(100, 10);
  EXPECT_TRUE(Store(*store, {7, 9, 7, 7, 9}, 0, 100).empty());
  EXPECT_EQ(5u, store->packets());
  EXPECT_EQ(2u, store->flows());

  EXPECT_EQ((std::vector<uint64_t>{100, 102, 103}), Release(*store, 7));
  EXPECT_EQ(2u, store->packets());
  EXPECT_EQ(1u, store->flows());
  EXPECT_TRUE(Release(*store, 7).empty());
  EXPECT_TRUE(Release(*store, 8).empty());
  EXPECT_EQ((std::vector<uint64_t>{101, 104}), Release(*store, 9));
  EXPECT_EQ(0u, store->packets());
  EXPECT_EQ(0u, store->flows());
}

// A flow at its cap gives its oldest packet to `evict`; an entry's own cap
// overrides the default, an entry's 0 is the default, and an arrival brings a
// flow down to a cap that was lowered under it.
TEST_F(PacketStoreTest, APerFlowCapEvictsTheOldest) {
  auto store = Make(100, 2);
  EXPECT_EQ((std::vector<uint64_t>{0}), Store(*store, {1, 1, 1}, 0, 0));
  EXPECT_EQ(
      (std::vector<uint64_t>{1, 2}),
      Store(*store, {1}, 0, 3, *Limits({{1, {.max_packets_per_flow = 1}}})));
  EXPECT_EQ(1u, store->packets());
  // A present 0 is the default cap (2), not "hold nothing".
  EXPECT_TRUE(
      Store(*store, {1}, 0, 4, *Limits({{1, {.max_packets_per_flow = 0}}}))
          .empty());
  EXPECT_EQ((std::vector<uint64_t>{3, 4}), Release(*store, 1));

  const auto table = Limits({{5, {.max_packets_per_flow = 4}}});
  const PacketStoreLimitsTable &limits = *table;
  EXPECT_TRUE(Store(*store, {5, 5, 5, 5}, 0, 10, limits).empty());
  EXPECT_EQ((std::vector<uint64_t>{10}), Store(*store, {5}, 0, 14, limits));
  EXPECT_EQ(4u, store->packets());
  store->Clear();
}

// A full store evicts the arriving packet, and a flow that leaves holding
// nothing is not counted as held.
TEST_F(PacketStoreTest, AFullStoreEvictsTheArrival) {
  auto store = Make(3, 10);
  EXPECT_TRUE(Store(*store, {1, 2, 3}, 0, 0).empty());
  EXPECT_EQ((std::vector<uint64_t>{3, 4}), Store(*store, {4, 1}, 0, 3));
  EXPECT_EQ(3u, store->packets());
  EXPECT_EQ(3u, store->flows()) << "flow 4 holds nothing, so it is not held";

  // Flow 1 at a cap of 1 while full: its packet goes to make room, then the
  // arrival fits.
  EXPECT_EQ(
      (std::vector<uint64_t>{0}),
      Store(*store, {1}, 0, 5, *Limits({{1, {.max_packets_per_flow = 1}}})));
  EXPECT_EQ((std::vector<uint64_t>{5}), Release(*store, 1));
  EXPECT_EQ(2u, store->flows());
  store->Clear();
}

// Drop frees a flow's packets and counts them; Clear frees everything; the
// destructor frees what is left (TearDown checks the pool).
TEST_F(PacketStoreTest, DropClearAndDestroyFree) {
  auto store = Make(200, 100);
  std::vector<uint32_t> ids(70, 4);
  ids.push_back(6);
  EXPECT_TRUE(Store(*store, ids, 0, 0).empty());
  EXPECT_EQ(71u, Outstanding());
  EXPECT_EQ(70u, store->Drop(4)) << "more than one free burst";
  EXPECT_EQ(0u, store->Drop(4));
  EXPECT_EQ(1u, Outstanding());
  EXPECT_EQ(1u, store->flows());
  EXPECT_EQ(1u, store->Clear());
  EXPECT_EQ(0u, store->packets());
  EXPECT_EQ(0u, store->flows());
  EXPECT_EQ(0u, Outstanding());

  EXPECT_TRUE(Store(*store, {1, 2, 3}, 0, 0).empty());
  store.reset();
  EXPECT_EQ(0u, Outstanding()) << "the destructor freed what it held";
}

// An internal disposal notifies exactly once, before free, with the right
// reason and in FIFO order within a flow. Destruction also notifies Clear;
// the context is declared first so it outlives the store.
TEST_F(PacketStoreTest, InternalDiscardsNotifyExactlyOnceBeforeFree) {
  DiscardLog log;
  auto store = Make(80, 80, 100);
  store->SetDiscardObserver(&log, DiscardLog::Observe);

  EXPECT_TRUE(Store(*store, std::vector<uint32_t>(40, 1), 0, 0).empty());
  EXPECT_EQ(40u, store->Drop(1));  // crosses the bulk-free boundary
  EXPECT_EQ(0u, store->Drop(1));
  EXPECT_EQ(40u, log.size);

  EXPECT_TRUE(Store(*store, {2, 2, 2}, 0, 40).empty());
  store->ExpireAll(NoLimits(), 101);
  store->ExpireAll(NoLimits(), 101);
  EXPECT_EQ(43u, log.size);

  EXPECT_TRUE(Store(*store, {3, 3, 3}, 0, 43).empty());
  store->ExpireBounded(NoLimits(), 101);
  store->ExpireBounded(NoLimits(), 101);
  EXPECT_EQ(46u, log.size);

  EXPECT_TRUE(Store(*store, {4, 4, 4}, 0, 46).empty());
  EXPECT_EQ(3u, store->Clear());
  EXPECT_EQ(0u, store->Clear());
  EXPECT_EQ(49u, log.size);
  EXPECT_EQ(0u, store->packets());
  EXPECT_EQ(0u, store->flows());
  EXPECT_EQ(0u, Outstanding());

  EXPECT_TRUE(Store(*store, {5, 5}, 0, 49).empty());
  store.reset();  // notifies while log still exists
  ASSERT_EQ(51u, log.size);
  EXPECT_FALSE(log.overflow);
  EXPECT_EQ(0u, Outstanding());
  for (size_t i = 0; i < log.size; i++) {
    const auto expected = i < 40 ? PacketStoreDiscardReason::kDropped
                          : i < 46 ? PacketStoreDiscardReason::kExpired
                                   : PacketStoreDiscardReason::kCleared;
    EXPECT_EQ(i, log.events[i].tag);
    EXPECT_EQ(expected, log.events[i].reason);
    EXPECT_EQ(1u, log.events[i].refcount) << "packet was live before free";
  }
}

// Eviction and release transfer ownership to their sinks, not the observer.
// Pressure-triggered expiry within Store still notifies, and the observer can
// be removed after Clear before its context is destroyed.
TEST_F(PacketStoreTest, TransfersDoNotNotifyButPressureExpiryDoes) {
  DiscardLog log;
  auto store = Make(2, 1, 100);
  store->SetDiscardObserver(&log, DiscardLog::Observe);
  EXPECT_EQ((std::vector<uint64_t>{0}), Store(*store, {1, 1}, 0, 0));
  EXPECT_TRUE(Store(*store, {2}, 0, 2).empty());
  EXPECT_EQ((std::vector<uint64_t>{3}), Store(*store, {3}, 0, 3));
  EXPECT_EQ((std::vector<uint64_t>{1}), Release(*store, 1));
  EXPECT_EQ(0u, log.size);

  EXPECT_TRUE(Store(*store, {4}, 0, 4).empty());
  EXPECT_TRUE(Store(*store, {5}, 101, 5).empty());  // full: expires tags 2 and 4
  ASSERT_EQ(2u, log.size);
  EXPECT_EQ(PacketStoreDiscardReason::kExpired, log.events[0].reason);
  EXPECT_EQ(PacketStoreDiscardReason::kExpired, log.events[1].reason);
  EXPECT_TRUE((log.events[0].tag == 2 && log.events[1].tag == 4) ||
              (log.events[0].tag == 4 && log.events[1].tag == 2));
  EXPECT_EQ((std::vector<uint64_t>{5}), Release(*store, 5));
  EXPECT_EQ(2u, log.size);

  EXPECT_TRUE(Store(*store, {6}, 101, 6).empty());
  EXPECT_EQ(1u, store->Clear());
  EXPECT_EQ(3u, log.size);
  store->SetDiscardObserver(nullptr, nullptr);
  EXPECT_TRUE(Store(*store, {7}, 101, 7).empty());
  store.reset();  // disabled: no context access
  EXPECT_EQ(3u, log.size);
}

TEST_F(PacketStoreTest, ObservedPacketOperationsDoNotAllocate) {
  DiscardLog log;
  auto store = Make(4, 2, 10);
  store->SetDiscardObserver(&log, DiscardLog::Observe);
  std::array<PacketHandle, 8> handles;
  for (size_t i = 0; i < handles.size(); i++) {
    handles[i] = Alloc(i);
  }
  const uint32_t ids[]{1, 1, 1, 2, 2, 3, 4, 5};
  const auto table = Limits();
  const PacketStoreLimitsTable &limits = *table;
  size_t evicted = 0;
  size_t released = 0;
  size_t dropped = 0;
  size_t cleared = 0;
  size_t allocations = 0;
  {
    fault_injection::AllocationFaults window;
    store->Store(handles.data(), ids, 3, limits, 0, [&](PacketHandle h) {
      evicted++;
      PacketFree(h);
    });
    released = store->Release(1, [](PacketHandle h) { PacketFree(h); });
    auto free_eviction = [](PacketHandle h) { PacketFree(h); };
    store->Store(handles.data() + 3, ids + 3, 2, limits, 0, free_eviction);
    store->ExpireBounded(limits, 11);
    store->Store(handles.data() + 5, ids + 5, 1, limits, 0, free_eviction);
    dropped = store->Drop(3);
    store->Store(handles.data() + 6, ids + 6, 1, limits, 0, free_eviction);
    cleared = store->Clear();
    store->Store(handles.data() + 7, ids + 7, 1, limits, 0, free_eviction);
    store.reset();
    allocations = window.allocations();
  }
  EXPECT_EQ(0u, allocations);
  EXPECT_EQ(1u, evicted);
  EXPECT_EQ(2u, released);
  EXPECT_EQ(1u, dropped);
  EXPECT_EQ(1u, cleared);
  ASSERT_EQ(5u, log.size);
  EXPECT_FALSE(log.overflow);
  for (size_t i = 0; i < log.size; i++) {
    EXPECT_EQ(i + 3, log.events[i].tag);
    const auto expected = i < 2 ? PacketStoreDiscardReason::kExpired
                          : i == 2 ? PacketStoreDiscardReason::kDropped
                                   : PacketStoreDiscardReason::kCleared;
    EXPECT_EQ(expected, log.events[i].reason);
  }
  EXPECT_EQ(0u, Outstanding());
}

// Packets older than their flow's timeout are freed; an entry's timeout
// overrides the default and 0 means never. A full store expires before it
// evicts an arrival.
TEST_F(PacketStoreTest, ExpiryFollowsEachFlowsTimeout) {
  auto store = Make(4, 10, 2 * kSecond);
  const auto table =
      Limits({{2, {.timeout_ns = 0}}, {3, {.timeout_ns = 10 * kSecond}}});
  const PacketStoreLimitsTable &limits = *table;
  EXPECT_TRUE(Store(*store, {1, 2, 3}, 0, 0, limits).empty());
  EXPECT_TRUE(Store(*store, {1}, 3 * kSecond, 3, limits).empty());

  store->ExpireAll(limits, 2 * kSecond);  // not older than: kept
  EXPECT_EQ(4u, store->packets());
  // Full: the store expires flow 1's first packet and takes the arrival.
  EXPECT_TRUE(Store(*store, {5}, 4 * kSecond, 4, limits).empty());
  EXPECT_EQ(4u, store->packets());

  store->ExpireAll(limits, 11 * kSecond);
  EXPECT_EQ(1u, store->packets()) << "only the flow that never expires";
  EXPECT_EQ((std::vector<uint64_t>{1}), Release(*store, 2));
}

// Every held flow is visited within one rotation, even in a sparse table.
TEST_F(PacketStoreTest, BoundedExpiryCoversHeldFlowsInOneRotation) {
  constexpr uint32_t kFlows = 1024;
  auto store = Make(kFlows, 1, kSecond);
  std::vector<uint32_t> ids;
  for (uint32_t i = 0; i < kFlows; i++) {
    ids.push_back(i * 7919 + 1);
  }
  EXPECT_TRUE(Store(*store, ids, 0, 0).empty());

  store->ExpireBounded(NoLimits(), 2 * kSecond);
  EXPECT_GT(store->packets(), 0u) << "one call is bounded";
  EXPECT_LT(store->packets(), size_t{kFlows});
  const size_t rotation =
      (kFlows + PacketStore::kExpireFlowsPerRun - 1) /
      PacketStore::kExpireFlowsPerRun;
  for (size_t i = 1; i < rotation; i++) {
    store->ExpireBounded(NoLimits(), 2 * kSecond);
  }
  EXPECT_EQ(0u, store->packets());
  EXPECT_EQ(0u, store->flows());
}

// Random stores, releases, drops and expiries against a model: per-flow FIFO
// contents and every count agree after each step (the flow table's erase
// shifts probe chains, which this exercises with colliding ids).
TEST_F(PacketStoreTest, RandomOperationsMatchAModel) {
  constexpr uint32_t kMax = 256;
  constexpr uint32_t kCap = 8;
  auto store = Make(kMax, kCap, 100);
  std::map<uint32_t, std::deque<std::pair<uint64_t, uint64_t>>> model;  // tag, ts
  size_t model_total = 0;
  std::mt19937 rng(195);
  uint64_t tag = 0;
  uint64_t now = 0;

  for (int step = 0; step < 4000; step++) {
    now += rng() % 5;
    const uint32_t id = rng() % 64;
    switch (rng() % 8) {
      case 0: {
        const std::vector<uint64_t> got = Release(*store, id);
        std::vector<uint64_t> want;
        for (const auto &[t, ts] : model[id]) {
          want.push_back(t);
        }
        model_total -= model[id].size();
        model.erase(id);
        ASSERT_EQ(want, got) << "step " << step;
        break;
      }
      case 1:
        ASSERT_EQ(model[id].size(), store->Drop(id)) << "step " << step;
        model_total -= model[id].size();
        model.erase(id);
        break;
      case 2:
        if (step % 2 == 0) {
          store->ExpireAll(NoLimits(), now);
        } else {
          // At most 64 held flows, so one bounded sweep visits them all.
          store->ExpireBounded(NoLimits(), now);
        }
        for (auto it = model.begin(); it != model.end();) {
          while (!it->second.empty() && now - it->second.front().second > 100) {
            it->second.pop_front();
            model_total--;
          }
          it = it->second.empty() ? model.erase(it) : std::next(it);
        }
        break;
      default: {
        if (model_total >= kMax) {
          // The model of "full" includes the expiry Store runs first.
          for (auto it = model.begin(); it != model.end();) {
            while (!it->second.empty() &&
                   now - it->second.front().second > 100) {
              it->second.pop_front();
              model_total--;
            }
            it = it->second.empty() ? model.erase(it) : std::next(it);
          }
        }
        std::vector<uint64_t> want_evicted;
        auto &queue = model[id];
        while (queue.size() >= kCap) {
          want_evicted.push_back(queue.front().first);
          queue.pop_front();
          model_total--;
        }
        if (model_total >= kMax) {
          want_evicted.push_back(tag);
        } else {
          queue.emplace_back(tag, now);
          model_total++;
        }
        if (queue.empty()) {
          model.erase(id);
        }
        ASSERT_EQ(want_evicted, Store(*store, {id}, now, tag)) << "step " << step;
        tag++;
        break;
      }
    }
    for (auto it = model.begin(); it != model.end();) {
      it = it->second.empty() ? model.erase(it) : std::next(it);
    }
    ASSERT_EQ(model_total, store->packets()) << "step " << step;
    ASSERT_EQ(model.size(), store->flows()) << "step " << step;
  }
  store->Clear();
}

// The flows resource through the transaction engine: each operation
// publishes or empties its own entry, an entry keeps exactly the fields it
// set, erasing an absent id is a no-op, and a wrong value type or an id past
// the table is refused without touching the table.
// Records the admission the table's slab asks (D-198); refuses growth while
// `refuse` is set.
struct CountingGrowth final : utils::MemoryGrowth {
  size_t grown = 0;
  size_t shrunk = 0;
  size_t calls = 0;  // Grow calls, refused ones too
  bool refuse = false;
  void Grow(size_t bytes, size_t glibc_bytes = 0) override {
    calls++;
    EXPECT_EQ(0u, glibc_bytes) << "the table is all DPDK memory";
    if (refuse) {
      throw utils::MemoryRefused(utils::MemoryRefused::Reason::kNormalLimit,
                                 bytes, "refused");
    }
    grown += bytes;
  }
  void Shrink(size_t bytes, size_t glibc_bytes = 0) noexcept override {
    EXPECT_EQ(0u, glibc_bytes);
    shrunk += bytes;
  }
  size_t net() const { return grown - shrunk; }
};

class PacketStoreFlowsResourceTest : public ::testing::Test {
 protected:
  static constexpr size_t kCapacity = size_t{1} << 17;

  void SetUp() override {
    table_.set_memory_growth(&growth_);
    ASSERT_TRUE(engine_.Register(&resource_));
  }
  void TearDown() override {
    const std::string name = "ps/flows";
    ASSERT_TRUE(engine_.ReleaseForTeardown(std::span<const std::string>(&name, 1)));
    domain_.Drain();
    EXPECT_EQ(table_.bytes(),
              PacketStoreLimitsTable::Bytes(kCapacity) + growth_.net())
        << "every slab chunk was admitted";
  }

  static Op Upsert(uint32_t id, PacketStoreLimits limits) {
    return Op::Upsert("ps/flows", EncodeKey(id), std::any(limits));
  }
  static Op Erase(uint32_t id) { return Op::Erase("ps/flows", EncodeKey(id)); }

  // The live entries, by id.
  std::map<uint32_t, PacketStoreLimits> Entries() const {
    std::map<uint32_t, PacketStoreLimits> entries;
    for (size_t id = 1; id <= kCapacity; id++) {
      const PacketStoreFlowId flow(static_cast<uint32_t>(id));
      if (const PacketStoreLimits *limits = table_.Current(flow)) {
        entries.emplace(static_cast<uint32_t>(id), *limits);
      }
    }
    return entries;
  }

  // Declared before the table, which admits through it.
  CountingGrowth growth_;
  rcu::RcuDomain &domain_ = bess::runtime::runtime().rcu();
  PacketStoreLimitsTable table_{kCapacity};
  PacketStoreFlowsResource resource_{"ps/flows", table_};
  TransactionEngine engine_{domain_};
};

TEST_F(PacketStoreFlowsResourceTest, ATransactionPublishesItsEntries) {
  auto r = engine_.Apply(std::vector<Op>{
      Upsert(1, {.timeout_ns = 7, .max_packets_per_flow = 4}),
      Upsert(2, {.timeout_ns = 0}), Erase(9)});
  ASSERT_EQ(TransactionEngine::Outcome::kApplied, r.outcome);
  EXPECT_EQ((std::map<uint32_t, PacketStoreLimits>{
                {1, {.timeout_ns = 7, .max_packets_per_flow = 4}},
                {2, {.timeout_ns = 0}}}),
            Entries());
  EXPECT_EQ(2u, resource_.LiveCount());
  EXPECT_TRUE(resource_.Contains(EncodeKey(uint32_t{2})));
  EXPECT_FALSE(table_.Lookup(PacketStoreFlowId(2))->max_packets_per_flow)
      << "an absent field stays absent";

  r = engine_.Apply(std::vector<Op>{Erase(1)});
  ASSERT_EQ(TransactionEngine::Outcome::kApplied, r.outcome);
  EXPECT_EQ((std::map<uint32_t, PacketStoreLimits>{{2, {.timeout_ns = 0}}}),
            Entries());

  const PacketStoreLimits *kept = table_.Lookup(PacketStoreFlowId(2));
  r = engine_.Apply(std::vector<Op>{
      Upsert(3, {}), Op::Upsert("ps/flows", EncodeKey(uint32_t{4}), std::any(5))});
  EXPECT_NE(TransactionEngine::Outcome::kApplied, r.outcome);
  EXPECT_FALSE(resource_.Contains(EncodeKey(uint32_t{3})))
      << "a refused transaction publishes nothing";
  EXPECT_EQ(kept, table_.Lookup(PacketStoreFlowId(2)));

  // Past the table: an upsert is refused, an erase is a no-op.
  const auto past = static_cast<uint32_t>(kCapacity + 1);
  EXPECT_NE(TransactionEngine::Outcome::kApplied,
            engine_.Apply(std::vector<Op>{Upsert(past, {})}).outcome);
  EXPECT_EQ(TransactionEngine::Outcome::kApplied,
            engine_.Apply(std::vector<Op>{Erase(past)}).outcome);
  EXPECT_EQ(1u, resource_.LiveCount());
}

// A refused slab chunk refuses the transaction with the table untouched;
// one admitted chunk then holds the next entries.
TEST_F(PacketStoreFlowsResourceTest, RefusedGrowthRefusesTheTransactionCleanly) {
  growth_.refuse = true;
  EXPECT_THROW((void)resource_.Reserve(Upsert(1, {})), utils::MemoryRefused);
  EXPECT_THROW((void)engine_.Apply(std::vector<Op>{Upsert(1, {})}),
               utils::MemoryRefused);
  growth_.refuse = false;
  EXPECT_EQ(0u, growth_.net());
  EXPECT_EQ(0u, resource_.LiveCount());

  ASSERT_EQ(
      TransactionEngine::Outcome::kApplied,
      engine_.Apply(std::vector<Op>{Upsert(1, {}), Upsert(2, {})}).outcome);
  EXPECT_EQ(utils::ObjectSlab<PacketStoreLimits>::chunk_bytes(), growth_.net());
  EXPECT_EQ(2u, resource_.LiveCount());
}

// BP-6 (DP-2): a transaction costs what it changes, not a copy of the
// table. 100K entries, then 100 one-op transactions (erase one id, add it
// back): at most one admission each, where a clone of the whole table per
// transaction made ~10^7 Grow calls; and what is held returns to where it
// was once the replaced objects are reclaimed.
TEST_F(PacketStoreFlowsResourceTest, AOneOpTransactionAdmitsOneEntryAtMost) {
  constexpr uint32_t kEntries = 100'000;
  for (uint32_t first = 1; first <= kEntries; first += 1000) {
    std::vector<Op> ops;
    for (uint32_t id = first; id < first + 1000; id++) {
      ops.push_back(Upsert(id, {.timeout_ns = id}));
    }
    ASSERT_EQ(TransactionEngine::Outcome::kApplied,
              engine_.Apply(std::move(ops)).outcome);
    domain_.Drain();
  }
  const size_t calls = growth_.calls;
  const size_t held = growth_.net();
  for (uint32_t i = 0; i < 100; i++) {
    const uint32_t id = 1 + i / 2 * 997;
    ASSERT_EQ(
        TransactionEngine::Outcome::kApplied,
        engine_.Apply(std::vector<Op>{i % 2 == 0 ? Erase(id) : Upsert(id, {})})
            .outcome);
  }
  domain_.Drain();
  RecordProperty("grow_calls", static_cast<int>(growth_.calls - calls));
  EXPECT_LE(growth_.calls - calls, 100u)
      << "Grow calls over 100 one-op transactions on " << kEntries;
  EXPECT_EQ(held, growth_.net()) << "held bytes after reclaim";
  EXPECT_EQ(kEntries, resource_.LiveCount());
}

// The BESS codec: flow id 0 is refused, a value keeps field presence both
// ways, and readback reports each entry with exactly the fields it set.
TEST(PacketStoreFlowsCodecTest, KeepsFieldPresenceAndRefusesTheKeylessId) {
  const auto table =
      Limits({{3, {.timeout_ns = 0}}, {4, {.max_packets_per_flow = 2}}});
  auto codec = MakePacketStoreFlowsCodec(*table);
  const std::string key_url = "type.googleapis.com/bess.pb.PacketStoreFlowKey";
  const std::string value_url =
      "type.googleapis.com/bess.pb.PacketStoreFlowValue";

  bess::pb::PacketStoreFlowKey key;
  key.set_id(0);
  EXPECT_FALSE(codec->Key(key_url, key.SerializeAsString()).has_value());
  key.set_id(9);
  auto encoded = codec->Key(key_url, key.SerializeAsString());
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(EncodeKey(uint32_t{9}), *encoded);

  bess::pb::PacketStoreFlowValue value;
  value.set_timeout_ns(0);
  auto decoded = codec->Value(value_url, value.SerializeAsString());
  ASSERT_TRUE(decoded.has_value());
  const PacketStoreLimits *limits = std::any_cast<PacketStoreLimits>(&*decoded);
  ASSERT_NE(nullptr, limits);
  EXPECT_EQ((PacketStoreLimits{.timeout_ns = 0}), *limits);

  std::vector<std::string> seen;
  codec->Entries([&](const google::protobuf::Message &k,
                     const google::protobuf::Message &v) {
    const auto &fk = static_cast<const bess::pb::PacketStoreFlowKey &>(k);
    const auto &fv = static_cast<const bess::pb::PacketStoreFlowValue &>(v);
    seen.push_back(std::to_string(fk.id()) + ":" +
                   (fv.has_timeout_ns() ? "t" : "") +
                   (fv.has_max_packets_per_flow() ? "c" : ""));
  });
  EXPECT_EQ((std::vector<std::string>{"3:t", "4:c"}), seen);
}

}  // namespace
}  // namespace bess::dataplane
