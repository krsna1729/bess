// SPDX-License-Identifier: BSD-3-Clause

// G1: the v2 desired-state API. Adapter round-trips, then the whole service
// over a real in-process gRPC channel -- wire encoding, status codes and the
// typed error trailer included.

#include "control/api_v2.h"
#include "traffic_class.h"

#include <google/protobuf/util/message_differencer.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>

#include <memory>
#include <optional>
#include <string>

#include "control/control_plane.h"
#include "runtime/runtime_state.h"
#include "runtime/numa_placement.h"
#include "runtime/opts.h"
#include "packet_pool.h"
#include "port.h"
#include "dataplane/resource.h"
#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "pb/module_msg.pb.h"

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

  const auto spec = bess::control::FromProto(p);
  ASSERT_TRUE(spec.has_value()) << spec.error().message;
  const v2::Pipeline back = bess::control::ToProto(*spec);
  EXPECT_TRUE(MessageDifferencer::Equals(p, back))
      << p.DebugString() << "\nvs\n" << back.DebugString();
}

// D-123: an unset worker core (automatic) stays unset through the adapter,
// and an explicit core 0 stays explicit.
TEST(ApiV2AdapterTest, AnUnsetWorkerCoreStaysUnsetAndCoreZeroStaysSet) {
  v2::Pipeline p = SamplePipeline();
  p.mutable_workers(0)->clear_core();
  auto *pinned = p.add_workers();
  pinned->set_wid(1);
  pinned->set_core(0);

  const auto spec = bess::control::FromProto(p);
  ASSERT_TRUE(spec.has_value()) << spec.error().message;
  ASSERT_EQ(2u, spec->workers.size());
  EXPECT_FALSE(spec->workers[0].core.has_value());
  EXPECT_EQ(std::optional<int>(0), spec->workers[1].core);
  const v2::Pipeline back = bess::control::ToProto(*spec);
  EXPECT_FALSE(back.workers(0).has_core());
  ASSERT_TRUE(back.workers(1).has_core());
  EXPECT_EQ(0, back.workers(1).core());
}

// Wire integers wider than their C++ field are refused before the cast. Each
// rejected value is one that would otherwise wrap to a *valid* low value
// (65536 -> gate 0, 256 -> "one queue"), so no later validator could catch
// it; the maximum that fits is accepted.
TEST(ApiV2AdapterTest, NarrowingIsCheckedBeforeTheCast) {
  struct Case {
    const char *field;
    std::function<void(v2::Pipeline *, uint32_t)> set;
    uint32_t max;
  };
  const Case cases[] = {
      {"ogate",
       [](v2::Pipeline *p, uint32_t v) { p->mutable_connections(0)->set_ogate(v); },
       0xffff},
      {"igate",
       [](v2::Pipeline *p, uint32_t v) { p->mutable_connections(0)->set_igate(v); },
       0xffff},
      {"num_rx_queues",
       [](v2::Pipeline *p, uint32_t v) { p->mutable_ports(0)->set_num_rx_queues(v); },
       0xff},
      {"num_tx_queues",
       [](v2::Pipeline *p, uint32_t v) { p->mutable_ports(0)->set_num_tx_queues(v); },
       0xff},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.field);
    for (uint32_t wraps : {c.max + 1, c.max + 2, 0xffffffffu}) {
      v2::Pipeline p = SamplePipeline();
      auto *port = p.add_ports();
      port->set_name("p0");
      port->set_driver("PMDPort");
      c.set(&p, wraps);
      const auto spec = bess::control::FromProto(p);
      ASSERT_FALSE(spec.has_value()) << wraps << " was narrowed silently";
      EXPECT_EQ(c.field, spec.error().field);
      EXPECT_EQ(EINVAL, spec.error().err);
    }
    v2::Pipeline p = SamplePipeline();
    auto *port = p.add_ports();
    port->set_name("p0");
    port->set_driver("PMDPort");
    c.set(&p, c.max);
    EXPECT_TRUE(bess::control::FromProto(p).has_value()) << "max must fit";
  }
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

// The server as bessd builds it (D-026): the standard health service
// answers SERVING and reflection lists BESS's services, over generic calls
// (raw protobuf bytes) as a tool without BESS's protos would make them.
TEST(ControlServerTest, HealthAndReflection) {
  InitRuntimeOnce();
  bess::control::PrepareControlServer();
  ControlPlane control_plane;
  ControlV2Service service(control_plane);
  grpc::ServerBuilder builder;
  bess::control::ConfigureControlServer(&builder);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  ASSERT_NE(nullptr, server);
  auto channel = server->InProcessChannel(grpc::ChannelArguments());
  grpc::GenericStub generic(channel);
  auto bytes = [](const grpc::ByteBuffer &buffer) {
    grpc::Slice slice;
    EXPECT_TRUE(buffer.DumpToSingleSlice(&slice).ok());
    return std::string(reinterpret_cast<const char *>(slice.begin()),
                       slice.size());
  };

  // grpc.health.v1.Health/Check with an empty HealthCheckRequest: the reply
  // is HealthCheckResponse{status: SERVING (1)} = field 1, varint 1.
  {
    grpc::ClientContext context;
    grpc::Slice empty;
    grpc::ByteBuffer request(&empty, 1), reply;  // a valid, empty message
    grpc::Status status;
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    generic.UnaryCall(&context, "/grpc.health.v1.Health/Check",
                      grpc::StubOptions(), &request, &reply,
                      [&](grpc::Status s) {
                        std::lock_guard<std::mutex> lock(mu);
                        status = s;
                        done = true;
                        cv.notify_one();
                      });
    std::unique_lock<std::mutex> lock(mu);
    cv.wait(lock, [&] { return done; });
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(bytes(reply), std::string("\x08\x01", 2));
  }

#ifdef BESS_GRPC_REFLECTION
  // grpc.reflection.v1alpha.ServerReflection/ServerReflectionInfo with
  // {list_services: ""} (field 7, empty string); the reply names services.
  {
    grpc::ClientContext context;
    grpc::CompletionQueue cq;
    auto call = generic.PrepareCall(
        &context,
        "/grpc.reflection.v1alpha.ServerReflection/ServerReflectionInfo", &cq);
    void *tag = nullptr;
    bool ok = false;
    call->StartCall(reinterpret_cast<void *>(1));
    ASSERT_TRUE(cq.Next(&tag, &ok) && ok);
    const std::string list_services("\x3a\x00", 2);
    grpc::Slice slice(list_services);
    grpc::ByteBuffer request(&slice, 1);
    call->Write(request, reinterpret_cast<void *>(2));
    ASSERT_TRUE(cq.Next(&tag, &ok) && ok);
    grpc::ByteBuffer reply;
    call->Read(&reply, reinterpret_cast<void *>(3));
    ASSERT_TRUE(cq.Next(&tag, &ok) && ok);
    const std::string services = bytes(reply);
    EXPECT_NE(services.find("bess.pb.v2.Control"), std::string::npos);
    EXPECT_NE(services.find("grpc.health.v1.Health"), std::string::npos);
    call->WritesDone(reinterpret_cast<void *>(4));
    ASSERT_TRUE(cq.Next(&tag, &ok));
    grpc::Status status;
    call->Finish(&status, reinterpret_cast<void *>(5));
    ASSERT_TRUE(cq.Next(&tag, &ok));
  }
#endif
  server->Shutdown();
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
      EXPECT_TRUE(detail.ParseFromString(
          std::string(it->second.data(), it->second.size())));
    }
    return detail;
  }

  // The epoch every mutating request names (D-106), learned as a client does.
  uint64_t Epoch() {
    grpc::ClientContext ctx;
    v2::ListTransactionResourcesResponse resp;
    EXPECT_TRUE(stub_->ListTransactionResources(&ctx, {}, &resp).ok());
    EXPECT_NE(0u, resp.daemon_epoch());
    return resp.daemon_epoch();
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
    EXPECT_EQ(Epoch(), resp.daemon_epoch());
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
    EXPECT_EQ(Epoch(), resp.daemon_epoch());
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
    req.set_daemon_epoch(Epoch());
    v2::ApplyPipelineResponse resp;
    const grpc::Status status = stub_->ApplyPipeline(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(generation + 1, resp.generation());
    EXPECT_GT(resp.applied_ops(), 0u);
    EXPECT_EQ(Epoch(), resp.daemon_epoch());
  }

  {
    grpc::ClientContext ctx;
    v2::GetPipelineResponse resp;
    ASSERT_TRUE(stub_->GetPipeline(&ctx, {}, &resp).ok());
    EXPECT_EQ(generation + 1, resp.generation());
    EXPECT_EQ(Epoch(), resp.daemon_epoch());
    EXPECT_EQ(2, resp.pipeline().modules_size());
    EXPECT_EQ(1, resp.pipeline().connections_size());
  }

  {  // Same desired state again: a no-op, no new generation.
    grpc::ClientContext ctx;
    v2::ApplyPipelineRequest req;
    *req.mutable_pipeline() = desired;
    req.set_daemon_epoch(Epoch());
    v2::ApplyPipelineResponse resp;
    ASSERT_TRUE(stub_->ApplyPipeline(&ctx, req, &resp).ok());
    EXPECT_EQ(generation + 1, resp.generation());
    EXPECT_EQ(0u, resp.applied_ops());
    EXPECT_FALSE(resp.workers_paused());
  }
}

// D-123: a worker applied without a core gets one; GetPipeline names it in
// the pipeline and reports where it runs (node, cross-NUMA, shared core,
// control CPU); re-applying the desired state without a core is a no-op; and
// GetCapabilities carries the process placement.
TEST_F(ApiV2ServiceTest, AnAutomaticWorkerIsReportedWhereItRunsAndReapplyIsANoOp) {
  const bess::runtime::Placement &placement = bess::runtime::ProcessPlacement();
  if (placement.WorkerCpus().empty()) {
    GTEST_SKIP() << "no CPU for automatic workers: " << placement.Summary();
  }
  v2::Pipeline desired = SamplePipeline();
  desired.mutable_workers(0)->clear_core();

  uint64_t generation = 0;
  {
    grpc::ClientContext ctx;
    v2::ApplyPipelineRequest req;
    *req.mutable_pipeline() = desired;
    req.set_daemon_epoch(Epoch());
    v2::ApplyPipelineResponse resp;
    const grpc::Status status = stub_->ApplyPipeline(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    generation = resp.generation();
  }

  {
    grpc::ClientContext ctx;
    v2::GetPipelineResponse resp;
    ASSERT_TRUE(stub_->GetPipeline(&ctx, {}, &resp).ok());
    ASSERT_EQ(1, resp.pipeline().workers_size());
    ASSERT_TRUE(resp.pipeline().workers(0).has_core()) << "the CPU it runs on";
    const int core = resp.pipeline().workers(0).core();
    EXPECT_EQ(1u, placement.WorkerCpus().count(core)) << placement.Summary();
    ASSERT_EQ(1, resp.workers_size());
    const v2::WorkerPlacement &observed = resp.workers(0);
    EXPECT_EQ(0, observed.wid());
    EXPECT_EQ(core, observed.core());
    const bess::runtime::WorkerCpuReport expected = placement.DescribeWorkerCpu(core, {});
    EXPECT_EQ(expected.numa_node, observed.numa_node());
    EXPECT_EQ(expected.cross_numa, observed.cross_numa());
    EXPECT_EQ(expected.shares_core, observed.shares_core());
    EXPECT_EQ(expected.on_control_cpu, observed.on_control_cpu());
    EXPECT_FALSE(observed.on_control_cpu()) << "an automatic worker avoids control CPUs";
  }

  {  // The same desired state, core still unset: nothing to do.
    grpc::ClientContext ctx;
    v2::ApplyPipelineRequest req;
    *req.mutable_pipeline() = desired;
    req.set_daemon_epoch(Epoch());
    v2::ApplyPipelineResponse resp;
    ASSERT_TRUE(stub_->ApplyPipeline(&ctx, req, &resp).ok());
    EXPECT_EQ(generation, resp.generation());
    EXPECT_EQ(0u, resp.applied_ops());
    EXPECT_FALSE(resp.workers_paused());
  }

  {
    grpc::ClientContext ctx;
    v2::GetCapabilitiesResponse resp;
    ASSERT_TRUE(stub_->GetCapabilities(&ctx, {}, &resp).ok());
    EXPECT_EQ(placement.Summary(), resp.placement());
    EXPECT_EQ(placement.node, resp.memory_node());
  }
}

// The same argument encoded in another field order (protobuf's JSON-to-binary
// keeps the JSON key order) is the same desired state, not a replacement.
TEST_F(ApiV2ServiceTest, ArgumentFieldOrderIsNotAChange) {
  v2::Pipeline desired = SamplePipeline();
  bess::pb::SourceArg arg;
  arg.set_pkt_size(60);
  arg.set_burst(32);
  ASSERT_TRUE(desired.mutable_modules(0)->mutable_arg()->PackFrom(arg));
  desired.mutable_modules(0)->set_mclass("Source");
  v2::Pipeline reversed = desired;
  // burst (field 2) = 32, then pkt_size (field 1) = 60.
  reversed.mutable_modules(0)->mutable_arg()->set_value(std::string("\x10\x20\x08\x3c", 4));
  ASSERT_NE(reversed.modules(0).arg().value(), desired.modules(0).arg().value());

  uint64_t first = 0;
  for (const v2::Pipeline *p : {&reversed, &desired, &reversed}) {
    grpc::ClientContext ctx;
    v2::ApplyPipelineRequest req;
    *req.mutable_pipeline() = *p;
    req.set_daemon_epoch(Epoch());
    v2::ApplyPipelineResponse resp;
    const grpc::Status status = stub_->ApplyPipeline(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    if (first == 0) {
      first = resp.generation();
      continue;
    }
    EXPECT_EQ(first, resp.generation()) << "only the first apply changes anything";
    EXPECT_EQ(0u, resp.applied_ops());
  }
}

TEST_F(ApiV2ServiceTest, StaleGenerationIsAbortedWithTypedDetail) {
  grpc::ClientContext ctx;
  v2::ApplyPipelineRequest req;
  *req.mutable_pipeline() = SamplePipeline();
  req.set_expected_generation(bess::runtime::runtime().generation() + 100);
  req.set_daemon_epoch(Epoch());
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
  req.set_daemon_epoch(Epoch());
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


// Every RPC that takes a pipeline refuses a wrapping wire value with a typed
// INVALID_ARGUMENT and changes nothing.
TEST_F(ApiV2ServiceTest, OutOfRangeWireValuesAreRefusedEverywhere) {
  v2::Pipeline bad = SamplePipeline();
  bad.mutable_connections(0)->set_ogate(65536);  // would be gate 0
  const uint64_t generation = bess::runtime::runtime().generation();

  const auto check = [&](grpc::ClientContext &ctx, const grpc::Status &st) {
    EXPECT_EQ(grpc::StatusCode::INVALID_ARGUMENT, st.error_code());
    EXPECT_EQ("ogate", DetailOf(ctx).field());
  };
  {
    grpc::ClientContext ctx;
    v2::ValidatePipelineRequest req;
    *req.mutable_pipeline() = bad;
    v2::ValidatePipelineResponse resp;
    check(ctx, stub_->ValidatePipeline(&ctx, req, &resp));
  }
  {
    grpc::ClientContext ctx;
    v2::DiffPipelineRequest req;
    *req.mutable_pipeline() = bad;
    v2::DiffPipelineResponse resp;
    check(ctx, stub_->DiffPipeline(&ctx, req, &resp));
  }
  {
    grpc::ClientContext ctx;
    v2::PlanPipelineRequest req;
    *req.mutable_pipeline() = bad;
    v2::PlanPipelineResponse resp;
    check(ctx, stub_->PlanPipeline(&ctx, req, &resp));
  }
  {
    grpc::ClientContext ctx;
    v2::ApplyPipelineRequest req;
    *req.mutable_pipeline() = bad;
    req.set_daemon_epoch(Epoch());
    v2::ApplyPipelineResponse resp;
    check(ctx, stub_->ApplyPipeline(&ctx, req, &resp));
  }
  EXPECT_EQ(generation, bess::runtime::runtime().generation());
  grpc::ClientContext ctx;
  v2::GetPipelineResponse resp;
  ASSERT_TRUE(stub_->GetPipeline(&ctx, {}, &resp).ok());
  EXPECT_EQ(0, resp.pipeline().modules_size()) << "nothing was created";
}

// A mutating request names the daemon epoch it was built under: without one it
// is invalid, under another (a restart since) it is refused with the daemon's
// epoch; either way before any side effect, and a refused transaction's
// request id is never recorded (D-106).
TEST_F(ApiV2ServiceTest, MutatingRequestsMustNameTheDaemonsEpoch) {
  for (const uint64_t sent : {uint64_t{0}, Epoch() + 1}) {
    SCOPED_TRACE(sent);
    const grpc::StatusCode want = sent == 0 ? grpc::StatusCode::INVALID_ARGUMENT
                                            : grpc::StatusCode::FAILED_PRECONDITION;
    const auto want_code =
        sent == 0 ? v2::ErrorDetail::INVALID_ARGUMENT : v2::ErrorDetail::EPOCH_MISMATCH;
    {
      grpc::ClientContext ctx;
      v2::ApplyPipelineRequest req;
      *req.mutable_pipeline() = SamplePipeline();
      req.set_daemon_epoch(sent);
      v2::ApplyPipelineResponse resp;
      EXPECT_EQ(want, stub_->ApplyPipeline(&ctx, req, &resp).error_code());
      const v2::ErrorDetail detail = DetailOf(ctx);
      EXPECT_EQ(want_code, detail.code());
      EXPECT_EQ("daemon_epoch", detail.field());
      EXPECT_EQ(sent == 0 ? 0 : Epoch(), detail.daemon_epoch());
    }
    {
      grpc::ClientContext ctx;
      v2::ApplyTransactionRequest req;
      req.set_request_id("epoch-refused");
      req.set_daemon_epoch(sent);
      v2::ApplyTransactionResponse resp;
      EXPECT_EQ(want, stub_->ApplyTransaction(&ctx, req, &resp).error_code());
      EXPECT_EQ(want_code, DetailOf(ctx).code());
    }
    grpc::ClientContext get_ctx;
    v2::GetPipelineResponse current;
    ASSERT_TRUE(stub_->GetPipeline(&get_ctx, {}, &current).ok());
    EXPECT_EQ(0, current.pipeline().modules_size()) << "nothing applied";
    grpc::ClientContext tx_ctx;
    v2::GetTransactionRequest tx_req;
    tx_req.set_request_id("epoch-refused");
    v2::GetTransactionResponse tx;
    ASSERT_TRUE(stub_->GetTransaction(&tx_ctx, tx_req, &tx).ok());
    EXPECT_FALSE(tx.known()) << "a refused request id is not recorded";
  }
}

// A module's commands over v2 (D-115): epoch-checked before anything runs; the
// answer comes back as the command's Any; a missing module, a command the
// class does not have and a command's own failure are typed errors carrying
// the module and the command's errno.
TEST_F(ApiV2ServiceTest, RunModuleCommand) {
  google::protobuf::Any arg;
  ASSERT_TRUE(arg.PackFrom(bess::pb::L2ForwardArg()));
  ASSERT_TRUE(control_plane_->CreateModule({"l2", "L2Forward", arg}));

  auto run = [&](const std::string &module, const std::string &command,
                 const google::protobuf::Message &command_arg, uint64_t epoch,
                 v2::RunModuleCommandResponse *resp, v2::ErrorDetail *detail = nullptr) {
    grpc::ClientContext ctx;
    v2::RunModuleCommandRequest req;
    req.set_daemon_epoch(epoch);
    req.set_module(module);
    req.set_command(command);
    EXPECT_TRUE(req.mutable_arg()->PackFrom(command_arg));
    const grpc::Status status = stub_->RunModuleCommand(&ctx, req, resp);
    if (detail != nullptr) {
      *detail = DetailOf(ctx);
    }
    return status.error_code();
  };
  bess::pb::L2ForwardCommandAddArg add;
  auto *entry = add.add_entries();
  entry->set_addr("02:00:00:00:00:01");
  entry->set_gate(3);
  bess::pb::L2ForwardCommandLookupArg lookup;
  lookup.add_addrs("02:00:00:00:00:01");

  v2::RunModuleCommandResponse resp;
  v2::ErrorDetail detail;
  ASSERT_EQ(grpc::StatusCode::INVALID_ARGUMENT, run("l2", "add", add, 0, &resp, &detail));
  EXPECT_EQ("daemon_epoch", detail.field());
  EXPECT_NE(grpc::StatusCode::OK, run("l2", "lookup", lookup, Epoch(), &resp))
      << "the refused add ran";

  ASSERT_EQ(grpc::StatusCode::OK, run("l2", "add", add, Epoch(), &resp));
  EXPECT_FALSE(resp.has_data()) << "add answers nothing";
  EXPECT_EQ(Epoch(), resp.daemon_epoch());
  ASSERT_EQ(grpc::StatusCode::OK, run("l2", "lookup", lookup, Epoch(), &resp));
  bess::pb::L2ForwardCommandLookupResponse found;
  ASSERT_TRUE(resp.data().UnpackTo(&found));
  ASSERT_EQ(1, found.gates_size());
  EXPECT_EQ(3u, found.gates(0));

  EXPECT_EQ(grpc::StatusCode::NOT_FOUND, run("nope", "lookup", lookup, Epoch(), &resp, &detail));
  EXPECT_EQ(v2::ErrorDetail::NOT_FOUND, detail.code());
  EXPECT_EQ("nope", detail.object());

  EXPECT_EQ(grpc::StatusCode::INVALID_ARGUMENT, run("l2", "nope", lookup, Epoch(), &resp, &detail));
  EXPECT_EQ(ENOTSUP, detail.errno_value());
  EXPECT_EQ("command", detail.field());

  // The command's own refusal: the address is already there.
  EXPECT_NE(grpc::StatusCode::OK, run("l2", "add", add, Epoch(), &resp, &detail));
  EXPECT_NE(0, detail.errno_value()) << detail.message();
  EXPECT_EQ("l2", detail.object());
}

// A command not marked thread-safe is refused (EBUSY) while a worker runs
// the module, after the v2 path ApplyPipeline + resume: the resume works out
// which workers run each module. Paused, it runs.
TEST_F(ApiV2ServiceTest, ThreadUnsafeCommandWaitsForPausedWorkers) {
  v2::Pipeline p;
  auto *w = p.add_workers();
  w->set_wid(0);
  w->set_core(0);
  auto *q = p.add_modules();
  q->set_name("q");
  q->set_mclass("Queue");
  ASSERT_TRUE(q->mutable_arg()->PackFrom(bess::pb::QueueArg()));
  auto *t = p.add_tasks();
  t->set_module("q");
  t->set_wid(0);
  {
    grpc::ClientContext ctx;
    v2::ApplyPipelineRequest req;
    *req.mutable_pipeline() = p;
    req.set_daemon_epoch(Epoch());
    v2::ApplyPipelineResponse resp;
    const grpc::Status status = stub_->ApplyPipeline(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  auto set_size = [&](v2::ErrorDetail *detail) {
    grpc::ClientContext ctx;
    v2::RunModuleCommandRequest req;
    req.set_daemon_epoch(Epoch());
    req.set_module("q");
    req.set_command("set_size");
    bess::pb::QueueCommandSetSizeArg arg;
    arg.set_size(256);
    EXPECT_TRUE(req.mutable_arg()->PackFrom(arg));
    v2::RunModuleCommandResponse resp;
    const grpc::StatusCode code = stub_->RunModuleCommand(&ctx, req, &resp).error_code();
    *detail = DetailOf(ctx);
    return code;
  };
  v2::ErrorDetail detail;
  ASSERT_TRUE(control_plane_->ResumeAll());
  EXPECT_NE(grpc::StatusCode::OK, set_size(&detail)) << "ran beside the running worker";
  EXPECT_EQ(EBUSY, detail.errno_value());
  EXPECT_EQ(v2::ErrorDetail::RESOURCE_BUSY, detail.code());
  ASSERT_TRUE(control_plane_->PauseAll());
  EXPECT_EQ(grpc::StatusCode::OK, set_size(&detail)) << detail.message();
}

// Counts the PreResume events it gets.
class PreResumeCounter final : public Module {
 public:
  CommandResponse Init(const bess::pb::EmptyArg &) { return CommandSuccess(); }
  int OnEvent(bess::Event event) override {
    if (event != bess::Event::PreResume) {
      return -ENOTSUP;
    }
    events++;
    return 0;
  }
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    RunNextModule(ctx, batch);
  }
  static inline std::atomic<int> events = 0;
};
ADD_MODULE(PreResumeCounter, "pre_resume_counter", "test: counts PreResume events")

// A worker resumed alone (the legacy per-worker resume) first sends PreResume
// to the modules it runs, so they rebind to what changed while it was paused.
TEST_F(ApiV2ServiceTest, ResumingOneWorkerSendsPreResumeToItsModules) {
  v2::Pipeline p;
  auto *w = p.add_workers();
  w->set_wid(0);
  w->set_core(0);
  auto *q = p.add_modules();
  q->set_name("q");
  q->set_mclass("Queue");
  ASSERT_TRUE(q->mutable_arg()->PackFrom(bess::pb::QueueArg()));
  auto *c = p.add_modules();
  c->set_name("c");
  c->set_mclass("PreResumeCounter");
  auto *conn = p.add_connections();
  conn->set_upstream("q");
  conn->set_downstream("c");
  auto *t = p.add_tasks();
  t->set_module("q");
  t->set_wid(0);
  {
    grpc::ClientContext ctx;
    v2::ApplyPipelineRequest req;
    *req.mutable_pipeline() = p;
    req.set_daemon_epoch(Epoch());
    v2::ApplyPipelineResponse resp;
    const grpc::Status status = stub_->ApplyPipeline(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }
  ASSERT_TRUE(control_plane_->ResumeAll());
  ASSERT_TRUE(control_plane_->PauseWorker(0));
  const int before = PreResumeCounter::events;
  ASSERT_TRUE(control_plane_->ResumeWorker(0));
  EXPECT_EQ(before + 1, PreResumeCounter::events);
  ASSERT_TRUE(control_plane_->PauseAll());
}

// The int64 traffic-class fields are range-checked against the scheduler's
// narrower types instead of being narrowed: priority 2^32 + 5 would become 5,
// share 2^32 + 1 would become 1.
TEST_F(ApiV2ServiceTest, TrafficClassRangesAreValidatedNotNarrowed) {
  struct Case {
    const char *field;
    std::function<void(v2::TrafficClass *)> set;
  };
  const Case cases[] = {
      {"priority", [](v2::TrafficClass *t) { t->set_priority((int64_t{1} << 32) + 5); }},
      {"priority", [](v2::TrafficClass *t) { t->set_priority(-1); }},
      {"share", [](v2::TrafficClass *t) { t->set_share((int64_t{1} << 32) + 1); }},
      {"share", [](v2::TrafficClass *t) { t->set_share(0); }},
      {"share", [](v2::TrafficClass *t) { t->set_share(-3); }},
      {"limit", [](v2::TrafficClass *t) { (*t->mutable_limit())["count"] = -1; }},
      {"max_burst", [](v2::TrafficClass *t) { (*t->mutable_max_burst())["count"] = -1; }},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.field);
    v2::Pipeline p = SamplePipeline();
    auto *tc = p.mutable_traffic_classes(1);  // "child"
    tc->clear_share();
    c.set(tc);
    grpc::ClientContext ctx;
    v2::ValidatePipelineRequest req;
    *req.mutable_pipeline() = p;
    v2::ValidatePipelineResponse resp;
    const grpc::Status st = stub_->ValidatePipeline(&ctx, req, &resp);
    EXPECT_EQ(grpc::StatusCode::INVALID_ARGUMENT, st.error_code());
    EXPECT_EQ(c.field, DetailOf(ctx).field());
  }
}

// The legacy (v1) path reaches AddTc without the pipeline validator, so the
// shared path range-checks the int64 fields before narrowing them to the
// scheduler's types: priority 2^32 + 5 must not become 5, share 2^32 + 1 must
// not become 1, a negative limit must not become ~1.8e19. A refused class is
// not created.
TEST_F(ApiV2ServiceTest, LegacyAddTcRangeChecksBeforeNarrowing) {
  ControlPlane &control_plane = *control_plane_;
  bess::control::TrafficClassSpec prio;
  prio.name = "rc_prio";
  prio.policy = "priority";
  ASSERT_TRUE(control_plane.AddTc(prio).has_value());
  bess::control::TrafficClassSpec wfq;
  wfq.name = "rc_wfq";
  wfq.policy = "weighted_fair";
  wfq.resource = "count";
  ASSERT_TRUE(control_plane.AddTc(wfq).has_value());

  bess::control::TrafficClassSpec child;
  child.name = "rc_child";
  child.policy = "round_robin";
  child.parent = "rc_prio";
  child.has_priority = true;
  child.priority = (int64_t{1} << 32) + 5;
  EXPECT_EQ(EINVAL, control_plane.AddTc(child).error().err);
  child.priority = -1;
  EXPECT_EQ(EINVAL, control_plane.AddTc(child).error().err);

  child.parent = "rc_wfq";
  child.has_priority = false;
  child.has_share = true;
  for (int64_t share : {(int64_t{1} << 32) + 1, int64_t{0}, int64_t{-3}}) {
    child.share = share;
    EXPECT_EQ(EINVAL, control_plane.AddTc(child).error().err) << share;
  }
  EXPECT_EQ(nullptr, bess::TrafficClassBuilder::Find("rc_child"))
      << "a refused class must not be left behind";

  bess::control::TrafficClassSpec limited;
  limited.name = "rc_limited";
  limited.policy = "rate_limit";
  limited.resource = "count";
  limited.limit["count"] = -1;
  EXPECT_EQ(EINVAL, control_plane.AddTc(limited).error().err);
  limited.limit["count"] = 1000;
  limited.max_burst["count"] = -1;
  EXPECT_EQ(EINVAL, control_plane.AddTc(limited).error().err);
  EXPECT_EQ(nullptr, bess::TrafficClassBuilder::Find("rc_limited"));

  child.share = 5;  // in range: accepted
  EXPECT_TRUE(control_plane.AddTc(child).has_value());
}

// A resource whose key and value types no class or command names (control_v2's
// own messages: nothing else in the schema imports control_v2.proto).
class SchemaOnlyResource final : public bess::dataplane::Resource {
 public:
  SchemaOnlyResource() : Resource("schema_only/r") {}
  bool Contains(const bess::dataplane::ResourceKey &) const override { return false; }
  std::expected<Reservation, std::string> Reserve(const bess::dataplane::Op &) override {
    return std::unexpected("unused");
  }
  size_t LiveCount() const override { return 0; }
};

// Schema discovery is what a generic client (bessctl, a manifest, a UI)
// writes arguments from. Every class and driver names its init type, every
// command's hand-written type name resolves to a message (a typo would leave
// that command unusable to such a client), and the descriptor set builds on
// its own, in its order, into an empty pool.
TEST_F(ApiV2ServiceTest, SchemaNamesEveryArgumentTypeAndCarriesItsDescriptors) {
  SchemaOnlyResource resource;
  bess::dataplane::TransactionEngine &engine = bess::runtime::runtime().transactions();
  ASSERT_TRUE(engine.registry().Register(&resource));
  bess::framework::ResourceBinding binding = bess::framework::ResourceBindings::ProcessDefault().Bind(
      resource, std::make_shared<bess::framework::TypedCodec<v2::Worker, v2::Connection>>(
                    [](const v2::Worker &) -> std::expected<bess::dataplane::ResourceKey, std::string> {
                      return std::unexpected("unused");
                    },
                    [](const v2::Connection &) -> std::expected<std::any, std::string> {
                      return std::unexpected("unused");
                    }));
  grpc::ClientContext ctx;
  v2::GetSchemaResponse resp;
  const grpc::Status status = stub_->GetSchema(&ctx, v2::GetSchemaRequest(), &resp);
  binding.Reset();
  ASSERT_TRUE(engine.registry().Unregister("schema_only/r"));
  ASSERT_TRUE(status.ok());
  ASSERT_GT(resp.module_classes_size(), 0);
  ASSERT_GT(resp.port_drivers_size(), 0);

  google::protobuf::DescriptorPool pool;  // only what the daemon sent
  for (const auto &file : resp.descriptors().file()) {
    ASSERT_NE(nullptr, pool.BuildFile(file)) << file.name();
  }
  const v2::ModuleClassSchema *exact_match = nullptr;
  for (const v2::ModuleClassSchema &c : resp.module_classes()) {
    EXPECT_NE(nullptr, pool.FindMessageTypeByName(c.init_arg_type()))
        << c.name() << " init '" << c.init_arg_type() << "'";
    for (const v2::CommandSchema &cmd : c.commands()) {
      EXPECT_NE(nullptr, pool.FindMessageTypeByName(cmd.arg_type()))
          << c.name() << "." << cmd.name() << " '" << cmd.arg_type() << "'";
    }
    if (c.name() == "ExactMatch") {
      exact_match = &c;
    }
  }
  for (const v2::PortDriverSchema &d : resp.port_drivers()) {
    EXPECT_NE(nullptr, pool.FindMessageTypeByName(d.init_arg_type()))
        << d.name() << " init '" << d.init_arg_type() << "'";
  }
  // The resource's types, from a file nothing else above brings in.
  EXPECT_NE(nullptr, pool.FindMessageTypeByName("bess.pb.v2.Worker"));
  EXPECT_NE(nullptr, pool.FindMessageTypeByName("bess.pb.v2.Connection"));

  ASSERT_NE(nullptr, exact_match);
  EXPECT_EQ("bess.pb.ExactMatchArg", exact_match->init_arg_type());
  EXPECT_EQ(1u, exact_match->igates());
  bool add_found = false;
  for (const v2::CommandSchema &cmd : exact_match->commands()) {
    if (cmd.name() == "add") {
      add_found = true;
      EXPECT_EQ("bess.pb.ExactMatchCommandAddArg", cmd.arg_type());
    }
  }
  EXPECT_TRUE(add_found);
}

}  // namespace

