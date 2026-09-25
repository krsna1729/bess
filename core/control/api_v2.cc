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

#include "control/api_v2.h"

#include <limits>
#include <string>
#include <type_traits>
#include <variant>

#include "control/pipeline_snapshot.h"
#include "control/wire_narrow.h"
#include "worker.h"

namespace bess {
namespace control {

namespace v2 = pb::v2;

// -- desired state ------------------------------------------------------------

ControlResult<PipelineSpec> FromProto(const v2::Pipeline &pipeline) {
  PipelineSpec spec;
  int index = 0;
  for (const auto &p : pipeline.ports()) {
    PortSpec port;
    port.name = p.name();
    port.driver = p.driver();
    auto rx = WireNarrow<queue_t>(p.num_rx_queues(), "port", "num_rx_queues",
                                  index);
    auto tx = WireNarrow<queue_t>(p.num_tx_queues(), "port", "num_tx_queues",
                                  index);
    if (!rx) return std::unexpected(rx.error());
    if (!tx) return std::unexpected(tx.error());
    port.num_rx_queues = *rx;
    port.num_tx_queues = *tx;
    index++;
    port.rx_queue_size = p.rx_queue_size();
    port.tx_queue_size = p.tx_queue_size();
    port.arg = p.arg();
    spec.ports.push_back(std::move(port));
  }
  for (const auto &m : pipeline.modules()) {
    spec.modules.push_back({m.name(), m.mclass(), m.arg()});
  }
  index = 0;
  for (const auto &c : pipeline.connections()) {
    auto ogate =
        WireNarrow<gate_idx_t>(c.ogate(), "connection", "ogate", index);
    auto igate =
        WireNarrow<gate_idx_t>(c.igate(), "connection", "igate", index);
    if (!ogate) return std::unexpected(ogate.error());
    if (!igate) return std::unexpected(igate.error());
    spec.connections.push_back({c.upstream(), *ogate, c.downstream(), *igate,
                                c.skip_default_hooks()});
    index++;
  }
  for (const auto &w : pipeline.workers()) {
    spec.workers.push_back({w.wid(), w.core(), w.scheduler()});
  }
  for (const auto &t : pipeline.traffic_classes()) {
    TrafficClassSpec tc;
    tc.name = t.name();
    tc.parent = t.parent();
    tc.policy = t.policy();
    tc.resource = t.resource();
    tc.wid = t.wid();
    tc.has_priority = t.has_priority();
    tc.priority = t.priority();
    tc.has_share = t.has_share();
    tc.share = t.share();
    tc.limit.insert(t.limit().begin(), t.limit().end());
    tc.max_burst.insert(t.max_burst().begin(), t.max_burst().end());
    tc.leaf_module_name = t.leaf_module_name();
    tc.leaf_module_taskid = t.leaf_module_taskid();
    spec.traffic_classes.push_back(std::move(tc));
  }
  return spec;
}

namespace {

// An absent argument and an empty Any mean the same thing; emit neither.
bool HasArg(const google::protobuf::Any &arg) {
  return !arg.type_url().empty() || !arg.value().empty();
}

}  // namespace

v2::Pipeline ToProto(const PipelineSpec &spec) {
  v2::Pipeline out;
  for (const auto &p : spec.ports) {
    auto *port = out.add_ports();
    port->set_name(p.name);
    port->set_driver(p.driver);
    port->set_num_rx_queues(p.num_rx_queues);
    port->set_num_tx_queues(p.num_tx_queues);
    port->set_rx_queue_size(p.rx_queue_size);
    port->set_tx_queue_size(p.tx_queue_size);
    if (HasArg(p.arg)) {
      *port->mutable_arg() = p.arg;
    }
  }
  for (const auto &m : spec.modules) {
    auto *module = out.add_modules();
    module->set_name(m.name);
    module->set_mclass(m.mclass);
    if (HasArg(m.arg)) {
      *module->mutable_arg() = m.arg;
    }
  }
  for (const auto &c : spec.connections) {
    auto *conn = out.add_connections();
    conn->set_upstream(c.upstream);
    conn->set_ogate(c.ogate);
    conn->set_downstream(c.downstream);
    conn->set_igate(c.igate);
    conn->set_skip_default_hooks(c.skip_default_hooks);
  }
  for (const auto &w : spec.workers) {
    auto *worker = out.add_workers();
    worker->set_wid(w.wid);
    worker->set_core(w.core);
    worker->set_scheduler(w.scheduler);
  }
  for (const auto &t : spec.traffic_classes) {
    auto *tc = out.add_traffic_classes();
    tc->set_name(t.name);
    tc->set_parent(t.parent);
    tc->set_policy(t.policy);
    tc->set_resource(t.resource);
    tc->set_wid(t.wid);
    if (t.has_priority) tc->set_priority(t.priority);
    if (t.has_share) tc->set_share(t.share);
    tc->mutable_limit()->insert(t.limit.begin(), t.limit.end());
    tc->mutable_max_burst()->insert(t.max_burst.begin(), t.max_burst.end());
    tc->set_leaf_module_name(t.leaf_module_name);
    tc->set_leaf_module_taskid(t.leaf_module_taskid);
  }
  return out;
}

// -- diff and plan --------------------------------------------------------------

namespace {

v2::ChangeKind ToProto(ChangeKind kind) {
  switch (kind) {
    case ChangeKind::kCreate:
      return v2::CREATE;
    case ChangeKind::kRemove:
      return v2::REMOVE;
    case ChangeKind::kReplace:
      return v2::REPLACE;
    case ChangeKind::kUpdate:
      return v2::UPDATE;
    case ChangeKind::kUpdateParams:
      return v2::UPDATE_PARAMS;
    case ChangeKind::kUnchanged:
      break;
  }
  return v2::CHANGE_KIND_UNSPECIFIED;
}

std::string ConnectionName(const std::string &up, gate_idx_t ogate,
                           const std::string &down, gate_idx_t igate) {
  return up + ":" + std::to_string(ogate) + "->" + down + ":" +
         std::to_string(igate);
}

void AddChange(v2::PipelineDiff *out, v2::ObjectType type,
               const std::string &object, ChangeKind kind) {
  if (kind == ChangeKind::kUnchanged) {
    return;
  }
  auto *change = out->add_changes();
  change->set_type(type);
  change->set_object(object);
  change->set_kind(ToProto(kind));
}

}  // namespace

v2::PipelineDiff ToProto(const PipelineDiff &diff) {
  v2::PipelineDiff out;
  for (const auto &c : diff.ports) AddChange(&out, v2::PORT, c.name, c.kind);
  for (const auto &c : diff.modules) {
    AddChange(&out, v2::MODULE, c.name, c.kind);
  }
  for (const auto &c : diff.connections) {
    AddChange(&out, v2::CONNECTION,
              ConnectionName(c.upstream, c.ogate, c.downstream, c.igate),
              c.kind);
  }
  for (const auto &c : diff.workers) {
    AddChange(&out, v2::WORKER, std::to_string(c.wid), c.kind);
  }
  for (const auto &c : diff.traffic_classes) {
    AddChange(&out, v2::TRAFFIC_CLASS, c.name, c.kind);
  }
  return out;
}

namespace {

struct StepDescription {
  const char *operation;
  v2::ObjectType type;
  std::string object;
};

StepDescription Describe(const PlanOperation &op) {
  return std::visit(
      [](const auto &o) -> StepDescription {
        using T = std::decay_t<decltype(o)>;
        if constexpr (std::is_same_v<T, CreatePortOp>) {
          return {"create_port", v2::PORT, o.spec.name};
        } else if constexpr (std::is_same_v<T, UpdatePortOp>) {
          return {"update_port", v2::PORT, o.spec.name};
        } else if constexpr (std::is_same_v<T, RemovePortOp>) {
          return {"remove_port", v2::PORT, o.name};
        } else if constexpr (std::is_same_v<T, CreateModuleOp>) {
          return {"create_module", v2::MODULE, o.spec.name};
        } else if constexpr (std::is_same_v<T, RemoveModuleOp>) {
          return {"remove_module", v2::MODULE, o.name};
        } else if constexpr (std::is_same_v<T, ConnectOp>) {
          const auto &c = o.connection;
          return {"connect", v2::CONNECTION,
                  ConnectionName(c.upstream, c.ogate, c.downstream, c.igate)};
        } else if constexpr (std::is_same_v<T, DisconnectOp>) {
          return {"disconnect", v2::CONNECTION,
                  o.connection.name + ":" + std::to_string(o.connection.ogate)};
        } else if constexpr (std::is_same_v<T, AddWorkerOp>) {
          return {"add_worker", v2::WORKER, std::to_string(o.spec.wid)};
        } else if constexpr (std::is_same_v<T, RemoveWorkerOp>) {
          return {"remove_worker", v2::WORKER, std::to_string(o.wid)};
        } else if constexpr (std::is_same_v<T, CreateTcOp>) {
          return {"create_tc", v2::TRAFFIC_CLASS, o.spec.name};
        } else if constexpr (std::is_same_v<T, ReparentTcOp>) {
          return {"reparent_tc", v2::TRAFFIC_CLASS, o.spec.name};
        } else if constexpr (std::is_same_v<T, UpdateTcParamsOp>) {
          return {"update_tc_params", v2::TRAFFIC_CLASS, o.spec.name};
        } else {
          static_assert(std::is_same_v<T, RemoveTcOp>);
          return {"remove_tc", v2::TRAFFIC_CLASS, o.name};
        }
      },
      op);
}

void AppendPhase(const std::vector<PlanOperation> &ops, v2::PlanPhase phase,
                 google::protobuf::RepeatedPtrField<v2::PlanStep> *out) {
  for (const auto &op : ops) {
    const StepDescription d = Describe(op);
    auto *step = out->Add();
    step->set_phase(phase);
    step->set_operation(d.operation);
    step->set_type(d.type);
    step->set_object(d.object);
  }
}

}  // namespace

void AppendPlanSteps(const PipelinePlan &plan,
                     google::protobuf::RepeatedPtrField<v2::PlanStep> *out) {
  AppendPhase(plan.prepare_ops, v2::PREPARE, out);
  AppendPhase(plan.commit_ops, v2::COMMIT, out);
  AppendPhase(plan.retire_ops, v2::RETIRE, out);
}

// -- errors -----------------------------------------------------------------------

v2::ErrorDetail ToProto(const ControlError &error) {
  v2::ErrorDetail out;
  switch (error.code) {
    case ControlErrorCode::kInvalidArgument:
      out.set_code(v2::ErrorDetail::INVALID_ARGUMENT);
      break;
    case ControlErrorCode::kNotFound:
      out.set_code(v2::ErrorDetail::NOT_FOUND);
      break;
    case ControlErrorCode::kAlreadyExists:
      out.set_code(v2::ErrorDetail::ALREADY_EXISTS);
      break;
    case ControlErrorCode::kConflict:
      out.set_code(v2::ErrorDetail::CONFLICT);
      break;
    case ControlErrorCode::kResourceBusy:
      out.set_code(v2::ErrorDetail::RESOURCE_BUSY);
      break;
    case ControlErrorCode::kUnsupportedTransaction:
      out.set_code(v2::ErrorDetail::UNSUPPORTED_TRANSACTION);
      break;
    case ControlErrorCode::kResourceFailure:
      out.set_code(v2::ErrorDetail::RESOURCE_FAILURE);
      break;
    case ControlErrorCode::kInternal:
      out.set_code(v2::ErrorDetail::INTERNAL);
      break;
  }
  out.set_errno_value(error.err);
  out.set_message(error.message);
  out.set_object(error.object);
  out.set_field(error.field);
  return out;
}

grpc::Status ToStatus(const ControlError &error,
                      grpc::ServerContext *context) {
  grpc::StatusCode code = grpc::StatusCode::INTERNAL;
  switch (error.code) {
    case ControlErrorCode::kInvalidArgument:
      code = grpc::StatusCode::INVALID_ARGUMENT;
      break;
    case ControlErrorCode::kNotFound:
      code = grpc::StatusCode::NOT_FOUND;
      break;
    case ControlErrorCode::kAlreadyExists:
      code = grpc::StatusCode::ALREADY_EXISTS;
      break;
    case ControlErrorCode::kConflict:
      code = grpc::StatusCode::ABORTED;
      break;
    case ControlErrorCode::kResourceBusy:
      code = grpc::StatusCode::FAILED_PRECONDITION;
      break;
    case ControlErrorCode::kUnsupportedTransaction:
      code = grpc::StatusCode::UNIMPLEMENTED;
      break;
    case ControlErrorCode::kResourceFailure:
      code = grpc::StatusCode::UNAVAILABLE;
      break;
    case ControlErrorCode::kInternal:
      code = grpc::StatusCode::INTERNAL;
      break;
  }
  if (context != nullptr) {
    context->AddTrailingMetadata("bess-error-bin",
                                 ToProto(error).SerializeAsString());
  }
  return grpc::Status(code, error.message);
}

// -- service ------------------------------------------------------------------------

grpc::Status ControlV2Service::GetPipeline(grpc::ServerContext *,
                                           const v2::GetPipelineRequest *,
                                           v2::GetPipelineResponse *response) {
  const auto snapshot = control_plane_.GetPipelineVersioned();
  *response->mutable_pipeline() = ToProto(SpecFromSnapshot(snapshot.value));
  response->set_generation(snapshot.generation);
  return grpc::Status::OK;
}

grpc::Status ControlV2Service::ValidatePipeline(
    grpc::ServerContext *context, const v2::ValidatePipelineRequest *request,
    v2::ValidatePipelineResponse *response) {
  auto desired = FromProto(request->pipeline());
  if (!desired) {
    return ToStatus(desired.error(), context);
  }
  auto validated = control_plane_.ValidatePipeline(*desired);
  if (!validated) {
    return ToStatus(validated.error(), context);
  }
  *response->mutable_normalized() = ToProto(validated->spec);
  return grpc::Status::OK;
}

grpc::Status ControlV2Service::DiffPipeline(
    grpc::ServerContext *context, const v2::DiffPipelineRequest *request,
    v2::DiffPipelineResponse *response) {
  auto desired = FromProto(request->pipeline());
  if (!desired) {
    return ToStatus(desired.error(), context);
  }
  auto diff = control_plane_.DiffPipelineVersioned(*desired);
  if (!diff) {
    return ToStatus(diff.error(), context);
  }
  *response->mutable_diff() = ToProto(diff->value);
  response->set_generation(diff->generation);
  return grpc::Status::OK;
}

grpc::Status ControlV2Service::PlanPipeline(
    grpc::ServerContext *context, const v2::PlanPipelineRequest *request,
    v2::PlanPipelineResponse *response) {
  auto desired = FromProto(request->pipeline());
  if (!desired) {
    return ToStatus(desired.error(), context);
  }
  auto plan = control_plane_.PlanPipelineVersioned(*desired);
  if (!plan) {
    return ToStatus(plan.error(), context);
  }
  AppendPlanSteps(plan->value, response->mutable_steps());
  response->set_generation(plan->generation);
  return grpc::Status::OK;
}

grpc::Status ControlV2Service::ApplyPipeline(
    grpc::ServerContext *context, const v2::ApplyPipelineRequest *request,
    v2::ApplyPipelineResponse *response) {
  // DPDK functions may be called (port and module creation).
  current_worker.SetNonWorker();

  ApplyOptions options;
  if (request->has_expected_generation()) {
    options.expected_generation = request->expected_generation();
  }
  auto desired = FromProto(request->pipeline());
  if (!desired) {
    return ToStatus(desired.error(), context);
  }
  auto applied = control_plane_.ApplyPipeline(*desired, options);
  if (!applied) {
    return ToStatus(applied.error(), context);
  }
  response->set_generation(applied->generation);
  response->set_applied_ops(applied->applied_ops);
  response->set_workers_paused(applied->workers_paused);
  response->set_validation_us(applied->timing.validation_us);
  response->set_prepare_us(applied->timing.prepare_us);
  response->set_paused_commit_us(applied->timing.paused_commit_us);
  response->set_retire_us(applied->timing.retire_us);
  return grpc::Status::OK;
}

}  // namespace control
}  // namespace bess
