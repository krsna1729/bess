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

// Scheduler-boundary RCU tests (K1.3): quiescence is a worker execution-state
// property. A worker is quiescent once a task invocation has returned to the
// scheduler and before the next one begins -- so a task that holds a published
// pointer must keep it alive, and reclamation may only happen after that task
// returns. An idle worker must still reach the boundary, or reclamation would
// stall whenever there is no traffic.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "control/runtime_state.h"
#include "module.h"
#include "module_graph.h"
#include "opts.h"
#include "packet_pool.h"
#include "port.h"
#include "rcu/rcu_domain.h"
#include "worker.h"

namespace {

using bess::rcu::GracePeriod;

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

std::atomic<int> g_payloads_alive{0};

// The published payload the test task holds across a replacement.
struct Payload {
  int value;
  ~Payload() { g_payloads_alive--; }
};

std::atomic<const Payload *> g_published{nullptr};
std::atomic<bool> g_holding{false};
std::atomic<bool> g_release{false};
std::atomic<int> g_held_value{0};
std::atomic<bool> g_done{false};
std::mutex g_hold_mutex;
std::condition_variable g_hold_cv;

// A module whose single task reads the published pointer once per invocation
// and then blocks until the test releases it: exactly the window in which a
// replacement must not destroy what the task is holding.
class RcuHoldModule final : public Module {
 public:
  static const gate_idx_t kNumIGates = 0;
  static const gate_idx_t kNumOGates = 0;
  static const Commands cmds;

  CommandResponse Init(const bess::pb::EmptyArg &) {
    if (RegisterTask(nullptr) == INVALID_TASK_ID) {
      return CommandFailure(ENOMEM, "task registration failed");
    }
    return CommandSuccess();
  }

  struct task_result RunTask(Context *, bess::PacketBatch *,
                             void *) override {
    // One shot: the first invocation that sees a published pointer holds it
    // until the test releases it. Later invocations must not re-read, or the
    // test could not tell which generation was actually held.
    const Payload *payload = g_published.load(std::memory_order_acquire);
    if (payload != nullptr && !g_done.load()) {
      g_holding.store(true);
      {
        std::unique_lock<std::mutex> lock(g_hold_mutex);
        g_hold_cv.wait(lock, [] { return g_release.load(); });
      }
      // Touched while still inside the invocation that acquired it: the
      // pointer is valid until this task returns and the worker reaches its
      // next quiescent state.
      g_held_value.store(payload->value);
      g_done.store(true);
    }
    return task_result{.block = false, .packets = 0, .bits = 0};
  }
};

const Commands RcuHoldModule::cmds = {};
ADD_MODULE(RcuHoldModule, "rcu_hold", "holds a published pointer (K1 test)")

// Waits for `predicate` for up to `timeout`, so a test fails on a broken
// invariant instead of hanging the suite.
template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

class RcuSchedulerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    InitRuntimeOnce();
    rcu_ = &bess::control::runtime().rcu();
    g_published.store(nullptr);
    g_holding.store(false);
    g_release.store(false);
    g_held_value.store(0);
    g_done.store(false);
  }

  void TearDown() override {
    {
      std::lock_guard<std::mutex> lock(g_hold_mutex);
      g_release.store(true);
    }
    g_hold_cv.notify_all();

    // Let the task return before anything destroys it: the module owns the
    // task, and the worker is inside it while it blocks.
    WaitFor([] { return g_done.load() || !g_holding.load(); },
            std::chrono::milliseconds(2000));

    // The daemon's order, and it matters: modules (which own tasks) are
    // destroyed under a pause *before* workers, because destroying a worker
    // deletes the scheduler tree -- including the leaf classes that own those
    // tasks. Doing it the other way round leaves Module::tasks_ dangling.
    {
      WorkerPauser pauser;
      ModuleGraph::DestroyAllModules();
    }
    destroy_all_workers();

    rcu_->Drain();
    EXPECT_EQ(0u, rcu_->registered_readers());
  }

  bess::rcu::RcuDomain *rcu_;
};

// A task that is holding a published pointer keeps it alive; the worker only
// reports quiescence once that task returns.
TEST_F(RcuSchedulerTest, TaskHoldingAPointerKeepsItAliveUntilItReturns) {
  const int wid = 0;
  launch_worker(wid, 0);

  bess::pb::EmptyArg arg;
  google::protobuf::Any packed;
  EXPECT_TRUE(packed.PackFrom(arg));

  const auto &builders = ModuleBuilder::all_module_builders();
  const auto it = builders.find("RcuHoldModule");
  ASSERT_NE(it, builders.end()) << "the test module was not registered";

  pb_error_t error;
  Module *module = ModuleGraph::CreateModule(it->second, "holder", packed,
                                             &error);
  ASSERT_NE(nullptr, module) << "could not create the test module: "
                             << error.errmsg();

  // Attach the module's task to the worker and start it.
  attach_orphans();
  resume_worker(wid);

  auto *first = new Payload{1};
  g_payloads_alive.fetch_add(1);
  g_published.store(first, std::memory_order_release);

  ASSERT_TRUE(WaitFor([] { return g_holding.load(); },
                      std::chrono::milliseconds(2000)))
      << "the task never picked up the published pointer";

  // Publish a replacement and retire the old object against a grace period
  // started *after* the publication.
  auto *second = new Payload{2};
  g_payloads_alive.fetch_add(1);
  g_published.store(second, std::memory_order_release);

  const GracePeriod token = rcu_->StartGracePeriod();
  rcu_->Retire(token, std::unique_ptr<Payload>(first));

  EXPECT_FALSE(rcu_->IsComplete(token))
      << "the task is still holding the old pointer: the worker has not "
         "reached a quiescent state";
  EXPECT_EQ(0u, rcu_->ReclaimReady());
  EXPECT_EQ(2, g_payloads_alive.load())
      << "the old object must not be destroyed while the task holds it";

  // Let the task return: the worker reaches the scheduler boundary, reports
  // quiescence, and only then may the old object go away.
  // The flag is set under the mutex: changing a wait predicate outside it is
  // how wakeups get lost, and the task would then sleep past its release.
  {
    std::lock_guard<std::mutex> lock(g_hold_mutex);
    g_release.store(true);
  }
  g_hold_cv.notify_all();

  EXPECT_TRUE(WaitFor([&] { return rcu_->IsComplete(token); },
                      std::chrono::milliseconds(2000)))
      << "the worker never reported quiescence after its task returned";
  EXPECT_EQ(1u, rcu_->ReclaimReady());
  EXPECT_EQ(1, g_payloads_alive.load());
  EXPECT_EQ(1, g_held_value.load()) << "the task must have used the pointer";

  // Clean up: drop the module (and its task) and the second payload.
  g_published.store(nullptr, std::memory_order_release);
  const GracePeriod cleanup = rcu_->StartGracePeriod();
  rcu_->Retire(cleanup, std::unique_ptr<Payload>(second));
  rcu_->Synchronize();
  rcu_->ReclaimReady();
  EXPECT_EQ(0, g_payloads_alive.load());
}

// An online worker with nothing to do must still reach the scheduler boundary
// and report quiescence, or reclamation would stall whenever traffic stops.
TEST_F(RcuSchedulerTest, IdleWorkerStillAdvancesGracePeriods) {
  const int wid = 0;
  launch_worker(wid, 0);
  resume_worker(wid);

  // The worker reported quiescence on resume; this token needs a fresh one.
  const GracePeriod token = rcu_->StartGracePeriod();
  EXPECT_TRUE(WaitFor([&] { return rcu_->IsComplete(token); },
                      std::chrono::milliseconds(2000)))
      << "an idle worker must still report quiescence";
}

}  // namespace
