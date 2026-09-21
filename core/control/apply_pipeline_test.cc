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

}  // namespace
