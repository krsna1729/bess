// SPDX-License-Identifier: BSD-3-Clause

#include "control/api_v2.h"

#include <gflags/gflags.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/message.h>
#include <grpcpp/health_check_service_interface.h>
#ifdef BESS_GRPC_REFLECTION
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include <rte_malloc.h>
#include <unistd.h>

#include "rcu/rcu_domain.h"
#include "control/pipeline_snapshot.h"
#include "gate_hooks/track.h"
#include "framework/memory_scope.h"
#include "framework/plugin_check.h"
#include "framework/plugin_loader.h"
#include "metadata.h"
#include "module.h"
#include "port.h"
#include "runtime/runtime_state.h"
#include "runtime/worker_manager.h"
#include "utils/dpdk_memory.h"
#include "arch/dpdk_simd.h"
#include "dpdk.h"
#include "runtime/numa_placement.h"
#include "runtime/simd_width.h"
#include "stats/event_hub.h"
#include "control/wire_narrow.h"
#include "worker.h"

namespace bess {
namespace control {

namespace v2 = pb::v2;

// -- desired state ------------------------------------------------------------

namespace {

// `arg` in canonical wire form: parsed as its type and re-serialized
// deterministically (field-number order). Desired and active arguments are
// compared by bytes, and clients do not all encode in field order
// (protobuf's JSON-to-binary keeps the JSON key order), so the same argument
// would otherwise read as a change. An unknown type or unparsable value is
// kept as sent; creating the object reports it.
google::protobuf::Any Canonical(const google::protobuf::Any &arg) {
  const std::string_view url = arg.type_url();
  const google::protobuf::Descriptor *type =
      google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(
          std::string(url.substr(url.rfind('/') + 1)));
  if (type == nullptr) {
    return arg;
  }
  std::unique_ptr<google::protobuf::Message> message(
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(type)->New());
  if (!message->ParseFromString(arg.value())) {
    return arg;
  }
  std::string value;
  {
    google::protobuf::io::StringOutputStream stream(&value);
    google::protobuf::io::CodedOutputStream out(&stream);
    out.SetSerializationDeterministic(true);
    if (!message->SerializeToCodedStream(&out)) {
      return arg;
    }
  }
  google::protobuf::Any canonical;
  canonical.set_type_url(arg.type_url());
  canonical.set_value(std::move(value));
  return canonical;
}

}  // namespace

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
    port.arg = Canonical(p.arg());
    spec.ports.push_back(std::move(port));
  }
  for (const auto &m : pipeline.modules()) {
    spec.modules.push_back({m.name(), m.mclass(), Canonical(m.arg())});
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
    spec.workers.push_back(
        {w.wid(), w.has_core() ? std::optional<int>(w.core()) : std::nullopt,
         w.scheduler()});
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
    spec.traffic_classes.push_back(std::move(tc));
  }
  for (const auto &t : pipeline.tasks()) {
    TaskPlacementSpec task;
    task.module = t.module();
    task.task = t.task();
    task.parent = t.parent();
    task.wid = t.has_wid() ? t.wid() : Worker::kAnyWorker;  // -1: none
    task.has_priority = t.has_priority();
    task.priority = t.priority();
    task.has_share = t.has_share();
    task.share = t.share();
    spec.tasks.push_back(std::move(task));
  }
  for (const auto &r : pipeline.resources()) {
    ResourceSpec resource;
    resource.name = r.name();
    for (const auto &e : r.entries()) {
      resource.entries.push_back(
          {e.has_key() ? Canonical(e.key()) : google::protobuf::Any(), Canonical(e.value())});
    }
    spec.resources.push_back(std::move(resource));
  }
  return spec;
}

namespace {

// An absent argument and an empty Any mean the same thing; emit neither.
bool HasArg(const google::protobuf::Any &arg) {
  return !arg.type_url().empty() || !arg.value().empty();
}

}  // namespace

namespace {

const char *MetadataModeName(bess::metadata::Attribute::AccessMode mode) {
  using AccessMode = bess::metadata::Attribute::AccessMode;
  switch (mode) {
    case AccessMode::kRead:
      return "read";
    case AccessMode::kWrite:
      return "write";
    case AccessMode::kUpdate:
      return "update";
  }
  return "unknown";
}

const char *MetadataContractName(
    bess::metadata::Attribute::Contract contract) {
  using Contract = bess::metadata::Attribute::Contract;
  switch (contract) {
    case Contract::kBestEffort:
      return "best_effort";
    case Contract::kRequired:
      return "required";
    case Contract::kDefault:
      return "default";
  }
  return "unknown";
}

v2::MetadataAttributeReport ToProto(
    const bess::metadata::MetadataDeclaration &declaration) {
  v2::MetadataAttributeReport out;
  out.set_module(declaration.module);
  out.set_attribute(declaration.attribute);
  out.set_size(static_cast<uint32_t>(declaration.size));
  out.set_mode(MetadataModeName(declaration.mode));
  out.set_contract(MetadataContractName(declaration.contract));
  return out;
}

void AddMetadataDeclarations(
    google::protobuf::RepeatedPtrField<v2::MetadataAttributeReport> *out,
    const std::vector<bess::metadata::MetadataDeclaration> &declarations) {
  for (const auto &declaration : declarations) {
    *out->Add() = ToProto(declaration);
  }
}

v2::MetadataReport ToProto(const bess::metadata::MetadataReport &report) {
  v2::MetadataReport out;
  out.set_declarations(static_cast<uint32_t>(report.declarations));
  out.set_resolved(static_cast<uint32_t>(report.resolved));
  AddMetadataDeclarations(out.mutable_writers_without_reader(),
                          report.writers_without_reader);
  AddMetadataDeclarations(out.mutable_readers_without_writer(),
                          report.readers_without_writer);
  AddMetadataDeclarations(out.mutable_unsatisfied_required(),
                          report.unsatisfied_required);
  AddMetadataDeclarations(out.mutable_defaults_applied(),
                          report.defaults_applied);
  AddMetadataDeclarations(out.mutable_allocation_failures(),
                          report.allocation_failures);
  for (const auto &conflict : report.conflicts) {
    v2::MetadataConflictReport *out_conflict = out.add_conflicts();
    out_conflict->set_attribute(conflict.attribute);
    out_conflict->set_reason(conflict.reason);
    for (const std::string &module : conflict.modules) {
      out_conflict->add_modules(module);
    }
  }
  return out;
}

// A module's capabilities (D-180): what its class declares for its argument,
// and how its resources change, from the class's resource tables. A module
// resource provides referential consistency (D-050); scope snapshots are
// ScopeResource's, which no module table is.
static_assert(static_cast<int>(ModuleCapabilities::State::kShared) ==
              v2::ModuleCapabilities::STATE_SHARED);
static_assert(static_cast<int>(ModuleCapabilities::State::kPerWorker) ==
              v2::ModuleCapabilities::STATE_PER_WORKER);
static_assert(static_cast<int>(ModuleCapabilities::State::kSingleWorker) ==
              v2::ModuleCapabilities::STATE_SINGLE_WORKER);
static_assert(static_cast<int>(ModuleCapabilities::State::kNone) ==
              v2::ModuleCapabilities::STATE_NONE);
static_assert(static_cast<int>(ModuleCapabilities::FlowAffinity::kRequired) ==
              v2::ModuleCapabilities::FLOW_AFFINITY_REQUIRED);
static_assert(static_cast<int>(ModuleCapabilities::FlowAffinity::kPreferred) ==
              v2::ModuleCapabilities::FLOW_AFFINITY_PREFERRED);
static_assert(static_cast<int>(ModuleCapabilities::FlowAffinity::kNone) ==
              v2::ModuleCapabilities::FLOW_AFFINITY_NONE);
v2::ModuleCapabilities ToProto(const ModuleCapabilities &declared,
                               const ResourceTables &tables) {
  v2::ModuleCapabilities out;
  out.set_state(static_cast<v2::ModuleCapabilities::State>(declared.state));
  out.set_flow_affinity(
      static_cast<v2::ModuleCapabilities::FlowAffinity>(declared.flow_affinity));
  v2::ModuleCapabilities::Updates updates = v2::ModuleCapabilities::UPDATES_NONE;
  for (const ResourceTable &t : tables) {
    v2::ModuleCapabilities::Updates table = t.requires_paused_workers
                                                ? v2::ModuleCapabilities::UPDATES_PAUSED_WORKERS
                                            : t.rebuilds ? v2::ModuleCapabilities::UPDATES_REBUILD
                                                         : v2::ModuleCapabilities::UPDATES_LIVE;
    updates = std::max(updates, table);  // the most disruptive table's
  }
  out.set_updates(updates);
  if (!tables.empty()) {
    out.set_consistency(v2::ApplyTransactionRequest::CONSISTENCY_REFERENTIAL);
  }
  return out;
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
    if (w.core.has_value()) {
      worker->set_core(*w.core);
    }
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
  }
  for (const auto &t : spec.tasks) {
    auto *task = out.add_tasks();
    task->set_module(t.module);
    task->set_task(static_cast<uint32_t>(t.task));
    task->set_parent(t.parent);
    if (t.parent.empty()) {
      task->set_wid(t.wid);
    }
    if (t.has_priority) task->set_priority(t.priority);
    if (t.has_share) task->set_share(t.share);
  }
  for (const auto &r : spec.resources) {
    auto *resource = out.add_resources();
    resource->set_name(r.name);
    for (const ResourceEntrySpec &e : r.entries) {
      auto *entry = resource->add_entries();
      if (HasArg(e.key)) {
        *entry->mutable_key() = e.key;
      }
      *entry->mutable_value() = e.value;
    }
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

std::string TaskName(const TaskPlacementSpec &task) {
  return task.module + ":" + std::to_string(task.task);
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
  for (const auto &c : diff.tasks) {
    AddChange(&out, v2::TASK, TaskName(c.desired), c.kind);
  }
  // A resource's contents change in place (D-129); the delta counts entries.
  for (const ResourceDelta &r : diff.resources) {
    AddChange(&out, v2::RESOURCE, r.name, ChangeKind::kUpdateParams);
    v2::ResourceDelta *delta = out.add_resources();
    delta->set_name(r.name);
    delta->set_created(r.created);
    delta->set_updated(r.updated);
    delta->set_removed(r.removed);
    delta->set_checked(r.checked);
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
        } else if constexpr (std::is_same_v<T, PlaceTasksOp>) {
          std::string names;
          for (const auto &task : o.tasks) {
            names += (names.empty() ? "" : ",") + TaskName(task);
          }
          return {"place_tasks", v2::TASK, names};
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
  // After the commit, before the retire phase: one dataplane transaction.
  for (const std::string &resource : plan.resources) {
    v2::PlanStep *step = out->Add();
    step->set_phase(v2::COMMIT);
    step->set_operation("apply_resources");
    step->set_type(v2::RESOURCE);
    step->set_object(resource);
  }
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
    case ControlErrorCode::kResourceExhausted:
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
    case ControlErrorCode::kResourceExhausted:
      code = grpc::StatusCode::RESOURCE_EXHAUSTED;
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

// Declared in api_v2.h; the Admin service refuses with it too.
std::optional<grpc::Status> RefuseOtherEpoch(uint64_t sent, uint64_t epoch,
                                             grpc::ServerContext *context) {
  if (sent == epoch) {
    return std::nullopt;
  }
  v2::ErrorDetail detail;
  detail.set_field("daemon_epoch");
  grpc::StatusCode code = grpc::StatusCode::INVALID_ARGUMENT;
  if (sent == 0) {
    detail.set_code(v2::ErrorDetail::INVALID_ARGUMENT);
    detail.set_message("daemon_epoch is required: the epoch of an earlier answer");
  } else {
    code = grpc::StatusCode::FAILED_PRECONDITION;
    detail.set_code(v2::ErrorDetail::EPOCH_MISMATCH);
    detail.set_daemon_epoch(epoch);
    detail.set_message(std::format(
        "built under daemon epoch {}, but the daemon restarted (epoch {}): nothing was applied; "
        "re-read before deciding again",
        sent, epoch));
  }
  if (context != nullptr) {
    context->AddTrailingMetadata("bess-error-bin", detail.SerializeAsString());
  }
  return grpc::Status(code, detail.message());
}

// -- service ------------------------------------------------------------------------

grpc::Status ControlV2Service::GetPipeline(grpc::ServerContext *context,
                                           const v2::GetPipelineRequest *request,
                                           v2::GetPipelineResponse *response) {
  ControlPlane::ResourceQuery query;
  query.include = request->include_resources();
  query.include_metadata = request->include_metadata();
  query.names.assign(request->resources().begin(), request->resources().end());
  auto read = control_plane_.ReadPipeline(query);
  if (!read) {
    return ToStatus(read.error(), context);
  }
  PipelineSpec spec = SpecFromSnapshot(read->value.snapshot);
  spec.resources = std::move(read->value.resources);
  *response->mutable_pipeline() = ToProto(spec);
  response->set_generation(read->generation);
  response->set_resource_generation(read->resource_generation);
  response->set_daemon_epoch(transactions_.epoch());
  if (request->include_metadata()) {
    const auto &metadata = read->value.metadata;
    if (metadata) {
      *response->mutable_metadata() = ToProto(*metadata);
    }
  }
  for (const WorkerSnapshot &w : read->value.snapshot.workers) {
    v2::WorkerPlacement *out = response->add_workers();
    out->set_wid(w.wid);
    out->set_core(w.core);
    out->set_numa_node(w.numa_node);
    out->set_cross_numa(w.cross_numa);
    out->set_shares_core(w.shares_core);
    out->set_on_control_cpu(w.on_control_cpu);
  }
  if (response->ByteSizeLong() > static_cast<size_t>(kMaxMessageBytes)) {
    const size_t bytes = response->ByteSizeLong();
    response->Clear();
    ControlError error = Err(EMSGSIZE,
                             "the pipeline with its resources is %zu bytes, above the %d-byte "
                             "message limit: name fewer resources",
                             bytes, kMaxMessageBytes);
    error.code = ControlErrorCode::kResourceExhausted;
    error.field = "resources";
    return ToStatus(error, context);
  }
  return grpc::Status::OK;
}

grpc::Status ControlV2Service::ReadResource(
    grpc::ServerContext *context, const v2::ReadResourceRequest *request,
    grpc::ServerWriter<v2::ResourcePage> *writer) {
  // One snapshot under the control-plane lock, then pages outside it: the
  // stream may block on a slow client, and holding the lock across the writes
  // would hold every writer behind it. The generation is the snapshot's, so a
  // reader can tell whether a later page is from the same dataplane state.
  auto read = control_plane_.ReadResource(request->resource());
  if (!read) {
    return ToStatus(read.error(), context);
  }
  // The budget is the whole serialized page -- its own fields, and for each
  // entry the repeated field's tag and length prefix -- not the entries alone:
  // framing is bytes on the wire, and the client decodes all of them.
  constexpr size_t kPageBytes = 3 << 20;  // below kMaxMessageBytes
  constexpr size_t kEntryTagBytes = 1;    // field 2, so a one-byte tag
  constexpr uint32_t kMaxPageEntries = 65536;
  size_t max_entries =
      request->max_entries() == 0 ? 4096 : request->max_entries();
  max_entries = std::min<size_t>(max_entries, kMaxPageEntries);

  v2::ResourcePage page;
  page.set_resource(read->value.name);
  page.set_resource_generation(read->resource_generation);
  page.set_daemon_epoch(transactions_.epoch());
  size_t count = 0;
  size_t bytes = page.ByteSizeLong();
  auto flush = [&]() -> std::optional<grpc::Status> {
    if (count != 0 || (page.entries_size() == 0 && read->value.entries.empty())) {
      if (!writer->Write(page)) {
        return grpc::Status(grpc::StatusCode::CANCELLED, "the reader went away");
      }
      page.mutable_entries()->Clear();
      count = 0;
      bytes = page.ByteSizeLong();
    }
    if (context->IsCancelled()) {
      return grpc::Status(grpc::StatusCode::CANCELLED, "the reader cancelled");
    }
    return std::nullopt;
  };
  for (const ResourceEntrySpec &entry : read->value.entries) {
    v2::ResourceEntry candidate;
    if (!entry.key.type_url().empty()) {
      *candidate.mutable_key() = entry.key;
    }
    *candidate.mutable_value() = entry.value;
    const size_t entry_bytes = candidate.ByteSizeLong();
    if (entry_bytes >
        static_cast<size_t>(kMaxMessageBytes) - (64 << 10)) {
      // One entry this large cannot leave in any page: refuse it by name
      // rather than let the transport fail the stream mid-table.
      ControlError error = Err(E2BIG, "resource '%s': one entry is %zu bytes",
                               read->value.name.c_str(), entry_bytes);
      error.code = ControlErrorCode::kResourceExhausted;
      error.field = "resource";
      return ToStatus(error, context);
    }
    const size_t framed_bytes =
        kEntryTagBytes +
        google::protobuf::io::CodedOutputStream::VarintSize32(
            static_cast<uint32_t>(entry_bytes)) +
        entry_bytes;
    if (count != 0 &&
        (count >= max_entries || bytes + framed_bytes > kPageBytes)) {
      if (auto stopped = flush(); stopped) {
        return *stopped;
      }
    }
    *page.add_entries() = std::move(candidate);
    count++;
    bytes += framed_bytes;
  }
  if (auto stopped = flush(); stopped) {
    return *stopped;
  }
  return grpc::Status::OK;
}

grpc::Status ControlV2Service::ResetResources(
    grpc::ServerContext *context, const v2::ResetResourcesRequest *request,
    v2::ResetResourcesResponse *response) {
  if (auto refused = RefuseOtherEpoch(request->daemon_epoch(), transactions_.epoch(), context)) {
    return *refused;
  }
  std::vector<std::string> names(request->resources().begin(),
                                 request->resources().end());
  std::optional<uint64_t> expected;
  if (request->has_expected_resource_generation()) {
    expected = request->expected_resource_generation();
  }
  auto reset = control_plane_.ResetResources(std::move(names), expected);
  if (!reset) {
    return ToStatus(reset.error(), context);
  }
  for (const std::string &name : reset->value.resources) {
    response->add_resources(name);
  }
  response->set_removed(reset->value.removed);
  response->set_resource_generation(reset->resource_generation);
  response->set_daemon_epoch(transactions_.epoch());
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
  auto *capabilities = response->mutable_capabilities();
  for (const auto &[name, report] : validated->capabilities) {
    (*capabilities)[name] = ToProto(report.declared, report.tables);
  }
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
  response->set_resource_generation(diff->resource_generation);
  response->set_daemon_epoch(transactions_.epoch());
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
  response->set_resource_generation(plan->resource_generation);
  response->set_daemon_epoch(transactions_.epoch());
  return grpc::Status::OK;
}

grpc::Status ControlV2Service::ApplyPipeline(
    grpc::ServerContext *context, const v2::ApplyPipelineRequest *request,
    v2::ApplyPipelineResponse *response) {
  if (auto refused = RefuseOtherEpoch(request->daemon_epoch(), transactions_.epoch(), context)) {
    return *refused;
  }
  // DPDK functions may be called (port and module creation).
  current_worker.SetNonWorker();

  ApplyOptions options;
  if (request->has_expected_generation()) {
    options.expected_generation = request->expected_generation();
  }
  if (request->has_expected_resource_generation()) {
    options.expected_resource_generation = request->expected_resource_generation();
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
  response->set_daemon_epoch(transactions_.epoch());
  response->set_resource_generation(applied->resource_generation);
  response->set_resource_ops(applied->resource_ops);
  return grpc::Status::OK;
}

void PrepareControlServer() {
  static const bool prepared = [] {
    grpc::EnableDefaultHealthCheckService(true);
#ifdef BESS_GRPC_REFLECTION
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
#endif
    return true;
  }();
  (void)prepared;
}

void ConfigureControlServer(grpc::ServerBuilder *builder) {
  builder->SetMaxReceiveMessageSize(kMaxMessageBytes);
  builder->SetMaxSendMessageSize(kMaxMessageBytes);
}

ControlV2Service::ControlV2Service(ControlPlane &control_plane)
    : ControlV2Service(control_plane, runtime::runtime().transactions(),
                       framework::ResourceBindings::ProcessDefault()) {}

// One event per outcome the daemon decided (not for a replay, which decided
// nothing new), emitted under the lock that decided it: events follow commit
// order.
static void EmitTransactionEvent(const v2::TransactionRecord &r) {
  std::string outcome = v2::TransactionRecord::Outcome_Name(r.outcome());
  if (outcome.rfind("OUTCOME_", 0) == 0) {
    outcome = outcome.substr(8);
  }
  for (char &c : outcome) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  runtime::runtime().events().Emit("bess.transaction", "control",
                                   {{"request_id", r.request_id()},
                                    {"outcome", outcome},
                                    {"operations", std::to_string(r.ops_size())}},
                                   r.generation());
}

grpc::Status ControlV2Service::ApplyTransaction(
    grpc::ServerContext *context, const v2::ApplyTransactionRequest *request,
    v2::ApplyTransactionResponse *response) {
  if (auto refused = RefuseOtherEpoch(request->daemon_epoch(), transactions_.epoch(), context)) {
    return *refused;
  }
  // Resources' tables are DPDK structures; commands writing the same tables
  // run under the same lock.
  current_worker.SetNonWorker();
  // BUSY means reclamation is behind the readers and nothing was attempted
  // (not recorded under the request id): wait a few grace periods here,
  // outside the lock, and try again -- each Apply reclaims what became ready
  // first. Bounded (kBusyRetries, about 6 ms in all) and never past the
  // caller's deadline, so a stalled reader still answers BUSY promptly. A
  // retried BUSY is not an answer: it is neither an event nor counted as one.
  auto backoff = kBusyFirstBackoff;
  ControlResult<v2::ApplyTransactionResponse> applied;
  for (int retry = 0;; retry++) {
    {
      auto lock = control_plane_.AcquireLock();
      // Decided before the attempt, so the engine counts a BUSY it will not
      // answer as a retry, never as an answer it has to take back.
      const bool may_retry = retry < kBusyRetries && !context->IsCancelled() &&
                             std::chrono::system_clock::now() + backoff < context->deadline();
      applied = transactions_.Apply(*request, may_retry ? dataplane::TransactionEngine::OnBusy::kRetry
                                                        : dataplane::TransactionEngine::OnBusy::kAnswer);
      if (!may_retry || !applied ||
          applied->record().outcome() != v2::TransactionRecord::OUTCOME_BUSY) {
        if (applied && !applied->replayed()) {
          EmitTransactionEvent(applied->record());
        }
        break;
      }
    }
    std::this_thread::sleep_for(backoff);
    backoff *= 2;
  }
  if (!applied) {
    return ToStatus(applied.error(), context);
  }
  *response = std::move(*applied);
  return grpc::Status::OK;
}

grpc::Status ControlV2Service::GetTransaction(
    grpc::ServerContext *, const v2::GetTransactionRequest *request,
    v2::GetTransactionResponse *response) {
  auto lock = control_plane_.AcquireLock();
  *response = transactions_.Get(request->request_id());
  return grpc::Status::OK;
}

grpc::Status ControlV2Service::ListTransactionResources(
    grpc::ServerContext *, const v2::ListTransactionResourcesRequest *,
    v2::ListTransactionResourcesResponse *response) {
  auto lock = control_plane_.AcquireLock();
  *response = transactions_.List();
  return grpc::Status::OK;
}

namespace {

// The daemon's resident set in bytes, from /proc/self/statm (field 2 is
// resident pages). Zero when the file cannot be read: the reply reports the
// facts it has, and a zero tells a controller the read failed rather than
// that the daemon is empty.
uint64_t ReadRssBytes() {
  std::ifstream statm("/proc/self/statm");
  uint64_t total_pages = 0;
  uint64_t resident_pages = 0;
  if (!(statm >> total_pages >> resident_pages)) {
    return 0;
  }
  const long page_size = ::sysconf(_SC_PAGESIZE);
  return resident_pages * static_cast<uint64_t>(page_size > 0 ? page_size : 0);
}

}  // namespace

grpc::Status ControlV2Service::GetResourceStatus(
    grpc::ServerContext *, const v2::GetResourceStatusRequest *,
    v2::GetResourceStatusResponse *response) {
  // Reads that need no control-plane lock come first, so a controller polling
  // this RPC does not hold the lock ApplyPipeline wants while they run: the
  // daemon's RSS, the EAL heap, and the two RCU numbers. The RCU numbers are
  // the ones PressureMonitor::Take() reads for the rcu_backlog alert, taken
  // from the same accessors, without the port walk Take() does under the lock
  // for samples this reply has no use for. The heap is the daemon's memory
  // node, not socket 0: bessd confines its DPDK memory to one node (D-123),
  // so every other socket's heap is empty and reading it would report a
  // successful zero for a daemon that has memory.
  const uint64_t rss_bytes = ReadRssBytes();
  rte_malloc_socket_stats stats{};
  const bool have_heap =
      rte_malloc_get_socket_stats(bess::utils::MemoryNode(), &stats) == 0;
  rcu::RcuDomain &rcu = runtime::runtime().rcu();
  const uint64_t rcu_pending = rcu.pending_retired();
  const uint64_t rcu_high_water = rcu.retire_high_water();
  // The memory policy and where the daemon stands against it (D-198): what a
  // controller compares a table's size with before it asks for one, so a
  // refusal is predictable rather than discovered. Read without the lock,
  // like the RSS above.
  const bess::utils::MemoryStatus memory = bess::utils::GetMemoryStatus();

  // The control-plane lock, as ReadPipeline takes it: the resource walk and
  // the worker list are control-plane state.
  auto lock = control_plane_.AcquireLock();
  *response = transactions_.ResourceStatus();
  response->set_rss_bytes(rss_bytes);
  if (have_heap) {
    response->set_heap_total_bytes(stats.heap_totalsz_bytes);
    response->set_heap_alloc_bytes(stats.heap_allocsz_bytes);
  }
  response->set_rcu_pending(rcu_pending);
  response->set_rcu_high_water(rcu_high_water);
  v2::MemoryStatus *out = response->mutable_memory();
  out->set_hugepages(memory.hugepages);
  out->set_fallback(memory.fallback);
  out->set_hugepage_cap_bytes(memory.hugepage_cap_bytes);
  out->set_hugepage_cap_source(memory.hugepage_cap_source);
  out->set_hugetlb_charged(memory.hugetlb_charged);
  out->set_hugepage_table_bytes(memory.hugepage_table_bytes);
  out->set_limit_bytes(memory.limit_bytes);
  out->set_limit_source(memory.limit_source);
  out->set_reserve_bytes(memory.reserve_bytes);
  out->set_used_bytes(memory.used_bytes);
  if (memory.limit_bytes > memory.reserve_bytes + memory.used_bytes) {
    out->set_available_bytes(memory.limit_bytes - memory.reserve_bytes - memory.used_bytes);
  }
  out->set_dpdk_heap_bytes(memory.dpdk_heap_bytes);
  out->set_dpdk_heap_alloc_bytes(memory.dpdk_heap_alloc_bytes);
  out->set_fallback_heap_bytes(memory.fallback_heap_bytes);
  out->set_fallback_heap_alloc_bytes(memory.fallback_heap_alloc_bytes);
  out->set_refusals(memory.refusals);
  out->set_spilled_bytes(memory.spilled_bytes);
  // What each module admitted, by resource (D-198) and by where it landed
  // (D-206), with what the objects hold now: their bytes() are read here,
  // under the control-plane lock that orders every module's creation,
  // growth and destruction.
  for (const bess::framework::ModuleMemoryUse &use :
       bess::framework::MemoryAccounts::List()) {
    v2::ModuleMemory *row = out->add_modules();
    row->set_module(use.module);
    row->set_resource(use.resource);
    row->set_admitted_bytes(use.admitted_bytes);
    row->set_current_bytes(use.current_bytes);
    row->set_hugepage_bytes(use.hugepage_bytes);
    row->set_normal_bytes(use.normal_bytes);
    row->set_glibc_bytes(use.glibc_bytes);
  }

  for (int core : runtime::runtime().workers().Cores()) {
    response->add_workers()->set_cpu(static_cast<uint32_t>(core));
  }
  return grpc::Status::OK;
}

grpc::Status ControlV2Service::InvokeAction(grpc::ServerContext *context,
                                            const v2::InvokeActionRequest *request,
                                            v2::InvokeActionResponse *response) {
  if (auto refused = RefuseOtherEpoch(request->daemon_epoch(), transactions_.epoch(), context)) {
    return *refused;
  }
  auto ran = control_plane_.RunModuleCommand(request->module(), request->action(),
                                             request->arg());
  if (!ran) {
    ControlError error = ran.error();
    error.object = request->module();
    return ToStatus(error, context);
  }
  if (ran->has_error()) {
    // The action's own failure: its errno decides the code (EBUSY: a
    // thread-unsafe action while a worker runs; ENOTSUP: no such action).
    ControlError error = Err(ran->error().code(), "%s", ran->error().errmsg().c_str());
    error.object = request->module();
    error.field = "action";
    return ToStatus(error, context);
  }
  if (ran->has_data()) {
    *response->mutable_result() = std::move(*ran->mutable_data());
  }
  response->set_daemon_epoch(transactions_.epoch());
  return grpc::Status::OK;
}

grpc::Status ControlV2Service::WatchEvents(grpc::ServerContext *context,
                                           const v2::WatchEventsRequest *request,
                                           grpc::ServerWriter<v2::Event> *writer) {
  // The hub has its own lock: no control-plane lock is held while streaming.
  stats::EventHub &hub = runtime::runtime().events();
  const std::set<std::string> types(request->types().begin(), request->types().end());
  const uint64_t epoch = transactions_.epoch();
  const uint64_t now_next = hub.next_sequence();
  uint64_t next = request->from_sequence() != 0 ? request->from_sequence() : now_next;
  // A sequence of another epoch, or one this daemon never reached, means the
  // daemon restarted since the caller's sequence: say so, then go live.
  if ((request->daemon_epoch() != 0 && request->daemon_epoch() != epoch) || next > now_next) {
    v2::Event restart;
    restart.set_type("bess.restart");
    restart.set_source("bessd");
    restart.set_daemon_epoch(epoch);
    if (!writer->Write(restart)) {
      return grpc::Status::OK;
    }
    next = now_next;
  }
  // Where this stream starts, before any event: a client that reconnects
  // before the first event still resumes from here, under this epoch.
  v2::Event start;
  start.set_type("bess.start");
  start.set_source("bessd");
  start.set_daemon_epoch(epoch);
  start.set_sequence(next);
  if (!writer->Write(start)) {
    return grpc::Status::OK;
  }
  while (!context->IsCancelled()) {
    // Short waits: cancellation is noticed within one.
    stats::EventHub::ReadResult got = hub.Read(next, 256, std::chrono::milliseconds(200));
    if (got.gap_to != 0) {
      v2::Event gap;
      gap.set_type("bess.gap");
      gap.set_source("bessd");
      gap.set_daemon_epoch(epoch);
      gap.set_gap_from(got.gap_from);
      gap.set_gap_to(got.gap_to);
      if (!writer->Write(gap)) {
        return grpc::Status::OK;
      }
    }
    bool skipped = false, wrote = false;
    for (const stats::Event &e : got.events) {
      if (!types.empty() && types.count(e.type) == 0) {
        skipped = true;
        continue;
      }
      wrote = true;
      v2::Event out;
      out.set_sequence(e.sequence);
      out.set_daemon_epoch(epoch);
      out.set_time_ns(e.time_ns);
      out.set_type(e.type);
      out.set_source(e.source);
      out.set_generation(e.generation);
      for (const auto &[k, v] : e.fields) {
        (*out.mutable_fields())[k] = v;
      }
      if (!writer->Write(out)) {
        return grpc::Status::OK;  // the client went away
      }
    }
    if (skipped && !wrote) {
      // Only filtered-out events: tell the client how far the stream got, so
      // a reconnect does not ask for (and report a gap in) what it filtered.
      v2::Event progress;
      progress.set_type("bess.progress");
      progress.set_source("bessd");
      progress.set_daemon_epoch(epoch);
      progress.set_sequence(got.next - 1);
      if (!writer->Write(progress)) {
        return grpc::Status::OK;
      }
    }
    next = got.next;
    if (got.closed) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "bessd is shutting down");
    }
  }
  return grpc::Status::OK;
}

grpc::Status ControlV2Service::ListMetrics(grpc::ServerContext *,
                                           const v2::ListMetricsRequest *,
                                           v2::ListMetricsResponse *response) {
  // The registry has its own lock: sources read their objects' counters,
  // which workers write, without the control-plane lock.
  std::vector<stats::MetricSample> samples = runtime::runtime().metrics().Collect();
  // Gate counters (D-116): the Track hook (a default hook) of every gate
  // that has one. Modules are read under the control-plane lock, which every
  // change to them takes; the counters, which workers write, are read as
  // the legacy GetModuleInfo reads them.
  {
    auto lock = control_plane_.AcquireLock();
    const auto add = [&](Module *m, bess::Gate *g, const char *direction) {
      const auto *t = reinterpret_cast<const Track *>(g->FindHookByClass(Track::kName));
      if (t == nullptr) {
        return;
      }
      const Track::Totals totals = t->totals();
      // Labels in name order, as the registry sorts them.
      const std::vector<std::pair<std::string, std::string>> labels = {
          {"direction", direction}, {"gate", std::to_string(g->gate_idx())}, {"module", m->name()}};
      samples.push_back({"bess_gate_batches_total", stats::MetricKind::kCounter,
                         "Batches through a module's gate", labels,
                         static_cast<double>(totals.cnt)});
      samples.push_back({"bess_gate_packets_total", stats::MetricKind::kCounter,
                         "Packets through a module's gate", labels,
                         static_cast<double>(totals.pkts)});
    };
    for (const auto &[name, module] : runtime::runtime().modules().All()) {
      samples.push_back({"bess_module_drops_total", stats::MetricKind::kCounter,
                         "Packets recorded by module drop and forwarding helpers",
                         {{"module", name}},
                         static_cast<double>(module->deadends())});
      for (bess::IGate *g : module->igates()) {
        if (g != nullptr) {
          add(module.get(), g, "in");
        }
      }
      for (bess::OGate *g : module->ogates()) {
        if (g != nullptr) {
          add(module.get(), g, "out");
        }
      }
    }
    transactions_.VisitReachableResources([&](const dataplane::Resource &resource) {
      const std::vector<std::pair<std::string, std::string>> labels = {
          {"resource", resource.name()}};
      samples.push_back({"bess_resource_entries", stats::MetricKind::kGauge,
                         "Live entries in a reachable resource", labels,
                         static_cast<double>(resource.LiveCount())});
      if (const auto capacity = resource.Capacity()) {
        samples.push_back({"bess_resource_capacity", stats::MetricKind::kGauge,
                           "Stated entry capacity of a reachable resource", labels,
                           static_cast<double>(*capacity)});
      }
    });
  }
  std::sort(samples.begin(), samples.end(),
            [](const stats::MetricSample &a, const stats::MetricSample &b) {
              return std::tie(a.name, a.labels) < std::tie(b.name, b.labels);
            });
  for (const stats::MetricSample &s : samples) {
    v2::MetricSample *out = response->add_samples();
    out->set_name(s.name);
    out->set_kind(s.kind == stats::MetricKind::kCounter ? v2::MetricSample::KIND_COUNTER
                                                         : v2::MetricSample::KIND_GAUGE);
    out->set_help(s.help);
    for (const auto &[key, value] : s.labels) {
      (*out->mutable_labels())[key] = value;
    }
    out->set_value(s.value);
  }
  response->set_daemon_epoch(transactions_.epoch());
  return grpc::Status::OK;
}

grpc::Status ControlV2Service::GetCapabilities(grpc::ServerContext *,
                                               const v2::GetCapabilitiesRequest *,
                                               v2::GetCapabilitiesResponse *response) {
  auto lock = control_plane_.AcquireLock();
  response->set_daemon_version(google::VersionString());
  // The RPCs as this binary's schema declares them (Control and Admin, both
  // served by bessd): a client built against a newer schema sees which ones
  // this daemon lacks.
  for (const char *name : {v2::Control::service_full_name(), v2::Admin::service_full_name()}) {
    const google::protobuf::ServiceDescriptor *service =
        google::protobuf::DescriptorPool::generated_pool()->FindServiceByName(name);
    if (service == nullptr) {
      continue;
    }
    for (int i = 0; i < service->method_count(); i++) {
      response->add_rpcs(std::string(service->full_name()) + "/" +
                         std::string(service->method(i)->name()));
    }
  }
  response->set_plugin_api_version(BESS_PLUGIN_API_VERSION);
  response->set_plugin_capabilities(framework::kSupportedPluginCapabilities);
  for (const auto &[name, builder] : ModuleBuilder::all_module_builders()) {
    response->add_module_classes(name);
  }
  for (const framework::LoadedPlugin &p : framework::LoadedPlugins()) {
    v2::PluginInfo *out = response->add_plugins();
    out->set_name(p.name);
    out->set_version(p.version);
    out->set_path(p.path);
    out->set_required_capabilities(p.required_capabilities);
  }
  for (const auto &[name, port] : runtime::runtime().ports().All()) {
    v2::PortInfo *out = response->add_ports();
    out->set_name(name);
    out->set_driver(port->port_builder() != nullptr ? port->port_builder()->class_name() : "");
    out->set_rx_queues(static_cast<uint32_t>(port->num_rx_queues()));
    out->set_tx_queues(static_cast<uint32_t>(port->num_tx_queues()));
    const packet::TxOffloadCapabilities tx = port->GetTxOffloadCapabilities();
    out->set_tx_ipv4_checksum(tx.checksums.ipv4_header);
    out->set_tx_udp_checksum(tx.checksums.udp);
    out->set_tx_tcp_checksum(tx.checksums.tcp);
    out->set_tx_outer_ipv4_checksum(tx.checksums.outer_ipv4_header);
    out->set_tx_outer_udp_checksum(tx.checksums.outer_udp);
    out->set_tx_multi_segment(tx.multi_segment_tx);
    out->set_symmetric_rss(port->symmetric_rss());
    out->set_rss_signature(port->rss_signature());
  }
  const v2::ListTransactionResourcesResponse listed = transactions_.List();
  for (const v2::TransactionResource &r : listed.resources()) {
    *response->add_resources() = r;
  }
  response->set_daemon_epoch(transactions_.epoch());
  const runtime::Placement &placement = runtime::ProcessPlacement();
  response->set_placement(placement.Summary());
  response->set_memory_node(placement.node);
  const runtime::SimdWidth &simd = runtime::ProcessSimdWidth();
  response->set_simd(simd.Summary());
  response->set_simd_width(simd.effective);
  // DPDK's own answer: equal to the above on x86, its default on arm64.
  if (IsDpdkInitialized()) {
    response->set_dpdk_simd_width(arch::DpdkMaxSimdWidth());
  }
  return grpc::Status::OK;
}

namespace {

using google::protobuf::Descriptor;
using google::protobuf::DescriptorPool;
using google::protobuf::FileDescriptor;

// An action's declared argument or result type by full name. Actions declare
// a short name by hand ("L2ForwardCommandLookupArg"): it is the full name if
// it is one, else the name in the package of the class's init argument, else
// in bess.pb (the built-in modules' package). "" when none names a message.
std::string ResolveCommandArgType(const DescriptorPool &pool, const std::string &declared,
                                  const std::string &init_arg_type) {
  if (declared.empty()) {
    return {};
  }
  if (pool.FindMessageTypeByName(declared) != nullptr) {
    return declared;
  }
  std::vector<std::string> packages;
  if (const Descriptor *init = pool.FindMessageTypeByName(init_arg_type)) {
    packages.emplace_back(init->file()->package());
  }
  packages.emplace_back("bess.pb");
  for (const std::string &package : packages) {
    std::string full = package;
    full.append(".").append(declared);
    if (pool.FindMessageTypeByName(full) != nullptr) {
      return full;
    }
  }
  return {};
}

// Adds `file` after everything it imports, once.
void AddFileClosure(const FileDescriptor *file, std::set<std::string> &added,
                    google::protobuf::FileDescriptorSet *out) {
  if (!added.emplace(file->name()).second) {
    return;
  }
  for (int i = 0; i < file->dependency_count(); i++) {
    AddFileClosure(file->dependency(i), added, out);
  }
  google::protobuf::FileDescriptorProto *proto = out->add_file();
  file->CopyTo(proto);
  file->CopyJsonNameTo(proto);  // so a client's JSON names match the daemon's
}

}  // namespace

grpc::Status ControlV2Service::GetSchema(grpc::ServerContext *, const v2::GetSchemaRequest *,
                                         v2::GetSchemaResponse *response) {
  // Plugin loads and unloads change the class registries under this lock.
  auto lock = control_plane_.AcquireLock();
  const DescriptorPool &pool = *DescriptorPool::generated_pool();
  std::set<std::string> types;
  for (const auto &[name, builder] : ModuleBuilder::all_module_builders()) {
    v2::ModuleClassSchema *out = response->add_module_classes();
    out->set_name(name);
    out->set_help(builder.help_text());
    out->set_init_arg_type(builder.init_arg_type());
    out->set_igates(builder.NumIGates());
    out->set_ogates(builder.NumOGates());
    types.insert(builder.init_arg_type());
    // Every command the class declares is an action (D-129, D-140).
    std::vector<const Command *> actions;
    for (const Command &cmd : builder.cmds()) {
      actions.push_back(&cmd);
    }
    std::sort(actions.begin(), actions.end(),
              [](const Command *a, const Command *b) { return a->cmd < b->cmd; });
    for (const Command *cmd : actions) {
      v2::ActionSchema *a = out->add_actions();
      a->set_name(cmd->cmd);
      a->set_arg_type(ResolveCommandArgType(pool, cmd->arg_type, builder.init_arg_type()));
      a->set_result_type(ResolveCommandArgType(pool, cmd->result_type, builder.init_arg_type()));
      a->set_thread_safe(cmd->mt_safe == Command::THREAD_SAFE);
      types.insert(a->arg_type());
      types.insert(a->result_type());
    }
    std::vector<const ResourceTable *> tables;
    for (const ResourceTable &t : builder.resource_tables()) {
      tables.push_back(&t);
    }
    std::sort(tables.begin(), tables.end(),
              [](const ResourceTable *a, const ResourceTable *b) { return a->table < b->table; });
    for (const ResourceTable *t : tables) {
      v2::ResourceSchema *r = out->add_resources();
      r->set_table(t->table);
      r->set_kind(t->kind == ResourceTable::SINGLETON ? v2::TransactionResource::KIND_SINGLETON
                                                      : v2::TransactionResource::KIND_MAP);
      r->set_key_type(t->key_type);
      r->set_value_type(t->value_type);
      r->set_requires_paused_workers(t->requires_paused_workers);
      r->set_rebuilds(t->rebuilds);
      types.insert(t->key_type);
      types.insert(t->value_type);
    }
  }
  for (const auto &[name, builder] : PortBuilder::all_port_builders()) {
    v2::PortDriverSchema *out = response->add_port_drivers();
    out->set_name(name);
    out->set_help(builder.help_text());
    out->set_init_arg_type(builder.init_arg_type());
    types.insert(builder.init_arg_type());
  }
  // Held: a range-for over a temporary's member destroys the temporary first
  // (before C++23's P2718, which GCC 14 and Clang 19 lack).
  const v2::ListTransactionResourcesResponse resources = transactions_.List();
  for (const v2::TransactionResource &r : resources.resources()) {
    types.insert(r.key_type());
    types.insert(r.value_type());
  }
  std::set<std::string> added;
  for (const std::string &type : types) {
    if (const Descriptor *d = pool.FindMessageTypeByName(type)) {
      AddFileClosure(d->file(), added, response->mutable_descriptors());
    }
  }
  response->set_daemon_epoch(transactions_.epoch());
  return grpc::Status::OK;
}

}  // namespace control
}  // namespace bess
