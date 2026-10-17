// SPDX-License-Identifier: BSD-3-Clause

// The daemon's API server, in process. main.cc constructs the only ApiServer in
// the tree, so nothing else exercises the ownership api_server.h describes: the
// builder and the gRPC plugins it owns are freed when the object goes out of
// scope, and until now no test had ever done that. This one starts the server
// on a unix socket, has a client stop it through Admin.Shutdown, and lets the
// object go out of scope inside the test body - so CI's clang-asan lane, which
// runs every non-live test under detect_leaks=1, is the check that the
// ownership holds (D-142: the local LSan on this host is blind, CI's is not).

#include "api_server.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "packet_pool.h"
#include "pb/control_v2.grpc.pb.h"
#include "pb/control_v2.pb.h"
#include "port.h"
#include "runtime/opts.h"

namespace {

namespace v2 = bess::pb::v2;

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

// Joins the server thread on the way out, including when an assertion returns
// early: a joinable std::thread destroyed without a join terminates the process.
struct JoinsOnExit {
  std::thread &thread;
  ~JoinsOnExit() {
    if (thread.joinable()) {
      thread.join();
    }
  }
};

TEST(ApiServerTest, ServesAndStopsOnAdminShutdown) {
  InitRuntimeOnce();

  const std::string sock = "/tmp/bess_api_server_test.sock";
  (void)std::remove(sock.c_str());

  ApiServer server;
  server.Listen("unix:" + sock);

  std::atomic<bool> returned{false};
  std::thread run([&server, &returned] {
    server.Run();
    returned.store(true);
  });
  JoinsOnExit joiner{run};

  auto channel =
      grpc::CreateChannel("unix:" + sock, grpc::InsecureChannelCredentials());

  // Ready when it answers, and the epoch of that answer: a request that asks
  // for a change must carry it (api_v2.cc's RefuseOtherEpoch refuses an epoch
  // of 0 with INVALID_ARGUMENT, "daemon_epoch is required: the epoch of an
  // earlier answer"), which is why the readiness answer is read for its epoch
  // and not just for its status.
  uint64_t epoch = 0;
  bool up = false;
  for (int i = 0; i < 100 && !up; i++) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(200));
    v2::GetCapabilitiesRequest request;
    v2::GetCapabilitiesResponse response;
    auto control = v2::Control::NewStub(channel);
    if (control->GetCapabilities(&context, request, &response).ok()) {
      up = true;
      epoch = response.daemon_epoch();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  ASSERT_TRUE(up) << "the API server never answered on " << sock;
  ASSERT_NE(0u, epoch) << "the daemon epoch an earlier answer carries";

  {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(2000));
    v2::ShutdownRequest request;
    request.set_daemon_epoch(epoch);
    v2::ShutdownResponse response;
    auto admin = v2::Admin::NewStub(channel);
    // The answer arrives before the server stops: the handler starts the
    // shutdown on a thread of its own for exactly that reason.
    const grpc::Status status = admin->Shutdown(&context, request, &response);
    EXPECT_TRUE(status.ok()) << "Admin.Shutdown: " << status.error_message();
  }

  for (int i = 0; i < 200 && !returned.load(); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_TRUE(returned.load()) << "Run() did not return after Admin.Shutdown";

  (void)std::remove(sock.c_str());
}

}  // namespace
