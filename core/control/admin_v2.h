// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_ADMIN_V2_H_
#define BESS_CONTROL_ADMIN_V2_H_

#include <cstdint>
#include <functional>

#include <grpcpp/server_context.h>

#include "control/control_plane.h"
#include "pb/control_v2.grpc.pb.h"

namespace bess {
namespace control {

// The v2 Admin service (D-107): plugins, port status, packet capture, packet
// pools, pausing and resuming the workers, and shutdown, as a protocol adapter over the ControlPlane like the
// Control service. Mutating calls name `epoch`, the daemon's (D-106).
class AdminV2Service final : public pb::v2::Admin::Service {
 public:
  // `epoch`: the daemon epoch the Control service answers with
  // (ControlV2Service::epoch()), so both services name one daemon.
  AdminV2Service(ControlPlane &control_plane, uint64_t epoch)
      : control_plane_(control_plane), epoch_(epoch) {}

  // What Shutdown runs, on a thread of its own after the answer is queued
  // (stopping the server from inside a call would wait for that call). Unset,
  // Shutdown is UNIMPLEMENTED.
  void set_shutdown(std::function<void()> shutdown) { shutdown_ = std::move(shutdown); }

  grpc::Status ImportPlugin(grpc::ServerContext *context,
                            const pb::v2::ImportPluginRequest *request,
                            pb::v2::ImportPluginResponse *response) override;
  grpc::Status UnloadPlugin(grpc::ServerContext *context,
                            const pb::v2::UnloadPluginRequest *request,
                            pb::v2::UnloadPluginResponse *response) override;
  grpc::Status GetPortStatus(grpc::ServerContext *context,
                             const pb::v2::GetPortStatusRequest *request,
                             pb::v2::GetPortStatusResponse *response) override;
  grpc::Status GetPacketPools(grpc::ServerContext *context,
                              const pb::v2::GetPacketPoolsRequest *request,
                              pb::v2::GetPacketPoolsResponse *response) override;
  grpc::Status SetCapture(grpc::ServerContext *context,
                          const pb::v2::SetCaptureRequest *request,
                          pb::v2::SetCaptureResponse *response) override;
  grpc::Status PauseWorkers(grpc::ServerContext *context,
                            const pb::v2::PauseWorkersRequest *request,
                            pb::v2::PauseWorkersResponse *response) override;
  grpc::Status ResumeWorkers(grpc::ServerContext *context,
                             const pb::v2::ResumeWorkersRequest *request,
                             pb::v2::ResumeWorkersResponse *response) override;
  grpc::Status Shutdown(grpc::ServerContext *context, const pb::v2::ShutdownRequest *request,
                        pb::v2::ShutdownResponse *response) override;

 private:
  ControlPlane &control_plane_;
  const uint64_t epoch_;
  std::function<void()> shutdown_;
};

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_ADMIN_V2_H_
