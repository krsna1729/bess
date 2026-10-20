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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <functional>
#include <mutex>

#include <memory>
#include <optional>
#include <set>
#include <string>

#include "control/control_plane.h"
#include "runtime/runtime_state.h"
#include "runtime/numa_placement.h"
#include "runtime/simd_width.h"
#include "runtime/opts.h"
#include "packet_pool.h"
#include "port.h"
#include "dataplane/resource.h"
#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "pb/module_msg.pb.h"
#include "worker.h"

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
    // The limits bessd's control server configures: a test that reads a table
    // above 64 MiB must not pass because the client's 4 MiB default refused it.
    builder.SetMaxReceiveMessageSize(bess::control::kMaxMessageBytes);
    builder.SetMaxSendMessageSize(bess::control::kMaxMessageBytes);
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    ASSERT_NE(nullptr, server_);
    grpc::ChannelArguments arguments;
    arguments.SetMaxReceiveMessageSize(bess::control::kMaxMessageBytes);
    arguments.SetMaxSendMessageSize(bess::control::kMaxMessageBytes);
    stub_ = v2::Control::NewStub(server_->InProcessChannel(arguments));
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

// GetCapabilities carries the SIMD width bessd decided at startup (D-126),
// the one BESS's kernels run at.
TEST_F(ApiV2ServiceTest, CapabilitiesCarryTheSimdWidth) {
  grpc::ClientContext ctx;
  v2::GetCapabilitiesResponse resp;
  ASSERT_TRUE(stub_->GetCapabilities(&ctx, {}, &resp).ok());
  const bess::runtime::SimdWidth &simd = bess::runtime::ProcessSimdWidth();
  EXPECT_EQ(simd.Summary(), resp.simd());
  EXPECT_EQ(simd.effective, resp.simd_width());
  EXPECT_EQ(bess::arch::SimdWidth(), resp.simd_width());
  EXPECT_GE(resp.dpdk_simd_width(), resp.simd_width()) << "DPDK's own limit, after the EAL";
  if (bess::arch::kCpuSimdWidthKnown) {
    EXPECT_EQ(resp.simd_width(), resp.dpdk_simd_width());
  }
}

// No free CPU for an automatic worker is RESOURCE_EXHAUSTED on the wire --
// not UNAVAILABLE, which clients retry as a lost answer -- with the detail.
TEST_F(ApiV2ServiceTest, NoFreeCpuForAnAutomaticWorkerIsResourceExhausted) {
  const bess::runtime::Placement &placement = bess::runtime::ProcessPlacement();
  const std::set<int> cpus = placement.WorkerCpus();
  if (cpus.size() + 1 > static_cast<size_t>(Worker::kMaxWorkers)) {
    GTEST_SKIP() << "more worker CPUs than workers: " << placement.Summary();
  }
  v2::Pipeline desired = SamplePipeline();
  desired.clear_workers();
  int wid = 0;
  for (int cpu : cpus) {  // every worker CPU taken explicitly
    auto *w = desired.add_workers();
    w->set_wid(wid++);
    w->set_core(cpu);
  }
  desired.add_workers()->set_wid(wid);  // and one automatic worker

  grpc::ClientContext ctx;
  v2::ApplyPipelineRequest req;
  *req.mutable_pipeline() = desired;
  req.set_daemon_epoch(Epoch());
  v2::ApplyPipelineResponse resp;
  const grpc::Status status = stub_->ApplyPipeline(&ctx, req, &resp);
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(grpc::StatusCode::RESOURCE_EXHAUSTED, status.error_code())
      << status.error_message();
  EXPECT_NE(std::string::npos, status.error_message().find("no free CPU"))
      << status.error_message();
  const auto detail = ctx.GetServerTrailingMetadata().find("bess-error-bin");
  ASSERT_NE(detail, ctx.GetServerTrailingMetadata().end());
  v2::ErrorDetail decoded;
  ASSERT_TRUE(decoded.ParseFromString(
      std::string(detail->second.data(), detail->second.size())));
  EXPECT_EQ(ENOSPC, decoded.errno_value());
  EXPECT_EQ("core", decoded.field());
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

// A module's actions over v2 (D-129): epoch-checked before anything runs; the
// result comes back as the action's Any; a missing module, an action the
// class does not have, a command that edits state (not an action), and an
// action's own failure are typed errors carrying the module and the errno.
TEST_F(ApiV2ServiceTest, InvokeAction) {
  google::protobuf::Any arg;
  ASSERT_TRUE(arg.PackFrom(bess::pb::L2ForwardArg()));
  ASSERT_TRUE(control_plane_->CreateModule({"l2", "L2Forward", arg}));

  auto invoke = [&](const std::string &module, const std::string &action,
                    const google::protobuf::Message &action_arg, uint64_t epoch,
                    v2::InvokeActionResponse *resp, v2::ErrorDetail *detail = nullptr) {
    grpc::ClientContext ctx;
    v2::InvokeActionRequest req;
    req.set_daemon_epoch(epoch);
    req.set_module(module);
    req.set_action(action);
    EXPECT_TRUE(req.mutable_arg()->PackFrom(action_arg));
    const grpc::Status status = stub_->InvokeAction(&ctx, req, resp);
    if (detail != nullptr) {
      *detail = DetailOf(ctx);
    }
    return status.error_code();
  };
  bess::pb::L2ForwardCommandPopulateArg populate;
  populate.set_base("02:00:00:00:00:01");
  populate.set_count(1);
  populate.set_gate_count(1);
  bess::pb::L2ForwardCommandLookupArg lookup;
  lookup.add_addrs("02:00:00:00:00:01");

  v2::InvokeActionResponse resp;
  v2::ErrorDetail detail;
  ASSERT_EQ(grpc::StatusCode::INVALID_ARGUMENT,
            invoke("l2", "populate", populate, 0, &resp, &detail));
  EXPECT_EQ("daemon_epoch", detail.field());
  EXPECT_NE(grpc::StatusCode::OK, invoke("l2", "lookup", lookup, Epoch(), &resp))
      << "the refused populate ran";

  ASSERT_EQ(grpc::StatusCode::OK, invoke("l2", "populate", populate, Epoch(), &resp));
  EXPECT_FALSE(resp.has_result()) << "populate answers nothing";
  EXPECT_EQ(Epoch(), resp.daemon_epoch());
  ASSERT_EQ(grpc::StatusCode::OK, invoke("l2", "lookup", lookup, Epoch(), &resp));
  bess::pb::L2ForwardCommandLookupResponse found;
  ASSERT_TRUE(resp.result().UnpackTo(&found));
  ASSERT_EQ(1, found.gates_size());
  EXPECT_EQ(0u, found.gates(0));

  EXPECT_EQ(grpc::StatusCode::NOT_FOUND,
            invoke("nope", "lookup", lookup, Epoch(), &resp, &detail));
  EXPECT_EQ(v2::ErrorDetail::NOT_FOUND, detail.code());
  EXPECT_EQ("nope", detail.object());

  EXPECT_EQ(grpc::StatusCode::INVALID_ARGUMENT,
            invoke("l2", "nope", lookup, Epoch(), &resp, &detail));
  EXPECT_EQ(ENOTSUP, detail.errno_value());
  EXPECT_EQ("action", detail.field());

  // The same state through the resource, as a client writes it (D-140): the
  // entry the legacy `add` would have written.
  bess::pb::L2ForwardCommandLookupArg lookup_added;
  lookup_added.add_addrs("02:00:00:00:00:09");
  {
    bess::pb::L2ForwardMacKey key;
    key.set_addr("02:00:00:00:00:09");
    bess::pb::L2ForwardMacValue value;
    value.set_gate(3);
    grpc::ClientContext ctx;
    v2::ApplyTransactionRequest req;
    req.set_request_id("l2-macs-add");
    req.set_daemon_epoch(Epoch());
    v2::TransactionOp *op = req.add_ops();
    op->set_resource("l2/macs");
    ASSERT_TRUE(op->mutable_key()->PackFrom(key));
    ASSERT_TRUE(op->mutable_value()->PackFrom(value));
    v2::ApplyTransactionResponse applied;
    const grpc::Status status = stub_->ApplyTransaction(&ctx, req, &applied);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }
  EXPECT_EQ(grpc::StatusCode::OK, invoke("l2", "lookup", lookup_added, Epoch(), &resp));

  // The action's own refusal: the base is not a MAC address.
  populate.set_base("not a mac");
  EXPECT_NE(grpc::StatusCode::OK, invoke("l2", "populate", populate, Epoch(), &resp, &detail));
  EXPECT_EQ(EINVAL, detail.errno_value()) << detail.message();
  EXPECT_EQ("l2", detail.object());
}

// ReadResource streams a table in bounded pages: no single message carries the
// whole resource, so a table above the message limit stays readable. The test
// uses five rules and a page of two; the wire shape is the claim, not the
// size, and the generation/epoch are the snapshot's.
TEST_F(ApiV2ServiceTest, ReadResourcePagesTheTable) {
  bess::pb::ExactMatchArg em;
  bess::pb::Field *field = em.add_fields();
  field->set_offset(26);
  field->set_num_bytes(4);
  google::protobuf::Any em_arg;
  ASSERT_TRUE(em_arg.PackFrom(em));
  ASSERT_TRUE(control_plane_->CreateModule({"em", "ExactMatch", em_arg}));
  ASSERT_TRUE(control_plane_->CreateModule({"empty", "ExactMatch", em_arg}));

  {
    grpc::ClientContext ctx;
    v2::ApplyTransactionRequest req;
    req.set_request_id("stream-read-rules");
    req.set_daemon_epoch(Epoch());
    for (char i = 0; i < 5; i++) {
      bess::pb::ExactMatchRuleKey key;
      key.add_fields()->set_value_bin(std::string(4, static_cast<char>('a' + i)));
      bess::pb::ExactMatchRuleValue value;
      value.set_gate(1);
      v2::TransactionOp *op = req.add_ops();
      op->set_resource("em/rules");
      ASSERT_TRUE(op->mutable_key()->PackFrom(key));
      ASSERT_TRUE(op->mutable_value()->PackFrom(value));
    }
    v2::ApplyTransactionResponse applied;
    const grpc::Status status = stub_->ApplyTransaction(&ctx, req, &applied);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  {
    grpc::ClientContext ctx;
    v2::ReadResourceRequest req;
    req.set_resource("em/rules");
    req.set_max_entries(2);
    auto reader = stub_->ReadResource(&ctx, req);
    v2::ResourcePage page;
    int pages = 0;
    int entries = 0;
    while (reader->Read(&page)) {
      pages++;
      entries += page.entries_size();
      EXPECT_LE(page.entries_size(), 2) << "one page above the requested bound";
      EXPECT_EQ("em/rules", page.resource());
      EXPECT_NE(0u, page.resource_generation());
      EXPECT_EQ(Epoch(), page.daemon_epoch());
    }
    ASSERT_TRUE(reader->Finish().ok());
    EXPECT_EQ(5, entries);
    EXPECT_EQ(3, pages) << "five entries at two per page";
  }

  // An empty resource still answers one page, so the caller learns the
  // resource exists and its generation.
  {
    grpc::ClientContext ctx;
    v2::ReadResourceRequest req;
    req.set_resource("empty/rules");
    auto reader = stub_->ReadResource(&ctx, req);
    v2::ResourcePage page;
    int pages = 0;
    int entries = 0;
    while (reader->Read(&page)) {
      pages++;
      entries += page.entries_size();
    }
    ASSERT_TRUE(reader->Finish().ok());
    EXPECT_EQ(1, pages);
    EXPECT_EQ(0, entries);
  }

  // A name no module registered is NOT_FOUND, as GetPipeline's is.
  {
    grpc::ClientContext ctx;
    v2::ReadResourceRequest req;
    req.set_resource("no-such/resource");
    auto reader = stub_->ReadResource(&ctx, req);
    v2::ResourcePage page;
    (void)reader->Read(&page);
    const grpc::Status status = reader->Finish();
    EXPECT_EQ(grpc::StatusCode::NOT_FOUND, status.error_code());
  }
}

// The page ceiling is the serialized page, framing included. A four-byte rule
// is about 114 bytes of entry and two more inside the page (the repeated
// field's tag and length prefix), so 30,000 rules cannot fit in one 3 MiB
// page. The entry bound is raised past the number of rules, so only the byte
// bound can split the stream.
TEST_F(ApiV2ServiceTest, ReadResourceBoundsTheSerializedPage) {
  bess::pb::ExactMatchArg em;
  bess::pb::Field *field = em.add_fields();
  field->set_offset(26);
  field->set_num_bytes(4);
  google::protobuf::Any em_arg;
  ASSERT_TRUE(em_arg.PackFrom(em));
  // A long module name grows the page's own fields (its resource string), so
  // the assertion also catches a budget that forgets to count them.
  constexpr char kPaged[] =
      "paged_rules_serialized_page_ceiling_regression_module";
  const std::string resource = std::string(kPaged) + "/rules";
  ASSERT_TRUE(control_plane_->CreateModule({kPaged, "ExactMatch", em_arg}));

  constexpr uint32_t kRules = 30000;
  constexpr uint32_t kChunk = 20000;
  for (uint32_t start = 0; start < kRules; start += kChunk) {
    grpc::ClientContext ctx;
    v2::ApplyTransactionRequest req;
    req.set_request_id("stream-page-" + std::to_string(start));
    req.set_daemon_epoch(Epoch());
    const uint32_t end = std::min(kRules, start + kChunk);
    for (uint32_t i = start; i < end; i++) {
      std::string key(4, '\0');
      key[0] = static_cast<char>((i >> 24) & 0xff);
      key[1] = static_cast<char>((i >> 16) & 0xff);
      key[2] = static_cast<char>((i >> 8) & 0xff);
      key[3] = static_cast<char>(i & 0xff);
      bess::pb::ExactMatchRuleKey rule_key;
      rule_key.add_fields()->set_value_bin(key);
      bess::pb::ExactMatchRuleValue rule_value;
      rule_value.set_gate(1);
      v2::TransactionOp *op = req.add_ops();
      op->set_resource(resource);
      ASSERT_TRUE(op->mutable_key()->PackFrom(rule_key));
      ASSERT_TRUE(op->mutable_value()->PackFrom(rule_value));
    }
    v2::ApplyTransactionResponse applied;
    const grpc::Status status = stub_->ApplyTransaction(&ctx, req, &applied);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  grpc::ClientContext ctx;
  v2::ReadResourceRequest req;
  req.set_resource(resource);
  req.set_max_entries(65536);
  auto reader = stub_->ReadResource(&ctx, req);
  v2::ResourcePage page;
  uint32_t pages = 0;
  uint32_t entries = 0;
  while (reader->Read(&page)) {
    pages++;
    entries += page.entries_size();
    EXPECT_LE(page.ByteSizeLong(), 3u << 20)
        << "a page's serialized size is the ceiling, framing included";
  }
  ASSERT_TRUE(reader->Finish().ok());
  EXPECT_EQ(kRules, entries);
  EXPECT_GT(pages, 1u) << "the byte bound split the table, not the entry count";
}

// A table above the message limit is the reason the stream exists: 700,000
// four-byte rules are about 70 MiB when read back, above the 64 MiB limit, so
// GetPipeline with include_resources is RESOURCE_EXHAUSTED while ReadResource
// pages the same table at the requested 4096 entries. The rules are applied in
// chunks so no request itself is near the limit.
TEST_F(ApiV2ServiceTest, ReadResourceReadsATableAboveTheMessageLimit) {
  bess::pb::ExactMatchArg em;
  bess::pb::Field *field = em.add_fields();
  field->set_offset(26);
  field->set_num_bytes(4);
  google::protobuf::Any em_arg;
  ASSERT_TRUE(em_arg.PackFrom(em));
  ASSERT_TRUE(control_plane_->CreateModule({"big", "ExactMatch", em_arg}));

  constexpr uint32_t kRules = 700000;
  constexpr uint32_t kChunk = 20000;
  for (uint32_t start = 0; start < kRules; start += kChunk) {
    grpc::ClientContext ctx;
    v2::ApplyTransactionRequest req;
    req.set_request_id("stream-big-" + std::to_string(start));
    req.set_daemon_epoch(Epoch());
    const uint32_t end = std::min(kRules, start + kChunk);
    for (uint32_t i = start; i < end; i++) {
      std::string key(4, '\0');
      key[0] = static_cast<char>((i >> 24) & 0xff);
      key[1] = static_cast<char>((i >> 16) & 0xff);
      key[2] = static_cast<char>((i >> 8) & 0xff);
      key[3] = static_cast<char>(i & 0xff);
      bess::pb::ExactMatchRuleKey rule_key;
      rule_key.add_fields()->set_value_bin(key);
      bess::pb::ExactMatchRuleValue rule_value;
      rule_value.set_gate(1);
      v2::TransactionOp *op = req.add_ops();
      op->set_resource("big/rules");
      ASSERT_TRUE(op->mutable_key()->PackFrom(rule_key));
      ASSERT_TRUE(op->mutable_value()->PackFrom(rule_value));
    }
    v2::ApplyTransactionResponse applied;
    const grpc::Status status = stub_->ApplyTransaction(&ctx, req, &applied);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  {
    grpc::ClientContext ctx;
    v2::GetPipelineRequest req;
    req.set_include_resources(true);
    req.add_resources("big/rules");
    v2::GetPipelineResponse resp;
    const grpc::Status status = stub_->GetPipeline(&ctx, req, &resp);
    EXPECT_EQ(grpc::StatusCode::RESOURCE_EXHAUSTED, status.error_code())
        << "the table is above the one-message limit";
  }

  constexpr uint32_t kPageEntries = 4096;
  const uint32_t want_pages = (kRules + kPageEntries - 1) / kPageEntries;
  {
    grpc::ClientContext ctx;
    v2::ReadResourceRequest req;
    req.set_resource("big/rules");
    req.set_max_entries(kPageEntries);
    auto reader = stub_->ReadResource(&ctx, req);
    v2::ResourcePage page;
    uint32_t pages = 0;
    uint32_t entries = 0;
    while (reader->Read(&page)) {
      pages++;
      entries += page.entries_size();
      EXPECT_FALSE(page.entries().empty()) << "an empty page is not sent";
      EXPECT_LE(page.entries_size(), kPageEntries);
    }
    ASSERT_TRUE(reader->Finish().ok());
    EXPECT_EQ(kRules, entries);
    EXPECT_EQ(want_pages, pages);
  }
}

// ResetResources empties only the tables it names, in the daemon: no key is
// read over the wire, and the unlisted table keeps its entries. The expected
// resource generation is checked before anything changes.
TEST_F(ApiV2ServiceTest, ResetResourcesErasesOnlyTheNamedTables) {
  bess::pb::ExactMatchArg em;
  bess::pb::Field *field = em.add_fields();
  field->set_offset(26);
  field->set_num_bytes(4);
  google::protobuf::Any em_arg;
  ASSERT_TRUE(em_arg.PackFrom(em));
  ASSERT_TRUE(control_plane_->CreateModule({"take", "ExactMatch", em_arg}));
  ASSERT_TRUE(control_plane_->CreateModule({"keep", "ExactMatch", em_arg}));

  {
    grpc::ClientContext ctx;
    v2::ApplyTransactionRequest req;
    req.set_request_id("reset-rules");
    req.set_daemon_epoch(Epoch());
    const char *keys[] = {"aaaa", "bbbb", "cccc", "dddd"};
    int i = 0;
    for (const char *resource : {"take/rules", "take/rules", "take/rules",
                                 "keep/rules"}) {
      bess::pb::ExactMatchRuleKey key;
      key.add_fields()->set_value_bin(keys[i++]);
      bess::pb::ExactMatchRuleValue value;
      value.set_gate(1);
      v2::TransactionOp *op = req.add_ops();
      op->set_resource(resource);
      ASSERT_TRUE(op->mutable_key()->PackFrom(key));
      ASSERT_TRUE(op->mutable_value()->PackFrom(value));
    }
    v2::ApplyTransactionResponse applied;
    const grpc::Status status = stub_->ApplyTransaction(&ctx, req, &applied);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  auto count = [&](const std::string &name) {
    grpc::ClientContext ctx;
    v2::ReadResourceRequest req;
    req.set_resource(name);
    auto reader = stub_->ReadResource(&ctx, req);
    v2::ResourcePage page;
    int entries = 0;
    while (reader->Read(&page)) {
      entries += page.entries_size();
    }
    EXPECT_TRUE(reader->Finish().ok());
    return entries;
  };
  EXPECT_EQ(3, count("take/rules"));
  EXPECT_EQ(1, count("keep/rules"));

  uint64_t generation = 0;
  {
    grpc::ClientContext ctx;
    v2::ResetResourcesRequest req;
    req.set_daemon_epoch(Epoch());
    req.add_resources("take/rules");
    auto before = control_plane_->ReadResource("take/rules");
    ASSERT_TRUE(before.has_value());
    req.set_expected_resource_generation(before->resource_generation);
    v2::ResetResourcesResponse resp;
    const grpc::Status status = stub_->ResetResources(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_EQ(1, resp.resources_size());
    EXPECT_EQ("take/rules", resp.resources(0));
    EXPECT_EQ(3, resp.removed());
    EXPECT_NE(0u, resp.resource_generation());
    EXPECT_EQ(Epoch(), resp.daemon_epoch());
    generation = resp.resource_generation();
  }
  EXPECT_EQ(0, count("take/rules"));
  EXPECT_EQ(1, count("keep/rules")) << "a table the request did not name was touched";

  {
    grpc::ClientContext ctx;
    v2::ResetResourcesRequest req;
    req.set_daemon_epoch(Epoch());
    req.add_resources("take/rules");
    req.add_resources("take/rules");  // one resource, once
    v2::ResetResourcesResponse resp;
    const grpc::Status status = stub_->ResetResources(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(1, resp.resources_size());
    EXPECT_EQ(0, resp.removed());
    EXPECT_EQ(generation, resp.resource_generation()) << "an empty table is no transaction";
  }

  {
    grpc::ClientContext ctx;
    v2::ResetResourcesRequest req;
    req.set_daemon_epoch(Epoch());
    req.add_resources("keep/rules");
    req.set_expected_resource_generation(generation - 1);
    v2::ResetResourcesResponse resp;
    const grpc::Status status = stub_->ResetResources(&ctx, req, &resp);
    EXPECT_EQ(grpc::StatusCode::ABORTED, status.error_code());
    EXPECT_EQ(1, count("keep/rules")) << "a stale request erased an entry";
  }

  {
    grpc::ClientContext ctx;
    v2::ResetResourcesRequest req;
    req.set_daemon_epoch(Epoch() + 1);  // a daemon that restarted since the read
    req.add_resources("keep/rules");
    v2::ResetResourcesResponse resp;
    const grpc::Status status = stub_->ResetResources(&ctx, req, &resp);
    EXPECT_EQ(grpc::StatusCode::FAILED_PRECONDITION, status.error_code());
    EXPECT_EQ(1, count("keep/rules")) << "a foreign epoch erased an entry";
  }

  {
    grpc::ClientContext ctx;
    v2::ResetResourcesRequest req;
    req.set_daemon_epoch(Epoch());
    req.add_resources("no-such/table");
    v2::ResetResourcesResponse resp;
    const grpc::Status status = stub_->ResetResources(&ctx, req, &resp);
    EXPECT_EQ(grpc::StatusCode::NOT_FOUND, status.error_code());
  }

  {
    grpc::ClientContext ctx;
    v2::ResetResourcesRequest req;
    req.set_daemon_epoch(Epoch());
    v2::ResetResourcesResponse resp;
    const grpc::Status status = stub_->ResetResources(&ctx, req, &resp);
    EXPECT_EQ(grpc::StatusCode::INVALID_ARGUMENT, status.error_code());
  }
}

// GetResourceStatus: what a controller reads before it grows the graph --
// what the resources hold, the daemon's own memory, the reclamation budget it
// would draw on, and the workers that would carry the change. One call, so a
// controller needs no second round trip to decide whether it can proceed.
TEST_F(ApiV2ServiceTest, ResourceStatusReportsWhatTheDaemonHolds) {
  // A resource with an entry: L2Forward's l2/macs, written through the
  // resource as a client does (the same write InvokeAction makes).
  google::protobuf::Any arg;
  ASSERT_TRUE(arg.PackFrom(bess::pb::L2ForwardArg()));
  ASSERT_TRUE(control_plane_->CreateModule({"l2", "L2Forward", arg}));
  bess::pb::ExactMatchArg em;
  bess::pb::Field *em_field = em.add_fields();
  em_field->set_offset(26);
  em_field->set_num_bytes(4);
  google::protobuf::Any em_arg;
  ASSERT_TRUE(em_arg.PackFrom(em));
  ASSERT_TRUE(control_plane_->CreateModule({"em", "ExactMatch", em_arg}));
  {
    bess::pb::L2ForwardMacKey key;
    key.set_addr("02:00:00:00:00:09");
    bess::pb::L2ForwardMacValue value;
    value.set_gate(3);
    grpc::ClientContext ctx;
    v2::ApplyTransactionRequest req;
    req.set_request_id("l2-macs-add");
    req.set_daemon_epoch(Epoch());
    v2::TransactionOp *op = req.add_ops();
    op->set_resource("l2/macs");
    ASSERT_TRUE(op->mutable_key()->PackFrom(key));
    ASSERT_TRUE(op->mutable_value()->PackFrom(value));
    v2::ApplyTransactionResponse applied;
    const grpc::Status status = stub_->ApplyTransaction(&ctx, req, &applied);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  grpc::ClientContext ctx;
  v2::GetResourceStatusResponse resp;
  ASSERT_TRUE(stub_->GetResourceStatus(&ctx, {}, &resp).ok());

  EXPECT_EQ(Epoch(), resp.daemon_epoch());
  EXPECT_NE(0u, resp.generation()) << "the generation a commit names";

  // The resource a controller would write: what it holds now, and the ceiling
  // this kind can state -- the most entries the table accepts, so a controller
  // sees the most room it has instead of learning the bound from a refused
  // transaction. A kind that cannot state one reports has_capacity false and
  // must leave no capacity behind, since a controller reading a zero would
  // see a full table.
  const v2::ResourceOccupancy *macs = nullptr;
  const v2::ResourceOccupancy *growable = nullptr;
  for (const v2::ResourceOccupancy &r : resp.resources()) {
    EXPECT_FALSE(r.name().empty());
    if (!r.has_capacity()) {
      EXPECT_EQ(0u, r.capacity()) << r.name() << ": absent means absent";
    }
    if (r.name() == "l2/macs") {
      macs = &r;
    } else if (r.name() == "em/rules") {
      growable = &r;
    }
  }
  ASSERT_NE(nullptr, macs) << "the module's table is listed";
  EXPECT_EQ(1u, macs->live()) << "the entry the transaction wrote";
  ASSERT_TRUE(macs->has_capacity()) << "a cuckoo table knows its bound";
  EXPECT_GE(macs->capacity(), macs->live()) << "the bound covers what is held";
  ASSERT_NE(nullptr, growable) << "the exact table is listed";
  EXPECT_FALSE(growable->has_capacity()) << "a table its owner can grow states no ceiling";
  EXPECT_EQ(0u, growable->capacity()) << "and leaves no number a reader could take for full";

  // The daemon's own memory, the EAL heap the dataplane allocates from, and
  // the reclamation budget: past half of rcu_high_water, retiring waits.
  EXPECT_NE(0u, resp.rss_bytes());
  EXPECT_NE(0u, resp.heap_total_bytes());
  EXPECT_NE(0u, resp.rcu_high_water());
}

// An action not marked thread-safe is refused (EBUSY) while a worker runs
// the module, after the v2 path ApplyPipeline + resume: the resume works out
// which workers run each module. Paused, it runs.
TEST_F(ApiV2ServiceTest, ThreadUnsafeActionWaitsForPausedWorkers) {
  v2::Pipeline p;
  auto *w = p.add_workers();
  w->set_wid(0);
  w->set_core(0);
  auto *q = p.add_modules();
  q->set_name("q");
  q->set_mclass("Queue");
  ASSERT_TRUE(q->mutable_arg()->PackFrom(bess::pb::QueueArg()));
  // Bridge's `flush` is the tree's remaining thread-unsafe action (PacketStore's
  // three are THREAD_SAFE since the D-129 amendment), and the Queue's task on
  // worker 0 puts a running worker in its path.
  auto *br = p.add_modules();
  br->set_name("br");
  br->set_mclass("Bridge");
  ASSERT_TRUE(br->mutable_arg()->PackFrom(bess::pb::BridgeArg()));
  auto *c = p.add_connections();
  c->set_upstream("q");
  c->set_downstream("br");
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

  auto flush = [&](v2::ErrorDetail *detail) {
    grpc::ClientContext ctx;
    v2::InvokeActionRequest req;
    req.set_daemon_epoch(Epoch());
    req.set_module("br");
    req.set_action("flush");
    EXPECT_TRUE(req.mutable_arg()->PackFrom(bess::pb::EmptyArg()));
    v2::InvokeActionResponse resp;
    const grpc::StatusCode code = stub_->InvokeAction(&ctx, req, &resp).error_code();
    *detail = DetailOf(ctx);
    return code;
  };
  v2::ErrorDetail detail;
  ASSERT_TRUE(control_plane_->ResumeAll());
  EXPECT_NE(grpc::StatusCode::OK, flush(&detail)) << "ran beside the running worker";
  EXPECT_EQ(EBUSY, detail.errno_value());
  EXPECT_EQ(v2::ErrorDetail::RESOURCE_BUSY, detail.code());
  ASSERT_TRUE(control_plane_->PauseAll());
  EXPECT_EQ(grpc::StatusCode::OK, flush(&detail)) << detail.message();
}

// A resource that changes only with its workers paused (Queue's settings):
// a transaction while a worker runs the module is refused RESOURCE_BUSY
// before anything is tried; ApplyPipeline listing it pauses the workers
// itself, and the value reads back.
TEST_F(ApiV2ServiceTest, PausedOnlyResourceIsBusyWhileWorkersRun) {
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
  auto apply = [&](const v2::Pipeline &pipeline) {
    grpc::ClientContext ctx;
    v2::ApplyPipelineRequest req;
    *req.mutable_pipeline() = pipeline;
    req.set_daemon_epoch(Epoch());
    v2::ApplyPipelineResponse resp;
    return stub_->ApplyPipeline(&ctx, req, &resp);
  };
  {
    const grpc::Status status = apply(p);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }
  ASSERT_TRUE(control_plane_->ResumeAll());

  bess::pb::QueueSettings settings;
  settings.set_size(256);
  settings.set_burst(32);
  {
    grpc::ClientContext ctx;
    v2::ApplyTransactionRequest req;
    req.set_request_id("paused-only-busy");
    req.set_daemon_epoch(Epoch());
    v2::TransactionOp *op = req.add_ops();
    op->set_resource("q/settings");
    ASSERT_TRUE(op->mutable_value()->PackFrom(settings));
    v2::ApplyTransactionResponse resp;
    const grpc::Status status = stub_->ApplyTransaction(&ctx, req, &resp);
    EXPECT_EQ(grpc::StatusCode::FAILED_PRECONDITION, status.error_code()) << status.error_message();
    const v2::ErrorDetail detail = DetailOf(ctx);
    EXPECT_EQ(v2::ErrorDetail::RESOURCE_BUSY, detail.code());
    EXPECT_EQ("q/settings", detail.object());
  }

  v2::ResourceContents *r = p.add_resources();
  r->set_name("q/settings");
  ASSERT_TRUE(r->add_entries()->mutable_value()->PackFrom(settings));
  {
    const grpc::Status status = apply(p);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }
  grpc::ClientContext ctx;
  v2::GetPipelineRequest req;
  req.set_include_resources(true);
  req.add_resources("q/settings");
  v2::GetPipelineResponse resp;
  ASSERT_TRUE(stub_->GetPipeline(&ctx, req, &resp).ok());
  ASSERT_EQ(1, resp.pipeline().resources_size());
  ASSERT_EQ(1, resp.pipeline().resources(0).entries_size());
  bess::pb::QueueSettings read;
  ASSERT_TRUE(resp.pipeline().resources(0).entries(0).value().UnpackTo(&read));
  EXPECT_EQ(256u, read.size());
  EXPECT_FALSE(resp.pipeline().resources(0).entries(0).has_key()) << "a singleton has no key";
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
// action's hand-written argument and result type names and every resource
// table's key and value types resolve to a message (a typo would leave it
// unusable to such a client), and the descriptor set builds on its own, in
// its order, into an empty pool. Legacy-only commands are not listed (D-129).
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
                    },
                    [](const auto &) {}));
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
  const v2::ModuleClassSchema *l2_forward = nullptr;
  for (const v2::ModuleClassSchema &c : resp.module_classes()) {
    EXPECT_NE(nullptr, pool.FindMessageTypeByName(c.init_arg_type()))
        << c.name() << " init '" << c.init_arg_type() << "'";
    for (const v2::ActionSchema &a : c.actions()) {
      EXPECT_NE(nullptr, pool.FindMessageTypeByName(a.arg_type()))
          << c.name() << "." << a.name() << " '" << a.arg_type() << "'";
      if (!a.result_type().empty()) {
        EXPECT_NE(nullptr, pool.FindMessageTypeByName(a.result_type()))
            << c.name() << "." << a.name() << " result '" << a.result_type() << "'";
      }
    }
    for (const v2::ResourceSchema &r : c.resources()) {
      EXPECT_NE(nullptr, pool.FindMessageTypeByName(r.key_type()))
          << c.name() << "/" << r.table() << " key '" << r.key_type() << "'";
      EXPECT_NE(nullptr, pool.FindMessageTypeByName(r.value_type()))
          << c.name() << "/" << r.table() << " value '" << r.value_type() << "'";
    }
    if (c.name() == "ExactMatch") {
      exact_match = &c;
    } else if (c.name() == "L2Forward") {
      l2_forward = &c;
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
  EXPECT_EQ(0, exact_match->actions_size()) << "every ExactMatch command edits state";
  ASSERT_EQ(2, exact_match->resources_size());
  EXPECT_EQ("rules", exact_match->resources(0).table());
  EXPECT_EQ(v2::TransactionResource::KIND_MAP, exact_match->resources(0).kind());
  EXPECT_EQ("bess.pb.ExactMatchRuleKey", exact_match->resources(0).key_type());
  EXPECT_EQ("bess.pb.ExactMatchRuleValue", exact_match->resources(0).value_type());
  EXPECT_EQ("settings", exact_match->resources(1).table());
  EXPECT_EQ(v2::TransactionResource::KIND_SINGLETON, exact_match->resources(1).kind());
  EXPECT_EQ("google.protobuf.Empty", exact_match->resources(1).key_type());
  EXPECT_EQ("bess.pb.ExactMatchSettings", exact_match->resources(1).value_type());

  ASSERT_NE(nullptr, l2_forward);
  std::vector<std::string> actions;
  for (const v2::ActionSchema &a : l2_forward->actions()) {
    actions.push_back(a.name());
    if (a.name() == "lookup") {
      EXPECT_EQ("bess.pb.L2ForwardCommandLookupArg", a.arg_type());
      EXPECT_EQ("bess.pb.L2ForwardCommandLookupResponse", a.result_type());
    }
  }
  EXPECT_EQ((std::vector<std::string>{"lookup", "populate"}), actions)
      << "sorted; the state commands went with the legacy service (D-140)";
}

}  // namespace

