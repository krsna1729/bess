// SPDX-License-Identifier: BSD-3-Clause

// Handoff channels between real threads (roadmap M11, Decision D-054).
//
// Every test follows the rules that keep a concurrent test honest on any
// machine: the volume of work is fixed by the loop (a producer makes exactly N
// packets) and never by how much fits in a time window; a wall-clock deadline
// only turns a hang into a failure; threads yield when they cannot make
// progress, so a one-CPU run interleaves them; and every thread is stopped and
// joined on every exit path, a failed ASSERT included.

#include "dataplane/handoff.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "dataplane/continuation.h"
#include "packet.h"
#include "packet_pool.h"
#include "utils/sanitizers.h"

#if BESS_THREAD_SANITIZER
// The packet pool recycles mbufs through DPDK's uninstrumented mempool code, so
// ThreadSanitizer cannot see that a free happens-before the next allocation of
// the same buffer. These tell it.
extern "C" void __tsan_acquire(void *addr);
extern "C" void __tsan_release(void *addr);
#define TSAN_ACQUIRE(p) __tsan_acquire(p)
#define TSAN_RELEASE(p) __tsan_release(p)
#else
#define TSAN_ACQUIRE(p) ((void)(p))
#define TSAN_RELEASE(p) ((void)(p))
#endif

namespace bess::dataplane {
namespace {

constexpr HandoffTopology kSpSc = HandoffTopology::kSpSc;
constexpr HandoffTopology kMpSc = HandoffTopology::kMpSc;
constexpr HandoffTopology kMpMc = HandoffTopology::kMpMc;

// What a punted packet carries: who sent it and which one it was. 16 bytes, so
// the ring element is 24 bytes (not a power of two).
struct Seq {
  uint32_t producer;
  uint32_t unused;
  uint64_t seq;
};

uint64_t Pack(uint32_t producer, uint64_t seq) {
  return (uint64_t{producer} << 48) | seq;
}

// Threads of a test: stopped and joined whatever way the test body exits.
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
  // Waits for the threads without telling them to stop (they end on their own).
  void Wait() {
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

// A hang becomes a failure: loops give up past this, and the test reports it.
class Deadline {
 public:
  explicit Deadline(std::chrono::seconds limit = std::chrono::seconds(25))
      : end_(std::chrono::steady_clock::now() + limit) {}
  bool Expired() const {
    if (expired_.load(std::memory_order_relaxed)) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= end_) {
      expired_.store(true, std::memory_order_relaxed);
      return true;
    }
    return false;
  }
  bool expired() const { return expired_.load(std::memory_order_relaxed); }

 private:
  std::chrono::steady_clock::time_point end_;
  mutable std::atomic<bool> expired_{false};
};

// Real packets from a pool; the pool's count is the proof that none leaked.
class Env {
 public:
  explicit Env(size_t packets) : pool_(packets) {}

  // nullptr when the pool is empty.
  PacketHandle Alloc(uint64_t tag) {
#if BESS_THREAD_SANITIZER
    // rte_pktmbuf_alloc split in two so that the acquire sits between the pool's
    // get (uninstrumented library code) and the reset (instrumented inline code).
    void *object = nullptr;
    if (rte_mempool_get(pool_.pool(), &object) != 0) {
      return nullptr;
    }
    PacketHandle packet = static_cast<PacketHandle>(object);
    TSAN_ACQUIRE(packet);
    rte_mbuf_refcnt_set(packet, 1);
    rte_pktmbuf_reset(packet);
    packet->pkt_len = 64;
    packet->data_len = 64;
#else
    PacketHandle packet = pool_.Alloc(64);
    if (packet == nullptr) {
      return nullptr;
    }
#endif
    std::memcpy(rte_pktmbuf_mtod(packet, void *), &tag, sizeof(tag));
    return packet;
  }
  static uint64_t TagOf(PacketHandle packet) {
    uint64_t tag;
    std::memcpy(&tag, rte_pktmbuf_mtod(packet, void *), sizeof(tag));
    return tag;
  }
  void Free(PacketHandle packet) {
#if BESS_THREAD_SANITIZER
    // rte_pktmbuf_free split the same way: the release follows the instrumented
    // prefree (refcount, next, nb_segs) and precedes the pool's put.
    rte_mempool *pool = packet->pool;
    if (rte_pktmbuf_prefree_seg(packet) != nullptr) {
      TSAN_RELEASE(packet);
      rte_mempool_put(pool, packet);
    }
#else
    bess::PacketFree(packet);
#endif
  }
  size_t Outstanding() const { return pool_.Capacity() - pool_.Size(); }

 private:
  bess::PlainPacketPool pool_;
};

// Every packet of every producer must end in exactly one of the places it can
// go. `Mark` returns false for a packet accounted twice (or never produced).
class Ledger {
 public:
  static constexpr uint8_t kDelivered = 1;  // a consumer got it
  static constexpr uint8_t kDropped = 2;    // the producer freed a refused packet
  static constexpr uint8_t kDrained = 4;    // the owner's Drain took it

  Ledger(unsigned producers, uint64_t capacity) : state_(producers) {
    for (auto &v : state_) {
      v = std::vector<std::atomic<uint8_t>>(capacity);
    }
  }
  bool Mark(uint32_t producer, uint64_t seq, uint8_t what) {
    if (producer >= state_.size() || seq >= state_[producer].size()) {
      return false;
    }
    return state_[producer][seq].exchange(what) == 0;
  }
  // Empty if every sequence number below `assigned[p]` was accounted exactly
  // once and none above was.
  std::string Verify(const std::vector<uint64_t> &assigned) const {
    for (size_t p = 0; p < state_.size(); p++) {
      for (uint64_t seq = 0; seq < state_[p].size(); seq++) {
        const uint8_t what = state_[p][seq].load();
        const bool expect = seq < assigned[p];
        if (expect != (what != 0)) {
          return "producer " + std::to_string(p) + " packet " +
                 std::to_string(seq) + (expect ? " was never accounted"
                                               : " was accounted but never made");
        }
      }
    }
    return "";
  }
  uint64_t Count(uint8_t what) const {
    uint64_t n = 0;
    for (const auto &producer : state_) {
      for (const auto &v : producer) {
        n += v.load() == what;
      }
    }
    return n;
  }

 private:
  std::vector<std::vector<std::atomic<uint8_t>>> state_;
};

enum class OnFull { kRetry, kDrop };

struct ProducerResult {
  uint64_t assigned = 0;   // sequence numbers handed to packets
  uint64_t accepted = 0;   // taken by the channel
  uint64_t dropped = 0;    // refused and freed (policy kDrop, or closed)
  uint64_t closed_at = 0;  // packets refused because the channel was closed
  bool saw_closed = false;
};

// Makes up to `count` packets in bursts of 1..32 and hands them over. A refused
// tail is retried (after a yield) or dropped, as the policy says; a closed
// channel ends the producer. Never blocks without checking `stop` and the
// deadline. With `wait_for_close`, a producer that has made its `count` does
// not leave until the channel is closed, then tries one more packet on the
// closed channel (refused, and still the producer's): whatever the schedule,
// the producer has met the closed channel, and a producer that finished before
// the owner got to Close is no different from one that was cut off.
template <typename Channel>
ProducerResult RunProducer(Channel &channel, Env &env, Ledger &ledger,
                           std::atomic<bool> &stop, const Deadline &deadline,
                           uint32_t id, uint64_t count, OnFull policy,
                           std::atomic<uint64_t> *progress = nullptr,
                           bool wait_for_close = false) {
  using Item = typename Channel::Item;
  ProducerResult result;
  std::mt19937 rng(id * 7919 + 13);
  Item items[32];
  while (result.assigned < count && !stop.load(std::memory_order_relaxed) &&
         !deadline.Expired()) {
    const size_t want = std::min<uint64_t>(1 + rng() % 32, count - result.assigned);
    size_t made = 0;
    while (made < want) {
      PacketHandle packet = env.Alloc(Pack(id, result.assigned + made));
      if (packet == nullptr) {
        break;  // the pool is empty: send what we have
      }
      items[made] = {packet, Seq{id, 0, result.assigned + made}};
      made++;
    }
    if (made == 0) {
      std::this_thread::yield();
      continue;
    }
    std::span<Item> rest(items, made);
    // Packets this burst will make carry sequence numbers from here on.
    result.assigned += made;
    auto release_rest = [&](uint8_t how) {
      for (Item &item : rest) {
        ledger.Mark(id, item.context.seq, how);
        env.Free(item.packet);
      }
      result.dropped += rest.size();
    };
    while (!rest.empty()) {
      const PuntResult r = channel.TryPuntBurst(rest);
      result.accepted += r.accepted;
      rest = rest.subspan(r.accepted);
      if (!r.refused.has_value()) {
        break;
      }
      if (*r.refused == HandoffError::kClosed) {
        result.saw_closed = true;
        result.closed_at += rest.size();
        release_rest(Ledger::kDropped);
        return result;
      }
      if (policy == OnFull::kDrop) {
        release_rest(Ledger::kDropped);
        break;
      }
      if (stop.load(std::memory_order_relaxed) || deadline.Expired()) {
        release_rest(Ledger::kDropped);
        return result;
      }
      std::this_thread::yield();
    }
    // (a burst is accounted once it has been handed over or released)
    if (progress != nullptr) {
      progress->store(result.assigned, std::memory_order_relaxed);
    }
  }
  if (wait_for_close && !result.saw_closed) {
    while (!channel.closed()) {
      if (stop.load(std::memory_order_relaxed) || deadline.Expired()) {
        return result;
      }
      std::this_thread::yield();
    }
    PacketHandle packet = nullptr;
    while ((packet = env.Alloc(Pack(id, result.assigned))) == nullptr) {
      if (deadline.Expired()) {
        return result;
      }
      std::this_thread::yield();
    }
    Item item{packet, Seq{id, 0, result.assigned}};
    result.assigned++;
    const PuntResult r = channel.TryPuntBurst(std::span<Item>(&item, 1));
    result.accepted += r.accepted;
    if (r.refused == HandoffError::kClosed) {
      result.saw_closed = true;
      result.closed_at++;
      result.dropped++;
      ledger.Mark(id, item.context.seq, Ledger::kDropped);
      env.Free(item.packet);
    }
  }
  return result;
}

struct ConsumerResult {
  uint64_t received = 0;
  uint64_t out_of_order = 0;  // an item older than one this consumer already had
  uint64_t bad = 0;           // a packet and its context disagreed, or a duplicate
};

// Takes bursts until `producers_done` is set and the channel is found empty
// afterwards, or until `stop` (it then leaves what is queued: the owner
// drains it) or `limit` items. Checks every item is whole, accounted once, and,
// per producer, newer than the last one this consumer saw.
template <typename Channel>
ConsumerResult RunConsumer(Channel &channel, Env &env, Ledger &ledger,
                           unsigned producers, std::atomic<bool> &producers_done,
                           std::atomic<bool> &stop, const Deadline &deadline,
                           uint64_t limit = UINT64_MAX) {
  using Item = typename Channel::Item;
  ConsumerResult result;
  std::vector<int64_t> last(producers, -1);
  Item items[32];
  std::mt19937 rng(99);
  while (result.received < limit && !deadline.Expired()) {
    if (stop.load(std::memory_order_relaxed)) {
      break;
    }
    const bool done = producers_done.load(std::memory_order_acquire);
    const size_t n = channel.Dequeue(std::span<Item>(items, 1 + rng() % 32));
    if (n == 0) {
      if (done) {
        break;
      }
      std::this_thread::yield();
      continue;
    }
    for (size_t i = 0; i < n; i++) {
      const Seq context = items[i].context;
      if (Env::TagOf(items[i].packet) != Pack(context.producer, context.seq) ||
          context.producer >= producers ||
          !ledger.Mark(context.producer, context.seq, Ledger::kDelivered)) {
        result.bad++;
      } else {
        if (static_cast<int64_t>(context.seq) <= last[context.producer]) {
          result.out_of_order++;
        }
        last[context.producer] = std::max<int64_t>(
            last[context.producer], static_cast<int64_t>(context.seq));
      }
      env.Free(items[i].packet);
    }
    result.received += n;
  }
  return result;
}

template <HandoffTopology T>
typename HandoffChannel<Seq, T>::Ptr MakeChannel(size_t capacity) {
  HandoffConfig config;
  config.capacity = capacity;
  auto created = HandoffChannel<Seq, T>::Create(config);
  EXPECT_TRUE(created.has_value());
  return std::move(created).value();
}

// -- streams --------------------------------------------------------------------------

// A producer thread and a consumer thread, a channel small enough that it is
// both full and empty many times: every packet arrives once, whole, in order.
TEST(HandoffThreadsTest, OneProducerOneConsumerDeliverEveryPacketOnceInOrder) {
  constexpr uint64_t kPackets = 100000;
  Env env(1024);
  Ledger ledger(1, kPackets);
  auto channel = MakeChannel<kSpSc>(64);
  const Deadline deadline;
  std::atomic<bool> producers_done{false};
  ProducerResult produced;
  ConsumerResult consumed;
  {
    Threads threads;
    threads.Start([&] {
      produced = RunProducer(*channel, env, ledger, threads.stop(), deadline, 0,
                             kPackets, OnFull::kRetry);
      producers_done.store(true, std::memory_order_release);
    });
    threads.Start([&] {
      consumed = RunConsumer(*channel, env, ledger, 1, producers_done,
                             threads.stop(), deadline);
    });
    threads.Wait();
  }
  ASSERT_FALSE(deadline.expired()) << "a thread stopped making progress";
  EXPECT_EQ(kPackets, produced.assigned);
  EXPECT_EQ(kPackets, produced.accepted);
  EXPECT_EQ(kPackets, consumed.received);
  EXPECT_EQ(0u, consumed.bad);
  EXPECT_EQ(0u, consumed.out_of_order);
  EXPECT_EQ("", ledger.Verify({kPackets}));
  EXPECT_EQ(0u, env.Outstanding());
  const HandoffStats stats = channel->stats();
  EXPECT_EQ(kPackets, stats.enqueued);
  EXPECT_EQ(kPackets, stats.dequeued);
  EXPECT_EQ(0u, stats.occupancy);
}

// The same with the producer dropping what the channel refuses (the fail/drop
// policy): every packet is either delivered or counted dropped, none both, none
// neither, and the channel's refusal count is the producer's drop count.
TEST(HandoffThreadsTest, DropPolicyAccountsForEveryPacket) {
  constexpr uint64_t kPackets = 100000;
  Env env(1024);
  Ledger ledger(1, kPackets);
  auto channel = MakeChannel<kSpSc>(16);
  const Deadline deadline;
  std::atomic<bool> producers_done{false};
  ProducerResult produced;
  ConsumerResult consumed;
  {
    Threads threads;
    threads.Start([&] {
      produced = RunProducer(*channel, env, ledger, threads.stop(), deadline, 0,
                             kPackets, OnFull::kDrop);
      producers_done.store(true, std::memory_order_release);
    });
    threads.Start([&] {
      consumed = RunConsumer(*channel, env, ledger, 1, producers_done,
                             threads.stop(), deadline);
    });
    threads.Wait();
  }
  ASSERT_FALSE(deadline.expired()) << "a thread stopped making progress";
  EXPECT_EQ(kPackets, produced.assigned);
  EXPECT_EQ(produced.accepted, consumed.received);
  EXPECT_EQ(kPackets, produced.accepted + produced.dropped);
  EXPECT_EQ(0u, consumed.bad);
  EXPECT_EQ(0u, consumed.out_of_order);
  EXPECT_EQ("", ledger.Verify({kPackets}));
  EXPECT_EQ(consumed.received, ledger.Count(Ledger::kDelivered));
  EXPECT_EQ(produced.dropped, ledger.Count(Ledger::kDropped));
  EXPECT_EQ(0u, env.Outstanding());
  const HandoffStats stats = channel->stats();
  EXPECT_EQ(produced.dropped, stats.refused_full)
      << "every refusal the producer saw, the channel counted";
  EXPECT_EQ(produced.accepted, stats.enqueued);
}

TEST(HandoffThreadsTest, ManyProducersOneConsumerKeepEachProducersOrder) {
  constexpr unsigned kProducers = 3;
  constexpr uint64_t kPerProducer = 40000;
  Env env(1024);
  Ledger ledger(kProducers, kPerProducer);
  auto channel = MakeChannel<kMpSc>(64);
  const Deadline deadline;
  std::atomic<bool> producers_done{false};
  std::vector<ProducerResult> produced(kProducers);
  ConsumerResult consumed;
  {
    std::atomic<unsigned> finished{0};
    Threads threads;
    for (unsigned p = 0; p < kProducers; p++) {
      threads.Start([&, p] {
        produced[p] = RunProducer(*channel, env, ledger, threads.stop(), deadline,
                                  p, kPerProducer, OnFull::kRetry);
        if (finished.fetch_add(1) + 1 == kProducers) {
          producers_done.store(true, std::memory_order_release);
        }
      });
    }
    threads.Start([&] {
      consumed = RunConsumer(*channel, env, ledger, kProducers, producers_done,
                             threads.stop(), deadline);
    });
    threads.Wait();
  }
  ASSERT_FALSE(deadline.expired()) << "a thread stopped making progress";
  EXPECT_EQ(kProducers * kPerProducer, consumed.received);
  EXPECT_EQ(0u, consumed.bad);
  EXPECT_EQ(0u, consumed.out_of_order) << "a producer's packets reordered";
  EXPECT_EQ("", ledger.Verify(std::vector<uint64_t>(kProducers, kPerProducer)));
  EXPECT_EQ(0u, env.Outstanding());
  EXPECT_EQ(kProducers * kPerProducer, channel->stats().enqueued);
}

TEST(HandoffThreadsTest, ManyProducersManyConsumersDeliverEachPacketExactlyOnce) {
  constexpr unsigned kProducers = 2, kConsumers = 2;
  constexpr uint64_t kPerProducer = 40000;
  Env env(1024);
  Ledger ledger(kProducers, kPerProducer);
  auto channel = MakeChannel<kMpMc>(64);
  const Deadline deadline;
  std::atomic<bool> producers_done{false};
  std::vector<ProducerResult> produced(kProducers);
  std::vector<ConsumerResult> consumed(kConsumers);
  {
    std::atomic<unsigned> finished{0};
    Threads threads;
    for (unsigned p = 0; p < kProducers; p++) {
      threads.Start([&, p] {
        produced[p] = RunProducer(*channel, env, ledger, threads.stop(), deadline,
                                  p, kPerProducer, OnFull::kRetry);
        if (finished.fetch_add(1) + 1 == kProducers) {
          producers_done.store(true, std::memory_order_release);
        }
      });
    }
    for (unsigned c = 0; c < kConsumers; c++) {
      threads.Start([&, c] {
        consumed[c] = RunConsumer(*channel, env, ledger, kProducers,
                                  producers_done, threads.stop(), deadline);
      });
    }
    threads.Wait();
  }
  ASSERT_FALSE(deadline.expired()) << "a thread stopped making progress";
  uint64_t received = 0, bad = 0, out_of_order = 0;
  for (const auto &c : consumed) {
    received += c.received;
    bad += c.bad;
    out_of_order += c.out_of_order;
  }
  EXPECT_EQ(kProducers * kPerProducer, received);
  EXPECT_EQ(0u, bad) << "a packet torn from its context, or delivered twice";
  EXPECT_EQ(0u, out_of_order) << "one consumer saw a producer's packets reordered";
  EXPECT_EQ("", ledger.Verify(std::vector<uint64_t>(kProducers, kPerProducer)));
  EXPECT_EQ(0u, env.Outstanding());
}

// -- a consumer that never drains ------------------------------------------------------

// Nothing consumes. The channel takes exactly its capacity, refuses everything
// else and says so, memory stays what it was, and tearing it down gives every
// packet back.
TEST(HandoffThreadsTest, ConsumerThatNeverDrainsBoundsTheQueueAndTeardownFreesIt) {
  constexpr size_t kCapacity = 32;
  constexpr uint64_t kPerProducer = 24000;
  constexpr unsigned kProducers = 2;
  Env env(1024);
  Ledger ledger(kProducers, kPerProducer);
  auto channel = MakeChannel<kMpSc>(kCapacity);
  const size_t bytes = channel->memory_bytes();
  const Deadline deadline;
  std::vector<ProducerResult> produced(kProducers);
  {
    Threads threads;
    for (unsigned p = 0; p < kProducers; p++) {
      threads.Start([&, p] {
        produced[p] = RunProducer(*channel, env, ledger, threads.stop(), deadline,
                                  p, kPerProducer, OnFull::kDrop);
      });
    }
    threads.Wait();
  }
  ASSERT_FALSE(deadline.expired());
  uint64_t accepted = 0, dropped = 0;
  for (const auto &r : produced) {
    accepted += r.accepted;
    dropped += r.dropped;
    EXPECT_EQ(kPerProducer, r.assigned);
  }
  EXPECT_EQ(kCapacity, accepted) << "the channel took exactly its capacity";
  EXPECT_EQ(kProducers * kPerProducer - kCapacity, dropped);
  const HandoffStats stats = channel->stats();
  EXPECT_EQ(kCapacity, stats.occupancy);
  EXPECT_EQ(kCapacity, stats.enqueued);
  EXPECT_EQ(dropped, stats.refused_full) << "every refusal was counted";
  EXPECT_EQ(0u, stats.dequeued);
  EXPECT_EQ(bytes, channel->memory_bytes()) << "no growth";
  EXPECT_EQ(kCapacity, env.Outstanding()) << "only the queued packets are out";
  channel.reset();
  EXPECT_EQ(0u, env.Outstanding()) << "teardown freed what was queued";
}

// -- teardown while packets are in flight ------------------------------------------------

// The shutdown order, run against live producers and a live consumer: Close the
// channel (producers see kClosed at once and stop), join the producers, stop
// the consumer (it leaves what is queued), Drain, destroy. Every packet made is
// accounted exactly once -- delivered, dropped by its producer, or drained --
// and none is outstanding.
TEST(HandoffThreadsTest, ShutdownOrderAccountsForEveryPacketInFlight) {
  constexpr unsigned kProducers = 2;
  // Each producer makes at most this many; whether the owner's Close comes
  // before or after (it depends on the schedule) the producers end up meeting
  // the closed channel (`wait_for_close`) and everything is accounted.
  constexpr uint64_t kMax = 300000;
  constexpr uint64_t kBeforeClose = 20000;  // each producer makes this many first
  Env env(1024);
  Ledger ledger(kProducers, kMax + 1);
  auto channel = MakeChannel<kMpSc>(48);
  const Deadline deadline;
  std::atomic<bool> producers_done{false};
  std::vector<ProducerResult> produced(kProducers);
  std::vector<std::atomic<uint64_t>> progress(kProducers);
  ConsumerResult consumed;
  uint64_t drained = 0;
  {
    Threads producers, consumer;
    for (unsigned p = 0; p < kProducers; p++) {
      producers.Start([&, p] {
        produced[p] = RunProducer(*channel, env, ledger, producers.stop(), deadline,
                                  p, kMax, OnFull::kRetry, &progress[p],
                                  /*wait_for_close=*/true);
      });
    }
    consumer.Start([&] {
      consumed = RunConsumer(*channel, env, ledger, kProducers, producers_done,
                             consumer.stop(), deadline);
    });
    // Let real traffic flow, then shut down in order.
    for (unsigned p = 0; p < kProducers; p++) {
      while (progress[p].load(std::memory_order_relaxed) < kBeforeClose &&
             !deadline.Expired()) {
        std::this_thread::yield();
      }
    }
    channel->Close();    // 1. no new work is taken
    producers.Wait();    // 2a. the producers see kClosed and finish
    consumer.Join();     // 2b. the consumer stops, leaving what is queued
    drained = channel->Drain([&](PuntItem<Seq> &item) noexcept {
      EXPECT_EQ(Env::TagOf(item.packet), Pack(item.context.producer, item.context.seq));
      EXPECT_TRUE(ledger.Mark(item.context.producer, item.context.seq, Ledger::kDrained))
          << "a drained packet was already accounted";
      env.Free(item.packet);
    });                  // 3. the owner takes the rest
  }
  ASSERT_FALSE(deadline.expired()) << "a thread stopped making progress";
  std::vector<uint64_t> assigned;
  uint64_t accepted = 0, dropped = 0;
  for (const auto &r : produced) {
    EXPECT_TRUE(r.saw_closed) << "a producer never learned the channel closed";
    assigned.push_back(r.assigned);
    accepted += r.accepted;
    dropped += r.dropped;
    EXPECT_GE(r.assigned, kBeforeClose);
  }
  EXPECT_EQ(0u, consumed.bad);
  EXPECT_EQ(0u, consumed.out_of_order);
  EXPECT_EQ("", ledger.Verify(assigned)) << "a packet was lost or duplicated";
  EXPECT_EQ(accepted, consumed.received + drained) << "taken = delivered + drained";
  EXPECT_EQ(consumed.received, ledger.Count(Ledger::kDelivered));
  EXPECT_EQ(drained, ledger.Count(Ledger::kDrained));
  EXPECT_EQ(dropped, ledger.Count(Ledger::kDropped));
  const HandoffStats stats = channel->stats();
  EXPECT_EQ(accepted, stats.enqueued);
  EXPECT_EQ(consumed.received, stats.dequeued);
  EXPECT_EQ(drained, stats.discarded);
  EXPECT_EQ(0u, stats.occupancy);
  EXPECT_EQ(stats.enqueued, stats.dequeued + stats.discarded + stats.occupancy);
  EXPECT_GT(stats.refused_closed, 0u) << "producers hit the closed channel";
  EXPECT_EQ(0u, env.Outstanding());
  channel.reset();
  EXPECT_EQ(0u, env.Outstanding());
}

// A consumer that stops on its own mid-stream (a crash, as far as the channel
// can tell): its producers see a full channel and drop, so memory stays bounded;
// the owner notices, closes, joins the producers, and drains what the consumer
// left. Nothing is lost.
TEST(HandoffThreadsTest, ConsumerThatStopsMidStreamIsTornDownByItsOwner) {
  constexpr unsigned kProducers = 2;
  constexpr uint64_t kMax = 100000;  // never reached: the consumer leaves first
  constexpr uint64_t kConsumerTakes = 5000;
  Env env(1024);
  Ledger ledger(kProducers, kMax + 1);
  auto channel = MakeChannel<kMpSc>(32);
  const Deadline deadline;
  std::atomic<bool> producers_done{false}, consumer_gone{false};
  std::vector<ProducerResult> produced(kProducers);
  ConsumerResult consumed;
  uint64_t drained = 0;
  {
    Threads producers, consumer;
    for (unsigned p = 0; p < kProducers; p++) {
      producers.Start([&, p] {
        produced[p] = RunProducer(*channel, env, ledger, producers.stop(), deadline,
                                  p, kMax, OnFull::kRetry, nullptr,
                                  /*wait_for_close=*/true);
      });
    }
    consumer.Start([&] {
      consumed = RunConsumer(*channel, env, ledger, kProducers, producers_done,
                             consumer.stop(), deadline, kConsumerTakes);
      consumer_gone.store(true, std::memory_order_release);
    });
    while (!consumer_gone.load(std::memory_order_acquire) && !deadline.Expired()) {
      std::this_thread::yield();
    }
    channel->Close();
    producers.Wait();
    consumer.Join();
    drained = channel->Drain([&](PuntItem<Seq> &item) noexcept {
      EXPECT_TRUE(ledger.Mark(item.context.producer, item.context.seq, Ledger::kDrained));
      env.Free(item.packet);
    });
  }
  ASSERT_FALSE(deadline.expired()) << "a thread stopped making progress";
  std::vector<uint64_t> assigned;
  uint64_t accepted = 0;
  for (const auto &r : produced) {
    assigned.push_back(r.assigned);
    accepted += r.accepted;
  }
  EXPECT_GE(consumed.received, kConsumerTakes);
  EXPECT_EQ(0u, consumed.bad);
  EXPECT_EQ("", ledger.Verify(assigned));
  EXPECT_EQ(accepted, consumed.received + drained);
  EXPECT_EQ(0u, env.Outstanding());
}

// Create, run traffic, shut down, destroy: over and over. Nothing leaks from
// the channel's memory (the allocator is asked once per channel and given it
// back once) or from the packet pool.
TEST(HandoffThreadsTest, RepeatedLifecycleUnderTrafficLeaksNothing) {
  static std::atomic<int> allocations{0}, deallocations{0};
  static const HandoffAllocator counting{
      [](size_t bytes, size_t align, int, int *placed) noexcept -> void * {
        allocations++;
        *placed = 0;
        return std::aligned_alloc(align, (bytes + align - 1) / align * align);
      },
      [](void *block) noexcept {
        deallocations++;
        std::free(block);
      }};
  allocations = 0;
  deallocations = 0;
  Env env(512);
  const Deadline deadline;
  constexpr int kRounds = 60;
  constexpr uint64_t kPackets = 600;
  for (int round = 0; round < kRounds; round++) {
    HandoffConfig config;
    config.capacity = 8;
    config.allocator = &counting;
    auto created = HandoffChannel<Seq, kSpSc>::Create(config);
    ASSERT_TRUE(created.has_value());
    auto &channel = **created;
    Ledger ledger(1, kPackets);
    std::atomic<bool> producers_done{false};
    ProducerResult produced;
    ConsumerResult consumed;
    uint64_t drained = 0;
    {
      Threads producer, consumer;
      producer.Start([&] {
        produced = RunProducer(channel, env, ledger, producer.stop(), deadline, 0,
                               kPackets, OnFull::kRetry);
        producers_done.store(true, std::memory_order_release);
      });
      // The consumer takes some and goes; the owner closes and drains the rest.
      consumer.Start([&] {
        consumed = RunConsumer(channel, env, ledger, 1, producers_done,
                               consumer.stop(), deadline, kPackets / 2);
      });
      consumer.Wait();
      channel.Close();
      producer.Wait();
      drained = channel.Drain([&](PuntItem<Seq> &item) noexcept {
        ledger.Mark(item.context.producer, item.context.seq, Ledger::kDrained);
        env.Free(item.packet);
      });
    }
    ASSERT_FALSE(deadline.expired()) << "round " << round;
    EXPECT_EQ(produced.accepted, consumed.received + drained) << "round " << round;
    EXPECT_EQ("", ledger.Verify({produced.assigned})) << "round " << round;
    created->reset();
    ASSERT_EQ(0u, env.Outstanding()) << "round " << round;
  }
  EXPECT_EQ(kRounds, allocations.load());
  EXPECT_EQ(kRounds, deallocations.load());
}

// -- punt, service, resume --------------------------------------------------------------

// The pattern the mechanism exists for. A worker punts packets, each with a
// continuation naming where it should resume; a service thread (which does not
// own the continuation table, only reads it) sends every packet back on a
// second channel; the worker resolves the continuation and resumes the packet.
// Meanwhile the worker retires some continuations while their packets are away
// (their target went away): those packets must come back and fail closed, and
// never reach a target that reused the slot.
TEST(HandoffThreadsTest, PuntServiceResumeFailsClosedForARetiredContinuation) {
  constexpr uint64_t kPackets = 60000;
  struct Request {
    ContinuationHandle continuation;
    uint64_t seq;
  };
  struct Reply {
    ContinuationHandle continuation;
    uint64_t seq;
    uint32_t service_saw_live;  // what the service's own Resolve said
    uint32_t unused;
  };
  struct Target {
    uint64_t seq;
    uint64_t inverted;
  };
  Env env(512);
  const Deadline deadline;
  HandoffConfig config;
  config.capacity = 64;
  auto punt = HandoffChannel<Request, kSpSc>::Create(config);
  auto resume = HandoffChannel<Reply, kSpSc>::Create(config);
  ASSERT_TRUE(punt.has_value());
  ASSERT_TRUE(resume.has_value());
  auto table_created = ContinuationTable<Target>::Create(96);
  ASSERT_TRUE(table_created.has_value());
  ContinuationTable<Target> &table = **table_created;

  std::atomic<bool> worker_done{false};
  std::atomic<uint64_t> service_stale{0}, service_bad{0};
  // Worker's results.
  uint64_t resumed = 0, stale = 0, wrong = 0, duplicates = 0, retired_early = 0;
  std::vector<uint8_t> returned(kPackets, 0);
  {
    Threads threads;
    // The service thread: forwards ownership, owns nothing.
    threads.Start([&] {
      PuntItem<Request> in[32];
      PuntItem<Reply> out[32];
      while (!threads.stop().load(std::memory_order_relaxed) && !deadline.Expired()) {
        const bool done = worker_done.load(std::memory_order_acquire);
        const size_t n = (*punt)->Dequeue(in);
        if (n == 0) {
          if (done) {
            break;
          }
          std::this_thread::yield();
          continue;
        }
        for (size_t i = 0; i < n; i++) {
          if (Env::TagOf(in[i].packet) != in[i].context.seq) {
            service_bad++;
          }
          // The service looks at the continuation from its own thread.
          const std::optional<Target> target = table.Resolve(in[i].context.continuation);
          if (target.has_value() && (target->seq != in[i].context.seq ||
                                     target->inverted != ~in[i].context.seq)) {
            service_bad++;
          }
          if (!target.has_value()) {
            service_stale++;
          }
          out[i] = {in[i].packet,
                    Reply{in[i].context.continuation, in[i].context.seq,
                          target.has_value() ? 1u : 0u, 0}};
        }
        std::span<PuntItem<Reply>> rest(out, n);
        while (!rest.empty() && !deadline.Expired()) {
          const PuntResult r = (*resume)->TryPuntBurst(rest);
          rest = rest.subspan(r.accepted);
          if (!rest.empty()) {
            std::this_thread::yield();
          }
        }
        for (PuntItem<Reply> &item : rest) {  // only on a deadline
          env.Free(item.packet);
        }
      }
    });

    // The worker (this thread of the test): punts, resumes, retires.
    std::mt19937 rng(5);
    std::vector<ContinuationHandle> in_flight;          // issued, packet away
    std::unordered_set<uint64_t> retired_handles;       // bits of handles we retired
    std::vector<PuntItem<Reply>> back(32);
    uint64_t made = 0;
    auto take_returns = [&] {
      const size_t n = (*resume)->Dequeue(back);
      for (size_t i = 0; i < n; i++) {
        const Reply &reply = back[i].context;
        if (reply.seq >= kPackets || returned[reply.seq]++ != 0) {
          duplicates++;
        }
        const bool expect_live =
            retired_handles.count(std::bit_cast<uint64_t>(reply.continuation)) == 0;
        const std::optional<Target> target = table.Resolve(reply.continuation);
        if (target.has_value() != expect_live) {
          wrong++;
        } else if (target.has_value()) {
          if (target->seq != reply.seq) {
            wrong++;
          }
          if (!table.Retire(reply.continuation)) {
            wrong++;
          }
          resumed++;
        } else {
          stale++;
        }
        if (Env::TagOf(back[i].packet) != reply.seq) {
          wrong++;
        }
        env.Free(back[i].packet);
      }
      return n;
    };
    std::vector<PuntItem<Request>> burst;
    uint64_t returned_total = 0;
    while ((made < kPackets || returned_total < kPackets) && !deadline.Expired()) {
      const size_t got = take_returns();
      returned_total += got;
      if (made == kPackets) {
        if (got == 0) {
          std::this_thread::yield();
        }
        continue;
      }
      burst.clear();
      const uint64_t want = std::min<uint64_t>(1 + rng() % 16, kPackets - made);
      for (uint64_t i = 0; i < want; i++) {
        PacketHandle packet = env.Alloc(made + burst.size());
        if (packet == nullptr) {
          break;
        }
        const ContinuationHandle handle =
            table.Issue(Target{made + burst.size(), ~(made + burst.size())});
        if (handle == kNoContinuation) {
          env.Free(packet);
          break;
        }
        burst.push_back({packet, Request{handle, made + burst.size()}});
      }
      if (burst.empty()) {
        std::this_thread::yield();
        continue;
      }
      std::span<PuntItem<Request>> rest(burst);
      const size_t count = burst.size();
      while (!rest.empty() && !deadline.Expired()) {
        const PuntResult r = (*punt)->TryPuntBurst(rest);
        rest = rest.subspan(r.accepted);
        if (!rest.empty()) {
          returned_total += take_returns();  // make room downstream
          std::this_thread::yield();
        }
      }
      made += count;
      // Some targets go away while their packets are away.
      for (size_t i = 0; i < count; i++) {
        if (rng() % 7 == 0) {
          const ContinuationHandle victim = burst[i].context.continuation;
          if (table.Retire(victim)) {
            retired_handles.insert(std::bit_cast<uint64_t>(victim));
            retired_early++;
          }
        }
      }
    }
    worker_done.store(true, std::memory_order_release);
    threads.Join();
  }
  ASSERT_FALSE(deadline.expired()) << "a thread stopped making progress";
  EXPECT_EQ(kPackets, resumed + stale) << "every packet came back exactly once";
  EXPECT_EQ(0u, duplicates);
  EXPECT_EQ(0u, wrong) << "a packet resumed at the wrong target, or a retired "
                          "continuation resolved";
  EXPECT_EQ(retired_early, stale)
      << "exactly the retired continuations failed closed";
  EXPECT_EQ(0u, service_bad.load());
  EXPECT_GT(retired_early, 0u);
  EXPECT_GT(resumed, 0u);
  EXPECT_EQ(0u, table.size()) << "every live continuation was retired on resume";
  EXPECT_EQ(0u, env.Outstanding());
  for (uint64_t seq = 0; seq < kPackets; seq++) {
    ASSERT_EQ(1, returned[seq]) << seq;
  }
  const HandoffStats a = (*punt)->stats(), b = (*resume)->stats();
  EXPECT_EQ(kPackets, a.enqueued);
  EXPECT_EQ(kPackets, a.dequeued);
  EXPECT_EQ(kPackets, b.enqueued);
  EXPECT_EQ(kPackets, b.dequeued);
}

// A single-producer role may move between threads when the move synchronises
// (here, a join): the second producer must see a consistent channel.
TEST(HandoffThreadsTest, ASingleProducerRoleMovesBetweenThreadsAtAJoin) {
  Env env(256);
  Ledger ledger(2, 5000);
  auto channel = MakeChannel<kSpSc>(32);
  const Deadline deadline;
  std::atomic<bool> producers_done{false};
  ProducerResult first, second;
  ConsumerResult consumed;
  {
    Threads consumer;
    consumer.Start([&] {
      consumed = RunConsumer(*channel, env, ledger, 2, producers_done,
                             consumer.stop(), deadline);
    });
    {
      Threads producer;
      producer.Start([&] {
        first = RunProducer(*channel, env, ledger, producer.stop(), deadline, 0,
                            5000, OnFull::kRetry);
      });
      producer.Wait();
    }
    {
      Threads producer;
      producer.Start([&] {
        second = RunProducer(*channel, env, ledger, producer.stop(), deadline, 1,
                             5000, OnFull::kRetry);
      });
      producer.Wait();
    }
    producers_done.store(true, std::memory_order_release);
    consumer.Wait();
  }
  ASSERT_FALSE(deadline.expired());
  EXPECT_EQ(10000u, consumed.received);
  EXPECT_EQ(0u, consumed.bad);
  EXPECT_EQ("", ledger.Verify({5000, 5000}));
  EXPECT_EQ(0u, env.Outstanding());
}

}  // namespace
}  // namespace bess::dataplane
