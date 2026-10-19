// Copyright (c) 2017, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "api_server.h"

#include <chrono>

#include <gflags/gflags.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include "control/admin_v2.h"
#include "control/api_v2.h"
#include "control/control_plane.h"
#include "control/http_server.h"
#include "control/maintenance_loop.h"
#include "control/resource_fill.h"
#include "dpdk.h"
#include "framework/resource_bindings.h"
#include "runtime/opts.h"
#include "runtime/runtime_state.h"
#include "runtime/startup.h"
#include "utils/logging.h"
#include "worker.h"

ApiServer::ApiServer() : builder_(nullptr) {}

ApiServer::~ApiServer() = default;

void ApiServer::ServeHttp(std::unique_ptr<bess::control::HttpServer> http) {
  http_ = std::move(http);
}

void ApiServer::Listen(const std::string &addr) {
  if (!builder_) {
    bess::control::PrepareControlServer();  // before the first builder
    builder_ = std::make_unique<grpc::ServerBuilder>();
  }

  LOG(INFO) << "Server listening on " << addr;

  builder_->AddListeningPort(addr, grpc::InsecureServerCredentials());
}

void ApiServer::Run() {
  if (!builder_) {
    // We are not listening on any sockets. There is nothing to do.
    return;
  }

  // One control plane behind both services: desired state (G1) and
  // administration (D-107).
  bess::control::ControlPlane control_plane;
  bess::control::ControlV2Service service_v2(control_plane);
  bess::control::AdminV2Service admin_v2(control_plane, service_v2.epoch());
  builder_->RegisterService(&service_v2);
  builder_->RegisterService(&admin_v2);
  builder_->SetSyncServerOption(grpc::ServerBuilder::MAX_POLLERS, 1);
  bess::control::ConfigureControlServer(builder_.get());

  std::unique_ptr<grpc::Server> server = builder_->BuildAndStart();
  if (server == nullptr) {
    LOG(ERROR) << "ServerBuilder::BuildAndStart() failed";
    return;
  }

  // HTTP (D-125) calls this server over an in-process channel: the same
  // services, limits and errors as a gRPC client sees.
  if (http_) {
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(bess::control::kMaxMessageBytes);
    args.SetMaxSendMessageSize(bess::control::kMaxMessageBytes);
    http_->Start(server->InProcessChannel(args));
  }

  // Shutdown closes the event log first: a WatchEvents stream waiting in it
  // returns, so Shutdown() is not left waiting for it.
  auto shutdown = [&server]() {
    bess::runtime::runtime().events().Close();
    // A deadline: a stream blocked writing to a client that stopped reading
    // is cancelled then, not waited for.
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(3));
  };
  admin_v2.set_shutdown(shutdown);
  {
    // Worker-to-control module requests (TP4, D-077), delivered under the
    // control-plane lock, and workers' events moved into the event log (M25);
    // stopped before the dataplane is torn down below.
    // The built-in fill watch (D-165): every registered resource's fill is
    // read against the level on this loop's tick, and each crossing is one
    // event on the log below.
    bess::control::ResourceFillWatch fills(bess::runtime::runtime().transactions(),
                                           bess::framework::ResourceBindings::ProcessDefault());
    bess::control::MaintenanceLoop maintenance(
        control_plane, bess::runtime::runtime().requests(), bess::runtime::runtime().ticks(),
        std::chrono::microseconds(FLAGS_maintenance_interval_us),
        &bess::runtime::runtime().events(), std::chrono::seconds(1),
        std::chrono::microseconds(250), &fills);
    // SIGTERM/SIGINT: the same shutdown as Admin.Shutdown (D-030).
    bess::startup::TerminationWatcher watcher(shutdown);
    server->Wait();
  }
  // Its calls already fail (the server is down); its event streams ended
  // with the event log.
  if (http_) {
    http_->Stop();
  }

  // Tear the dataplane down in order while the runtime is still alive:
  // modules (and the state they publish through RCU), then ports, traffic
  // classes and workers. Left to static destruction, the runtime destroyed
  // its RcuDomain before the modules that publish through it, and aborted on
  // RcuPtr's online-reader check.
  // DPDK's trace (--dpdk_trace) before the reset: a worker's trace buffer is
  // freed when its thread unregisters from EAL, and the reset ends the
  // workers. Paused, they write no trace points while it is saved.
  if (!FLAGS_dpdk_trace.empty()) {
    pause_all_workers();
    bess::SaveDpdkTrace();
  }
  if (auto reset = control_plane.Reset(); !reset) {
    LOG(ERROR) << "Shutdown reset failed: " << reset.error().message;
  }
}
