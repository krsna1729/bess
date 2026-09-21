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

// Worker integration for the RCU domain (K1.2): a worker registers when its
// thread starts, stays offline until dataplane execution resumes, goes offline
// before it blocks, and unregisters when it is done. This binary brings up a
// runtime the way the daemon does, because launching a worker needs a live EAL.

#include <gtest/gtest.h>

#include <cerrno>

#include "control/runtime_state.h"
#include "opts.h"
#include "packet_pool.h"
#include "port.h"
#include "rcu/rcu_domain.h"
#include "worker.h"

namespace {

constexpr size_t kPoolCapacity = 32767;

void InitRuntimeOnce() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;

  FLAGS_m = 0;  // malloc-backed, sandbox-safe: PlainPacketPool
  bess::PacketPool::CreateDefaultPools(kPoolCapacity);
  PortBuilder::InitDrivers();
}

class RcuWorkerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    InitRuntimeOnce();
    rcu_ = &bess::control::runtime().rcu();
  }

  void TearDown() override {
    // Leave no worker and no reader behind for the next test.
    destroy_all_workers();
    EXPECT_EQ(0u, rcu_->registered_readers());
  }

  bess::rcu::RcuDomain *rcu_;
};

TEST_F(RcuWorkerTest, WorkerRegistersOfflineAndFollowsPauseResume) {
  const int wid = 0;
  EXPECT_FALSE(rcu_->IsRegistered(wid));

  // Launching a worker returns only once the thread is up and paused, so the
  // registration below is already in place when this returns.
  launch_worker(wid, 0);
  EXPECT_TRUE(rcu_->IsRegistered(wid))
      << "a worker registers its reader slot when its thread starts";
  EXPECT_FALSE(rcu_->IsOnline(wid))
      << "a worker that merely exists as a thread is not an active reader";

  resume_worker(wid);
  EXPECT_TRUE(rcu_->IsOnline(wid))
      << "resuming brings the reader online before dataplane work";

  pause_worker(wid);
  EXPECT_FALSE(rcu_->IsOnline(wid))
      << "pausing takes the reader offline before it blocks";

  // A grace period must not wait for a paused worker.
  const bess::rcu::GracePeriod token = rcu_->StartGracePeriod();
  EXPECT_TRUE(rcu_->IsComplete(token));

  resume_worker(wid);
  EXPECT_TRUE(rcu_->IsOnline(wid));

  destroy_worker(wid);
  EXPECT_FALSE(rcu_->IsRegistered(wid))
      << "a destroyed worker gives its reader slot back";
}

// Destroying a worker must not leave anything behind that blocks grace periods,
// and the id has to be reusable.
TEST_F(RcuWorkerTest, WorkerIdCanBeRecreated) {
  const int wid = 3;

  launch_worker(wid, 0);
  resume_worker(wid);
  EXPECT_TRUE(rcu_->IsOnline(wid));

  const bess::rcu::GracePeriod held = rcu_->StartGracePeriod();
  EXPECT_FALSE(rcu_->IsComplete(held))
      << "an online worker holds the grace period until it reports quiescence";

  destroy_worker(wid);
  EXPECT_FALSE(rcu_->IsRegistered(wid));
  EXPECT_TRUE(rcu_->IsComplete(held))
      << "a destroyed worker must not block grace periods";

  // Recreating the same wid registers normally.
  launch_worker(wid, 0);
  EXPECT_TRUE(rcu_->IsRegistered(wid));
  EXPECT_FALSE(rcu_->IsOnline(wid));
  resume_worker(wid);
  EXPECT_TRUE(rcu_->IsOnline(wid));

  const bess::rcu::GracePeriod next = rcu_->StartGracePeriod();
  pause_worker(wid);
  EXPECT_TRUE(rcu_->IsComplete(next))
      << "the recreated worker is offline while paused";
}

// The pause-during-grace case: a worker that holds a grace period goes offline
// as part of pausing, so the writer is not stuck behind it.
TEST_F(RcuWorkerTest, PauseDuringGracePeriodReleasesIt) {
  const int wid = 1;

  launch_worker(wid, 0);
  resume_worker(wid);

  // The worker is online and has not reported quiescence since this token.
  const bess::rcu::GracePeriod token = rcu_->StartGracePeriod();
  EXPECT_FALSE(rcu_->IsComplete(token));

  // Pausing transitions it offline before it blocks.
  pause_worker(wid);
  EXPECT_TRUE(rcu_->IsComplete(token))
      << "pause must take the reader offline, or reclamation would depend on a "
         "thread that is not running";
}

}  // namespace
