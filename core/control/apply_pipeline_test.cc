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

// End-to-end exercise of the transactional control plane from C++ (G0 commit
// 6): a complete multi-object pipeline -- worker, modules, connections and a
// traffic-class hierarchy -- is applied, re-applied (no-op), failed at commit
// (the active pipeline must survive) and then changed again.
//
// Unlike the other control tests, this binary brings up a runtime the way the
// daemon does (DPDK EAL with --no-huge, packet pools, port drivers), because
// launching a worker needs a live EAL.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "control/control_plane.h"
#include "control/pipeline_spec.h"
#include "control/runtime_state.h"
#include "control/transaction.h"
#include "control/worker_manager.h"
#include "pb/module_msg.pb.h"
#include "pb/port_msg.pb.h"
#include "opts.h"
#include "packet_pool.h"
#include "port.h"
#include "worker.h"

namespace {

using bess::control::ApplyOptions;
using bess::control::ApplyResult;
using bess::control::ControlPlane;
using bess::control::PipelineSnapshot;
using bess::control::PipelineSpec;
using bess::control::SpecFromSnapshot;

const size_t kPoolCapacity = 32767;

// One runtime for the whole binary: EAL cannot be initialized twice, and the
// daemon-equivalent setup is the point of this test.
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

// A pipeline with everything the structural path can do: a worker, two
// modules, a connection between them, and a traffic-class hierarchy whose root
// lands on that worker.
PipelineSpec FullPipeline() {
  PipelineSpec spec;

  bess::control::WorkerSpec worker;
  worker.wid = 0;
  worker.core = 0;
  spec.workers.push_back(worker);

  bess::control::ModuleSpec src;
  src.name = "src";
  src.mclass = "Bypass";
  spec.modules.push_back(src);

  bess::control::ModuleSpec sink;
  sink.name = "sink";
  sink.mclass = "Bypass";
  spec.modules.push_back(sink);

  bess::control::ConnectionSpec connection;
  connection.upstream = "src";
  connection.ogate = 0;
  connection.downstream = "sink";
  connection.igate = 0;
  spec.connections.push_back(connection);

  bess::control::TrafficClassSpec root;
  root.name = "root";
  root.policy = "round_robin";
  root.wid = 0;
  spec.traffic_classes.push_back(root);

  bess::control::TrafficClassSpec child;
  child.name = "child";
  child.policy = "round_robin";
  child.parent = "root";
  spec.traffic_classes.push_back(child);

  return spec;
}

class ApplyPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    InitRuntimeOnce();
    control_plane_ = std::make_unique<ControlPlane>();
  }

  void TearDown() override {
    // Leave the shared runtime empty for the next test.
    (void)control_plane_->Reset();
  }

  std::unique_ptr<ControlPlane> control_plane_;
};

TEST_F(ApplyPipelineTest, AppliesACompletePipeline) {
  const uint64_t generation_before = bess::control::runtime().generation();

  auto applied = control_plane_->ApplyPipeline(FullPipeline(), {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  EXPECT_EQ(generation_before + 1, applied->generation);
  EXPECT_TRUE(applied->workers_paused);

  const PipelineSnapshot snapshot = control_plane_->GetPipeline();
  ASSERT_EQ(1u, snapshot.workers.size());
  EXPECT_EQ(0, snapshot.workers[0].wid);
  ASSERT_EQ(2u, snapshot.modules.size());
  EXPECT_EQ("sink", snapshot.modules[0].name);
  EXPECT_EQ("src", snapshot.modules[1].name);
  ASSERT_EQ(1u, snapshot.connections.size());
  EXPECT_EQ("src", snapshot.connections[0].upstream);
  EXPECT_EQ("sink", snapshot.connections[0].downstream);

  // The user-visible traffic classes are there, and the internal ones are not
  // part of desired state.
  size_t user_tcs = 0;
  for (const auto &tc : snapshot.traffic_classes) {
    if (!tc.name.empty() && tc.name[0] != '!') {
      user_tcs++;
    }
  }
  EXPECT_EQ(2u, user_tcs);

  // Applying the same desired state again changes nothing.
  auto again = control_plane_->ApplyPipeline(FullPipeline(), {});
  ASSERT_TRUE(again.has_value()) << again.error().message;
  EXPECT_EQ(generation_before + 1, again->generation);
  EXPECT_EQ(0u, again->applied_ops);
  EXPECT_FALSE(again->workers_paused);
}

TEST_F(ApplyPipelineTest, SnapshotOfTheActiveRuntimeIsTheDesiredState) {
  ASSERT_TRUE(control_plane_->ApplyPipeline(FullPipeline(), {}).has_value());

  const PipelineSpec running =
      SpecFromSnapshot(control_plane_->GetPipeline());
  auto diff = control_plane_->DiffPipeline(running);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  EXPECT_TRUE(diff->empty())
      << "the running pipeline must diff clean against its own description";
}

// The acceptance test that matters most: a transaction that fails while
// committing must leave the previous pipeline active and usable.
TEST_F(ApplyPipelineTest, FailedCommitKeepsTheActivePipeline) {
  ASSERT_TRUE(control_plane_->ApplyPipeline(FullPipeline(), {}).has_value());

  const PipelineSnapshot active = control_plane_->GetPipeline();
  const uint64_t generation = bess::control::runtime().generation();
  const size_t modules = bess::control::runtime().modules().Size();
  const size_t tcs = bess::control::runtime().traffic_classes().Size();

  // Add a traffic-class hierarchy whose second half cannot attach: a child of a
  // priority class without a priority. The parent is created first, so the
  // commit fails with the parent already in place.
  PipelineSpec broken = FullPipeline();
  bess::control::TrafficClassSpec parent;
  parent.name = "zprio";
  parent.policy = "priority";
  parent.wid = 0;
  broken.traffic_classes.push_back(parent);

  bess::control::TrafficClassSpec child;
  child.name = "achild";
  child.policy = "round_robin";
  child.parent = "zprio";  // no priority: rejected at attach time
  broken.traffic_classes.push_back(child);

  auto applied = control_plane_->ApplyPipeline(broken, {});
  ASSERT_FALSE(applied.has_value());

  EXPECT_EQ(generation, bess::control::runtime().generation());
  EXPECT_EQ(modules, bess::control::runtime().modules().Size());
  EXPECT_EQ(tcs, bess::control::runtime().traffic_classes().Size());
  EXPECT_TRUE(control_plane_->GetPipeline() == active)
      << "the previous pipeline must still be the active one";

  // ... and it is still usable: the same desired state still diffs clean.
  auto diff = control_plane_->DiffPipeline(FullPipeline());
  ASSERT_TRUE(diff.has_value());
  EXPECT_TRUE(diff->empty());
}

// Removal goes through the retire phase, and the runtime ends up consistent.
TEST_F(ApplyPipelineTest, RemovesModulesAndConnectionsAgain) {
  ASSERT_TRUE(control_plane_->ApplyPipeline(FullPipeline(), {}).has_value());
  const uint64_t after_apply = bess::control::runtime().generation();

  PipelineSpec reduced = FullPipeline();
  reduced.connections.clear();
  reduced.modules.pop_back();  // drop "sink"
  reduced.traffic_classes.pop_back();  // drop "child"

  auto applied = control_plane_->ApplyPipeline(reduced, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  EXPECT_EQ(after_apply + 1, applied->generation);
  EXPECT_TRUE(applied->workers_paused);

  const PipelineSnapshot snapshot = control_plane_->GetPipeline();
  ASSERT_EQ(1u, snapshot.modules.size());
  EXPECT_EQ("src", snapshot.modules[0].name);
  EXPECT_TRUE(snapshot.connections.empty());
}


// ---------------------------------------------------------------------------
// Failure injection: every injected failure must leave no trace
// ---------------------------------------------------------------------------

namespace {

// Everything a transaction must leave untouched when it fails.
struct Fingerprint {
  uint64_t generation = 0;
  size_t ports = 0;
  size_t modules = 0;
  size_t traffic_classes = 0;
  size_t workers = 0;
  size_t orphan_tcs = 0;
  PipelineSnapshot snapshot;
  std::vector<size_t> queue_users;  // one entry per port queue, must all be 0

  bool operator==(const Fingerprint &other) const {
    return generation == other.generation && ports == other.ports &&
           modules == other.modules &&
           traffic_classes == other.traffic_classes &&
           workers == other.workers && orphan_tcs == other.orphan_tcs &&
           snapshot == other.snapshot && queue_users == other.queue_users;
  }
};

Fingerprint Capture() {
  Fingerprint fingerprint;
  const bess::control::RuntimeState &state = bess::control::runtime();

  fingerprint.generation = state.generation();
  fingerprint.ports = state.ports().Size();
  fingerprint.modules = state.modules().Size();
  fingerprint.traffic_classes = state.traffic_classes().Size();
  fingerprint.workers = static_cast<size_t>(state.workers().num_workers());
  fingerprint.orphan_tcs = state.workers().orphan_tcs().size();
  fingerprint.snapshot = bess::control::SnapshotRuntime(state);

  for (const auto &pair : state.ports().All()) {
    const Port *port = pair.second.get();
    for (packet_dir_t dir : {PACKET_DIR_INC, PACKET_DIR_OUT}) {
      for (queue_t qid = 0; qid < port->num_queues[dir]; qid++) {
        fingerprint.queue_users.push_back(port->users[dir][qid] != nullptr);
      }
    }
  }

  return fingerprint;
}

}  // namespace

// Every operation of a real plan is injected in turn: each failure must leave
// the runtime exactly as it was, with no leaked module, port, traffic class,
// worker, orphan task or acquired queue.
TEST_F(ApplyPipelineTest, InjectedFailuresLeaveNoTrace) {
  PipelineSpec desired = FullPipeline();

  bess::control::ModuleSpec extra;
  extra.name = "extra";
  extra.mclass = "Bypass";
  desired.modules.push_back(extra);

  // Apply it once, so failures happen against a populated, active runtime.
  ASSERT_TRUE(control_plane_->ApplyPipeline(desired, {}).has_value());
  const Fingerprint active = Capture();
  ASSERT_GT(active.modules, 0u);
  ASSERT_GT(active.traffic_classes, 0u);

  // Now change it in a way that has to create a module, connect it and attach a
  // traffic class -- and fail at each operation of that plan in turn.
  PipelineSpec changed = desired;
  bess::control::ModuleSpec added;
  added.name = "added";
  added.mclass = "Bypass";
  changed.modules.push_back(added);

  bess::control::ConnectionSpec edge;
  edge.upstream = "added";
  edge.ogate = 0;
  edge.downstream = "src";
  edge.igate = 0;
  changed.connections.push_back(edge);

  bess::control::TrafficClassSpec tc;
  tc.name = "added_tc";
  tc.policy = "round_robin";
  tc.wid = 0;
  changed.traffic_classes.push_back(tc);

  auto plan = control_plane_->PlanPipeline(changed);
  ASSERT_TRUE(plan.has_value()) << plan.error().message;

  struct Step {
    bess::control::TransactionPhase phase;
    size_t index;
    const char *phase_name;
  };
  std::vector<Step> steps;
  for (size_t i = 0; i < plan->prepare_ops.size(); i++) {
    steps.push_back({bess::control::TransactionPhase::kPrepare, i, "prepare"});
  }
  for (size_t i = 0; i < plan->commit_ops.size(); i++) {
    steps.push_back({bess::control::TransactionPhase::kCommit, i, "commit"});
  }
  ASSERT_GE(steps.size(), 3u)
      << "the plan should have several operations to inject into";

  for (const Step &step : steps) {
    size_t seen = 0;
    bess::control::SetFailureInjector(
        [&step, &seen](bess::control::TransactionPhase phase,
                       const bess::control::PlanOperation &) {
          if (phase == step.phase && seen++ == step.index) {
            return std::optional<bess::control::ControlError>(
                bess::control::Err(EIO, "injected failure"));
          }
          return std::optional<bess::control::ControlError>();
        });

    auto applied = control_plane_->ApplyPipeline(changed, {});
    bess::control::ClearFailureInjector();

    EXPECT_FALSE(applied.has_value())
        << "injection at " << step.phase_name << " op " << step.index
        << " did not fail the apply";
    EXPECT_TRUE(Capture() == active)
        << "injection at " << step.phase_name << " op " << step.index
        << " left a trace";
  }

  // The active pipeline is still usable afterwards.
  auto diff = control_plane_->DiffPipeline(desired);
  ASSERT_TRUE(diff.has_value());
  EXPECT_TRUE(diff->empty());
}

// Retirement is not undoable, so its preconditions are proven before the
// commit; if a step still fails, the caller is told -- an ordinary successful
// apply must never claim a pipeline that does not exist.
TEST_F(ApplyPipelineTest, InjectedRetireFailureIsReportedAsFailure) {
  ASSERT_TRUE(control_plane_->ApplyPipeline(FullPipeline(), {}).has_value());
  const uint64_t generation = bess::control::runtime().generation();

  PipelineSpec reduced = FullPipeline();
  reduced.connections.clear();  // the edge into "sink" goes away with it
  reduced.modules.pop_back();   // "sink" has to be retired

  bess::control::SetFailureInjector(
      [](bess::control::TransactionPhase phase,
         const bess::control::PlanOperation &) {
        if (phase != bess::control::TransactionPhase::kRetire) {
          return std::optional<bess::control::ControlError>();
        }
        return std::optional<bess::control::ControlError>(
            bess::control::Err(EIO, "injected retire failure"));
      });
  auto applied = control_plane_->ApplyPipeline(reduced, {});
  bess::control::ClearFailureInjector();

  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(bess::control::ControlErrorCode::kResourceFailure,
            applied.error().code);
  EXPECT_NE(std::string::npos,
            applied.error().message.find("retirement failed"));

  // The new state *is* active -- the commit happened -- so the generation says
  // so, and the module that could not be retired is still there. The caller
  // knows, which is the difference from silently succeeding.
  EXPECT_EQ(generation + 1, bess::control::runtime().generation());
  EXPECT_TRUE(bess::control::runtime().modules().Contains("sink"));
}

// The invariant: a successful apply leaves exactly the desired state.
TEST_F(ApplyPipelineTest, SuccessfulApplyMatchesDesiredState) {
  auto expect_desired = [this](const PipelineSpec &desired) {
    const PipelineSpec running =
        SpecFromSnapshot(control_plane_->GetPipeline());
    PipelineSpec expected = desired;
    bess::control::Normalize(&expected);
    PipelineSpec observed = running;
    bess::control::Normalize(&observed);
    EXPECT_TRUE(observed == expected) << "active state differs from desired";
  };

  // Creation.
  PipelineSpec full = FullPipeline();
  ASSERT_TRUE(control_plane_->ApplyPipeline(full, {}).has_value());
  expect_desired(full);

  // Change: add a module and a connection.
  bess::control::ModuleSpec extra;
  extra.name = "extra";
  extra.mclass = "Bypass";
  full.modules.push_back(extra);
  bess::control::ConnectionSpec edge;
  edge.upstream = "sink";
  edge.ogate = 0;
  edge.downstream = "extra";
  edge.igate = 0;
  full.connections.push_back(edge);
  ASSERT_TRUE(control_plane_->ApplyPipeline(full, {}).has_value());
  expect_desired(full);

  // Removal.
  full.modules.pop_back();
  full.connections.pop_back();
  ASSERT_TRUE(control_plane_->ApplyPipeline(full, {}).has_value());
  expect_desired(full);
}

// Traffic-class parameters are part of desired state: a changed rate limit is a
// change, and applying it moves the runtime.
TEST_F(ApplyPipelineTest, RateLimitParameterChangesAreVisibleAndApplied) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  bess::control::TrafficClassSpec limiter;
  limiter.name = "limiter";
  limiter.policy = "rate_limit";
  limiter.resource = "bit";
  limiter.limit["bit"] = 1000000000;  // 1 Gbps
  limiter.max_burst["bit"] = 1000000;
  limiter.wid = 0;
  spec.traffic_classes.push_back(limiter);

  bess::control::TrafficClassSpec child;
  child.name = "child";
  child.policy = "round_robin";
  child.parent = "limiter";
  spec.traffic_classes.push_back(child);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  // Same pipeline, ten times slower: this must not be "unchanged".
  PipelineSpec slower = spec;
  slower.traffic_classes[0].limit["bit"] = 100000000;  // 100 Mbps

  auto diff = control_plane_->DiffPipeline(slower);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  ASSERT_EQ(1u, diff->traffic_classes.size());
  EXPECT_EQ(bess::control::ChangeKind::kUpdateParams,
            diff->traffic_classes[0].kind);
  EXPECT_EQ("limiter", diff->traffic_classes[0].name);

  auto applied = control_plane_->ApplyPipeline(slower, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;

  // The snapshot shows the new rate, and the runtime matches desired state.
  bool found = false;
  for (const auto &tc : control_plane_->GetPipeline().traffic_classes) {
    if (tc.name == "limiter") {
      found = true;
      EXPECT_EQ(100000000u, tc.limit);
    }
  }
  EXPECT_TRUE(found);

  const PipelineSpec running =
      SpecFromSnapshot(control_plane_->GetPipeline());
  auto settled = control_plane_->DiffPipeline(running);
  ASSERT_TRUE(settled.has_value());
  EXPECT_TRUE(settled->empty());
}

// Attachment changes (priority, share) are reversible: detach and reattach.
TEST_F(ApplyPipelineTest, AttachmentChangesAreReversibleUpdates) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  bess::control::TrafficClassSpec priority;
  priority.name = "prio";
  priority.policy = "priority";
  priority.wid = 0;
  spec.traffic_classes.push_back(priority);

  bess::control::TrafficClassSpec child;
  child.name = "child";
  child.policy = "round_robin";
  child.parent = "prio";
  child.has_priority = true;
  child.priority = 10;
  spec.traffic_classes.push_back(child);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  PipelineSpec reprioritised = spec;
  reprioritised.traffic_classes[1].priority = 20;

  auto diff = control_plane_->DiffPipeline(reprioritised);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  ASSERT_EQ(1u, diff->traffic_classes.size());
  EXPECT_EQ(bess::control::ChangeKind::kUpdate, diff->traffic_classes[0].kind);

  auto applied = control_plane_->ApplyPipeline(reprioritised, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;

  const PipelineSpec running =
      SpecFromSnapshot(control_plane_->GetPipeline());
  auto settled = control_plane_->DiffPipeline(running);
  ASSERT_TRUE(settled.has_value());
  EXPECT_TRUE(settled->empty());
}

// A different policy is a different class: refused, not silently ignored.
TEST_F(ApplyPipelineTest, PolicyChangeIsRefusedTransactionally) {
  // A weighted-fair parent with a round-robin child, so that the child can
  // legitimately carry a share; the change is the child's policy.
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();
  bess::control::TrafficClassSpec parent;
  parent.name = "wparent";
  parent.policy = "weighted_fair";
  parent.resource = "packet";
  parent.wid = 0;
  spec.traffic_classes.push_back(parent);
  bess::control::TrafficClassSpec child;
  child.name = "child";
  child.policy = "round_robin";
  child.parent = "wparent";
  child.has_share = true;
  child.share = 3;
  spec.traffic_classes.push_back(child);
  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());
  const uint64_t generation = bess::control::runtime().generation();

  PipelineSpec changed = spec;
  for (auto &tc : changed.traffic_classes) {
    if (tc.name == "child") {
      tc.policy = "rate_limit";
      tc.resource = "packet";
      tc.limit["packet"] = 1000;
      tc.max_burst["packet"] = 100;
    }
  }

  auto diff = control_plane_->DiffPipeline(changed);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  ASSERT_EQ(1u, diff->traffic_classes.size());
  EXPECT_EQ(bess::control::ChangeKind::kReplace, diff->traffic_classes[0].kind);

  auto applied = control_plane_->ApplyPipeline(changed, {});
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(bess::control::ControlErrorCode::kUnsupportedTransaction,
            applied.error().code);
  EXPECT_EQ(generation, bess::control::runtime().generation());
}

// A plan whose retirement cannot be guaranteed is refused up front: the port is
// still in use by a module the plan keeps.
TEST_F(ApplyPipelineTest, RetirementPreconditionsAreProvenUpFront) {
  PipelineSpec spec = FullPipeline();

  bess::pb::PMDPortArg port_arg;
  port_arg.set_vdev("net_null0");
  bess::control::PortSpec port;
  port.name = "p0";
  port.driver = "PMDPort";
  port.num_rx_queues = 1;
  port.num_tx_queues = 1;
  port.rx_queue_size = 1024;
  port.tx_queue_size = 1024;
  port.arg.PackFrom(port_arg);
  spec.ports.push_back(port);

  bess::pb::QueueIncArg queue_arg;
  queue_arg.set_port("p0");
  queue_arg.set_qid(0);
  bess::control::ModuleSpec reader;
  reader.name = "reader";
  reader.mclass = "QueueInc";
  reader.arg.PackFrom(queue_arg);
  spec.modules.push_back(reader);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  // Keep the module, drop the port it reads from: the port cannot be retired
  // while the module holds its queues, so the plan must be refused before
  // anything happens.
  PipelineSpec without_port = spec;
  without_port.ports.clear();

  const uint64_t generation = bess::control::runtime().generation();
  const PipelineSnapshot active = control_plane_->GetPipeline();

  auto applied = control_plane_->ApplyPipeline(without_port, {});
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(bess::control::ControlErrorCode::kUnsupportedTransaction,
            applied.error().code);
  EXPECT_EQ("port", applied.error().object);
  EXPECT_NE(std::string::npos, applied.error().message.find("still in use"));
  EXPECT_EQ(generation, bess::control::runtime().generation());
  EXPECT_TRUE(control_plane_->GetPipeline() == active);
}

// The acceptance test for the reparent ownership split: a refused move must
// not lose the class, its siblings, or their priorities.
TEST_F(ApplyPipelineTest, RefusedReparentKeepsEveryClassAndPlacement) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  bess::control::TrafficClassSpec parent;
  parent.name = "prio";
  parent.policy = "priority";
  parent.wid = 0;
  spec.traffic_classes.push_back(parent);

  bess::control::TrafficClassSpec child_a;
  child_a.name = "child_a";
  child_a.policy = "round_robin";
  child_a.parent = "prio";
  child_a.has_priority = true;
  child_a.priority = 10;
  spec.traffic_classes.push_back(child_a);

  bess::control::TrafficClassSpec child_b;
  child_b.name = "child_b";
  child_b.policy = "round_robin";
  child_b.parent = "prio";
  child_b.has_priority = true;
  child_b.priority = 20;
  spec.traffic_classes.push_back(child_b);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  const PipelineSnapshot active = control_plane_->GetPipeline();
  const uint64_t generation = bess::control::runtime().generation();
  const size_t tcs = bess::control::runtime().traffic_classes().Size();

  // child_a would collide with child_b's priority: refused.
  PipelineSpec collision = spec;
  collision.traffic_classes[1].priority = 20;

  auto applied = control_plane_->ApplyPipeline(collision, {});
  ASSERT_FALSE(applied.has_value());

  EXPECT_EQ(generation, bess::control::runtime().generation());
  EXPECT_EQ(tcs, bess::control::runtime().traffic_classes().Size());
  EXPECT_TRUE(control_plane_->GetPipeline() == active);
  EXPECT_TRUE(bess::control::runtime().traffic_classes().Contains("child_a"));
  EXPECT_TRUE(bess::control::runtime().traffic_classes().Contains("child_b"));

  // Both children still hold their original priorities.
  for (const auto &tc : control_plane_->GetPipeline().traffic_classes) {
    if (tc.name == "child_a") {
      EXPECT_TRUE(tc.has_priority);
      EXPECT_EQ(10, tc.priority);
    }
    if (tc.name == "child_b") {
      EXPECT_TRUE(tc.has_priority);
      EXPECT_EQ(20, tc.priority);
    }
  }

  // And the pipeline still diffs clean against its own description.
  auto diff = control_plane_->DiffPipeline(spec);
  ASSERT_TRUE(diff.has_value());
  EXPECT_TRUE(diff->empty());
}

// An attach failure that validation cannot see (the parent's current child is
// only removed in the same transaction's retire phase) must roll the whole
// transaction back, including the class that was created for it.
TEST_F(ApplyPipelineTest, AttachFailureDuringCommitRollsBackCleanly) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  bess::control::TrafficClassSpec limiter;
  limiter.name = "limiter";
  limiter.policy = "rate_limit";
  limiter.resource = "bit";
  limiter.limit["bit"] = 1000000;
  limiter.max_burst["bit"] = 1000;
  limiter.wid = 0;
  spec.traffic_classes.push_back(limiter);

  bess::control::TrafficClassSpec old_child;
  old_child.name = "old_child";
  old_child.policy = "round_robin";
  old_child.parent = "limiter";
  spec.traffic_classes.push_back(old_child);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  const PipelineSnapshot active = control_plane_->GetPipeline();
  const uint64_t generation = bess::control::runtime().generation();
  const size_t tcs = bess::control::runtime().traffic_classes().Size();

  // Rename the child: the new class is created during the commit, while the old
  // one is only retired afterwards, so the rate limiter still holds its child
  // and the attach fails.
  PipelineSpec renamed = spec;
  renamed.traffic_classes[1].name = "new_child";

  auto applied = control_plane_->ApplyPipeline(renamed, {});
  ASSERT_FALSE(applied.has_value());

  EXPECT_EQ(generation, bess::control::runtime().generation());
  EXPECT_EQ(tcs, bess::control::runtime().traffic_classes().Size());
  EXPECT_TRUE(bess::control::runtime().traffic_classes().Contains("old_child"));
  EXPECT_FALSE(bess::control::runtime().traffic_classes().Contains("new_child"));
  EXPECT_TRUE(control_plane_->GetPipeline() == active);
}

// Weighted-fair shares must be positive; validation refuses zero up front.
TEST_F(ApplyPipelineTest, ZeroShareIsRejectedBeforeAnyChange) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  bess::control::TrafficClassSpec parent;
  parent.name = "wparent";
  parent.policy = "weighted_fair";
  parent.resource = "packet";
  parent.wid = 0;
  spec.traffic_classes.push_back(parent);

  bess::control::TrafficClassSpec child;
  child.name = "child";
  child.policy = "round_robin";
  child.parent = "wparent";
  child.has_share = true;
  child.share = 0;
  spec.traffic_classes.push_back(child);

  const uint64_t generation = bess::control::runtime().generation();
  auto applied = control_plane_->ApplyPipeline(spec, {});
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(EINVAL, applied.error().err);
  EXPECT_EQ("share", applied.error().field);
  EXPECT_EQ(generation, bess::control::runtime().generation());
  EXPECT_TRUE(bess::control::runtime().traffic_classes().Empty());
}

// Pause duration is observable from the start.
TEST_F(ApplyPipelineTest, RecordsPhaseTimings) {
  auto applied = control_plane_->ApplyPipeline(FullPipeline(), {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;

  EXPECT_TRUE(applied->workers_paused);
  EXPECT_GE(applied->timing.validation_us, 0u);
  EXPECT_GE(applied->timing.prepare_us, 0u);
  EXPECT_GE(applied->timing.paused_commit_us, 0u);
  EXPECT_GE(applied->timing.retire_us, 0u);
}


// The legacy `UpdateTcParent` used to snapshot the old attachment *after*
// detaching the class, so a refused move restored it as a root/orphan instead
// of putting it back under its parent with its priority or share. It now
// snapshots first, which is what this pins down.
TEST_F(ApplyPipelineTest, LegacyReparentRestoresTheOriginalAttachment) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  // A port and a module with a task, so there is a real leaf traffic class.
  bess::pb::PMDPortArg port_arg;
  port_arg.set_vdev("net_null1");
  bess::control::PortSpec port;
  port.name = "p0";
  port.driver = "PMDPort";
  port.num_rx_queues = 1;
  port.num_tx_queues = 1;
  port.rx_queue_size = 1024;
  port.tx_queue_size = 1024;
  port.arg.PackFrom(port_arg);
  spec.ports.push_back(port);

  bess::pb::QueueIncArg queue_arg;
  queue_arg.set_port("p0");
  queue_arg.set_qid(0);
  bess::control::ModuleSpec reader;
  reader.name = "reader";
  reader.mclass = "QueueInc";
  reader.arg.PackFrom(queue_arg);
  spec.modules.push_back(reader);

  // A round-robin home for the leaf, and a priority class whose slot is taken.
  bess::control::TrafficClassSpec home;
  home.name = "home";
  home.policy = "round_robin";
  home.wid = 0;
  spec.traffic_classes.push_back(home);

  bess::control::TrafficClassSpec prio;
  prio.name = "prio";
  prio.policy = "priority";
  prio.wid = 0;
  spec.traffic_classes.push_back(prio);

  bess::control::TrafficClassSpec holder;
  holder.name = "holder";
  holder.policy = "round_robin";
  holder.parent = "prio";
  holder.has_priority = true;
  holder.priority = 10;
  spec.traffic_classes.push_back(holder);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  // Find the leaf the module created, and give it a known home through the
  // legacy path (this one is expected to succeed).
  const bess::control::PipelineSnapshot after_apply =
      control_plane_->GetPipeline();
  std::string leaf_name;
  for (const auto &tc : after_apply.traffic_classes) {
    if (tc.policy == "leaf" && tc.leaf_module_name == "reader") {
      leaf_name = tc.name;
    }
  }
  ASSERT_FALSE(leaf_name.empty());

  bess::control::TrafficClassSpec move;
  move.leaf_module_name = "reader";
  move.leaf_module_taskid = 0;
  move.parent = "home";
  ASSERT_TRUE(control_plane_->UpdateTcParent(move).has_value());

  const uint64_t generation = bess::control::runtime().generation();
  {
    const bess::control::PipelineSnapshot moved = control_plane_->GetPipeline();
    bool homed = false;
    for (const auto &tc : moved.traffic_classes) {
      if (tc.name == leaf_name) {
        homed = tc.parent == "home";
      }
    }
    ASSERT_TRUE(homed) << "the leaf should be under 'home'";
  }

  // Now ask for a move that must be refused: priority 10 is already taken.
  bess::control::TrafficClassSpec collision = move;
  collision.parent = "prio";
  collision.has_priority = true;
  collision.priority = 10;

  auto refused = control_plane_->UpdateTcParent(collision);
  ASSERT_FALSE(refused.has_value());

  EXPECT_EQ(generation, bess::control::runtime().generation());
  EXPECT_TRUE(bess::control::runtime().traffic_classes().Contains(leaf_name));

  // The class went back where it was, not to the orphan list.
  bool restored = false;
  for (const auto &tc : control_plane_->GetPipeline().traffic_classes) {
    if (tc.name == leaf_name) {
      restored = tc.parent == "home";
    }
  }
  EXPECT_TRUE(restored) << "the leaf should still be under 'home'";

  const PipelineSpec running =
      SpecFromSnapshot(control_plane_->GetPipeline());
  auto diff = control_plane_->DiffPipeline(running);
  ASSERT_TRUE(diff.has_value());
  EXPECT_TRUE(diff->empty());
}

// The legacy rule for non-leaf classes is stricter: they may only move as
// orphans. A refusal there must leave everything exactly as it was.
TEST_F(ApplyPipelineTest, LegacyReparentRefusesToMoveAnAttachedRoot) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  bess::control::TrafficClassSpec root;
  root.name = "solo";
  root.policy = "round_robin";
  root.wid = 0;
  spec.traffic_classes.push_back(root);

  bess::control::TrafficClassSpec prio;
  prio.name = "prio";
  prio.policy = "priority";
  prio.wid = 0;
  spec.traffic_classes.push_back(prio);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  const PipelineSnapshot active = control_plane_->GetPipeline();
  const uint64_t generation = bess::control::runtime().generation();

  bess::control::TrafficClassSpec move;
  move.name = "solo";
  move.parent = "prio";
  move.has_priority = true;
  move.priority = 5;  // free: the move would succeed if it were allowed

  auto refused = control_plane_->UpdateTcParent(move);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(EINVAL, refused.error().err);

  EXPECT_EQ(generation, bess::control::runtime().generation());
  EXPECT_TRUE(control_plane_->GetPipeline() == active);
  EXPECT_TRUE(bess::control::runtime().traffic_classes().Contains("solo"));
}

}  // namespace
