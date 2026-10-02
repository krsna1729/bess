// SPDX-License-Identifier: BSD-3-Clause

// The reference consumer of the expiry engine (roadmap M10, D-053): idle-
// timeout expiry of flows in a WorkerFlowTable, wired through the table's
// Observer seam and nothing else. Written only against the public headers of
// flow/ and dataplane/; the two libraries know nothing of each other.
//
//   OnCreate  -> Schedule(now + timeout, flow handle); keep the ExpiryHandle in
//                the flow's State
//   a packet  -> Refresh(state.timer, now + timeout)   (engine refresh), or
//                state.last_seen = now                  (owner-side refresh)
//   OnErase   -> Cancel(state.timer)
//   Poll      -> callback: table.Erase(flow handle)
//
// The generic unit tests of the engine are in dataplane/expiry_wheel_test.cc.

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include "dataplane/expiry_wheel.h"
#include "flow/worker_flow_table.h"

namespace bess::flow {
namespace {

using dataplane::ExpiryHandle;
using dataplane::ExpiryWheel;
using dataplane::kNoExpiry;

using IdleWheel = ExpiryWheel<FlowHandle>;

struct Conn {
  Conn() = default;
  uint64_t last_seen = 0;  // owner-side refresh keeps the real time here
  ExpiryHandle timer = kNoExpiry;
  uint64_t packets = 0;
};

// The consumer's half of the seam. `cancel_on_erase` can be turned off to
// simulate an owner that forgets: the engine's record then outlives the flow
// and the table's generation check is the only protection.
struct IdleObserver {
  IdleWheel *wheel = nullptr;
  const uint64_t *now = nullptr;
  uint64_t timeout = 0;
  bool cancel_on_erase = true;
  uint64_t full = 0;

  void OnCreate(FlowHandle h, Conn &c) noexcept {
    c.last_seen = *now;
    c.timer = wheel->Schedule(*now + timeout, h);
  }
  void OnErase(FlowHandle, Conn &c) noexcept {
    if (cancel_on_erase) {
      wheel->Cancel(c.timer);
    }
  }
  void OnFull() noexcept { full++; }
};

struct IdleTraits : DefaultFlowTableTraits {
  static constexpr size_t kAliases = 1;  // the reverse key of a bidirectional flow
  using Observer = IdleObserver;
};
using IdleTable = WorkerFlowTable<uint64_t, Conn, DefaultFlowHash<uint64_t>,
                                  DefaultFlowEqual<uint64_t>, IdleTraits>;

enum class Refresh { kEngine, kOwnerStore };

class IdleFlows {
 public:
  IdleFlows(size_t capacity, uint64_t timeout, Refresh mode, unsigned shift = 0,
            bool cancel_on_erase = true)
      : timeout_(timeout), mode_(mode) {
    // One engine timer per possible flow, so Schedule cannot run dry (twice
    // that when stale records are allowed to pile up).
    wheel_ = std::move(IdleWheel::Create(cancel_on_erase ? capacity : 2 * capacity,
                                         0, shift))
                 .value();
    IdleObserver observer;
    observer.wheel = wheel_.get();
    observer.now = &now_;
    observer.timeout = timeout;
    observer.cancel_on_erase = cancel_on_erase;
    table_ = std::move(*IdleTable::Create(capacity, {}, {}, observer));
  }

  // A packet of flow `key` (forward direction) or of its reverse (`key` +
  // kReverse). Creates the flow on a forward miss; nullptr if the table is full
  // or a reverse packet has no flow.
  static constexpr uint64_t kReverse = uint64_t{1} << 40;
  Conn *Packet(uint64_t key, uint64_t now) {
    now_ = now;
    Conn *c = table_->Find(key);
    if (c == nullptr) {
      if (key >= kReverse) return nullptr;
      c = table_->EmplaceAliased(key, key + kReverse).state;
      if (c == nullptr) return nullptr;
    }
    c->packets++;
    if (mode_ == Refresh::kEngine) {
      c->last_seen = now;
      EXPECT_TRUE(wheel_->Refresh(c->timer, now + timeout_));
    } else {
      c->last_seen = now;  // one store; the engine is not called
    }
    return c;
  }

  // Expires what is due at `now`, erasing at most `budget` units' worth.
  dataplane::PollResult Expire(uint64_t now, size_t budget) {
    now_ = now;
    if (mode_ == Refresh::kEngine) {
      return wheel_->Poll(now, budget, [&](const FlowHandle &h) noexcept {
        erased_ += table_->Erase(h) ? 1 : 0;
      });
    }
    return wheel_->Poll(
        now, budget, [&](const FlowHandle &h) noexcept -> std::optional<uint64_t> {
          const Conn *c = table_->Lookup(h);
          if (c == nullptr) {
            stale_++;
            return std::nullopt;
          }
          const uint64_t real = c->last_seen + timeout_;
          if (real > now) {
            return real;  // touched since the record was made: keep it
          }
          erased_ += table_->Erase(h) ? 1 : 0;
          return std::nullopt;
        });
  }

  void ExpireAll(uint64_t now) {
    while (Expire(now, 1 + (now % 7) * 9).exhausted) {
    }
  }

  IdleTable &table() { return *table_; }
  IdleWheel &wheel() { return *wheel_; }
  uint64_t erased() const { return erased_; }
  uint64_t stale() const { return stale_; }

 private:
  uint64_t timeout_;
  Refresh mode_;
  uint64_t now_ = 0;
  std::unique_ptr<IdleWheel> wheel_;
  std::unique_ptr<IdleTable> table_;
  uint64_t erased_ = 0;
  uint64_t stale_ = 0;
};

class IdleExpiryTest : public ::testing::TestWithParam<Refresh> {};

TEST_P(IdleExpiryTest, AFlowExpiresExactlyOneTimeoutAfterItsLastPacket) {
  IdleFlows flows(16, 1000, GetParam());
  ASSERT_NE(nullptr, flows.Packet(1, 100));
  ASSERT_NE(nullptr, flows.Packet(2, 400));
  flows.ExpireAll(1099);
  EXPECT_EQ(2u, flows.table().size());
  flows.ExpireAll(1100);
  EXPECT_NE(nullptr, flows.table().Find(2));
  EXPECT_EQ(nullptr, flows.table().Find(1));
  EXPECT_EQ(nullptr, flows.table().Find(1 + IdleFlows::kReverse))
      << "the alias key leaves with the flow";
  flows.ExpireAll(1399);
  EXPECT_EQ(1u, flows.table().size());
  flows.ExpireAll(1400);
  EXPECT_EQ(0u, flows.table().size());
  EXPECT_EQ(0u, flows.wheel().size()) << "no timer outlives its flow";
}

TEST_P(IdleExpiryTest, AHitRefreshesAndAnIdleNeighbourStillExpires) {
  IdleFlows flows(16, 250, GetParam());
  flows.Packet(1, 0);  // active: a packet every 100 ticks
  flows.Packet(2, 0);  // idle after the first packet
  for (uint64_t t = 100; t <= 100'000; t += 100) {
    flows.Packet(1, t);
    flows.ExpireAll(t);
    if (t < 250) {
      ASSERT_NE(nullptr, flows.table().Find(2)) << t;
    } else {
      ASSERT_EQ(nullptr, flows.table().Find(2)) << t;
    }
    ASSERT_NE(nullptr, flows.table().Find(1)) << t;
  }
  EXPECT_EQ(1001u, flows.table().Find(1)->packets);
  EXPECT_EQ(1u, flows.erased());
}

TEST_P(IdleExpiryTest, AReversePacketRefreshesTheSameFlow) {
  IdleFlows flows(4, 500, GetParam());
  flows.Packet(7, 0);
  // Only the server talks, through the alias key.
  for (uint64_t t = 100; t <= 5000; t += 100) {
    ASSERT_NE(nullptr, flows.Packet(7 + IdleFlows::kReverse, t));
    flows.ExpireAll(t);
    ASSERT_EQ(1u, flows.table().size()) << t;
  }
  flows.ExpireAll(5500);
  EXPECT_EQ(0u, flows.table().size());
}

TEST_P(IdleExpiryTest, AStormOfSimultaneousExpiriesIsSpreadOverBudgetedPolls) {
  constexpr size_t kFlows = 5000;
  IdleFlows flows(kFlows, 1000, GetParam());
  for (uint64_t k = 1; k <= kFlows; k++) {
    ASSERT_NE(nullptr, flows.Packet(k, 0));
  }
  size_t previous = flows.table().size();
  size_t polls = 0;
  for (;;) {
    const auto r = flows.Expire(1000, 64);
    polls++;
    ASSERT_LE(r.work(), 64u);
    ASSERT_LE(flows.table().size(), previous);
    ASSERT_LE(previous - flows.table().size(), 64u) << "more than a poll's budget";
    previous = flows.table().size();
    ASSERT_LT(polls, 100'000u);
    if (!r.exhausted) break;
  }
  EXPECT_GE(polls, kFlows / 64);
  EXPECT_EQ(0u, flows.table().size());
  EXPECT_EQ(kFlows, flows.erased()) << "each flow erased exactly once";
  EXPECT_EQ(0u, flows.wheel().size());
}

TEST_P(IdleExpiryTest, ARefusedFlowHasNoTimer) {
  IdleFlows flows(2, 100, GetParam());
  ASSERT_NE(nullptr, flows.Packet(1, 0));
  ASSERT_NE(nullptr, flows.Packet(2, 0));
  EXPECT_EQ(nullptr, flows.Packet(3, 0));
  EXPECT_EQ(1u, flows.table().observer().full);
  EXPECT_EQ(2u, flows.wheel().size());
  flows.ExpireAll(100);
  EXPECT_NE(nullptr, flows.Packet(3, 100)) << "the slot and timer are free again";
  EXPECT_EQ(1u, flows.wheel().size());
}

// The engine and the table agree on every operation of a long random run,
// against a model of "last packet + timeout" in a std::map. Flow creation is
// refused when the table is full; the model refuses too.
TEST_P(IdleExpiryTest, RandomPacketsAndPollsMatchTheModel) {
  for (const unsigned shift : {0u, 3u}) {
    for (uint64_t seed = 1; seed <= 3; seed++) {
      constexpr size_t kCapacity = 100;
      constexpr uint64_t kTimeout = 5000;
      IdleFlows flows(kCapacity, kTimeout, GetParam(), shift);
      std::mt19937_64 rng(seed);
      std::map<uint64_t, uint64_t> model;  // key -> last packet time
      uint64_t now = 0;
      const uint64_t unit = uint64_t{1} << shift;
      size_t partial_polls = 0, refused = 0;
      for (int step = 0; step < 60'000; step++) {
        const uint64_t op = rng() % 10;
        if (op < 6) {
          // Mostly existing-looking keys, some brand new; a few reverse hits.
          const uint64_t key = 1 + (rng() % 300);
          const bool reverse = (rng() % 8) == 0;
          const bool existed = model.count(key) != 0;
          const bool room = flows.table().size() < kCapacity;
          Conn *c = flows.Packet(reverse ? key + IdleFlows::kReverse : key, now);
          if (existed) {
            ASSERT_NE(nullptr, c);
            model[key] = now;
          } else if (reverse) {
            ASSERT_EQ(nullptr, c);
          } else if (room) {
            ASSERT_NE(nullptr, c);
            model[key] = now;
          } else {
            ASSERT_EQ(nullptr, c) << "full";
            refused++;
          }
        } else {
          now += rng() % 60;
          const size_t budget = std::vector<size_t>{1, 4, 25, 1000}[rng() % 4];
          auto r = flows.Expire(now, budget);
          for (;;) {
            ASSERT_LE(r.work(), budget);
            // A partial poll never erases a flow that is not yet idle.
            for (const auto &[key, last] : model) {
              if (last + kTimeout > now) {
                ASSERT_NE(nullptr, flows.table().Find(key)) << "erased early";
              }
            }
            if (!r.exhausted) break;
            partial_polls++;
            r = flows.Expire(now, budget);
          }
          // Everything idle for a whole timeout (to the granularity) is gone,
          // and nothing younger than that is. Between the two, a flow may go
          // either way: the model adopts the table's answer.
          const uint64_t floor_now = now & ~(unit - 1);
          for (auto it = model.begin(); it != model.end();) {
            const uint64_t real = it->second + kTimeout;
            const Conn *c = flows.table().Find(it->first);
            if (real <= floor_now) {
              ASSERT_EQ(nullptr, c) << "idle flow " << it->first << " survived";
            } else if (real > now) {
              ASSERT_NE(nullptr, c);
            }
            if (c == nullptr) {
              it = model.erase(it);
            } else {
              ASSERT_EQ(it->second, c->last_seen);
              ++it;
            }
          }
          ASSERT_EQ(model.size(), flows.table().size()) << "step " << step;
        }
        ASSERT_EQ(flows.table().size(), flows.wheel().size())
            << "one timer per live flow, step " << step;
      }
      // The run must have exercised what it claims to.
      EXPECT_GT(flows.erased(), 1000u);
      EXPECT_GT(partial_polls, 100u);
      EXPECT_GT(refused, 10u);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    BothRefreshStyles, IdleExpiryTest,
    ::testing::Values(Refresh::kEngine, Refresh::kOwnerStore),
    [](const ::testing::TestParamInfo<Refresh> &info) {
      return info.param == Refresh::kEngine ? "EngineRefresh" : "OwnerSideRefresh";
    });

// -- stale records -----------------------------------------------------------------

// A flow is erased by someone else and a new flow takes its slot. The
// consumer forgot to cancel the old flow's timer (cancel_on_erase = false), so
// the engine delivers a record whose handle names a dead lifetime of the
// slot: the table's generation makes the erase a no-op.
TEST(IdleExpiryStale, ARecordThatOutlivedItsFlowCannotEraseTheFlowInItsSlot) {
  IdleFlows flows(1, 100, Refresh::kEngine, 0, /*cancel_on_erase=*/false);
  Conn *a = flows.Packet(1, 0);
  ASSERT_NE(nullptr, a);
  const FlowHandle old_handle = flows.table().FindRef(1).handle;
  ASSERT_TRUE(flows.table().Erase(uint64_t{1}));  // not by expiry

  // Capacity 1: the new flow necessarily reuses the slot.
  Conn *b = flows.Packet(2, 50);
  ASSERT_NE(nullptr, b);
  const FlowHandle new_handle = flows.table().FindRef(2).handle;
  ASSERT_EQ(old_handle.id, new_handle.id) << "test premise: the slot was reused";
  ASSERT_NE(old_handle.generation, new_handle.generation);

  // The old record is due at 100; the new flow's own (refreshed) one at 150.
  EXPECT_EQ(2u, flows.wheel().size()) << "the stale record is still queued";
  const auto r = flows.Expire(100, 10);
  EXPECT_EQ(1u, r.fired);
  EXPECT_NE(nullptr, flows.table().Find(2)) << "the stale record killed the new flow";
  EXPECT_EQ(0u, flows.erased());
  flows.ExpireAll(150);
  EXPECT_EQ(nullptr, flows.table().Find(2));
  EXPECT_EQ(1u, flows.erased());
  EXPECT_EQ(0u, flows.wheel().size());
}

TEST(IdleExpiryStale, ACancelledTimerLeavesNothingToDeliver) {
  IdleFlows flows(1, 100, Refresh::kEngine);  // cancels on erase
  flows.Packet(1, 0);
  const ExpiryHandle first = flows.table().Find(1)->timer;
  ASSERT_TRUE(flows.table().Erase(uint64_t{1}));
  EXPECT_EQ(0u, flows.wheel().size());
  flows.Packet(2, 10);
  EXPECT_FALSE(flows.wheel().Alive(first));
  EXPECT_FALSE(flows.wheel().Cancel(first));
  const auto r = flows.Expire(105, 10);
  EXPECT_EQ(0u, r.fired) << "the first flow's timer was cancelled with it";
  EXPECT_NE(nullptr, flows.table().Find(2));
}

TEST(IdleExpiryStale, OwnerSideRefreshToleratesARecordWhoseFlowIsGone) {
  IdleFlows flows(1, 100, Refresh::kOwnerStore, 0, /*cancel_on_erase=*/false);
  flows.Packet(1, 0);
  ASSERT_TRUE(flows.table().Erase(uint64_t{1}));
  flows.Packet(2, 50);
  flows.ExpireAll(100);  // the stale record fires: Lookup fails, nothing erased
  EXPECT_EQ(1u, flows.stale());
  EXPECT_NE(nullptr, flows.table().Find(2));
  flows.ExpireAll(149);
  EXPECT_NE(nullptr, flows.table().Find(2));
  flows.ExpireAll(150);
  EXPECT_EQ(nullptr, flows.table().Find(2));
}

}  // namespace
}  // namespace bess::flow
