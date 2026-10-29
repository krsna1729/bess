// SPDX-License-Identifier: BSD-3-Clause

// The v2 Admin service (D-107) over a real in-process gRPC channel, beside
// the Control service that builds the pipelines it administers.

#include "control/admin_v2.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "control/api_v2.h"
#include "control/control_plane.h"
#include "gate.h"
#include "gate_hooks/pcapng.h"
#include "gate_hooks/tcpdump.h"
#include "module.h"
#include "packet_pool.h"
#include "pb/port_msg.pb.h"
#include "port.h"
#include "runtime/runtime_state.h"

namespace {

namespace v2 = bess::pb::v2;
using bess::control::AdminV2Service;
using bess::control::ControlPlane;
using bess::control::ControlV2Service;

void InitRuntimeOnce() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;
  // The library default (D-198): normal pages, malloc-backed, sandbox-safe.
  bess::PacketPool::CreateDefaultPools(32767);
  PortBuilder::InitDrivers();
}

class AdminV2ServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    InitRuntimeOnce();
    control_plane_ = std::make_unique<ControlPlane>();
    control_ = std::make_unique<ControlV2Service>(*control_plane_);
    admin_ = std::make_unique<AdminV2Service>(*control_plane_, control_->epoch());
    grpc::ServerBuilder builder;
    builder.RegisterService(control_.get());
    builder.RegisterService(admin_.get());
    server_ = builder.BuildAndStart();
    ASSERT_NE(nullptr, server_);
    auto channel = server_->InProcessChannel(grpc::ChannelArguments());
    control_stub_ = v2::Control::NewStub(channel);
    admin_stub_ = v2::Admin::NewStub(channel);
    // The capture target exists, as a FIFO the client made would (the hook
    // opens it for writing; it never creates it).
    std::ofstream(capture_).close();
  }

  void TearDown() override {
    server_->Shutdown();
    (void)control_plane_->Reset();
    std::remove(capture_.c_str());
  }

  static v2::ErrorDetail DetailOf(const grpc::ClientContext &context) {
    v2::ErrorDetail detail;
    const auto &trailers = context.GetServerTrailingMetadata();
    const auto it = trailers.find("bess-error-bin");
    if (it != trailers.end()) {
      EXPECT_TRUE(detail.ParseFromString(std::string(it->second.data(), it->second.size())));
    }
    return detail;
  }

  uint64_t Epoch() const { return control_->epoch(); }

  void Apply(const v2::Pipeline &pipeline) {
    grpc::ClientContext ctx;
    v2::ApplyPipelineRequest req;
    *req.mutable_pipeline() = pipeline;
    req.set_daemon_epoch(Epoch());
    v2::ApplyPipelineResponse resp;
    const grpc::Status s = control_stub_->ApplyPipeline(&ctx, req, &resp);
    ASSERT_TRUE(s.ok()) << s.error_message();
  }

  // src sends on output gates 0 and 1 to sink0 and sink1.
  static v2::Pipeline TwoGates() {
    v2::Pipeline p;
    for (const char *name : {"src", "sink0", "sink1"}) {
      auto *m = p.add_modules();
      m->set_name(name);
      m->set_mclass("Bypass");
    }
    for (uint32_t gate : {0u, 1u}) {
      auto *c = p.add_connections();
      c->set_upstream("src");
      c->set_ogate(gate);
      c->set_downstream(gate == 0 ? "sink0" : "sink1");
    }
    return p;
  }

  grpc::Status Capture(const v2::SetCaptureRequest &req, grpc::ClientContext *ctx) {
    v2::SetCaptureResponse resp;
    return admin_stub_->SetCapture(ctx, req, &resp);
  }

  v2::SetCaptureRequest PcapOn(const std::string &module, std::optional<uint32_t> gate) {
    v2::SetCaptureRequest req;
    req.set_module(module);
    req.set_direction(v2::SetCaptureRequest::DIRECTION_OUTPUT);
    if (gate) {
      req.set_gate(*gate);
    }
    req.set_format(v2::SetCaptureRequest::FORMAT_PCAP);
    req.set_fifo(capture_);
    req.set_enable(true);
    req.set_daemon_epoch(Epoch());
    return req;
  }

  v2::SetCaptureRequest NgOn(const std::string &module, std::optional<uint32_t> gate) {
    v2::SetCaptureRequest req = PcapOn(module, gate);
    req.set_format(v2::SetCaptureRequest::FORMAT_PCAPNG);
    return req;
  }

  static bool Hooked(const char *module, size_t ogate,
                     const std::string &hook_class = Tcpdump::kName) {
    Module *m = bess::runtime::runtime().modules().Find(module);
    return m != nullptr && ogate < m->ogates().size() && m->ogates()[ogate] != nullptr &&
           m->ogates()[ogate]->FindHookByClass(hook_class) != nullptr;
  }

  // A regular file stands in for the FIFO a client opens (the hook opens and
  // writes either alike).
  std::string capture_ = std::format("/tmp/bess_admin_capture_{}.pcap", getpid());
  std::unique_ptr<ControlPlane> control_plane_;
  std::unique_ptr<ControlV2Service> control_;
  std::unique_ptr<AdminV2Service> admin_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<v2::Control::Stub> control_stub_;
  std::unique_ptr<v2::Admin::Stub> admin_stub_;
};

// D-106 for Admin: a mutating call without this daemon's epoch is refused
// before it does anything.
TEST_F(AdminV2ServiceTest, MutatingCallsMustNameTheDaemonsEpoch) {
  Apply(TwoGates());
  std::atomic<int> shutdowns{0};
  admin_->set_shutdown([&] { shutdowns++; });
  for (const uint64_t sent : {uint64_t{0}, Epoch() + 1}) {
    SCOPED_TRACE(sent);
    const grpc::StatusCode want =
        sent == 0 ? grpc::StatusCode::INVALID_ARGUMENT : grpc::StatusCode::FAILED_PRECONDITION;
    const auto want_detail =
        sent == 0 ? v2::ErrorDetail::INVALID_ARGUMENT : v2::ErrorDetail::EPOCH_MISMATCH;
    {
      grpc::ClientContext ctx;
      v2::SetCaptureRequest req = PcapOn("src", std::nullopt);
      req.set_daemon_epoch(sent);
      EXPECT_EQ(want, Capture(req, &ctx).error_code());
      EXPECT_EQ(want_detail, DetailOf(ctx).code());
    }
    {
      grpc::ClientContext ctx;
      v2::ImportPluginRequest req;
      req.set_path("/nonexistent.so");
      req.set_daemon_epoch(sent);
      v2::ImportPluginResponse resp;
      EXPECT_EQ(want, admin_stub_->ImportPlugin(&ctx, req, &resp).error_code());
    }
    {
      grpc::ClientContext ctx;
      v2::UnloadPluginRequest req;
      req.set_path("/nonexistent.so");
      req.set_daemon_epoch(sent);
      v2::UnloadPluginResponse resp;
      EXPECT_EQ(want, admin_stub_->UnloadPlugin(&ctx, req, &resp).error_code());
    }
    {
      grpc::ClientContext ctx;
      v2::PauseWorkersRequest req;
      req.set_daemon_epoch(sent);
      v2::PauseWorkersResponse resp;
      EXPECT_EQ(want, admin_stub_->PauseWorkers(&ctx, req, &resp).error_code());
    }
    {
      grpc::ClientContext ctx;
      v2::ResumeWorkersRequest req;
      req.set_daemon_epoch(sent);
      v2::ResumeWorkersResponse resp;
      EXPECT_EQ(want, admin_stub_->ResumeWorkers(&ctx, req, &resp).error_code());
    }
    {
      grpc::ClientContext ctx;
      v2::ShutdownRequest req;
      req.set_daemon_epoch(sent);
      v2::ShutdownResponse resp;
      EXPECT_EQ(want, admin_stub_->Shutdown(&ctx, req, &resp).error_code());
    }
  }
  EXPECT_FALSE(Hooked("src", 0));
  EXPECT_FALSE(Hooked("src", 1));
  EXPECT_EQ(0, shutdowns.load());

  // With the epoch, Shutdown answers and then runs the daemon's shutdown.
  grpc::ClientContext ctx;
  v2::ShutdownRequest req;
  req.set_daemon_epoch(Epoch());
  v2::ShutdownResponse resp;
  ASSERT_TRUE(admin_stub_->Shutdown(&ctx, req, &resp).ok());
  EXPECT_EQ(Epoch(), resp.daemon_epoch());
  for (int i = 0; i < 1000 && shutdowns.load() == 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(1, shutdowns.load());
}

TEST_F(AdminV2ServiceTest, CaptureStartsAndStopsOnTheGatesAsked) {
  Apply(TwoGates());
  {
    grpc::ClientContext ctx;
    ASSERT_TRUE(Capture(PcapOn("src", 1), &ctx).ok());
  }
  EXPECT_FALSE(Hooked("src", 0));
  EXPECT_TRUE(Hooked("src", 1));
  // The hook opened the file and wrote the libpcap file header.
  std::ifstream in(capture_, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ASSERT_EQ(24u, bytes.size());
  EXPECT_EQ(std::string("\xd4\xc3\xb2\xa1", 4), bytes.substr(0, 4));

  v2::SetCaptureRequest stop = PcapOn("src", 1);
  stop.set_enable(false);
  stop.clear_fifo();
  {
    grpc::ClientContext ctx;
    ASSERT_TRUE(Capture(stop, &ctx).ok());
  }
  EXPECT_FALSE(Hooked("src", 1));

  // Every gate of a direction (pcapng: a section per gate).
  v2::SetCaptureRequest every = NgOn("src", std::nullopt);
  {
    grpc::ClientContext ctx;
    ASSERT_TRUE(Capture(every, &ctx).ok());
  }
  EXPECT_TRUE(Hooked("src", 0, Pcapng::kName));
  EXPECT_TRUE(Hooked("src", 1, Pcapng::kName));
  every.set_enable(false);
  every.clear_fifo();
  {
    grpc::ClientContext ctx;
    ASSERT_TRUE(Capture(every, &ctx).ok());
  }
  EXPECT_FALSE(Hooked("src", 0, Pcapng::kName));
  EXPECT_FALSE(Hooked("src", 1, Pcapng::kName));

  // pcapng is its own hook class, beside a pcap capture on the same gate.
  v2::SetCaptureRequest ng = NgOn("src", 0);
  for (const auto &req : {PcapOn("src", 0), ng}) {
    grpc::ClientContext ctx;
    ASSERT_TRUE(Capture(req, &ctx).ok());
  }
  EXPECT_TRUE(Hooked("src", 0, Pcapng::kName));
  EXPECT_TRUE(Hooked("src", 0));
  ng.set_enable(false);
  {
    grpc::ClientContext ctx;
    ASSERT_TRUE(Capture(ng, &ctx).ok());
  }
  EXPECT_FALSE(Hooked("src", 0, Pcapng::kName));
  EXPECT_TRUE(Hooked("src", 0)) << "stopping pcapng stopped pcap";
}

// Capturing every gate is all or nothing: when one gate refuses (it already
// has a capture), the hooks this call put on the other gates are removed and
// the existing capture stays.
TEST_F(AdminV2ServiceTest, CaptureOnEveryGateIsAllOrNothing) {
  Apply(TwoGates());
  {
    grpc::ClientContext ctx;
    ASSERT_TRUE(Capture(NgOn("src", 1), &ctx).ok());
  }
  {
    grpc::ClientContext ctx;
    EXPECT_EQ(grpc::StatusCode::ALREADY_EXISTS,
              Capture(NgOn("src", std::nullopt), &ctx).error_code());
  }
  EXPECT_FALSE(Hooked("src", 0, Pcapng::kName)) << "a hook this call created was left behind";
  EXPECT_TRUE(Hooked("src", 1, Pcapng::kName)) << "the capture that was there was removed";
}

// A libpcap stream holds one file header and each hook writes one, so pcap
// without a gate is the module's only gate in that direction; with more it is
// refused before any hook is made.
TEST_F(AdminV2ServiceTest, PcapWithoutAGateNeedsTheOnlyGate) {
  Apply(TwoGates());
  {
    grpc::ClientContext ctx;
    EXPECT_EQ(grpc::StatusCode::INVALID_ARGUMENT,
              Capture(PcapOn("src", std::nullopt), &ctx).error_code());
  }
  EXPECT_FALSE(Hooked("src", 0));
  EXPECT_FALSE(Hooked("src", 1));
  EXPECT_EQ(0u, std::filesystem::file_size(capture_)) << "a hook wrote to the capture";

  v2::Pipeline one = TwoGates();
  one.mutable_connections()->RemoveLast();
  Apply(one);
  {
    grpc::ClientContext ctx;
    ASSERT_TRUE(Capture(PcapOn("src", std::nullopt), &ctx).ok());
  }
  EXPECT_TRUE(Hooked("src", 0));
  std::ifstream in(capture_, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(24u, bytes.size()) << "not exactly one file header";
}

TEST_F(AdminV2ServiceTest, CaptureRequestsAreCheckedBeforeAnything) {
  Apply(TwoGates());
  struct Case {
    const char *field;
    std::function<void(v2::SetCaptureRequest *)> spoil;
  };
  const Case cases[] = {
      {"module", [](v2::SetCaptureRequest *r) { r->clear_module(); }},
      {"direction", [](v2::SetCaptureRequest *r) { r->clear_direction(); }},
      {"format", [](v2::SetCaptureRequest *r) { r->clear_format(); }},
      {"fifo", [](v2::SetCaptureRequest *r) { r->clear_fifo(); }},
      // Above every gate index: narrowed, 65536 would be gate 0.
      {"gate", [](v2::SetCaptureRequest *r) { r->set_gate(65536); }},
      {"gate", [](v2::SetCaptureRequest *r) { r->set_gate(MAX_GATES); }},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.field);
    v2::SetCaptureRequest req = PcapOn("src", 0);
    c.spoil(&req);
    grpc::ClientContext ctx;
    EXPECT_EQ(grpc::StatusCode::INVALID_ARGUMENT, Capture(req, &ctx).error_code());
    EXPECT_EQ(c.field, DetailOf(ctx).field());
  }
  EXPECT_FALSE(Hooked("src", 0));
  grpc::ClientContext ctx;
  EXPECT_EQ(grpc::StatusCode::NOT_FOUND, Capture(PcapOn("nosuch", 0), &ctx).error_code());
}

TEST_F(AdminV2ServiceTest, PortStatusNamesEveryPortOrTheOnesAsked) {
  v2::Pipeline p;
  for (const char *name : {"b", "a"}) {
    auto *port = p.add_ports();
    port->set_name(name);
    port->set_driver("UnixSocketPort");
    bess::pb::UnixSocketPortArg arg;
    arg.set_path(std::format("@bess_admin_test_{}_{}", getpid(), name));
    ASSERT_TRUE(port->mutable_arg()->PackFrom(arg));
  }
  Apply(p);
  {
    grpc::ClientContext ctx;
    v2::GetPortStatusResponse resp;
    ASSERT_TRUE(admin_stub_->GetPortStatus(&ctx, {}, &resp).ok());
    ASSERT_EQ(2, resp.ports_size());
    EXPECT_EQ("a", resp.ports(0).name());  // by name
    EXPECT_EQ("b", resp.ports(1).name());
    EXPECT_EQ("UnixSocketPort", resp.ports(0).driver());
    EXPECT_EQ(0u, resp.ports(0).rx().packets());
    EXPECT_EQ(Epoch(), resp.daemon_epoch());
  }
  {
    grpc::ClientContext ctx;
    v2::GetPortStatusRequest req;
    req.add_names("b");
    req.add_names("a");
    v2::GetPortStatusResponse resp;
    ASSERT_TRUE(admin_stub_->GetPortStatus(&ctx, req, &resp).ok());
    ASSERT_EQ(2, resp.ports_size());
    EXPECT_EQ("b", resp.ports(0).name());  // in request order
  }
  grpc::ClientContext ctx;
  v2::GetPortStatusRequest req;
  req.add_names("a");
  req.add_names("nosuch");
  v2::GetPortStatusResponse resp;
  EXPECT_EQ(grpc::StatusCode::NOT_FOUND, admin_stub_->GetPortStatus(&ctx, req, &resp).error_code());
  EXPECT_EQ("nosuch", DetailOf(ctx).object());
}

TEST_F(AdminV2ServiceTest, CapabilitiesListAdminRpcs) {
  grpc::ClientContext ctx;
  v2::GetCapabilitiesResponse resp;
  ASSERT_TRUE(control_stub_->GetCapabilities(&ctx, {}, &resp).ok());
  EXPECT_NE(std::find(resp.rpcs().begin(), resp.rpcs().end(), "bess.pb.v2.Admin/SetCapture"),
            resp.rpcs().end());
  EXPECT_NE(std::find(resp.rpcs().begin(), resp.rpcs().end(), "bess.pb.v2.Control/ApplyPipeline"),
            resp.rpcs().end());
}

}  // namespace
