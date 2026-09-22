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

// K2.3/K2.5: the parts of the ObjectTable publication proof that need no EAL, so
// they can run under ASan/UBSan -- DPDK cannot initialise under ASan, which puts
// anything that launches a worker out of the sanitizer lane's reach. The
// worker-holding half of the proof lives in `object_table_rcu_test.cc`.
//
// K1's shared-grace-period capability is exercised here the way K2 needs it: one
// token retiring several tables, which is what a coordinated generation update
// looks like. The storm is the leak/use-after-free case: thousands of generations
// through the substrate with a live reader.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "control/runtime_state.h"
#include "dataplane/action_id.h"
#include "dataplane/object_table.h"
#include "dataplane/strong_id.h"
#include "rcu/rcu_domain.h"
#include "rcu/rcu_ptr.h"

namespace {

using bess::dataplane::ActionId;
using bess::dataplane::ObjectTable;
using bess::dataplane::ObjectTableBuilder;
using bess::dataplane::StrongId;
using bess::rcu::GracePeriod;
using bess::rcu::RcuPtr;

// The immutable object behind an ActionId, with destruction tracking so the tests
// can prove when and where it happens.
struct TestAction {
  static std::atomic<int> alive;
  static std::atomic<int> destroyed;

  explicit TestAction(int v) : value(v) { alive++; }
  ~TestAction() {
    alive--;
    destroyed++;
  }
  TestAction(const TestAction &) = delete;
  TestAction &operator=(const TestAction &) = delete;

  int value;
};

std::atomic<int> TestAction::alive{0};
std::atomic<int> TestAction::destroyed{0};

using ActionTable = ObjectTable<ActionId, TestAction>;

std::unique_ptr<const ActionTable> BuildTable(int value, size_t capacity = 16) {
  ObjectTableBuilder<ActionId, TestAction> builder(capacity);
  if (!builder.Emplace(ActionId(1), value)) {
    return nullptr;
  }
  return std::move(builder).Build();
}

class ObjectTablePublicationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    domain_ = &bess::control::runtime().rcu();
    TestAction::alive = 0;
    TestAction::destroyed = 0;
  }

  void TearDown() override {
    domain_->Drain();
    EXPECT_EQ(0u, domain_->registered_readers());
    EXPECT_EQ(0, TestAction::alive) << "an action was leaked";
  }

  bess::rcu::RcuDomain *domain_;
};

// K1's shared-grace-period capability, exercised the way K2 needs it: several
// tables published, one grace period retiring all of them.
TEST_F(ObjectTablePublicationTest, SharedGracePeriodCoversSeveralTables) {
  struct NextHopIdTag;
  using NextHopId = StrongId<NextHopIdTag, uint32_t>;
  struct NextHop {
    int port;
  };

  const uint32_t reader = 1;
  ASSERT_TRUE(domain_->Register(reader).has_value());
  domain_->Online(reader);

  RcuPtr<ActionTable> actions(*domain_);
  RcuPtr<ObjectTable<NextHopId, NextHop>> next_hops(*domain_);

  actions.Initialize(BuildTable(1));
  {
    ObjectTableBuilder<NextHopId, NextHop> builder(8);
    ASSERT_TRUE(builder.Emplace(NextHopId(1), NextHop{7}));
    next_hops.Initialize(std::move(builder).Build());
  }

  std::unique_ptr<const ActionTable> old_actions = actions.Exchange(BuildTable(2));
  std::unique_ptr<const ObjectTable<NextHopId, NextHop>> old_next_hops;
  {
    ObjectTableBuilder<NextHopId, NextHop> builder(8);
    ASSERT_TRUE(builder.Emplace(NextHopId(1), NextHop{9}));
    old_next_hops = next_hops.Exchange(std::move(builder).Build());
  }
  ASSERT_NE(nullptr, old_actions);
  ASSERT_NE(nullptr, old_next_hops);

  const GracePeriod token = domain_->StartGracePeriod();
  domain_->Retire(token, std::move(old_actions));
  domain_->Retire(token, std::move(old_next_hops));

  // One token, two tables: neither may be reclaimed while the reader is online.
  EXPECT_EQ(2u, domain_->Stats().pending_retired_objects);
  EXPECT_FALSE(domain_->IsComplete(token));
  EXPECT_EQ(0u, domain_->ReclaimReady());
  EXPECT_EQ(2, TestAction::alive)
      << "the retired generation's action and the new one are both alive";

  domain_->Quiescent(reader);
  EXPECT_TRUE(domain_->IsComplete(token));
  EXPECT_EQ(2u, domain_->ReclaimReady());
  EXPECT_EQ(0u, domain_->Stats().pending_retired_objects);

  // Both replacements are active.
  ASSERT_NE(nullptr, actions.Read()->Lookup(ActionId(1)));
  EXPECT_EQ(2, actions.Read()->Lookup(ActionId(1))->value);
  ASSERT_NE(nullptr, next_hops.Read()->Lookup(NextHopId(1)));
  EXPECT_EQ(9, next_hops.Read()->Lookup(NextHopId(1))->port);

  domain_->Offline(reader);
  domain_->Unregister(reader);
}

// A long run of generations through the substrate: nothing leaks, nothing is
// double-freed, and the active generation is never reclaimed.
TEST_F(ObjectTablePublicationTest, PublicationStormReclaimsEveryGeneration) {
  const uint32_t reader = 2;
  ASSERT_TRUE(domain_->Register(reader).has_value());
  domain_->Online(reader);

  RcuPtr<ActionTable> published(*domain_);
  published.Initialize(BuildTable(0));

  constexpr int kGenerations = 10000;
  std::atomic<bool> stop{false};
  std::atomic<int> reader_rounds{0};

  std::thread reader_thread([&]() {
    while (!stop.load()) {
      domain_->Quiescent(reader);
      reader_rounds++;
      const ActionTable *table = published.Read();
      EXPECT_NE(nullptr, table->Lookup(ActionId(1)));
    }
  });

  for (int i = 1; i <= kGenerations; i++) {
    published.Publish(BuildTable(i));
    domain_->ReclaimReady();
  }

  stop.store(true);
  reader_thread.join();
  EXPECT_GT(reader_rounds.load(), 0);

  domain_->Offline(reader);
  domain_->Synchronize();
  domain_->ReclaimReady();

  EXPECT_EQ(1, TestAction::alive) << "exactly the active generation remains";
  EXPECT_EQ(0u, domain_->Stats().pending_retired_objects);
  EXPECT_EQ(kGenerations, published.Read()->Lookup(ActionId(1))->value);
  EXPECT_EQ(kGenerations, TestAction::destroyed.load());

  domain_->Unregister(reader);
}

}  // namespace
