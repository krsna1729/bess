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

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <memory>
#include <thread>
#include <vector>

#include "rcu/rcu_domain.h"

namespace {

using bess::rcu::GracePeriod;
using bess::rcu::RcuDomain;
using bess::rcu::ReaderId;

constexpr uint32_t kMaxReaders = 64;

// A retired object that records where it was destroyed, so the tests can prove
// reclamation happens on the reclaimer's thread and not before its grace
// period completes.
struct Tracked {
  static std::atomic<int> alive;
  static std::atomic<int> destroyed;
  static std::atomic<size_t> destroyed_on_thread;

  Tracked() { alive++; }
  ~Tracked() {
    alive--;
    destroyed++;
    destroyed_on_thread = std::hash<std::thread::id>()(std::this_thread::get_id());
  }
};

std::atomic<int> Tracked::alive{0};
std::atomic<int> Tracked::destroyed{0};
std::atomic<size_t> Tracked::destroyed_on_thread{0};

size_t CurrentThreadHash() {
  return std::hash<std::thread::id>()(std::this_thread::get_id());
}

class RcuDomainTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Tracked::alive = 0;
    Tracked::destroyed = 0;
    Tracked::destroyed_on_thread = 0;
    domain_ = std::make_unique<RcuDomain>(kMaxReaders);
  }

  void TearDown() override {
    domain_.reset();
    EXPECT_EQ(0, Tracked::alive) << "a retired object was leaked";
  }

  std::unique_ptr<RcuDomain> domain_;
};

TEST_F(RcuDomainTest, RegisterOnlineQuiescentOfflineUnregister) {
  const ReaderId id = 3;

  EXPECT_FALSE(domain_->IsRegistered(id));
  EXPECT_TRUE(domain_->Register(id).has_value());
  EXPECT_TRUE(domain_->IsRegistered(id));
  EXPECT_EQ(1u, domain_->registered_readers());
  EXPECT_FALSE(domain_->IsOnline(id)) << "a reader starts offline";

  // Registering twice is an error, and out-of-range ids are refused.
  auto again = domain_->Register(id);
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(EEXIST, again.error().err);
  EXPECT_FALSE(domain_->Register(kMaxReaders).has_value());

  domain_->Online(id);
  EXPECT_TRUE(domain_->IsOnline(id));
  domain_->Quiescent(id);
  domain_->Offline(id);
  EXPECT_FALSE(domain_->IsOnline(id));

  domain_->Unregister(id);
  EXPECT_FALSE(domain_->IsRegistered(id));
  EXPECT_EQ(0u, domain_->registered_readers());
}

// A domain with no online readers must not wait for anything.
TEST_F(RcuDomainTest, NoReaderGracePeriodCompletesImmediately) {
  const GracePeriod token = domain_->StartGracePeriod();
  EXPECT_TRUE(domain_->IsComplete(token));

  auto object = std::make_unique<Tracked>();
  domain_->Retire(token, std::move(object));
  EXPECT_EQ(1, Tracked::alive);
  EXPECT_EQ(1u, domain_->ReclaimReady());
  EXPECT_EQ(0, Tracked::alive);
}

// An offline reader does not participate: this is what keeps a paused worker
// from stalling reclamation.
TEST_F(RcuDomainTest, OfflineReaderDoesNotBlockGracePeriod) {
  const ReaderId id = 1;
  ASSERT_TRUE(domain_->Register(id).has_value());

  const GracePeriod token = domain_->StartGracePeriod();
  EXPECT_TRUE(domain_->IsComplete(token)) << "a registered but offline reader "
                                             "must not hold a grace period";
  domain_->Unregister(id);
}

// An online reader blocks the grace period until it reports quiescence.
TEST_F(RcuDomainTest, OnlineReaderBlocksUntilQuiescent) {
  const ReaderId id = 2;
  ASSERT_TRUE(domain_->Register(id).has_value());
  domain_->Online(id);

  const GracePeriod token = domain_->StartGracePeriod();
  EXPECT_FALSE(domain_->IsComplete(token))
      << "an online reader that has not reported quiescence must hold it";

  domain_->Quiescent(id);
  EXPECT_TRUE(domain_->IsComplete(token));

  domain_->Offline(id);
  domain_->Unregister(id);
}

TEST_F(RcuDomainTest, UnregisterDuringGracePeriodStopsBeingWaitedOn) {
  const ReaderId id = 4;
  ASSERT_TRUE(domain_->Register(id).has_value());
  domain_->Online(id);

  const GracePeriod token = domain_->StartGracePeriod();
  EXPECT_FALSE(domain_->IsComplete(token));

  // The reader goes away without ever reporting quiescence.
  domain_->Unregister(id);
  EXPECT_TRUE(domain_->IsComplete(token));
}

TEST_F(RcuDomainTest, AllOnlineReadersMustReport) {
  const ReaderId a = 0;
  const ReaderId b = 5;
  ASSERT_TRUE(domain_->Register(a).has_value());
  ASSERT_TRUE(domain_->Register(b).has_value());
  domain_->Online(a);
  domain_->Online(b);

  const GracePeriod token = domain_->StartGracePeriod();
  EXPECT_FALSE(domain_->IsComplete(token));

  domain_->Quiescent(a);
  EXPECT_FALSE(domain_->IsComplete(token)) << "b has not reported yet";

  domain_->Quiescent(b);
  EXPECT_TRUE(domain_->IsComplete(token));

  domain_->Offline(a);
  domain_->Offline(b);
  domain_->Unregister(a);
  domain_->Unregister(b);
}

TEST_F(RcuDomainTest, OverlappingGracePeriods) {
  const ReaderId id = 7;
  ASSERT_TRUE(domain_->Register(id).has_value());
  domain_->Online(id);

  const GracePeriod first = domain_->StartGracePeriod();
  domain_->Quiescent(id);
  EXPECT_TRUE(domain_->IsComplete(first));

  const GracePeriod second = domain_->StartGracePeriod();
  EXPECT_FALSE(domain_->IsComplete(second))
      << "a new grace period needs a fresh quiescent state";
  domain_->Quiescent(id);
  EXPECT_TRUE(domain_->IsComplete(second));

  domain_->Offline(id);
  domain_->Unregister(id);
}

// A worker that is destroyed and recreated must be able to reuse its id.
TEST_F(RcuDomainTest, ReaderIdCanBeReusedAfterUnregister) {
  const ReaderId id = 9;
  ASSERT_TRUE(domain_->Register(id).has_value());
  domain_->Online(id);
  domain_->Unregister(id);

  ASSERT_TRUE(domain_->Register(id).has_value());
  EXPECT_TRUE(domain_->IsRegistered(id));
  EXPECT_FALSE(domain_->IsOnline(id)) << "a fresh registration starts offline";

  domain_->Online(id);
  const GracePeriod token = domain_->StartGracePeriod();
  EXPECT_FALSE(domain_->IsComplete(token));
  domain_->Quiescent(id);
  EXPECT_TRUE(domain_->IsComplete(token));

  domain_->Offline(id);
  domain_->Unregister(id);
}

TEST_F(RcuDomainTest, RetiredObjectIsNotDestroyedEarly) {
  const ReaderId id = 11;
  ASSERT_TRUE(domain_->Register(id).has_value());
  domain_->Online(id);

  const GracePeriod token = domain_->StartGracePeriod();
  domain_->Retire(token, std::make_unique<Tracked>());
  EXPECT_EQ(1, Tracked::alive);

  // The reader has not reported quiescence: the object must stay alive even
  // though a reclaimer ran.
  EXPECT_EQ(0u, domain_->ReclaimReady());
  EXPECT_EQ(1, Tracked::alive);

  domain_->Quiescent(id);
  EXPECT_EQ(1u, domain_->ReclaimReady());
  EXPECT_EQ(0, Tracked::alive);
  EXPECT_EQ(1, Tracked::destroyed);

  domain_->Offline(id);
  domain_->Unregister(id);
}

// Destruction happens on the thread that reclaims, which is a control thread --
// never a packet worker that merely finished with the object.
TEST_F(RcuDomainTest, ReclaimerThreadPerformsDestruction) {
  const ReaderId id = 17;
  ASSERT_TRUE(domain_->Register(id).has_value());
  domain_->Online(id);

  const GracePeriod token = domain_->StartGracePeriod();
  domain_->Retire(token, std::make_unique<Tracked>());

  const size_t reclaimer = CurrentThreadHash();
  std::atomic<size_t> other_thread{0};

  std::thread worker([&]() {
    other_thread = CurrentThreadHash();
    // A packet worker must not be the one reclaiming: the grace period is
    // still held by the online reader.
    EXPECT_EQ(0u, domain_->ReclaimReady());
  });
  worker.join();
  ASSERT_NE(reclaimer, other_thread.load());
  EXPECT_EQ(1, Tracked::alive);

  // The reader reaches quiescence; the control thread reclaims.
  domain_->Quiescent(id);
  EXPECT_EQ(1u, domain_->ReclaimReady());
  EXPECT_EQ(reclaimer, Tracked::destroyed_on_thread.load())
      << "the reclaiming (control) thread must run the destructor";

  domain_->Offline(id);
  domain_->Unregister(id);
}

TEST_F(RcuDomainTest, DrainReclaimsEverything) {
  std::vector<GracePeriod> tokens;
  for (int i = 0; i < 8; i++) {
    tokens.push_back(domain_->StartGracePeriod());
    domain_->Retire(tokens.back(), std::make_unique<Tracked>());
  }
  EXPECT_EQ(8, Tracked::alive);

  domain_->Drain();
  EXPECT_EQ(0, Tracked::alive);
  EXPECT_EQ(8, Tracked::destroyed);
  EXPECT_EQ(0u, domain_->Stats().pending_retired_objects);
}

// One grace period retires several objects: the shape K2/K3 need when a
// transaction publishes several tables at once.
TEST_F(RcuDomainTest, OneGracePeriodRetiresSeveralObjects) {
  const ReaderId id = 13;
  ASSERT_TRUE(domain_->Register(id).has_value());
  domain_->Online(id);

  const GracePeriod token = domain_->StartGracePeriod();
  domain_->Retire(token, std::make_unique<Tracked>());
  domain_->Retire(token, std::make_unique<Tracked>());
  domain_->Retire(token, std::make_unique<Tracked>());
  EXPECT_EQ(3, Tracked::alive);
  EXPECT_EQ(3u, domain_->Stats().pending_retired_objects);

  domain_->Quiescent(id);
  EXPECT_EQ(3u, domain_->ReclaimReady());
  EXPECT_EQ(0, Tracked::alive);

  domain_->Offline(id);
  domain_->Unregister(id);
}

// A pathological update rate must not grow the queue without bound: the control
// side reclaims, and waits for outstanding grace periods if it has to.
TEST_F(RcuDomainTest, RetirementQueueIsBounded) {
  const ReaderId id = 15;
  RcuDomain bounded(kMaxReaders, /*retire_high_water=*/4);
  ASSERT_TRUE(bounded.Register(id).has_value());
  bounded.Online(id);

  // Every grace period completes only once the reader reports quiescence, which
  // happens here after each retirement.
  for (int i = 0; i < 32; i++) {
    const GracePeriod token = bounded.StartGracePeriod();
    bounded.Quiescent(id);
    bounded.Retire(token, std::make_unique<Tracked>());
  }

  EXPECT_LT(bounded.Stats().pending_retired_objects, 32u)
      << "the queue must not grow without bound";

  // Teardown: the reader goes offline, so Drain() has nothing to wait for.
  bounded.Offline(id);
  bounded.Drain();
  EXPECT_EQ(0, Tracked::alive);

  bounded.Unregister(id);
}

// Teardown invariant: a domain must not disappear while readers are registered.
TEST_F(RcuDomainTest, DestroyWithRegisteredReaderIsADebugFailure) {
  EXPECT_DEATH(
      {
        auto *domain = new RcuDomain(8);
        EXPECT_TRUE(domain->Register(1).has_value());
        delete domain;  // must trip the teardown assertion
      },
      "still registered");
}

}  // namespace
