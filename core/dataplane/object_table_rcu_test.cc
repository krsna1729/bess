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

// K2.3: ObjectTable published through K1's RcuPtr, proven the way K1 proved
// IPLookup -- a worker holding an old generation keeps it alive, the new
// generation is visible immediately after publication, and the retired
// generation is destroyed by the control thread after the worker reaches a
// quiescent state. No production module takes part: K2 has no consumer yet, and
// inventing one to make the phase look used would be worse than admitting that.
//
// This is the EAL-backed half of the proof (it needs a real worker). The parts
// that need no EAL -- shared grace period, publication storm -- live in
// `object_table_publication_test.cc`, which is what lets them run under
// ASan/UBSan: DPDK cannot initialise under ASan (IOVA exceeds the DMA mask), so
// anything that launches a worker is out of reach for the sanitizer lane.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "control/runtime_state.h"
#include "control/worker_manager.h"
#include "dataplane/action_id.h"
#include "dataplane/object_table.h"
#include "module.h"
#include "module_graph.h"
#include "opts.h"
#include "packet_pool.h"
#include "port.h"
#include "rcu/rcu_domain.h"
#include "rcu/rcu_ptr.h"
#include "worker.h"

namespace {

using bess::dataplane::ActionId;
using bess::dataplane::ObjectTable;
using bess::dataplane::ObjectTableBuilder;
using bess::rcu::GracePeriod;
using bess::rcu::RcuDomain;
using bess::rcu::RcuPtr;

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

// The immutable object behind an ActionId, with destruction tracking so the
// tests can prove when and where it happens.
struct TestAction {
  static std::atomic<int> alive;
  static std::atomic<int> destroyed;
  static std::atomic<size_t> destroyed_on_thread;

  explicit TestAction(int v) : value(v) { alive++; }
  ~TestAction() {
    alive--;
    destroyed++;
    destroyed_on_thread = std::hash<std::thread::id>()(std::this_thread::get_id());
  }
  TestAction(const TestAction &) = delete;
  TestAction &operator=(const TestAction &) = delete;

  int value;
};

std::atomic<int> TestAction::alive{0};
std::atomic<int> TestAction::destroyed{0};
std::atomic<size_t> TestAction::destroyed_on_thread{0};

size_t CurrentThreadHash() {
  return std::hash<std::thread::id>()(std::this_thread::get_id());
}

using ActionTable = ObjectTable<ActionId, TestAction>;

std::unique_ptr<const ActionTable> BuildTable(int value, size_t capacity = 16) {
  ObjectTableBuilder<ActionId, TestAction> builder(capacity);
  if (!builder.Emplace(ActionId(1), value)) {
    return nullptr;
  }
  return std::move(builder).Build();
}

// -- test-only task that holds a published generation ------------------------

std::unique_ptr<RcuPtr<ActionTable>> g_published;
std::atomic<bool> g_holding{false};
std::atomic<bool> g_release{false};
std::atomic<bool> g_done{false};
std::atomic<int> g_observed{0};
std::mutex g_hold_mutex;
std::condition_variable g_hold_cv;

class TableHoldModule final : public Module {
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
    if (g_done.load()) {
      return task_result{.block = false, .packets = 0, .bits = 0};
    }

    // Read the published table once for this invocation, hold it across a
    // publication, and resolve *after* the publication: the old generation must
    // still be valid and must still map the id to the old object.
    const ActionTable *table = g_published->Read();
    if (table == nullptr) {
      return task_result{.block = false, .packets = 0, .bits = 0};
    }

    g_holding.store(true);
    {
      std::unique_lock<std::mutex> lock(g_hold_mutex);
      g_hold_cv.wait(lock, [] { return g_release.load(); });
    }

    const TestAction *action = table->Lookup(ActionId(1));
    if (action != nullptr) {
      g_observed.store(action->value);
    }
    g_done.store(true);
    return task_result{.block = false, .packets = 0, .bits = 0};
  }
};

const Commands TableHoldModule::cmds = {};
ADD_MODULE(TableHoldModule, "table_hold", "holds a published table (K2 test)")

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

void ReleaseTask() {
  {
    std::lock_guard<std::mutex> lock(g_hold_mutex);
    g_release.store(true);
  }
  g_hold_cv.notify_all();
}

class ObjectTableRcuTest : public ::testing::Test {
 protected:
  void SetUp() override {
    InitRuntimeOnce();
    domain_ = &bess::control::runtime().rcu();
    TestAction::alive = 0;
    TestAction::destroyed = 0;
    TestAction::destroyed_on_thread = 0;
    g_holding.store(false);
    g_release.store(false);
    g_done.store(false);
    g_observed.store(0);
    g_published = std::make_unique<RcuPtr<ActionTable>>(*domain_);
  }

  void TearDown() override {
    ReleaseTask();
    WaitFor([] { return g_done.load() || !g_holding.load(); },
            std::chrono::milliseconds(2000));

    if (bess::control::runtime().workers().num_workers() > 0) {
      // The daemon's order: modules (which own tasks) under a pause, then
      // workers.
      {
        WorkerPauser pauser;
        ModuleGraph::DestroyAllModules();
      }
      destroy_all_workers();
    }

    g_published.reset();
    domain_->Drain();
    EXPECT_EQ(0u, domain_->registered_readers());
    EXPECT_EQ(0, TestAction::alive) << "an action was leaked";
  }

  bess::rcu::RcuDomain *domain_;
};

// The central K2 proof: a worker holding an old generation still resolves the
// old object after the control plane has published a replacement, and the old
// generation is destroyed only once the worker reaches a quiescent state --
// on the control thread.
TEST_F(ObjectTableRcuTest, WorkerHoldingAnOldGenerationKeepsItAlive) {
  const int wid = 0;

  g_published->Initialize(BuildTable(/*value=*/1));

  launch_worker(wid, 0);

  bess::pb::EmptyArg arg;
  google::protobuf::Any packed;
  packed.PackFrom(arg);
  const auto &builders = ModuleBuilder::all_module_builders();
  const auto it = builders.find("TableHoldModule");
  ASSERT_NE(it, builders.end());

  pb_error_t error;
  ASSERT_NE(nullptr, ModuleGraph::CreateModule(it->second, "holder", packed,
                                                &error))
      << error.errmsg();

  attach_orphans();
  resume_worker(wid);

  ASSERT_TRUE(WaitFor([] { return g_holding.load(); },
                      std::chrono::milliseconds(2000)))
      << "the task never picked up the published table";

  // The worker is now holding generation 1 and is blocked inside its task.
  const size_t control_thread = CurrentThreadHash();
  std::atomic<size_t> worker_thread{0};
  {
    std::thread probe([&] { worker_thread = CurrentThreadHash(); });
    probe.join();
  }
  ASSERT_NE(control_thread, worker_thread.load());

  // Publish a replacement: same id, different object.
  const GracePeriod token = g_published->Publish(BuildTable(/*value=*/2));

  // A new reader sees the new generation immediately.
  const ActionTable *active = g_published->Read();
  ASSERT_NE(nullptr, active);
  const TestAction *fresh = active->Lookup(ActionId(1));
  ASSERT_NE(nullptr, fresh);
  EXPECT_EQ(2, fresh->value) << "publication is visible right away";

  // The old generation is still alive, and the worker holding it is not
  // affected by the publication.
  EXPECT_EQ(2, TestAction::alive);
  EXPECT_FALSE(domain_->IsComplete(token));
  EXPECT_EQ(0u, domain_->ReclaimReady());

  // Let the task finish: it resolves the id through the *old* generation.
  ReleaseTask();
  ASSERT_TRUE(WaitFor([] { return g_done.load(); },
                      std::chrono::milliseconds(2000)));
  EXPECT_EQ(1, g_observed.load())
      << "a worker holding an old generation must keep seeing the old object";

  // The worker reaches its next quiescent state, and only then may the retired
  // generation go away -- on the control thread.
  ASSERT_TRUE(WaitFor([&] { return domain_->IsComplete(token); },
                      std::chrono::milliseconds(2000)));
  EXPECT_EQ(1u, domain_->ReclaimReady());
  EXPECT_EQ(1, TestAction::alive) << "only the active generation remains";
  EXPECT_EQ(control_thread, TestAction::destroyed_on_thread.load())
      << "a packet worker must never destroy a retired generation";
}

}  // namespace
