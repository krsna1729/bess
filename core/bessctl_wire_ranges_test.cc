// SPDX-License-Identifier: BSD-3-Clause

// The legacy (v1) BESSControl service refuses wire integers that do not fit
// the daemon's narrower fields instead of narrowing them: gate 65536 must not
// quietly become gate 0, 257 queues must not become "one queue", worker
// 2^32 + 1 must not become worker 1. Each refused value wraps to a *valid*
// one, so without the check the request would succeed wrongly. The service
// runs on an in-process channel; the v2 paths are api_v2_test.cc's. Deleted
// with core/bessctl.cc.

#include "bessctl.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "pb/service.grpc.pb.h"
#pragma GCC diagnostic pop

#include "control/control_plane.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "pb/port_msg.pb.h"
#include "port.h"
#include "runtime/opts.h"

namespace {

namespace pb = bess::pb;
using bess::control::ControlPlane;

constexpr uint64_t k2To32 = uint64_t{1} << 32;

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

class LegacyWireRangesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    InitRuntimeOnce();
    control_plane_ = std::make_unique<ControlPlane>();
    service_ = NewLegacyControlService(*control_plane_);
    grpc::ServerBuilder builder;
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    ASSERT_NE(nullptr, server_);
    stub_ = pb::BESSControl::NewStub(server_->InProcessChannel(grpc::ChannelArguments()));
  }

  void TearDown() override {
    server_->Shutdown();
    (void)control_plane_->Reset();
  }

  // Calls `rpc` and returns the legacy error the answer carries (the call
  // itself is always OK in v1).
  template <typename Req, typename Resp, typename Rpc>
  pb::Error Call(Rpc rpc, const Req &req) {
    grpc::ClientContext ctx;
    Resp resp;
    const grpc::Status status = ((*stub_).*rpc)(&ctx, req, &resp);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return resp.error();
  }

  // An out-of-range refusal: EINVAL, saying so.
  static void ExpectOutOfRange(const pb::Error &e) {
    EXPECT_EQ(EINVAL, e.code()) << e.errmsg();
    EXPECT_NE(std::string::npos, e.errmsg().find("out of range")) << e.errmsg();
  }

  void CreateBypass(const std::string &name) {
    pb::CreateModuleRequest req;
    req.set_name(name);
    req.set_mclass("Bypass");
    ASSERT_TRUE(req.mutable_arg()->PackFrom(pb::BypassArg()));
    const pb::Error e = Call<pb::CreateModuleRequest, pb::CreateModuleResponse>(
        &pb::BESSControl::Stub::CreateModule, req);
    ASSERT_EQ(0, e.code()) << e.errmsg();
  }

  pb::Error Connect(uint64_t ogate, uint64_t igate) {
    pb::ConnectModulesRequest req;
    req.set_m1("wr_up");
    req.set_m2("wr_down");
    req.set_ogate(ogate);
    req.set_igate(igate);
    return Call<pb::ConnectModulesRequest, pb::EmptyResponse>(
        &pb::BESSControl::Stub::ConnectModules, req);
  }

  int ConnectedOgates(const std::string &module) {
    grpc::ClientContext ctx;
    pb::GetModuleInfoRequest req;
    req.set_name(module);
    pb::GetModuleInfoResponse resp;
    EXPECT_TRUE(stub_->GetModuleInfo(&ctx, req, &resp).ok());
    EXPECT_EQ(0, resp.error().code()) << resp.error().errmsg();
    return resp.ogates_size();
  }

  std::unique_ptr<ControlPlane> control_plane_;
  std::unique_ptr<grpc::Service> service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<pb::BESSControl::Stub> stub_;
};

TEST_F(LegacyWireRangesTest, GatesDoNotWrap) {
  CreateBypass("wr_up");
  CreateBypass("wr_down");
  ExpectOutOfRange(Connect(65536, 0));
  ExpectOutOfRange(Connect(0, 65537));
  EXPECT_EQ(0, ConnectedOgates("wr_up")) << "nothing may be connected";

  const pb::Error ok = Connect(0, 0);
  ASSERT_EQ(0, ok.code()) << ok.errmsg();
  pb::DisconnectModulesRequest req;
  req.set_name("wr_up");
  req.set_ogate(65536);
  ExpectOutOfRange(Call<pb::DisconnectModulesRequest, pb::EmptyResponse>(
      &pb::BESSControl::Stub::DisconnectModules, req));
  EXPECT_EQ(1, ConnectedOgates("wr_up")) << "gate 0 must stay connected";
}

TEST_F(LegacyWireRangesTest, QueueCountsDoNotWrap) {
  for (const auto &[inc, out] : {std::pair<uint64_t, uint64_t>{257, 0}, {0, 256}}) {
    SCOPED_TRACE(inc);
    pb::CreatePortRequest req;
    req.set_name("wr_p0");
    req.set_driver("PCAPPort");
    req.set_num_inc_q(inc);
    req.set_num_out_q(out);
    pb::PCAPPortArg arg;
    arg.set_dev("lo");
    ASSERT_TRUE(req.mutable_arg()->PackFrom(arg));
    ExpectOutOfRange(Call<pb::CreatePortRequest, pb::CreatePortResponse>(
        &pb::BESSControl::Stub::CreatePort, req));
  }
  grpc::ClientContext ctx;
  pb::ListPortsResponse ports;
  ASSERT_TRUE(stub_->ListPorts(&ctx, pb::EmptyRequest(), &ports).ok());
  for (const auto &p : ports.ports()) {
    EXPECT_NE("wr_p0", p.name());
  }
}

TEST_F(LegacyWireRangesTest, GateHooksDoNotWrap) {
  CreateBypass("wr_h");
  {
    pb::ConfigureGateHookRequest req;
    req.set_enable(true);
    pb::GateHookInfo *hook = req.mutable_hook();
    hook->set_class_name("Track");
    hook->set_hook_name("wr_track");
    hook->set_module_name("wr_h");
    hook->set_igate(65536);
    ASSERT_TRUE(hook->mutable_arg()->PackFrom(pb::TrackArg()));
    ExpectOutOfRange(Call<pb::ConfigureGateHookRequest, pb::ConfigureGateHookResponse>(
        &pb::BESSControl::Stub::ConfigureGateHook, req));
  }
  {
    pb::GateHookCommandRequest req;
    req.set_cmd("reset");
    pb::GateHookInfo *hook = req.mutable_hook();
    hook->set_hook_name("wr_track");
    hook->set_module_name("wr_h");
    hook->set_ogate(65536);
    ASSERT_TRUE(hook->mutable_arg()->PackFrom(pb::EmptyArg()));
    ExpectOutOfRange(Call<pb::GateHookCommandRequest, pb::CommandResponse>(
        &pb::BESSControl::Stub::GateHookCommand, req));
  }
}

// 2^32 narrowed to int is worker 0.
TEST_F(LegacyWireRangesTest, WorkerIdsDoNotWrap) {
  pb::PauseWorkerRequest pause;
  pause.set_wid(k2To32);
  ExpectOutOfRange(Call<pb::PauseWorkerRequest, pb::EmptyResponse>(
      &pb::BESSControl::Stub::PauseWorker, pause));
  pb::ResumeWorkerRequest resume;
  resume.set_wid(k2To32);
  ExpectOutOfRange(Call<pb::ResumeWorkerRequest, pb::EmptyResponse>(
      &pb::BESSControl::Stub::ResumeWorker, resume));
}

// 2^32 + 1 narrowed to int is worker 1; 2^32 - 1 is -1, "any worker".
TEST_F(LegacyWireRangesTest, TrafficClassWorkerIdsDoNotWrap) {
  for (const uint64_t wid : {k2To32 + 1, k2To32 - 1}) {
    SCOPED_TRACE(wid);
    pb::AddTcRequest req;
    req.mutable_class_()->set_name("wr_tc");
    req.mutable_class_()->set_policy("round_robin");
    req.mutable_class_()->set_wid(static_cast<int64_t>(wid));
    ExpectOutOfRange(
        Call<pb::AddTcRequest, pb::EmptyResponse>(&pb::BESSControl::Stub::AddTc, req));
  }
  {
    pb::ListTcsRequest req;
    req.set_wid(-1);
    grpc::ClientContext ctx;
    pb::ListTcsResponse resp;
    ASSERT_TRUE(stub_->ListTcs(&ctx, req, &resp).ok());
    ASSERT_EQ(0, resp.error().code()) << resp.error().errmsg();
    for (const auto &s : resp.classes_status()) {
      EXPECT_NE("wr_tc", s.class_().name());
    }
  }
  // ListTcs compares the filter before narrowing it.
  pb::ListTcsRequest req;
  req.set_wid(static_cast<int64_t>(k2To32 + 1));
  const pb::Error e =
      Call<pb::ListTcsRequest, pb::ListTcsResponse>(&pb::BESSControl::Stub::ListTcs, req);
  EXPECT_EQ(EINVAL, e.code()) << e.errmsg();
}

}  // namespace
