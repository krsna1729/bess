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
#include "rcu/rcu_ptr.h"

namespace {

using bess::rcu::GracePeriod;
using bess::rcu::RcuDomain;
using bess::rcu::RcuPtr;
using bess::rcu::ReaderId;

constexpr uint32_t kMaxReaders = 16;

struct State {
  static std::atomic<int> alive;

  explicit State(int v) : value(v) { alive++; }
  ~State() { alive--; }

  const int value;
};

std::atomic<int> State::alive{0};

class RcuPtrTest : public ::testing::Test {
 protected:
  void SetUp() override {
    State::alive = 0;
    domain_ = std::make_unique<RcuDomain>(kMaxReaders);
    published_ = std::make_unique<RcuPtr<State>>(*domain_);
  }

  void TearDown() override {
    published_.reset();
    domain_.reset();
    EXPECT_EQ(0, State::alive) << "a published object was leaked";
  }

  std::unique_ptr<RcuDomain> domain_;
  std::unique_ptr<RcuPtr<State>> published_;
};

TEST_F(RcuPtrTest, InitializeThenRead) {
  published_->Initialize(std::make_unique<const State>(7));
  const State *state = published_->Read();
  ASSERT_NE(nullptr, state);
  EXPECT_EQ(7, state->value);
  EXPECT_EQ(1, State::alive);
}

// Replacing a published object leaves the old one alive until the grace period
// that retires it completes, and then it is reclaimed.
TEST_F(RcuPtrTest, PublishDefersReclamationUntilTheGracePeriodCompletes) {
  const ReaderId id = 1;
  ASSERT_TRUE(domain_->Register(id).has_value());
  domain_->Online(id);

  published_->Initialize(std::make_unique<const State>(1));

  const GracePeriod token =
      published_->Publish(std::make_unique<const State>(2));

  // Readers see the replacement immediately.
  ASSERT_NE(nullptr, published_->Read());
  EXPECT_EQ(2, published_->Read()->value);

  // The old object is still alive: the online reader has not reported
  // quiescence since the publication.
  EXPECT_EQ(2, State::alive);
  EXPECT_FALSE(domain_->IsComplete(token));
  EXPECT_EQ(0u, domain_->ReclaimReady());

  domain_->Quiescent(id);
  EXPECT_TRUE(domain_->IsComplete(token));
  EXPECT_EQ(1u, domain_->ReclaimReady());
  EXPECT_EQ(1, State::alive);

  domain_->Offline(id);
  domain_->Unregister(id);
}

// Publishing repeatedly while a reader never reports quiescence must not
// destroy anything early, and everything must be reclaimed once it does.
TEST_F(RcuPtrTest, RepeatedPublishesAccumulateUntilReadersReport) {
  const ReaderId id = 2;
  ASSERT_TRUE(domain_->Register(id).has_value());
  domain_->Online(id);

  published_->Initialize(std::make_unique<const State>(0));

  constexpr int kGenerations = 32;
  for (int i = 1; i <= kGenerations; i++) {
    published_->Publish(std::make_unique<const State>(i));
  }

  // Every generation ever published is still alive: kGenerations retired ones
  // (the initial generation was retired by the first publish) plus the active.
  EXPECT_EQ(kGenerations + 1, State::alive);
  EXPECT_EQ(static_cast<size_t>(kGenerations),
            domain_->Stats().pending_retired_objects);
  EXPECT_EQ(0u, domain_->ReclaimReady()) << "the reader is still online";
  EXPECT_EQ(kGenerations + 1, State::alive);

  domain_->Quiescent(id);
  domain_->ReclaimReady();
  EXPECT_EQ(1, State::alive) << "only the active generation may remain";
  EXPECT_EQ(0u, domain_->Stats().pending_retired_objects);

  domain_->Offline(id);
  domain_->Unregister(id);
}

// One grace period can retire several published objects: the shape a
// transaction needs when it swaps several tables at once.
TEST_F(RcuPtrTest, OneGracePeriodRetiresSeveralPublishedObjects) {
  const ReaderId id = 3;
  ASSERT_TRUE(domain_->Register(id).has_value());
  domain_->Online(id);

  RcuPtr<State> a(*domain_);
  RcuPtr<State> b(*domain_);
  a.Initialize(std::make_unique<const State>(10));
  b.Initialize(std::make_unique<const State>(20));

  std::unique_ptr<const State> old_a =
      a.Exchange(std::make_unique<const State>(11));
  std::unique_ptr<const State> old_b =
      b.Exchange(std::make_unique<const State>(21));

  const GracePeriod token = domain_->StartGracePeriod();
  domain_->Retire(token, std::move(old_a));
  domain_->Retire(token, std::move(old_b));

  EXPECT_EQ(4, State::alive);
  EXPECT_EQ(2u, domain_->Stats().pending_retired_objects);
  EXPECT_EQ(0u, domain_->ReclaimReady());

  domain_->Quiescent(id);
  EXPECT_EQ(2u, domain_->ReclaimReady());
  EXPECT_EQ(2, State::alive) << "the two active objects remain";

  domain_->Offline(id);
  domain_->Unregister(id);
}

// A quiesced teardown drops the active object without waiting for a grace
// period, and hands it to the caller.
TEST_F(RcuPtrTest, ResetQuiescedReturnsTheActiveObject) {
  published_->Initialize(std::make_unique<const State>(5));

  std::unique_ptr<const State> dropped = published_->ResetQuiesced();
  ASSERT_NE(nullptr, dropped);
  EXPECT_EQ(5, dropped->value);
  EXPECT_EQ(nullptr, published_->Read());
  EXPECT_EQ(1, State::alive);

  dropped.reset();
  EXPECT_EQ(0, State::alive);
}

// Writers are serialized: concurrent publishers must not lose or leak an
// object, and exactly one object is active at the end.
TEST_F(RcuPtrTest, ConcurrentWritersAreSerialized) {
  const ReaderId id = 4;
  ASSERT_TRUE(domain_->Register(id).has_value());
  domain_->Online(id);

  published_->Initialize(std::make_unique<const State>(0));

  constexpr int kThreads = 4;
  constexpr int kPublishesPerThread = 25;
  std::vector<std::thread> writers;
  for (int t = 0; t < kThreads; t++) {
    writers.emplace_back([&, t]() {
      for (int i = 0; i < kPublishesPerThread; i++) {
        published_->Publish(std::make_unique<const State>(t * 100 + i));
        // A reader keeps running while this happens.
        EXPECT_NE(nullptr, published_->Read());
      }
    });
  }
  for (std::thread &writer : writers) {
    writer.join();
  }

  const int retired = kThreads * kPublishesPerThread;
  EXPECT_EQ(retired + 1, State::alive)
      << "every generation is either active or queued, none lost";
  EXPECT_EQ(static_cast<size_t>(retired),
            domain_->Stats().pending_retired_objects);

  domain_->Quiescent(id);
  domain_->ReclaimReady();
  EXPECT_EQ(1, State::alive);
  EXPECT_EQ(0u, domain_->Stats().pending_retired_objects);

  domain_->Offline(id);
  domain_->Unregister(id);
}

// Teardown invariant: an RcuPtr must not be destroyed while a reader could
// still be holding what it published.
TEST_F(RcuPtrTest, DestroyWithOnlineReaderIsADebugFailure) {
  EXPECT_DEATH(
      {
        RcuDomain domain(kMaxReaders);
        RcuPtr<State> published(domain);
        published.Initialize(std::make_unique<const State>(1));
        EXPECT_TRUE(domain.Register(1).has_value());
        domain.Online(1);
        // Destroying `published` here would free an object a reader may hold.
      },
      "are online");
}

}  // namespace
