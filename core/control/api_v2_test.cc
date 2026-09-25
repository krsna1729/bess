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

// G1: the v2 desired-state API. Adapter round-trips, then the whole service
// over a real in-process gRPC channel -- wire encoding, status codes and the
// typed error trailer included.

#include "control/api_v2.h"

#include <google/protobuf/util/message_differencer.h>
#include <grpc++/grpc++.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "control/control_plane.h"
#include "control/runtime_state.h"
#include "opts.h"
#include "packet_pool.h"
#include "port.h"

namespace {

namespace v2 = bess::pb::v2;
using bess::control::ControlPlane;
using bess::control::ControlV2Service;
using google::protobuf::util::MessageDifferencer;

void InitRuntimeOnce() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;
  FLAGS_m = 0;  // malloc-backed, sandbox-safe
  bess::PacketPool::CreateDefaultPools(32767);
  PortBuilder::InitDrivers();
}

// A worker, two Bypass modules connected src:0 -> sink:0, and two traffic
// classes (a root and a weighted child with limits).
v2::Pipeline SamplePipeline() {
  v2::Pipeline p;
  auto *w = p.add_workers();
  w->set_wid(0);
  w->set_core(0);
  for (const char *name : {"src", "sink"}) {
    auto *m = p.add_modules();
    m->set_name(name);
    m->set_mclass("Bypass");
  }
  auto *c = p.add_connections();
  c->set_upstream("src");
  c->set_downstream("sink");
  auto *root = p.add_traffic_classes();
  root->set_name("root");
  root->set_policy("weighted_fair");
  root->set_resource("count");
  root->set_wid(0);
  auto *child = p.add_traffic_classes();
  child->set_name("child");
  child->set_policy("round_robin");
  child->set_parent("root");
  child->set_wid(-1);
  child->set_share(3);
  return p;
}

TEST(ApiV2AdapterTest, PipelineRoundTrips) {
  v2::Pipeline p = SamplePipeline();
  auto *port = p.add_ports();
  port->set_name("p0");
  port->set_driver("PMDPort");
  port->set_num_rx_queues(2);
  port->set_rx_queue_size(512);
  auto *rl = p.add_traffic_classes();
  rl->set_name("limited");
  rl->set_policy("rate_limit");
  rl->set_resource("bit");
  rl->set_priority(7);
  (*rl->mutable_limit())["bit"] = 1000000;
  (*rl->mutable_max_burst())["bit"] = 5000;

  const v2::Pipeline back = bess::control::ToProto(bess::control::FromProto(p));
  EXPECT_TRUE(MessageDifferencer::Equals(p, back))
      << p.DebugString() << "\nvs\n" << back.DebugString();
}

TEST(ApiV2AdapterTest, ErrorsMapToGrpcCodes) {
  using bess::control::ControlErrorCode;
  const auto code = [](ControlErrorCode c) {
    return bess::control::ToStatus({.code = c, .err = 0, .message = "m"})
        .error_code();
  };
  EXPECT_EQ(grpc::StatusCode::INVALID_ARGUMENT,
            code(ControlErrorCode::kInvalidArgument));
  EXPECT_EQ(grpc::StatusCode::NOT_FOUND, code(ControlErrorCode::kNotFound));
  EXPECT_EQ(grpc::StatusCode::ALREADY_EXISTS,
            code(ControlErrorCode::kAlreadyExists));
  EXPECT_EQ(grpc::StatusCode::ABORTED, code(ControlErrorCode::kConflict));
  EXPECT_EQ(grpc::StatusCode::FAILED_PRECONDITION,
            code(ControlErrorCode::kResourceBusy));
  EXPECT_EQ(grpc::StatusCode::UNIMPLEMENTED,
            code(ControlErrorCode::kUnsupportedTransaction));
  EXPECT_EQ(grpc::StatusCode::INTERNAL, code(ControlErrorCode::kInternal));
}

// The service end to end over an in-process channel.
class ApiV2ServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    InitRuntimeOnce();
    control_plane_ = std::make_unique<ControlPlane>();
    service_ = std::make_unique<ControlV2Service>(*control_plane_);
    grpc::ServerBuilder builder;
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    ASSERT_NE(nullptr, server_);
    stub_ = v2::Control::NewStub(
        server_->InProcessChannel(grpc::ChannelArguments()));
  }

  void TearDown() override {
    server_->Shutdown();
    (void)control_plane_->Reset();
  }

  // The typed error a failed call carried in its trailer.
  static v2::ErrorDetail DetailOf(const grpc::ClientContext &context) {
    v2::ErrorDetail detail;
    const auto &trailers = context.GetServerTrailingMetadata();
    const auto it = trailers.find("bess-error-bin");
    if (it != trailers.end()) {
      detail.ParseFromString(std::string(it->second.data(), it->second.size()));
    }
    return detail;
  }

  std::unique_ptr<ControlPlane> control_plane_;
  std::unique_ptr<ControlV2Service> service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<v2::Control::Stub> stub_;
};

TEST_F(ApiV2ServiceTest, ValidateDiffPlanApplyGet) {
  const v2::Pipeline desired = SamplePipeline();

  {
    grpc::ClientContext ctx;
    v2::ValidatePipelineRequest req;
    *req.mutable_pipeline() = desired;
    v2::ValidatePipelineResponse resp;
    ASSERT_TRUE(stub_->ValidatePipeline(&ctx, req, &resp).ok());
    EXPECT_EQ(2, resp.normalized().modules_size());
    EXPECT_EQ("sink", resp.normalized().modules(0).name()) << "sorted";
  }

  uint64_t generation = 0;
  {
    grpc::ClientContext ctx;
    v2::DiffPipelineRequest req;
    *req.mutable_pipeline() = desired;
    v2::DiffPipelineResponse resp;
    ASSERT_TRUE(stub_->DiffPipeline(&ctx, req, &resp).ok());
    generation = resp.generation();
    int creates = 0;
    bool saw_connection = false;
    for (const auto &c : resp.diff().changes()) {
      creates += c.kind() == v2::CREATE;
      if (c.type() == v2::CONNECTION) {
        saw_connection = true;
        EXPECT_EQ("src:0->sink:0", c.object());
      }
    }
    EXPECT_EQ(resp.diff().changes_size(), creates) << "empty runtime: all new";
    EXPECT_TRUE(saw_connection);
  }

  {
    grpc::ClientContext ctx;
    v2::PlanPipelineRequest req;
    *req.mutable_pipeline() = desired;
    v2::PlanPipelineResponse resp;
    ASSERT_TRUE(stub_->PlanPipeline(&ctx, req, &resp).ok());
    EXPECT_EQ(generation, resp.generation());
    ASSERT_GT(resp.steps_size(), 0);
    int last_phase = 0;
    for (const auto &s : resp.steps()) {
      EXPECT_GE(static_cast<int>(s.phase()), last_phase) << "phase order";
      last_phase = s.phase();
      EXPECT_FALSE(s.operation().empty());
    }
  }

  {
    grpc::ClientContext ctx;
    v2::ApplyPipelineRequest req;
    *req.mutable_pipeline() = desired;
    req.set_expected_generation(generation);
    v2::ApplyPipelineResponse resp;
    const grpc::Status status = stub_->ApplyPipeline(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(generation + 1, resp.generation());
    EXPECT_GT(resp.applied_ops(), 0u);
  }

  {
    grpc::ClientContext ctx;
    v2::GetPipelineResponse resp;
    ASSERT_TRUE(stub_->GetPipeline(&ctx, {}, &resp).ok());
    EXPECT_EQ(generation + 1, resp.generation());
    EXPECT_EQ(2, resp.pipeline().modules_size());
    EXPECT_EQ(1, resp.pipeline().connections_size());
  }

  {  // Same desired state again: a no-op, no new generation.
    grpc::ClientContext ctx;
    v2::ApplyPipelineRequest req;
    *req.mutable_pipeline() = desired;
    v2::ApplyPipelineResponse resp;
    ASSERT_TRUE(stub_->ApplyPipeline(&ctx, req, &resp).ok());
    EXPECT_EQ(generation + 1, resp.generation());
    EXPECT_EQ(0u, resp.applied_ops());
    EXPECT_FALSE(resp.workers_paused());
  }
}

TEST_F(ApiV2ServiceTest, StaleGenerationIsAbortedWithTypedDetail) {
  grpc::ClientContext ctx;
  v2::ApplyPipelineRequest req;
  *req.mutable_pipeline() = SamplePipeline();
  req.set_expected_generation(bess::control::runtime().generation() + 100);
  v2::ApplyPipelineResponse resp;
  const grpc::Status status = stub_->ApplyPipeline(&ctx, req, &resp);
  EXPECT_EQ(grpc::StatusCode::ABORTED, status.error_code());
  EXPECT_EQ(v2::ErrorDetail::CONFLICT, DetailOf(ctx).code());

  grpc::ClientContext get_ctx;
  v2::GetPipelineResponse current;
  ASSERT_TRUE(stub_->GetPipeline(&get_ctx, {}, &current).ok());
  EXPECT_EQ(0, current.pipeline().modules_size()) << "nothing applied";
}

TEST_F(ApiV2ServiceTest, InvalidPipelineIsRejectedBeforeAnySideEffect) {
  v2::Pipeline bad = SamplePipeline();
  bad.mutable_connections(0)->set_downstream("nowhere");

  grpc::ClientContext ctx;
  v2::ApplyPipelineRequest req;
  *req.mutable_pipeline() = bad;
  v2::ApplyPipelineResponse resp;
  const grpc::Status status = stub_->ApplyPipeline(&ctx, req, &resp);
  EXPECT_FALSE(status.ok());
  const v2::ErrorDetail detail = DetailOf(ctx);
  EXPECT_NE(v2::ErrorDetail::CODE_UNSPECIFIED, detail.code());
  EXPECT_FALSE(detail.message().empty());

  grpc::ClientContext get_ctx;
  v2::GetPipelineResponse current;
  ASSERT_TRUE(stub_->GetPipeline(&get_ctx, {}, &current).ok());
  EXPECT_EQ(0, current.pipeline().modules_size());
}

}  // namespace
