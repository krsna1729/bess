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

}  // namespace
