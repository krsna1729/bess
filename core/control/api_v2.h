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

#ifndef BESS_CONTROL_API_V2_H_
#define BESS_CONTROL_API_V2_H_

#include <grpc++/server_context.h>

#include "control/control_error.h"
#include "control/control_plane.h"
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

PipelineSpec FromProto(const pb::v2::Pipeline &pipeline);
pb::v2::Pipeline ToProto(const PipelineSpec &spec);
pb::v2::PipelineDiff ToProto(const PipelineDiff &diff);
void AppendPlanSteps(const PipelinePlan &plan,
                     google::protobuf::RepeatedPtrField<pb::v2::PlanStep> *out);
pb::v2::ErrorDetail ToProto(const ControlError &error);

// The gRPC status for a control-plane error; with a ServerContext, the typed
// ErrorDetail is also attached as the "bess-error-bin" trailer.
grpc::Status ToStatus(const ControlError &error,
                      grpc::ServerContext *context = nullptr);

class ControlV2Service final : public pb::v2::Control::Service {
 public:
  explicit ControlV2Service(ControlPlane &control_plane)
      : control_plane_(control_plane) {}

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

 private:
  ControlPlane &control_plane_;
};

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_API_V2_H_
