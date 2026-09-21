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

#include <cerrno>
#include <string>

#include "control/control_plane.h"
#include "control/pipeline_diff.h"
#include "control/pipeline_plan.h"
#include "control/pipeline_spec.h"
#include "control/pipeline_validator.h"
#include "control/runtime_state.h"
#include "module.h"
#include "port.h"
#include "worker.h"

namespace {

using bess::control::ControlPlane;
using bess::control::PipelineSpec;
using bess::control::PortSpec;
using bess::control::ModuleSpec;
using bess::control::ConnectionSpec;
using bess::control::TrafficClassSpec;
using bess::control::WorkerSpec;
using bess::control::ValidatedPipeline;
using bess::control::ValidatePipeline;

class ControlPlaneTest : public ::testing::Test {
 protected:
  void SetUp() override { runtime_ = &bess::control::runtime(); }

  bess::control::RuntimeState *runtime_;
};

// A minimal valid desired pipeline: one worker, one module, no ports (the
// test binary has no usable PMD in a sandbox, and module-only pipelines are
// legitimate desired state).
PipelineSpec MinimalSpec() {
  PipelineSpec spec;
  WorkerSpec worker;
  worker.wid = 0;
  worker.core = 0;
  spec.workers.push_back(worker);

  ModuleSpec module;
  module.name = "bypass0";
  module.mclass = "Bypass";
  spec.modules.push_back(module);
  return spec;
}

TEST_F(ControlPlaneTest, ValidatesMinimalSpec) {
  auto validated = ValidatePipeline(*runtime_, MinimalSpec());
  ASSERT_TRUE(validated.has_value()) << validated.error().message;
  EXPECT_EQ(1u, validated->spec.modules.size());
  EXPECT_EQ("bypass0", validated->spec.modules[0].name);
}

TEST_F(ControlPlaneTest, NormalizesQueueCountsAndOrdering) {
  PipelineSpec spec = MinimalSpec();
  PortSpec port;
  port.name = "zport";
  port.driver = "PMDPort";
  port.num_rx_queues = 0;  // "one queue"
  spec.ports.push_back(port);

  auto validated = ValidatePipeline(*runtime_, spec);
  ASSERT_TRUE(validated.has_value()) << validated.error().message;
  ASSERT_EQ(1u, validated->spec.ports.size());
  EXPECT_EQ(1, validated->spec.ports[0].num_rx_queues);
  EXPECT_EQ(1, validated->spec.ports[0].num_tx_queues);

  // Two descriptions of the same pipeline normalize to the same thing.
  PipelineSpec same = MinimalSpec();
  PortSpec explicit_port;
  explicit_port.name = "zport";
  explicit_port.driver = "PMDPort";
  explicit_port.num_rx_queues = 1;
  explicit_port.num_tx_queues = 1;
  same.ports.push_back(explicit_port);

  auto validated_same = ValidatePipeline(*runtime_, same);
  ASSERT_TRUE(validated_same.has_value());
  EXPECT_TRUE(validated->spec.ports == validated_same->spec.ports);
}

TEST_F(ControlPlaneTest, RejectsUnknownPortDriver) {
  PipelineSpec spec = MinimalSpec();
  PortSpec port;
  port.name = "p0";
  port.driver = "NoSuchDriver";
  spec.ports.push_back(port);

  auto validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ(ENOENT, validated.error().err);
  EXPECT_EQ("port", validated.error().object);
  EXPECT_EQ("driver", validated.error().field);
}

TEST_F(ControlPlaneTest, RejectsUnknownMclass) {
  PipelineSpec spec = MinimalSpec();
  spec.modules[0].mclass = "NoSuchModule";

  auto validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ(ENOENT, validated.error().err);
}

TEST_F(ControlPlaneTest, RejectsDuplicateNames) {
  PipelineSpec spec = MinimalSpec();
  spec.modules.push_back(spec.modules[0]);

  auto validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ(EEXIST, validated.error().err);
}

TEST_F(ControlPlaneTest, RejectsConnectionToMissingModule) {
  PipelineSpec spec = MinimalSpec();
  ConnectionSpec connection;
  connection.upstream = "bypass0";
  connection.downstream = "nope";
  spec.connections.push_back(connection);

  auto validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ(ENOENT, validated.error().err);
  EXPECT_EQ("downstream", validated.error().field);
}

TEST_F(ControlPlaneTest, RejectsOutOfRangeGate) {
  PipelineSpec spec = MinimalSpec();
  ModuleSpec second;
  second.name = "bypass1";
  second.mclass = "Bypass";
  spec.modules.push_back(second);

  ConnectionSpec connection;
  connection.upstream = "bypass0";
  connection.downstream = "bypass1";
  connection.ogate = MAX_GATES;  // Bypass has a single output gate
  spec.connections.push_back(connection);

  auto validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ(EINVAL, validated.error().err);
  EXPECT_EQ("ogate", validated.error().field);
}

TEST_F(ControlPlaneTest, RejectsDuplicateOutputGate) {
  PipelineSpec spec = MinimalSpec();
  ModuleSpec second;
  second.name = "bypass1";
  second.mclass = "Bypass";
  spec.modules.push_back(second);

  ConnectionSpec connection;
  connection.upstream = "bypass0";
  connection.downstream = "bypass1";
  spec.connections.push_back(connection);
  spec.connections.push_back(connection);

  auto validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ(EEXIST, validated.error().err);
}

TEST_F(ControlPlaneTest, RejectsBadWorker) {
  PipelineSpec spec = MinimalSpec();
  spec.workers[0].wid = Worker::kMaxWorkers;

  auto validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ(EINVAL, validated.error().err);
  EXPECT_EQ("wid", validated.error().field);

  spec = MinimalSpec();
  spec.workers[0].core = 100000;  // no such CPU
  validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ("core", validated.error().field);

  spec = MinimalSpec();
  spec.workers[0].scheduler = "nonsense";
  validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ("scheduler", validated.error().field);
}

TEST_F(ControlPlaneTest, RejectsDuplicateWorkerCore) {
  PipelineSpec spec = MinimalSpec();
  WorkerSpec second;
  second.wid = 1;
  second.core = spec.workers[0].core;
  spec.workers.push_back(second);

  auto validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ(EINVAL, validated.error().err);
  EXPECT_EQ("core", validated.error().field);
}

TEST_F(ControlPlaneTest, RejectsBadTrafficClass) {
  PipelineSpec spec = MinimalSpec();

  TrafficClassSpec tc;
  tc.name = "t0";
  tc.policy = "nonsense";
  spec.traffic_classes.push_back(tc);
  auto validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ("policy", validated.error().field);

  spec = MinimalSpec();
  tc.policy = "weighted_fair";
  tc.resource = "nonsense";
  spec.traffic_classes.push_back(tc);
  validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ("resource", validated.error().field);

  spec = MinimalSpec();
  tc.policy = "round_robin";
  tc.resource = "";
  tc.parent = "missing";
  spec.traffic_classes.push_back(tc);
  validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ(ENOENT, validated.error().err);
  EXPECT_EQ("parent", validated.error().field);

  spec = MinimalSpec();
  TrafficClassSpec leaf;
  leaf.name = "!reserved";
  leaf.policy = "round_robin";
  spec.traffic_classes.push_back(leaf);
  validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ("name", validated.error().field);
}

TEST_F(ControlPlaneTest, RejectsTrafficClassParentCycle) {
  PipelineSpec spec = MinimalSpec();

  TrafficClassSpec a;
  a.name = "a";
  a.policy = "round_robin";
  a.parent = "b";
  spec.traffic_classes.push_back(a);

  TrafficClassSpec b;
  b.name = "b";
  b.policy = "round_robin";
  b.parent = "a";
  spec.traffic_classes.push_back(b);

  auto validated = ValidatePipeline(*runtime_, spec);
  ASSERT_FALSE(validated.has_value());
  EXPECT_EQ(EINVAL, validated.error().err);
  EXPECT_EQ("parent", validated.error().field);
}

// Validation is pure: a rejected spec must leave the runtime untouched, and a
// valid one must not create anything either.
TEST_F(ControlPlaneTest, ValidationHasNoSideEffects) {
  const size_t ports_before = runtime_->ports().Size();
  const size_t modules_before = runtime_->modules().Size();
  const size_t tcs_before = runtime_->traffic_classes().Size();

  PipelineSpec spec = MinimalSpec();
  PortSpec port;
  port.name = "p0";
  port.driver = "NoSuchDriver";
  spec.ports.push_back(port);
  ASSERT_FALSE(ValidatePipeline(*runtime_, spec).has_value());

  ASSERT_TRUE(ValidatePipeline(*runtime_, MinimalSpec()).has_value());

  EXPECT_EQ(ports_before, runtime_->ports().Size());
  EXPECT_EQ(modules_before, runtime_->modules().Size());
  EXPECT_EQ(tcs_before, runtime_->traffic_classes().Size());
}

// The snapshot reflects what the control plane actually created, in a stable
// order, and validation stays available through the ControlPlane object.
TEST_F(ControlPlaneTest, SnapshotIsDeterministicAndReflectsRuntime) {
  ControlPlane control_plane;

  ASSERT_TRUE(control_plane.CreateModule(ModuleSpec{"snap0", "Bypass", {}})
                  .has_value());
  ASSERT_TRUE(control_plane.CreateModule(ModuleSpec{"snap1", "Bypass", {}})
                  .has_value());

  ConnectionSpec connection;
  connection.upstream = "snap0";
  connection.downstream = "snap1";
  ASSERT_TRUE(control_plane.ConnectModules(connection).has_value());

  const bess::control::PipelineSnapshot first = control_plane.GetPipeline();
  const bess::control::PipelineSnapshot second = control_plane.GetPipeline();
  EXPECT_TRUE(first == second);

  ASSERT_EQ(2u, first.modules.size());
  EXPECT_EQ("snap0", first.modules[0].name);  // name order, not insertion order
  EXPECT_EQ("snap1", first.modules[1].name);

  ASSERT_EQ(1u, first.connections.size());
  EXPECT_EQ("snap0", first.connections[0].upstream);
  EXPECT_EQ("snap1", first.connections[0].downstream);

  // The control plane validates through the same pure path.
  ASSERT_TRUE(control_plane.ValidatePipeline(MinimalSpec()).has_value());

  // Cleanup: destroy what this test created, then verify the snapshot follows.
  ASSERT_TRUE(control_plane.DisconnectModules({"snap0", 0}).has_value());
  ASSERT_TRUE(control_plane.DestroyModule("snap1").has_value());
  ASSERT_TRUE(control_plane.DestroyModule("snap0").has_value());
  EXPECT_TRUE(control_plane.GetPipeline().modules.empty());
  EXPECT_TRUE(control_plane.GetPipeline().connections.empty());
}


// ---------------------------------------------------------------------------
// Diff and plan
// ---------------------------------------------------------------------------

TEST_F(ControlPlaneTest, IdenticalStateProducesEmptyDiff) {
  ControlPlane control_plane;

  ASSERT_TRUE(control_plane.CreateModule(ModuleSpec{"d0", "Bypass", {}})
                  .has_value());
  ASSERT_TRUE(control_plane.CreateModule(ModuleSpec{"d1", "Bypass", {}})
                  .has_value());
  ConnectionSpec connection;
  connection.upstream = "d0";
  connection.downstream = "d1";
  ASSERT_TRUE(control_plane.ConnectModules(connection).has_value());

  // The desired state that describes exactly what is running.
  const PipelineSpec running =
      bess::control::SpecFromSnapshot(control_plane.GetPipeline());

  auto diff = control_plane.DiffPipeline(running);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  EXPECT_TRUE(diff->empty());

  auto plan = control_plane.PlanPipeline(running);
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  EXPECT_TRUE(plan->empty());

  ASSERT_TRUE(control_plane.DisconnectModules({"d0", 0}).has_value());
  ASSERT_TRUE(control_plane.DestroyModule("d1").has_value());
  ASSERT_TRUE(control_plane.DestroyModule("d0").has_value());
}

TEST_F(ControlPlaneTest, DiffClassifiesCreateAndRemove) {
  ControlPlane control_plane;

  ASSERT_TRUE(control_plane.CreateModule(ModuleSpec{"old", "Bypass", {}})
                  .has_value());

  PipelineSpec desired;
  ModuleSpec module;
  module.name = "new";
  module.mclass = "Bypass";
  desired.modules.push_back(module);

  auto diff = control_plane.DiffPipeline(desired);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  ASSERT_EQ(2u, diff->modules.size());
  EXPECT_EQ("new", diff->modules[0].name);
  EXPECT_EQ(bess::control::ChangeKind::kCreate, diff->modules[0].kind);
  EXPECT_EQ("old", diff->modules[1].name);
  EXPECT_EQ(bess::control::ChangeKind::kRemove, diff->modules[1].kind);

  auto plan = control_plane.PlanPipeline(desired);
  ASSERT_TRUE(plan.has_value());
  ASSERT_EQ(1u, plan->prepare_ops.size());
  EXPECT_TRUE(std::holds_alternative<bess::control::CreateModuleOp>(
      plan->prepare_ops[0]));
  ASSERT_EQ(1u, plan->retire_ops.size());
  EXPECT_TRUE(std::holds_alternative<bess::control::RemoveModuleOp>(
      plan->retire_ops[0]));

  ASSERT_TRUE(control_plane.DestroyModule("old").has_value());
}

// Module replacement is a retire-then-prepare pair, never an in-place mutation.
TEST_F(ControlPlaneTest, DiffReplacesModuleWithDifferentArg) {
  ControlPlane control_plane;

  bess::pb::EmptyArg empty;
  ModuleSpec original;
  original.name = "m0";
  original.mclass = "Bypass";
  original.arg.PackFrom(empty);
  ASSERT_TRUE(control_plane.CreateModule(original).has_value());

  PipelineSpec desired;
  ModuleSpec changed = original;
  changed.arg.Clear();  // no argument at all: a different construction
  desired.modules.push_back(changed);

  auto diff = control_plane.DiffPipeline(desired);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  ASSERT_EQ(1u, diff->modules.size());
  EXPECT_EQ(bess::control::ChangeKind::kReplace, diff->modules[0].kind);

  auto plan = control_plane.PlanPipeline(desired);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(1u, plan->prepare_ops.size());
  EXPECT_EQ(1u, plan->retire_ops.size());

  ASSERT_TRUE(control_plane.DestroyModule("m0").has_value());
}

// Connections are diffed from the graph itself, in a stable order.
TEST_F(ControlPlaneTest, DiffSeesConnectionChanges) {
  ControlPlane control_plane;

  ASSERT_TRUE(control_plane.CreateModule(ModuleSpec{"c0", "Bypass", {}})
                  .has_value());
  ASSERT_TRUE(control_plane.CreateModule(ModuleSpec{"c1", "Bypass", {}})
                  .has_value());

  PipelineSpec desired = bess::control::SpecFromSnapshot(
      control_plane.GetPipeline());
  ConnectionSpec connection;
  connection.upstream = "c0";
  connection.downstream = "c1";
  desired.connections.push_back(connection);

  auto diff = control_plane.DiffPipeline(desired);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  ASSERT_EQ(1u, diff->connections.size());
  EXPECT_EQ(bess::control::ChangeKind::kCreate, diff->connections[0].kind);
  EXPECT_EQ("c0", diff->connections[0].upstream);

  auto plan = control_plane.PlanPipeline(desired);
  ASSERT_TRUE(plan.has_value());
  ASSERT_EQ(1u, plan->commit_ops.size());
  EXPECT_TRUE(std::holds_alternative<bess::control::ConnectOp>(
      plan->commit_ops[0]));

  // And the reverse: an existing connection that desired state drops.
  ASSERT_TRUE(control_plane.ConnectModules(connection).has_value());
  desired.connections.clear();
  diff = control_plane.DiffPipeline(desired);
  ASSERT_TRUE(diff.has_value());
  ASSERT_EQ(1u, diff->connections.size());
  EXPECT_EQ(bess::control::ChangeKind::kRemove, diff->connections[0].kind);
  plan = control_plane.PlanPipeline(desired);
  ASSERT_TRUE(plan.has_value());
  ASSERT_EQ(1u, plan->commit_ops.size());
  EXPECT_TRUE(std::holds_alternative<bess::control::DisconnectOp>(
      plan->commit_ops[0]));

  ASSERT_TRUE(control_plane.DestroyModule("c1").has_value());
  ASSERT_TRUE(control_plane.DestroyModule("c0").has_value());
}

// An internal traffic class (module leaf) is never part of desired state and
// therefore never shows up as a change.
TEST_F(ControlPlaneTest, DiffIgnoresInternalTrafficClasses) {
  ControlPlane control_plane;

  // A module leaf class and a scheduler default, exactly as modules and
  // workers create them.
  bess::TrafficClass *internal =
      bess::TrafficClassBuilder::CreateTrafficClass<
          bess::RoundRobinTrafficClass>("!internal_rr");
  ASSERT_NE(nullptr, internal);

  const PipelineSpec running =
      bess::control::SpecFromSnapshot(control_plane.GetPipeline());
  EXPECT_TRUE(running.traffic_classes.empty());

  auto diff = control_plane.DiffPipeline(running);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  EXPECT_TRUE(diff->traffic_classes.empty());

  bess::control::runtime().traffic_classes().Release(internal);
  delete internal;
}

// The plan orders phases by dependency: workers and ports and modules before
// connections, teardown in reverse.
TEST_F(ControlPlaneTest, PlanOrdersPhasesByDependency) {
  bess::control::PipelineDiff diff;
  diff.ports.push_back(
      bess::control::PortChange{"p0", bess::control::ChangeKind::kCreate, {}});
  diff.modules.push_back(bess::control::ModuleChange{
      "m0", bess::control::ChangeKind::kCreate, {}});
  diff.connections.push_back(bess::control::ConnectionChange{
      "m0", 0, "m1", 0, bess::control::ChangeKind::kCreate});
  diff.traffic_classes.push_back(bess::control::TrafficClassChange{
      "t0", bess::control::ChangeKind::kRemove, {}});
  diff.modules.push_back(bess::control::ModuleChange{
      "m1", bess::control::ChangeKind::kRemove, {}});

  const bess::control::PipelinePlan plan = bess::control::Plan(diff);

  ASSERT_EQ(2u, plan.prepare_ops.size());
  EXPECT_TRUE(std::holds_alternative<bess::control::CreatePortOp>(
      plan.prepare_ops[0]));
  EXPECT_TRUE(std::holds_alternative<bess::control::CreateModuleOp>(
      plan.prepare_ops[1]));

  ASSERT_EQ(1u, plan.commit_ops.size());
  EXPECT_TRUE(std::holds_alternative<bess::control::ConnectOp>(
      plan.commit_ops[0]));

  ASSERT_EQ(2u, plan.retire_ops.size());
  EXPECT_TRUE(std::holds_alternative<bess::control::RemoveTcOp>(
      plan.retire_ops[0]));  // TCs detach before their module goes
  EXPECT_TRUE(std::holds_alternative<bess::control::RemoveModuleOp>(
      plan.retire_ops[1]));
}

// Rebuilding the same diff twice yields the same plan, in the same order.
TEST_F(ControlPlaneTest, DiffAndPlanAreDeterministic) {
  PipelineSpec desired = MinimalSpec();
  ModuleSpec extra;
  extra.name = "zzz";
  extra.mclass = "Bypass";
  desired.modules.push_back(extra);
  ModuleSpec first;
  first.name = "aaa";
  first.mclass = "Bypass";
  desired.modules.insert(desired.modules.begin(), first);

  ControlPlane control_plane;
  const auto diff_a = control_plane.DiffPipeline(desired);
  const auto diff_b = control_plane.DiffPipeline(desired);
  ASSERT_TRUE(diff_a.has_value() && diff_b.has_value());
  ASSERT_EQ(diff_a->modules.size(), diff_b->modules.size());
  for (size_t i = 0; i < diff_a->modules.size(); i++) {
    EXPECT_EQ(diff_a->modules[i].name, diff_b->modules[i].name);
  }
  // Sorted by name, not by the order they were listed.
  ASSERT_EQ(3u, diff_a->modules.size());
  EXPECT_EQ("aaa", diff_a->modules[0].name);
  EXPECT_EQ("bypass0", diff_a->modules[1].name);
  EXPECT_EQ("zzz", diff_a->modules[2].name);
}

}  // namespace
