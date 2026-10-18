// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_API_V2_H_
#define BESS_CONTROL_API_V2_H_

#include <chrono>
#include <optional>

#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include "control/control_error.h"
#include "control/control_plane.h"
#include "control/dataplane_transactions.h"
#include "control/pipeline_diff.h"
#include "control/pipeline_plan.h"
#include "control/pipeline_spec.h"
#include "pb/control_v2.grpc.pb.h"

namespace bess {
namespace control {

// G1: the v2 desired-state API as a protocol adapter over the G0 engine.
//
//   FromProto -> ControlPlane call -> ToProto
//
// Every decision (validation, diff, ordering, pause, rollback, generations)
// is the ControlPlane's; this layer only translates.

// Fallible: a wire value that does not fit the narrower C++ field (a gate
// above gate_idx_t, a queue count above queue_t) is rejected here, before the
// cast, because after it the evidence is gone -- 65536 would arrive as gate 0.
ControlResult<PipelineSpec> FromProto(const pb::v2::Pipeline &pipeline);
pb::v2::Pipeline ToProto(const PipelineSpec &spec);
pb::v2::PipelineDiff ToProto(const PipelineDiff &diff);
void AppendPlanSteps(const PipelinePlan &plan,
                     google::protobuf::RepeatedPtrField<pb::v2::PlanStep> *out);
pb::v2::ErrorDetail ToProto(const ControlError &error);

// The gRPC status for a control-plane error; with a ServerContext, the typed
// ErrorDetail is also attached as the "bess-error-bin" trailer.
grpc::Status ToStatus(const ControlError &error,
                      grpc::ServerContext *context = nullptr);

// A mutating request names the daemon epoch it was built under (D-106).
// Refused before any side effect: INVALID_ARGUMENT for 0, FAILED_PRECONDITION
// with ErrorDetail EPOCH_MISMATCH for another; nullopt if it is `epoch`.
std::optional<grpc::Status> RefuseOtherEpoch(uint64_t sent, uint64_t epoch,
                                             grpc::ServerContext *context);

// The control server's settings, shared by bessd and the tests (D-026).
// Messages up to kMaxMessageBytes each way: gRPC's 4 MiB default caps one
// dataplane transaction at roughly 30K typed rules; 64 MiB allows about
// half a million, beyond which a bulk load belongs on a streaming path.
inline constexpr int kMaxMessageBytes = 64 << 20;
// Once per process, before the first ServerBuilder exists (gRPC collects
// server plugins in the builder's constructor): the standard health service
// (grpc.health.v1.Health, SERVING while bessd serves) and, when built with
// it, server reflection -- so orchestrators and generic tools (grpcurl) work
// without BESS's protos.
void PrepareControlServer();
// Per builder: message size limits.
void ConfigureControlServer(grpc::ServerBuilder *builder);

// ApplyTransaction waits out a BUSY answer (reclamation behind the readers)
// itself before passing it on: up to kBusyRetries more attempts, sleeping
// kBusyFirstBackoff, then double that, between them (about 6 ms in all; a
// grace period is ~11 us, or one task invocation when that is longer, D-012).
inline constexpr int kBusyRetries = 5;
inline constexpr std::chrono::microseconds kBusyFirstBackoff{200};

class ControlV2Service final : public pb::v2::Control::Service {
 public:
  // Dataplane transactions go to `engine` (the runtime's, by default).
  explicit ControlV2Service(ControlPlane &control_plane);
  ControlV2Service(ControlPlane &control_plane,
                   dataplane::TransactionEngine &engine,
                   const framework::ResourceBindings &bindings)
      : control_plane_(control_plane), transactions_(engine, bindings) {}

  // This daemon's epoch, drawn once (the Admin service answers with it too).
  uint64_t epoch() const { return transactions_.epoch(); }

  grpc::Status GetPipeline(grpc::ServerContext *context,
                           const pb::v2::GetPipelineRequest *request,
                           pb::v2::GetPipelineResponse *response) override;
  grpc::Status ValidatePipeline(
      grpc::ServerContext *context,
      const pb::v2::ValidatePipelineRequest *request,
      pb::v2::ValidatePipelineResponse *response) override;
  grpc::Status DiffPipeline(grpc::ServerContext *context,
                            const pb::v2::DiffPipelineRequest *request,
                            pb::v2::DiffPipelineResponse *response) override;
  grpc::Status PlanPipeline(grpc::ServerContext *context,
                            const pb::v2::PlanPipelineRequest *request,
                            pb::v2::PlanPipelineResponse *response) override;
  grpc::Status ApplyPipeline(grpc::ServerContext *context,
                             const pb::v2::ApplyPipelineRequest *request,
                             pb::v2::ApplyPipelineResponse *response) override;

  // Dataplane transactions (G1.2c, D-025), under the control-plane lock.
  grpc::Status ApplyTransaction(
      grpc::ServerContext *context,
      const pb::v2::ApplyTransactionRequest *request,
      pb::v2::ApplyTransactionResponse *response) override;
  grpc::Status GetTransaction(
      grpc::ServerContext *context,
      const pb::v2::GetTransactionRequest *request,
      pb::v2::GetTransactionResponse *response) override;
  grpc::Status ListTransactionResources(
      grpc::ServerContext *context,
      const pb::v2::ListTransactionResourcesRequest *request,
      pb::v2::ListTransactionResourcesResponse *response) override;
  // Module actions (D-129).
  grpc::Status InvokeAction(grpc::ServerContext *context,
                            const pb::v2::InvokeActionRequest *request,
                            pb::v2::InvokeActionResponse *response) override;
  // What the daemon has and has used: the resources it would write, the
  // memory it would spend from, and the reclamation budget it would draw on.
  grpc::Status GetResourceStatus(
      grpc::ServerContext *context,
      const pb::v2::GetResourceStatusRequest *request,
      pb::v2::GetResourceStatusResponse *response) override;
  grpc::Status ListMetrics(grpc::ServerContext *context,
                           const pb::v2::ListMetricsRequest *request,
                           pb::v2::ListMetricsResponse *response) override;
  grpc::Status GetCapabilities(grpc::ServerContext *context,
                               const pb::v2::GetCapabilitiesRequest *request,
                               pb::v2::GetCapabilitiesResponse *response) override;
  grpc::Status GetSchema(grpc::ServerContext *context,
                         const pb::v2::GetSchemaRequest *request,
                         pb::v2::GetSchemaResponse *response) override;
  grpc::Status WatchEvents(grpc::ServerContext *context,
                           const pb::v2::WatchEventsRequest *request,
                           grpc::ServerWriter<pb::v2::Event> *writer) override;

 private:
  ControlPlane &control_plane_;
  DataplaneTransactions transactions_;
};

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_API_V2_H_
