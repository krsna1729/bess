// SPDX-License-Identifier: BSD-3-Clause

#include "control/control_plane.h"
#include "runtime/numa_placement.h"
#include "runtime/thread_placement.h"
#include "runtime/worker_manager.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <memory>

#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "utils/logging.h"

#include "control/pipeline_resources.h"
#include "dataplane/transaction_engine.h"
#include "framework/instance_registry.h"
#include "framework/plugin_loader.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "stats/event_hub.h"
#include "gate.h"
#include "message.h"
#include "module.h"
#include "module_graph.h"
#include "port.h"
#include "rcu/rcu_domain.h"
#include "runtime/opts.h"
#include "resume_hook.h"
#include "scheduler.h"
#include "utils/format.h"
#include "worker.h"

namespace bess {
namespace control {

namespace {

// Legacy internal APIs (ModuleGraph::CreateModule, Port::InitWithGenericArg)
// still report failures through a protobuf error or a CommandResponse. Bridge
// them into the control-plane error model at the call site; this goes away as
// the runtime/registry layer acquires its own result types (G0 commit 2).
ControlError ErrorFromLegacy(int code, const std::string& message) {
  return ControlError{CodeFromErrno(code), code, message, "", ""};
}

// D-129: the RPC-reachable resources a new module registered are exactly
// those its class declares, of the declared kind and types, so GetSchema and
// desired-state validation tell the truth about every module of the class.
ControlResult<void> CheckDeclaredResources(const ModuleBuilder& builder,
                                           const std::string& module) {
  const dataplane::TransactionEngine& engine = runtime::runtime().transactions();
  const framework::ResourceBindings& bindings = framework::ResourceBindings::ProcessDefault();
  auto refuse = [&](const std::string& what) -> ControlResult<void> {
    ControlError error = Err(EINVAL, "module '%s' (class %s): %s", module.c_str(),
                             builder.class_name().c_str(), what.c_str());
    error.object = module;
    return std::unexpected(error);
  };
  const std::string prefix = module + "/";
  size_t found = 0;
  for (const std::string& name : engine.ResourceNames()) {
    if (!name.starts_with(prefix)) {
      continue;
    }
    const dataplane::Resource* resource = engine.FindResource(name);
    const framework::ResourceCodec* codec =
        resource == nullptr ? nullptr : bindings.Find(*resource);
    if (codec == nullptr) {
      continue;  // not reachable over the RPC
    }
    const ResourceTable* table = builder.FindResourceTable(name.substr(prefix.size()));
    if (table == nullptr) {
      return refuse("registered resource '" + name + "', which its class does not declare");
    }
    const bool singleton = codec->kind() == framework::ResourceCodec::Kind::kSingleton;
    if (singleton != (table->kind == ResourceTable::SINGLETON) ||
        codec->key_type() != table->key_type || codec->value_type() != table->value_type ||
        codec->requires_paused_workers() != table->requires_paused_workers) {
      return refuse("resource '" + name + "' differs from its declaration (kind, " +
                    codec->key_type() + " -> " + codec->value_type() + ", paused workers)");
    }
    found++;
  }
  if (found != builder.resource_tables().size()) {
    return refuse("registered " + std::to_string(found) + " of its class's " +
                  std::to_string(builder.resource_tables().size()) + " resource tables");
  }
  return {};
}

// Describes how a live traffic class is attached right now, so a refused move
// can put it back: parent (or root placement), plus the priority or share its
// parent holds for it.
TrafficClassSpec AttachmentSpecOf(const bess::TrafficClass* c) {
  TrafficClassSpec spec;
  spec.name = c->name();
  spec.policy = (c->policy() >= 0 && c->policy() < NUM_POLICIES)
                    ? TrafficPolicyName[c->policy()]
                    : "";
  spec.wid = WidOrAny(c->OwnerWorker());

  const bess::TrafficClass* parent = c->parent();
  if (parent == nullptr) {
    return spec;  // a root: wid is the placement
  }

  // An internal wrapper (a scheduler's default round-robin root) is not a
  // reattach target: the class is a root that happens to sit under it.
  if (!parent->name().empty() && parent->name()[0] == '!') {
    spec.parent = "";
    spec.wid = WidOrAny(c->OwnerWorker());
    return spec;
  }

  spec.parent = parent->name();
  spec.wid = Worker::kAnyWorker;

  switch (parent->policy()) {
    case bess::POLICY_PRIORITY: {
      const auto* prio = static_cast<const bess::PriorityTrafficClass*>(parent);
      for (const auto& child : prio->children()) {
        if (child.c_ == c) {
          spec.has_priority = true;
          spec.priority = child.priority_;
          break;
        }
      }
      break;
    }
    case bess::POLICY_WEIGHTED_FAIR: {
      const auto* wrr =
          static_cast<const bess::WeightedFairTrafficClass*>(parent);
      for (const auto& child : wrr->children()) {
        if (child.first == c) {
          spec.has_share = true;
          spec.share = child.second;
          break;
        }
      }
      break;
    }
    default:
      break;
  }

  return spec;
}

// A rate limit or burst below zero would wrap when narrowed to the
// scheduler's unsigned fields; refuse it instead (v1 and v2 share this path).
std::optional<ControlError> NegativeRate(const TrafficClassSpec &spec) {
  for (const auto *map : {&spec.limit, &spec.max_burst}) {
    for (const auto &[resource, value] : *map) {
      if (value < 0) {
        return Err(EINVAL, "%s for '%s' must not be negative (%lld)",
                   map == &spec.limit ? "limit" : "max_burst",
                   resource.c_str(), static_cast<long long>(value));
      }
    }
  }
  return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

ControlResult<PortInfo> ControlPlane::CreatePort(const PortSpec& spec) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = CreatePortLocked(spec);
  if (result) {
    // Every successful structural mutation advances the generation, so
    // an apply and a direct change share one sequence
    // (MODERNIZATION.md section 9.8). The *Locked() primitives never bump:
    // ApplyPipeline() bumps once for the whole transaction.
    runtime::runtime().BumpGeneration();
  }
  return result;
}

ControlResult<PortInfo> ControlPlane::CreatePortLocked(const PortSpec& spec) {

  if (spec.driver.empty()) {
    return std::unexpected(Err(EINVAL, "Missing 'driver' field"));
  }

  const auto& builders = PortBuilder::all_port_builders();
  const auto& it = builders.find(spec.driver);
  if (it == builders.end()) {
    return std::unexpected(
        Err(ENOENT, "No port driver '%s' found", spec.driver.c_str()));
  }
  const PortBuilder& driver = it->second;

  std::unique_ptr<::Port> p;

  queue_t num_inc_q = spec.num_rx_queues;
  queue_t num_out_q = spec.num_tx_queues;
  uint64_t size_inc_q = spec.rx_queue_size;
  uint64_t size_out_q = spec.tx_queue_size;

  if (num_inc_q == 0) {
    num_inc_q = 1;
  }

  if (num_out_q == 0) {
    num_out_q = 1;
  }

  if (num_inc_q > MAX_QUEUES_PER_DIR || num_out_q > MAX_QUEUES_PER_DIR) {
    return std::unexpected(Err(EINVAL, "Invalid number of queues"));
  }

  if (size_inc_q > MAX_QUEUE_SIZE || size_out_q > MAX_QUEUE_SIZE) {
    return std::unexpected(Err(EINVAL, "Invalid queue size"));
  }

  runtime::PortRegistry &ports = runtime::runtime().ports();
  std::string port_name;

  if (spec.name.length() > 0) {
    if (ports.Contains(spec.name)) {
      return std::unexpected(
          Err(EEXIST, "Port '%s' already exists", spec.name.c_str()));
    }
    port_name = spec.name;
  } else {
    port_name = ports.GenerateDefaultName(driver.class_name(),
                                          driver.name_template());
  }

  // Try to create and initialize the port.
  p.reset(driver.CreatePort(port_name));

  if (size_inc_q == 0) {
    size_inc_q = p->DefaultIncQueueSize();
  }

  if (size_out_q == 0) {
    size_out_q = p->DefaultOutQueueSize();
  }

  p->num_queues[PACKET_DIR_INC] = num_inc_q;
  p->num_queues[PACKET_DIR_OUT] = num_out_q;
  p->queue_size[PACKET_DIR_INC] = size_inc_q;
  p->queue_size[PACKET_DIR_OUT] = size_out_q;

  // DPDK functions may be called, so be prepared
  current_worker.SetNonWorker();

  // D-198: memory refused while the driver initializes (a queue, a pool)
  // ends like an Init that returned an error: `p` is destroyed on the way
  // out and nothing was registered.
  CommandResponse ret;
  try {
    ret = p->InitWithGenericArg(spec.arg);
  } catch (const std::bad_alloc& e) {
    ControlError error = OutOfMemory(e);
    error.object = port_name;
    return std::unexpected(std::move(error));
  }

  {
    google::protobuf::Any empty;

    if (ret.data().SerializeAsString() != empty.SerializeAsString()) {
      LOG(WARNING) << port_name << "::" << driver.class_name()
                   << " Init() returned non-empty response: "
                   << ret.data().DebugString();
    }
  }

  if (ret.error().code() != 0) {
    return std::unexpected(
        ErrorFromLegacy(ret.error().code(), ret.error().errmsg()));
  }

  PortInfo info;
  info.name = p->name();
  info.driver = driver.class_name();
  info.mac_addr = p->conf().mac_addr.ToString();

  if (!ports.Add(std::move(p))) {
    return std::unexpected(
        Err(EEXIST, "Port '%s' already exists", info.name.c_str()));
  }

  return info;
}

ControlResult<void> ControlPlane::DestroyPortLocked(const std::string& name) {

  if (name.length() == 0) {
    return std::unexpected(
        Err(EINVAL, "Argument must be a name in str"));
  }

  int ret = runtime::runtime().ports().Destroy(name);
  if (ret == -ENOENT) {
    return std::unexpected(Err(ENOENT, "No port `%s' found", name.c_str()));
  }
  if (ret) {
    return std::unexpected(Errno(-ret));
  }

  return {};
}

ControlResult<bess::pb::CommandResponse> ControlPlane::RunModuleCommand(
    const std::string& module, const std::string& command,
    const google::protobuf::Any& arg) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (module.empty()) {
    return std::unexpected(Err(EINVAL, "Missing module name field 'name'"));
  }
  Module* m = runtime::runtime().modules().Find(module);
  if (m == nullptr) {
    return std::unexpected(Err(ENOENT, "No module '%s' found", module.c_str()));
  }
  // DPDK functions may be called, so be prepared.
  current_worker.SetNonWorker();
  auto ran = m->module_builder()->RunCommand(m, command, arg);
  // D-129: an action that can edit a resource's contents advances the
  // resource generation, whether or not it succeeded (it may have edited
  // part), so an ApplyPipeline that read the old contents conflicts instead
  // of undoing the edit. An action that does not exist ran nothing.
  for (const auto& cmd : m->module_builder()->cmds()) {
    if (cmd.cmd == command && cmd.EditsResources()) {
      runtime::runtime().transactions().AdvanceGeneration();
    }
  }
  return ran;
}

ControlResult<void> ControlPlane::ResetPortsLocked() {
  WorkerPauser wp;

  runtime::PortRegistry &ports = runtime::runtime().ports();

  std::vector<std::string> names;
  names.reserve(ports.Size());
  for (const auto &pair : ports.All()) {
    names.push_back(pair.first);
  }

  for (const std::string &name : names) {
    int ret = ports.Destroy(name);
    if (ret) {
      return std::unexpected(Errno(-ret));
    }
  }

  LOG(INFO) << "*** All ports have been destroyed ***";
  return {};
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

ControlResult<std::string> ControlPlane::CreateModule(const ModuleSpec& spec) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = CreateModuleLocked(spec);
  if (result) {
    // Every successful structural mutation advances the generation, so
    // an apply and a direct change share one sequence
    // (MODERNIZATION.md section 9.8). The *Locked() primitives never bump:
    // ApplyPipeline() bumps once for the whole transaction.
    runtime::runtime().BumpGeneration();
  }
  return result;
}

ControlResult<std::string> ControlPlane::CreateModuleLocked(const ModuleSpec& spec) {

  if (spec.mclass.length() == 0) {
    return std::unexpected(Err(EINVAL, "Missing 'mclass' field"));
  }

  const auto& builders = ModuleBuilder::all_module_builders();
  const auto& it = builders.find(spec.mclass);
  if (it == builders.end()) {
    return std::unexpected(
        Err(ENOENT, "No mclass '%s' found", spec.mclass.c_str()));
  }
  const ModuleBuilder& builder = it->second;

  std::string mod_name;
  if (spec.name.length()) {
    if (runtime::runtime().modules().Contains(spec.name)) {
      return std::unexpected(
          Err(EEXIST, "Module %s exists", spec.name.c_str()));
    }
    mod_name = spec.name;
  } else {
    mod_name = ModuleGraph::GenerateDefaultName(builder.class_name(),
                                                builder.name_template());
  }

  // DPDK functions may be called, so be prepared
  current_worker.SetNonWorker();

  pb_error_t error;
  Module* module =
      ModuleGraph::CreateModule(builder, mod_name, spec.arg, &error);
  if (!module) {
    return std::unexpected(ErrorFromLegacy(error.code(), error.errmsg()));
  }
  bess::event_modules[bess::Event::PreResume].insert(module);

  std::string name = module->name();
  if (auto declared = CheckDeclaredResources(builder, name); !declared) {
    auto destroyed = DestroyModuleLocked(name);
    CHECK(destroyed) << destroyed.error().message;
    return std::unexpected(declared.error());
  }

  return name;
}

ControlResult<void> ControlPlane::DestroyModule(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = DestroyModuleLocked(name);
  if (result) {
    // Every successful structural mutation advances the generation, so
    // an apply and a direct change share one sequence
    // (MODERNIZATION.md section 9.8). The *Locked() primitives never bump:
    // ApplyPipeline() bumps once for the whole transaction.
    runtime::runtime().BumpGeneration();
  }
  return result;
}

ControlResult<void> ControlPlane::DestroyModuleLocked(const std::string& name) {

  WorkerPauser wp;

  if (name.length() == 0) {
    return std::unexpected(Err(EINVAL, "Argument must be a name in str"));
  }

  Module* m = runtime::runtime().modules().Find(name);
  if (!m) {
    return std::unexpected(Err(ENOENT, "No module '%s' found", name.c_str()));
  }

  auto& resume_modules = bess::event_modules[bess::Event::PreResume];
  if (resume_modules.erase(m) > 0) {
    VLOG(1) << "Cleared pre-resume hook for module '" << m->name() << "'";
  }

  ModuleGraph::DestroyModule(m);

  return {};
}

ControlResult<void> ControlPlane::ResetModulesLocked() {
  WorkerPauser wp;

  ModuleGraph::DestroyAllModules();
  bess::event_modules.clear();
  LOG(INFO) << "*** All modules have been destroyed ***";
  return {};
}

ControlResult<void> ControlPlane::ConnectModules(const ConnectionSpec& spec) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = ConnectModulesLocked(spec);
  if (result) {
    // Every successful structural mutation advances the generation, so
    // an apply and a direct change share one sequence
    // (MODERNIZATION.md section 9.8). The *Locked() primitives never bump:
    // ApplyPipeline() bumps once for the whole transaction.
    runtime::runtime().BumpGeneration();
  }
  return result;
}

ControlResult<void> ControlPlane::ConnectModulesLocked(const ConnectionSpec& spec) {

  VLOG(1) << "ConnectModules " << spec.upstream << ":" << spec.ogate << " -> "
          << spec.igate << ":" << spec.downstream;

  if (spec.upstream.empty() || spec.downstream.empty()) {
    return std::unexpected(Err(EINVAL, "Missing 'm1' or 'm2' field"));
  }

  Module* m1 = runtime::runtime().modules().Find(spec.upstream);
  if (!m1) {
    return std::unexpected(
        Err(ENOENT, "No module '%s' found", spec.upstream.c_str()));
  }

  Module* m2 = runtime::runtime().modules().Find(spec.downstream);
  if (!m2) {
    return std::unexpected(
        Err(ENOENT, "No module '%s' found", spec.downstream.c_str()));
  }

  int ret;
  if (is_any_worker_running()) {
    ModuleGraph::PropagateActiveWorker();
    if (m1->num_active_workers() || m2->num_active_workers()) {
      WorkerPauser wp;  // Only pause when absolutely required
      ret = ModuleGraph::ConnectModules(m1, spec.ogate, m2, spec.igate,
                                        spec.skip_default_hooks);
      if (ret < 0) {
        return std::unexpected(
            Err(-ret, "Connection %s:%d->%d:%s failed", spec.upstream.c_str(),
                spec.ogate, spec.igate, spec.downstream.c_str()));
      }
      return {};
    }
  }

  ret = ModuleGraph::ConnectModules(m1, spec.ogate, m2, spec.igate,
                                    spec.skip_default_hooks);
  if (ret < 0) {
    return std::unexpected(Err(-ret, "Connection %s:%d->%d:%s failed",
                               spec.upstream.c_str(), spec.ogate, spec.igate,
                               spec.downstream.c_str()));
  }

  return {};
}

ControlResult<void> ControlPlane::DisconnectModules(
    const DisconnectionSpec& spec) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = DisconnectModulesLocked(spec);
  if (result) {
    // Every successful structural mutation advances the generation, so
    // an apply and a direct change share one sequence
    // (MODERNIZATION.md section 9.8). The *Locked() primitives never bump:
    // ApplyPipeline() bumps once for the whole transaction.
    runtime::runtime().BumpGeneration();
  }
  return result;
}

ControlResult<void> ControlPlane::DisconnectModulesLocked(const DisconnectionSpec& spec) {

  WorkerPauser wp;

  if (spec.name.length() == 0) {
    return std::unexpected(Err(EINVAL, "Missing 'name' field"));
  }

  Module* m = runtime::runtime().modules().Find(spec.name);
  if (!m) {
    return std::unexpected(
        Err(ENOENT, "No module '%s' found", spec.name.c_str()));
  }

  int ret = ModuleGraph::DisconnectModule(m, spec.ogate);
  if (ret < 0) {
    return std::unexpected(
        Err(-ret, "Disconnection %s:%d failed", spec.name.c_str(), spec.ogate));
  }

  return {};
}

// ---------------------------------------------------------------------------
// Workers
// ---------------------------------------------------------------------------

ControlResult<void> ControlPlane::AddWorker(uint64_t wid,
                                            std::optional<uint64_t> core,
                                            const std::string& scheduler) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = AddWorkerLocked(wid, core, scheduler);
  if (result) {
    // Every successful structural mutation advances the generation, so
    // an apply and a direct change share one sequence
    // (MODERNIZATION.md section 9.8). The *Locked() primitives never bump:
    // ApplyPipeline() bumps once for the whole transaction.
    runtime::runtime().BumpGeneration();
  }
  return result;
}

ControlResult<void> ControlPlane::AddWorkerLocked(
    uint64_t wid, std::optional<uint64_t> core, const std::string& scheduler) {

  const std::optional<dataplane::WorkerId> id = dataplane::WorkerIdFrom(wid);
  if (!id) {
    return std::unexpected(Err(EINVAL, "Invalid worker id"));
  }
  // An explicit core anywhere in the process's CPU set is honoured -- on
  // another node, on a control CPU, beside another worker -- and diagnosed
  // at launch (D-123); only one outside the set is refused (D-027).
  std::optional<dataplane::CpuId> cpu;
  if (core.has_value()) {
    cpu = dataplane::CpuIdFrom(*core);
    if (!cpu || !runtime::CpuPresent(*cpu)) {
      return std::unexpected(Err(EINVAL, "Invalid core %llu",
                                 static_cast<unsigned long long>(*core)));
    }
    if (!runtime::CpuAllowed(*cpu)) {
      return std::unexpected(
          Err(EINVAL, "core %u is not in bessd's CPU set (%s)", cpu->value(),
              runtime::CpuList(runtime::ProcessCpus()).c_str()));
    }
  }
  if (is_worker_active(*id)) {
    return std::unexpected(Err(EEXIST, "worker:%u is already active",
                               unsigned{id->value()}));
  }
  if (scheduler != "" && scheduler != "experimental") {
    return std::unexpected(Err(EINVAL, "Invalid scheduler %s",
                               scheduler.c_str()));
  }

  if (!cpu.has_value()) {
    // Automatic: a CPU of the memory node no live worker runs on, explicit
    // or automatic. The worker keeps it for its life.
    const runtime::Placement& placement = runtime::ProcessPlacement();
    const std::vector<int> used = runtime::runtime().workers().Cores();
    const std::optional<int> next = placement.NextWorkerCpu(used);
    if (!next) {
      const std::set<int> worker_cpus = placement.WorkerCpus();
      ControlError error = Err(
          ENOSPC,
          "no free CPU for an automatic worker on node %d; worker CPUs: %s "
          "(each runs a worker, or none is left beside the control CPUs %s); "
          "give the worker an explicit core, or bessd more CPUs",
          placement.node,
          worker_cpus.empty() ? "none"
                              : runtime::FormatCpuList(worker_cpus).c_str(),
          placement.control_cpus.empty()
              ? "(none)"
              : runtime::FormatCpuList(placement.control_cpus).c_str());
      error.code = ControlErrorCode::kResourceExhausted;
      error.object = std::to_string(wid);
      error.field = "core";
      return std::unexpected(error);
    }
    cpu = dataplane::CpuId(static_cast<uint32_t>(*next));  // a CPU of the set
  }

  runtime::runtime().workers().Launch(*id, *cpu, scheduler);
  return {};
}

ControlResult<void> ControlPlane::DestroyWorker(uint64_t wid) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = DestroyWorkerLocked(wid);
  if (result) {
    // Every successful structural mutation advances the generation, so
    // an apply and a direct change share one sequence
    // (MODERNIZATION.md section 9.8). The *Locked() primitives never bump:
    // ApplyPipeline() bumps once for the whole transaction.
    runtime::runtime().BumpGeneration();
  }
  return result;
}

ControlResult<void> ControlPlane::DestroyWorkerLocked(uint64_t wid) {

  const std::optional<dataplane::WorkerId> id = dataplane::WorkerIdFrom(wid);
  if (!id) {
    return std::unexpected(Err(EINVAL, "Invalid worker id"));
  }
  Worker* worker = runtime::runtime().workers().Get(*id);
  if (!worker) {
    return std::unexpected(
        Err(ENOENT, "Worker %u is not active", unsigned{id->value()}));
  }

  bess::TrafficClass* root = worker->scheduler()->root();
  if (root) {
    for (const auto& it : TrafficClassBuilder::all_tcs()) {
      bess::TrafficClass* c = it.second.get();
      if (c->policy() == bess::POLICY_LEAF && c->Root() == root) {
        return std::unexpected(Err(EBUSY, "Worker %u has active tasks: %s",
                                   unsigned{id->value()},
                                   c->name().c_str()));
      }
    }
  }

  destroy_worker(*id);
  return {};
}

ControlResult<void> ControlPlane::ResetWorkers() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = ResetWorkersLocked();
  if (result) {
    // Every successful structural mutation advances the generation, so
    // an apply and a direct change share one sequence
    // (MODERNIZATION.md section 9.8). The *Locked() primitives never bump:
    // ApplyPipeline() bumps once for the whole transaction.
    runtime::runtime().BumpGeneration();
  }
  return result;
}

ControlResult<void> ControlPlane::ResetWorkersLocked() {
  WorkerPauser wp;
  destroy_all_workers();
  LOG(INFO) << "*** All workers have been destroyed ***";
  return {};
}

ControlResult<void> ControlPlane::PauseAll() {
  std::lock_guard<std::mutex> lock(mutex_);

  pause_all_workers();
  LOG(INFO) << "*** All workers have been paused ***";
  return {};
}

ControlResult<void> ControlPlane::PauseWorker(uint64_t wid) {
  std::lock_guard<std::mutex> lock(mutex_);

  // The wire id is uint64: refused out of range, never narrowed (2^32 was
  // worker 0).
  const std::optional<dataplane::WorkerId> id = dataplane::WorkerIdFrom(wid);
  if (!id) {
    return std::unexpected(Err(EINVAL, "worker id %llu is out of range (max %d)",
                               static_cast<unsigned long long>(wid),
                               Worker::kMaxWorkers - 1));
  }

  // TODO: It should be made harder to wreak havoc on the rest of the daemon
  // when using PauseWorker(). For now a warning and suggestion that this is
  // for experts only is sufficient.
  LOG(WARNING) << "PauseWorker() is an experimental operation and should be"
               << " used with care. Long-term support not guaranteed.";
  pause_worker(*id);
  LOG(INFO) << "*** Worker " << wid << " has been paused ***";
  return {};
}

ControlResult<void> ControlPlane::ResumeAll() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!is_any_worker_running()) {
    attach_orphans();
  }

  std::optional<CommandResponse> hooks = bess::run_global_resume_hooks();
  if (hooks) {
    return std::unexpected(
        ErrorFromLegacy(hooks->error().code(), hooks->error().errmsg()));
  }

  LOG(INFO) << "*** Resuming ***";
  resume_all_workers();
  return {};
}

ControlResult<void> ControlPlane::ResumeWorker(uint64_t wid) {
  std::lock_guard<std::mutex> lock(mutex_);

  // The wire id is uint64: refused out of range, never narrowed (2^32 was
  // worker 0).
  const std::optional<dataplane::WorkerId> id = dataplane::WorkerIdFrom(wid);
  if (!id) {
    return std::unexpected(Err(EINVAL, "worker id %llu is out of range (max %d)",
                               static_cast<unsigned long long>(wid),
                               Worker::kMaxWorkers - 1));
  }

  LOG(INFO) << "*** Resuming worker " << wid << " ***";
  // The worker's modules rebind to the offsets and graph a change made while
  // it was paused (a pauser of the other workers sent PreResume only to the
  // modules they run). The hooks are not rerun: other workers may be running.
  if (!is_worker_running(*id)) {
    std::set<Module *> sent;
    send_pre_resume(*id, &sent);
  }
  resume_worker(*id);
  return {};
}

// ---------------------------------------------------------------------------
// Traffic classes
// ---------------------------------------------------------------------------

ControlResult<void> ControlPlane::AddTc(const TrafficClassSpec& spec) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = AddTcLocked(spec);
  if (result) {
    // Every successful structural mutation advances the generation, so
    // an apply and a direct change share one sequence
    // (MODERNIZATION.md section 9.8). The *Locked() primitives never bump:
    // ApplyPipeline() bumps once for the whole transaction.
    runtime::runtime().BumpGeneration();
  }
  return result;
}

ControlResult<void> ControlPlane::AddTcLocked(const TrafficClassSpec& spec) {

  WorkerPauser wp;

  const char* tc_name = spec.name.c_str();
  if (spec.name.length() == 0) {
    return std::unexpected(Err(EINVAL, "Missing 'name' field"));
  } else if (tc_name[0] == '!') {
    return std::unexpected(
        Err(EINVAL, "TC names starting with \'!\' are reserved"));
  }

  if (TrafficClassBuilder::all_tcs().count(tc_name)) {
    return std::unexpected(
        Err(EINVAL, "Name '%s' already exists", tc_name));
  }

  const std::string& policy = spec.policy;

  bess::TrafficClass* c = nullptr;
  if (policy == bess::TrafficPolicyName[bess::POLICY_PRIORITY]) {
    c = reinterpret_cast<bess::TrafficClass*>(
        TrafficClassBuilder::CreateTrafficClass<bess::PriorityTrafficClass>(
            tc_name));
  } else if (policy == bess::TrafficPolicyName[bess::POLICY_WEIGHTED_FAIR]) {
    const std::optional<bess::Resource> resource =
        bess::ResourceFromName(spec.resource);
    if (!resource) {
      return std::unexpected(Err(EINVAL, "Invalid resource"));
    }
    c = reinterpret_cast<bess::TrafficClass*>(
        TrafficClassBuilder::CreateTrafficClass<
            bess::WeightedFairTrafficClass>(tc_name, *resource));
  } else if (policy == bess::TrafficPolicyName[bess::POLICY_ROUND_ROBIN]) {
    c = reinterpret_cast<bess::TrafficClass*>(
        TrafficClassBuilder::CreateTrafficClass<bess::RoundRobinTrafficClass>(
            tc_name));
  } else if (policy == bess::TrafficPolicyName[bess::POLICY_RATE_LIMIT]) {
    uint64_t limit = 0;
    uint64_t max_burst = 0;
    const std::optional<bess::Resource> resource =
        bess::ResourceFromName(spec.resource);
    if (!resource) {
      return std::unexpected(Err(EINVAL, "Invalid resource"));
    }
    if (auto bad = NegativeRate(spec)) {
      return std::unexpected(*bad);
    }
    if (spec.limit.find(spec.resource) != spec.limit.end()) {
      limit = spec.limit.at(spec.resource);
    }
    if (spec.max_burst.find(spec.resource) != spec.max_burst.end()) {
      max_burst = spec.max_burst.at(spec.resource);
    }
    c = reinterpret_cast<bess::TrafficClass*>(
        TrafficClassBuilder::CreateTrafficClass<bess::RateLimitTrafficClass>(
            tc_name, *resource, limit, max_burst));
  } else if (policy == bess::TrafficPolicyName[bess::POLICY_LEAF]) {
    return std::unexpected(Err(EINVAL, "Cannot create leaf TC"));
  } else {
    return std::unexpected(Err(EINVAL, "Invalid traffic policy"));
  }

  if (!c) {
    return std::unexpected(Err(ENOMEM, "CreateTrafficClass failed"));
  }

  return AttachTc(c, spec);
}

ControlResult<void> ControlPlane::UpdateTcParamsLocked(
    const TrafficClassSpec& spec) {
  WorkerPauser wp;

  ControlResult<bess::TrafficClass*> found = FindTc(spec);
  if (!found) {
    return std::unexpected(found.error());
  }
  bess::TrafficClass* c = *found;

  if (c->policy() == bess::POLICY_RATE_LIMIT) {
    bess::RateLimitTrafficClass* tc =
        reinterpret_cast<bess::RateLimitTrafficClass*>(c);
    const std::optional<bess::Resource> resource =
        bess::ResourceFromName(spec.resource);
    if (!resource) {
      return std::unexpected(Err(EINVAL, "Invalid resource"));
    }
    // Before any mutation: a refused update must change nothing.
    if (auto bad = NegativeRate(spec)) {
      return std::unexpected(*bad);
    }
    tc->set_resource(*resource);
    if (spec.limit.find(spec.resource) != spec.limit.end()) {
      tc->set_limit(spec.limit.at(spec.resource));
    }
    if (spec.max_burst.find(spec.resource) != spec.max_burst.end()) {
      tc->set_max_burst(spec.max_burst.at(spec.resource));
    }
  } else if (c->policy() == bess::POLICY_WEIGHTED_FAIR) {
    bess::WeightedFairTrafficClass* tc =
        reinterpret_cast<bess::WeightedFairTrafficClass*>(c);
    const std::optional<bess::Resource> resource =
        bess::ResourceFromName(spec.resource);
    if (!resource) {
      return std::unexpected(Err(EINVAL, "Invalid resource"));
    }
    tc->set_resource(*resource);
  } else {
    return std::unexpected(Err(EINVAL,
                               "Only 'rate_limit' and"
                               " 'weighted_fair' can be updated"));
  }

  return {};
}

ControlResult<void> ControlPlane::UpdateTcParent(const TrafficClassSpec& spec) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = UpdateTcParentLocked(spec);
  if (result) {
    // Every successful structural mutation advances the generation, so
    // an apply and a direct change share one sequence
    // (MODERNIZATION.md section 9.8). The *Locked() primitives never bump:
    // ApplyPipeline() bumps once for the whole transaction.
    runtime::runtime().BumpGeneration();
  }
  return result;
}

ControlResult<void> ControlPlane::UpdateTcParentLocked(const TrafficClassSpec& spec) {

  WorkerPauser wp;

  ControlResult<bess::TrafficClass*> found = FindTc(spec);
  if (!found) {
    return std::unexpected(found.error());
  }
  bess::TrafficClass* c = *found;

  // Where it is now, *before* anything detaches it: a refused reattach has to
  // put the class back where it came from, priority or share included, and once
  // it is detached that description is gone.
  const TrafficClassSpec previous = AttachmentSpecOf(c);

  if (c->policy() == bess::POLICY_LEAF) {
    if (!detach_tc(c)) {
      return std::unexpected(Err(EINVAL,
                                 "Cannot detach '%s'"
                                 " from parent",
                                 spec.name.c_str()));
    }
  }

  // XXX Leaf nodes can always be moved, other nodes can be moved only if
  // they're orphans. The scheduler maintains state which would need to be
  // updated otherwise.
  if (c->policy() != bess::POLICY_LEAF) {
    if (!remove_tc_from_orphan(c)) {
      return std::unexpected(Err(EINVAL,
                                 "Cannot detach '%s'."
                                 " while it is part of a worker",
                                 spec.name.c_str()));
    }
  }

  // Keep the legacy rule for non-leaf classes (they may only move as orphans),
  // but a refused reattach must not destroy the class: put it back where
  // `previous` says it was.
  auto attached = AttachExistingTcLocked(c, spec);
  if (!attached) {
    auto restored = AttachExistingTcLocked(c, previous);
    if (!restored) {
      LOG(ERROR) << "Tc '" << spec.name
                 << "' could not be restored to its previous attachment ("
                 << restored.error().message << ")";
    }
    return std::unexpected(attached.error());
  }

  return {};
}

ControlResult<void> ControlPlane::PlaceTasksLocked(
    std::span<const TrafficClassSpec> leaves, std::span<const std::string> leaving,
    std::vector<TrafficClassSpec> *previous_out) {
  // Every leaf first, before anything moves: a missing task changes nothing.
  // classes[0, leaves.size()) are placed; the rest, tasks of leaving modules
  // that sit in a class, only make room.
  std::vector<bess::TrafficClass*> classes;
  for (const TrafficClassSpec& leaf : leaves) {
    ControlResult<bess::TrafficClass*> found = FindTc(leaf);
    if (!found) {
      return std::unexpected(found.error());
    }
    if ((*found)->policy() != bess::POLICY_LEAF) {
      return std::unexpected(
          Err(EINVAL, "'%s' is not a module task", (*found)->name().c_str()));
    }
    classes.push_back(*found);
  }
  for (const std::string& name : leaving) {
    const Module* m = runtime::runtime().modules().Find(name);
    if (m == nullptr) {
      continue;
    }
    for (const auto* task : m->tasks()) {
      if (task->GetTC()->parent() != nullptr) {
        classes.push_back(task->GetTC());
      }
    }
  }

  std::vector<TrafficClassSpec> previous;
  for (bess::TrafficClass* c : classes) {
    // Where it is now: a scheduler root at its worker; an orphan waits for the
    // worker the orphan list names.
    TrafficClassSpec where = AttachmentSpecOf(c);
    if (c->parent() == nullptr) {
      for (const auto& [wid, orphan] : runtime::runtime().workers().orphan_tcs()) {
        if (orphan == c) {
          where.wid = WidOrAny(wid);
          break;
        }
      }
    }
    previous.push_back(std::move(where));
  }
  if (previous_out != nullptr) {
    *previous_out = previous;
  }

  // Two phases, so placements may trade places (a rate_limit class takes one
  // child: moving one task in before the other is out would be refused).
  size_t detached = 0;
  for (; detached < classes.size(); detached++) {
    if (!detach_tc(classes[detached])) {
      break;
    }
    if (detached >= leaves.size()) {
      // A leaving module's task: an orphan (detachable) until it is removed.
      add_tc_to_orphan(classes[detached], std::nullopt);
    }
  }
  size_t attached = 0;
  ControlResult<void> result;
  if (detached < classes.size()) {
    result = std::unexpected(Err(EINVAL, "Cannot detach '%s' from its parent",
                                 classes[detached]->name().c_str()));
  } else {
    for (; attached < leaves.size(); attached++) {
      result = AttachExistingTcLocked(classes[attached], leaves[attached]);
      if (!result) {
        break;
      }
    }
    if (result) {
      return {};
    }
  }

  // Refused: take back what moved (and the orphaned), then put everything
  // where it was.
  for (size_t i = 0; i < attached; i++) {
    (void)detach_tc(classes[i]);
  }
  for (size_t i = leaves.size(); i < detached; i++) {
    (void)detach_tc(classes[i]);
  }
  for (size_t i = 0; i < detached; i++) {
    if (auto restored = AttachExistingTcLocked(classes[i], previous[i]); !restored) {
      LOG(ERROR) << "Task '" << classes[i]->name()
                 << "' could not be restored to its previous placement ("
                 << restored.error().message << ")";
    }
  }
  return result;
}

// A class that is waiting to be woken up must not stay reachable from a
// scheduler once it (or an ancestor) is moved or destroyed: the wakeup queue
// holds raw pointers, and a stale one would be followed on the next wakeup.
void ControlPlane::RemoveSubtreeFromWakeupQueues(bess::TrafficClass* c) {
  for (const dataplane::WorkerId wid : dataplane::AllWorkerIds()) {
    Worker* worker = runtime::runtime().workers().Get(wid);
    if (worker != nullptr) {
      worker->scheduler()->wakeup_queue().Remove(c);
    }
  }

  for (bess::TrafficClass* child : c->Children()) {
    RemoveSubtreeFromWakeupQueues(child);
  }
}

// Destroys a traffic class that belongs to the control plane (leaf classes
// belong to their module). Used by the transaction engine's retire phase and
// to undo a TC it created.
ControlResult<void> ControlPlane::RemoveTcLocked(const std::string& name) {
  bess::TrafficClass* c = TrafficClassBuilder::Find(name);
  if (!c) {
    return std::unexpected(Err(ENOENT, "Tc '%s' doesn't exist", name.c_str()));
  }
  if (c->policy() == bess::POLICY_LEAF) {
    return std::unexpected(
        Err(EINVAL, "Tc '%s' is a leaf class owned by a module", name.c_str()));
  }

  // Deleting a class deletes its subtree; a module's task there would leave
  // the module holding a freed leaf. (Plans remove the modules first, and
  // CheckReversibility refuses a kept task left there.)
  std::vector<bess::TrafficClass*> pending = c->Children();
  while (!pending.empty()) {
    bess::TrafficClass* child = pending.back();
    pending.pop_back();
    if (child->policy() == bess::POLICY_LEAF) {
      return std::unexpected(Err(EBUSY, "Tc '%s' still holds the task of module '%s'",
                                 name.c_str(),
                                 static_cast<bess::LeafTrafficClass*>(child)
                                     ->task()
                                     ->module()
                                     ->name()
                                     .c_str()));
    }
    for (bess::TrafficClass* grandchild : child->Children()) {
      pending.push_back(grandchild);
    }
  }

  RemoveSubtreeFromWakeupQueues(c);

  if (!detach_tc(c)) {
    return std::unexpected(
        Err(EBUSY, "Cannot detach '%s' while it is part of a worker",
            name.c_str()));
  }

  runtime::runtime().traffic_classes().Release(c);
  delete c;
  return {};
}

ControlResult<void> ControlPlane::ReparentTcLocked(const TrafficClassSpec& spec) {
  bess::TrafficClass* c = TrafficClassBuilder::Find(spec.name);
  if (!c) {
    return std::unexpected(
        Err(ENOENT, "Tc '%s' doesn't exist", spec.name.c_str()));
  }
  if (c->policy() == bess::POLICY_LEAF) {
    return std::unexpected(Err(EINVAL, "Tc '%s' is a leaf class owned by a module",
                               spec.name.c_str()));
  }

  // Where it is now, so a refused move can put it back.
  const TrafficClassSpec previous = AttachmentSpecOf(c);

  RemoveSubtreeFromWakeupQueues(c);

  if (!detach_tc(c)) {
    return std::unexpected(Err(EBUSY, "Cannot detach '%s'", spec.name.c_str()));
  }

  auto attached = AttachExistingTcLocked(c, spec);
  if (!attached) {
    // The class is detached at this point: put it back rather than losing it.
    auto restored = AttachExistingTcLocked(c, previous);
    if (!restored) {
      LOG(ERROR) << "Tc '" << spec.name
                 << "' could not be restored to its previous attachment ("
                 << restored.error().message << ")";
    }
    return std::unexpected(attached.error());
  }

  return {};
}

ControlResult<void> ControlPlane::ResetTcsLocked() {
  WorkerPauser wp;

  if (!TrafficClassBuilder::ClearAll()) {
    return std::unexpected(Err(EBUSY, "TCs still have tasks"));
  }

  return {};
}

ControlResult<SchedulingConstraintsReport>
ControlPlane::CheckSchedulingConstraints() {
  std::lock_guard<std::mutex> lock(mutex_);

  SchedulingConstraintsReport report;

  // Start by attaching orphans -- this is essential to make sure we visit
  // every TC.
  if (!is_any_worker_running()) {
    // If any worker is running (i.e., not everything is paused), then there
    // is no point in attaching orphans.
    attach_orphans();
  }
  ModuleGraph::PropagateActiveWorker();
  LOG(INFO) << "Checking scheduling constraints";
  // Check constraints around chains run by each worker. This checks that
  // global constraints are met.
  for (const dataplane::WorkerId wid : dataplane::AllWorkerIds()) {
    Worker* worker = runtime::runtime().workers().Get(wid);
    if (worker == nullptr) {
      continue;
    }
    // Reported for the legacy check only, never a refusal: a worker on
    // another node than a task's is diagnosed (D-123). An unconstrained task
    // (every port since D-123) runs anywhere, whatever node the worker is on.
    const int node = static_cast<int>(worker->socket().value());
    const int core = static_cast<int>(worker->core().value());
    bess::TrafficClass* root = worker->scheduler()->root();

    for (const auto& tc_pair : TrafficClassBuilder::all_tcs()) {
      bess::TrafficClass* c = tc_pair.second.get();
      if (c->policy() == bess::POLICY_LEAF && root == c->Root()) {
        auto leaf = static_cast<bess::LeafTrafficClass*>(c);
        const placement_constraint constraints =
            leaf->task()->GetSocketConstraints();
        if (constraints == UNCONSTRAINED_SOCKET) {
          continue;
        }
        const bool allowed = node >= 0 && node < MAX_NUMA_NODE &&
                             (constraints & (placement_constraint{1} << node)) != 0;
        if (!allowed) {
          LOG(WARNING) << "Scheduler constraints are violated for wid "
                       << wid.value()
                       << " node " << node << " constraint " << constraints;
          report.violations.push_back(SchedulingConstraintViolation{
              c->name(), static_cast<int>(constraints), node, core});
        }
      }
    }
  }

  // Check local constraints
  for (const auto& pair : ModuleGraph::GetAllModules()) {
    const Module* m = pair.second.get();
    auto ret = m->CheckModuleConstraints();
    if (ret != CHECK_OK) {
      LOG(WARNING) << "Module " << m->name() << " failed check " << ret;
      report.modules.push_back(ModuleConstraintViolation{m->name()});
      if (ret == CHECK_FATAL_ERROR) {
        LOG(WARNING) << " --- FATAL CONSTRAINT FAILURE ---";
        report.fatal = true;
      }
    }
  }

  return report;
}

ControlResult<bess::TrafficClass*> ControlPlane::FindTc(
    const TrafficClassSpec& spec) {
  bess::TrafficClass* c = nullptr;

  if (spec.name.length() != 0) {
    const char* name = spec.name.c_str();
    c = TrafficClassBuilder::Find(name);
    if (!c) {
      return std::unexpected(Err(ENOENT, "Tc '%s' doesn't exist", name));
    }
  } else if (spec.leaf_module_name.length() != 0) {
    const std::string& module_name = spec.leaf_module_name;
    Module* m = runtime::runtime().modules().Find(module_name);
    if (!m) {
      return std::unexpected(
          Err(ENOENT, "No module '%s' found", module_name.c_str()));
    }

    task_id_t tid = spec.leaf_module_taskid;
    if (tid >= MAX_TASKS_PER_MODULE) {
      return std::unexpected(
          Err(EINVAL, "'taskid' must be between 0 and %d",
              MAX_TASKS_PER_MODULE - 1));
    }

    if (tid >= m->tasks().size()) {
      return std::unexpected(Err(ENOENT, "Task %s:%hu does not exist",
                                 spec.leaf_module_name.c_str(), tid));
    }

    c = m->tasks()[tid]->GetTC();
  } else {
    return std::unexpected(Err(EINVAL,
                               "One of 'name' or "
                               "'leaf_module_name' must be specified"));
  }

  if (!c) {
    return std::unexpected(Err(ENOENT, "Error finding TC"));
  }

  return c;
}

ControlResult<void> ControlPlane::AttachExistingTcLocked(
    bess::TrafficClass* c, const TrafficClassSpec& spec) {
  // Never unregisters or destroys `c`: the class exists, and a failed attach
  // has to leave it where it was. The caller decides whether it is destroyed,
  // and only when it created the class for this operation.
  if (spec.parent == "") {
    if (spec.wid != Worker::kAnyWorker && !dataplane::WorkerIdFrom(spec.wid)) {
      return std::unexpected(Err(EINVAL, "'wid' must be %d or between 0 and %d",
                                 Worker::kAnyWorker, Worker::kMaxWorkers - 1));
    }
    const std::optional<dataplane::WorkerId> wid = WorkerOrAny(spec.wid);

    int active_workers = runtime::runtime().workers().num_workers();
    if ((wid && !is_worker_active(*wid)) || (!wid && active_workers == 0)) {
      if (active_workers == 0 && (!wid || wid->value() == 0)) {
        runtime::runtime().workers().Launch(dataplane::WorkerId(0),
                                            runtime::DefaultWorkerCore(), "");
      } else {
        return std::unexpected(Err(EINVAL, "worker:%d does not exist", spec.wid));
      }
    }

    add_tc_to_orphan(c, wid);
    return {};
  }

  if (spec.wid != Worker::kAnyWorker) {
    return std::unexpected(Err(EINVAL,
                               "Both 'parent' and 'wid'"
                               "have been specified"));
  }

  bess::TrafficClass* parent = TrafficClassBuilder::Find(spec.parent);
  if (!parent) {
    return std::unexpected(
        Err(ENOENT, "Parent TC '%s' not found", spec.parent.c_str()));
  }

  bool fail_add = false;
  switch (parent->policy()) {
    case bess::POLICY_PRIORITY: {
      if (!spec.has_priority) {
        return std::unexpected(Err(EINVAL, "No priority specified"));
      }
      if (spec.priority < 0 ||
          spec.priority > std::numeric_limits<bess::priority_t>::max()) {
        return std::unexpected(Err(EINVAL, "Priority %lld is out of range",
                                   static_cast<long long>(spec.priority)));
      }
      const auto pri = static_cast<bess::priority_t>(spec.priority);
      if (pri == DEFAULT_PRIORITY) {
        return std::unexpected(
            Err(EINVAL, "Priority %d is reserved", DEFAULT_PRIORITY));
      }
      fail_add = !static_cast<bess::PriorityTrafficClass*>(parent)->AddChild(
          c, pri);
      break;
    }
    case bess::POLICY_WEIGHTED_FAIR:
      if (!spec.has_share) {
        return std::unexpected(Err(EINVAL, "No share specified"));
      }
      if (spec.share < 1 ||
          spec.share > std::numeric_limits<bess::resource_share_t>::max()) {
        return std::unexpected(Err(EINVAL, "Share %lld is out of range",
                                   static_cast<long long>(spec.share)));
      }
      fail_add = !static_cast<bess::WeightedFairTrafficClass*>(parent)->AddChild(
          c, static_cast<bess::resource_share_t>(spec.share));
      break;
    case bess::POLICY_ROUND_ROBIN:
      fail_add =
          !static_cast<bess::RoundRobinTrafficClass*>(parent)->AddChild(c);
      break;
    case bess::POLICY_RATE_LIMIT:
      fail_add =
          !static_cast<bess::RateLimitTrafficClass*>(parent)->AddChild(c);
      break;
    default:
      return std::unexpected(Err(EPERM, "Parent tc doesn't support children"));
  }
  if (fail_add) {
    return std::unexpected(Err(EINVAL, "AddChild() failed"));
  }

  return {};
}

// Attaches a class that was just created for this operation: on failure the
// registry entry goes back and the unique_ptr destroys it, so a failed create
// leaves nothing behind.
ControlResult<void> ControlPlane::AttachTc(bess::TrafficClass* c_,
                                           const TrafficClassSpec& spec) {
  std::unique_ptr<bess::TrafficClass> c(c_);

  auto attached = AttachExistingTcLocked(c.get(), spec);
  if (!attached) {
    runtime::runtime().traffic_classes().Release(c.get());
    return std::unexpected(attached.error());
  }

  c.release();
  return {};
}

// ---------------------------------------------------------------------------
// Gate hooks
// ---------------------------------------------------------------------------

namespace {

bess::Gate* module_gate(const Module* m, bool is_igate, gate_idx_t gate_idx) {
  if (is_igate) {
    if (is_active_gate(m->igates(), gate_idx)) {
      return m->igates()[gate_idx];
    }
  } else {
    if (is_active_gate(m->ogates(), gate_idx)) {
      return m->ogates()[gate_idx];
    }
  }
  return nullptr;
}

ControlResult<std::string> enable_hook_for_module(
    const GateHookSpec& spec, const Module* m,
    const bess::GateHookBuilder& builder) {
  if (spec.use_gate) {
    bess::Gate* gate = module_gate(m, spec.is_igate, spec.gate_idx);
    if (gate == nullptr) {
      return std::unexpected(
          Err(ENOENT, "'%s': %cgate '%hu' does not exist", m->name().c_str(),
              spec.is_igate ? 'i' : 'o',
              static_cast<unsigned short>(spec.gate_idx)));
    }

    pb_error_t error;
    bess::GateHook* hook = gate->CreateGateHook(&builder, gate, spec.hook_name,
                                               spec.arg, &error);
    if (!hook) {
      return std::unexpected(ErrorFromLegacy(error.code(), error.errmsg()));
    }
    return hook->name();
  }

  // Every gate of one direction, or none: a failure removes the hooks this
  // call created, from the gates they were created on.
  std::vector<std::pair<bess::Gate*, std::string>> created;
  auto hook_every = [&](const auto& gates) -> ControlResult<void> {
    for (bess::Gate* gate : gates) {
      if (!gate) {
        continue;
      }
      pb_error_t error;
      bess::GateHook* hook = gate->CreateGateHook(&builder, gate,
                                                  spec.hook_name, spec.arg,
                                                  &error);
      if (error.code() != 0 || !hook) {
        for (const auto& [g, name] : created) {
          g->RemoveHook(name);
        }
        return std::unexpected(ErrorFromLegacy(error.code(), error.errmsg()));
      }
      created.emplace_back(gate, hook->name());
    }
    return {};
  };
  ControlResult<void> ret = spec.is_igate ? hook_every(m->igates()) : hook_every(m->ogates());
  if (!ret) {
    return std::unexpected(ret.error());
  }
  return std::string();
}

ControlResult<void> disable_hook_for_module(const GateHookSpec& spec,
                                            const Module* m) {
  if (spec.use_gate) {
    bess::Gate* gate = module_gate(m, spec.is_igate, spec.gate_idx);
    if (gate == nullptr) {
      return std::unexpected(
          Err(EINVAL, "'%s': %cgate '%hu' does not exist", m->name().c_str(),
              spec.is_igate ? 'i' : 'o',
              static_cast<unsigned short>(spec.gate_idx)));
    }
    if (spec.hook_name != "") {
      gate->RemoveHook(spec.hook_name);
    } else {
      gate->RemoveHookByClass(spec.class_name);
    }
    return {};
  }

  if (spec.is_igate) {
    for (auto& gate : m->igates()) {
      if (!gate) {
        continue;
      }
      if (spec.hook_name != "") {
        gate->RemoveHook(spec.hook_name);
      } else {
        gate->RemoveHookByClass(spec.class_name);
      }
    }
  } else {
    for (auto& gate : m->ogates()) {
      if (!gate) {
        continue;
      }
      if (spec.hook_name != "") {
        gate->RemoveHook(spec.hook_name);
      } else {
        gate->RemoveHookByClass(spec.class_name);
      }
    }
  }
  return {};
}

}  // namespace

ControlResult<std::string> ControlPlane::ConfigureGateHook(
    const GateHookSpec& spec, bool enable) {
  std::lock_guard<std::mutex> lock(mutex_);

  WorkerPauser wp;

  const auto builder = bess::GateHookBuilder::all_gate_hook_builders().find(
      spec.class_name);
  if (builder == bess::GateHookBuilder::all_gate_hook_builders().end()) {
    return std::unexpected(
        Err(ENOENT, "No such gate hook: %s", spec.class_name.c_str()));
  }

  if (spec.module_name.length() == 0) {
    // Install this hook on all modules
    std::string created;
    for (const auto& it : ModuleGraph::GetAllModules()) {
      if (enable) {
        ControlResult<std::string> ret =
            enable_hook_for_module(spec, it.second.get(), builder->second);
        if (!ret) {
          return std::unexpected(ret.error());
        }
        if (!ret->empty()) {
          created = *ret;
        }
      } else {
        ControlResult<void> ret =
            disable_hook_for_module(spec, it.second.get());
        if (!ret) {
          return std::unexpected(ret.error());
        }
      }
    }
    return created;
  }

  // Install this hook on the specified module
  Module *m = runtime::runtime().modules().Find(spec.module_name);
  if (!m) {
    return std::unexpected(
        Err(ENOENT, "No module '%s' found", spec.module_name.c_str()));
  }
  if (enable) {
    return enable_hook_for_module(spec, m, builder->second);
  }
  ControlResult<void> ret = disable_hook_for_module(spec, m);
  if (!ret) {
    return std::unexpected(ret.error());
  }
  return std::string();
}

// ---------------------------------------------------------------------------
// Plugins
// ---------------------------------------------------------------------------

ControlResult<void> ControlPlane::ImportPlugin(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);

  WorkerPauser wp;
  VLOG(1) << "Loading plugin: " << path;
  if (!framework::LoadPlugin(path)) {
    return std::unexpected(
        Err(-1, "Failed loading plugin %s", path.c_str()));
  }
  return {};
}

// Roadmap 28.4: a plugin leaves only when nothing reachable runs its code --
// no module of its classes (their resources, metrics, requests, handoff and
// offload state are the module's), no port of its drivers, no gate hook of
// its classes, no application instance its code destroys -- and objects its
// code retired to RCU have been destroyed (workers paused: a grace period
// needs none).
ControlResult<void> ControlPlane::UnloadPlugin(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);

  framework::PluginContents contents;
  if (!framework::PluginContentsOf(path, &contents)) {
    return std::unexpected(Err(ENOENT, "Plugin %s is not loaded", path.c_str()));
  }
  auto in = [](const std::vector<std::string> &names, const std::string &name) {
    return std::find(names.begin(), names.end(), name) != names.end();
  };
  std::vector<std::string> users;
  for (const auto &[name, m] : runtime::runtime().modules().All()) {
    if (m->module_builder() != nullptr &&
        in(contents.module_classes, m->module_builder()->class_name())) {
      users.push_back("module " + name);
    }
    // A named reference, not the structured binding: clang-analyzer 19 loses
    // a binding captured by a lambda (a false "uninitialized" report).
    const std::string &module_name = name;
    auto hooks_of = [&](const auto &gates) {
      for (const auto *gate : gates) {
        if (gate == nullptr) {
          continue;
        }
        for (const bess::GateHook *hook : gate->hooks()) {
          if (in(contents.gate_hooks, hook->class_name())) {
            users.push_back("gate hook " + hook->name() + " on " + module_name);
          }
        }
      }
    };
    hooks_of(m->igates());
    hooks_of(m->ogates());
  }
  for (const auto &[name, port] : runtime::runtime().ports().All()) {
    if (port->port_builder() != nullptr &&
        in(contents.port_drivers, port->port_builder()->class_name())) {
      users.push_back("port " + name);
    }
  }
  for (const std::string &name : runtime::runtime().instances().DestroyedBy(
           [&](const void *code) { return framework::CodeInPlugin(path, code); })) {
    users.push_back("instance " + name);
  }
  if (!users.empty()) {
    std::string list;
    for (const std::string &u : users) {
      list += (list.empty() ? "" : ", ") + u;
    }
    return std::unexpected(
        Err(EBUSY, "Plugin %s is in use: %s", path.c_str(), list.c_str()));
  }

  WorkerPauser wp;
  runtime::runtime().rcu().Drain();
  while (runtime::runtime().transactions().ReclaimRetired() != 0) {
  }

  VLOG(1) << "Unloading plugin: " << path;
  if (!framework::UnloadPlugin(path)) {
    return std::unexpected(
        Err(-1, "Failed unloading plugin %s", path.c_str()));
  }
  return {};
}

// ---------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------

ControlResult<void> ControlPlane::DiffResourcesLocked(const PipelineSpec &desired,
                                                      PipelineDiff *diff) const {
  const dataplane::TransactionEngine &engine = runtime::runtime().transactions();
  const framework::ResourceBindings &bindings = framework::ResourceBindings::ProcessDefault();
  for (const ResourceSpec &resource : desired.resources) {
    const std::string module = resource.name.substr(0, resource.name.rfind('/'));
    const bool rebuilt = std::any_of(
        diff->modules.begin(), diff->modules.end(), [&](const ModuleChange &change) {
          return change.name == module && (change.kind == ChangeKind::kCreate ||
                                           change.kind == ChangeKind::kReplace);
        });
    if (rebuilt) {
      // Listed, even with no entries: the apply converges it, and entries the
      // module seeds from its init argument (Vif's interfaces) that are not
      // listed are removed. How many is unknown until the module exists.
      diff->resources.push_back(
          ResourceDelta{resource.name, resource.entries.size(), 0, 0, false});
      continue;
    }
    auto converged = Converge(engine, bindings, resource, /*build_ops=*/false);
    if (!converged) {
      return std::unexpected(converged.error());
    }
    if (!converged->delta.empty()) {
      diff->resources.push_back(std::move(converged->delta));
    }
  }
  return {};
}

ControlResult<size_t> ControlPlane::ApplyResourcesLocked(
    std::span<const ResourceSpec> resources) {
  dataplane::TransactionEngine &engine = runtime::runtime().transactions();
  const framework::ResourceBindings &bindings = framework::ResourceBindings::ProcessDefault();
  std::vector<dataplane::Op> ops;
  bool pause = false;
  // D-198: memory refused while building the operations (a pure read of the
  // resources) or while the engine reserves them answers RESOURCE_EXHAUSTED.
  // The engine allocates only in its Reserve phase, whose abort guard undoes
  // the batch's reservations on unwind (D-172); batches before it stay
  // applied, as they do when a later batch is refused with an error, and the
  // caller treats this like any other refusal (ApplyPipeline aborts the
  // topology).
  const auto refused = [](const std::bad_alloc &e) {
    ControlError error = OutOfMemory(e);
    error.field = "resources";
    return std::unexpected(std::move(error));
  };
  try {
    for (const ResourceSpec &resource : resources) {
      auto converged = Converge(engine, bindings, resource, /*build_ops=*/true);
      if (!converged) {
        return std::unexpected(converged.error());
      }
      pause = pause || (converged->requires_paused_workers && !converged->ops.empty());
      std::move(converged->ops.begin(), converged->ops.end(), std::back_inserter(ops));
    }
  } catch (const std::bad_alloc &e) {
    return refused(e);
  }
  if (ops.empty()) {
    return size_t{0};
  }
  // The spec lists resources by name, not by what refers to what. Reserve
  // follows the operation order, and a resource may decide what an entry
  // refers to from entries written earlier in the same transaction (a
  // Router route names a neighbor binding or a static next hop), and
  // ApplyBatched may split the list: so the referenced come first. Upserts
  // in ascending rank (a referent before its referrers), then erases in
  // descending rank (a referrer before its referent), the order the engine
  // publishes in.
  {
    auto rank_of = [&engine](const dataplane::Op &op) {
      const dataplane::Resource *resource = engine.FindResource(op.resource);
      return resource != nullptr ? resource->rank() : 0;
    };
    std::stable_sort(ops.begin(), ops.end(),
                     [&rank_of](const dataplane::Op &a, const dataplane::Op &b) {
                       const bool au = a.kind == dataplane::OpKind::kUpsert;
                       const bool bu = b.kind == dataplane::OpKind::kUpsert;
                       if (au != bu) {
                         return au;
                       }
                       return au ? rank_of(a) < rank_of(b) : rank_of(a) > rank_of(b);
                     });
  }
  // A resource whose values workers read unsynchronized changes only with
  // the workers paused (they may be paused for the commit already).
  std::optional<WorkerPauser> pauser;
  if (pause) {
    pauser.emplace();
  }
  // BUSY: reclamation is behind the readers and nothing was attempted. Wait
  // a few grace periods (each Apply reclaims what became ready first), as
  // ApplyTransaction does, but under the lock: about 6 ms at most.
  //
  // ApplyBatched, not Apply: a resource whose op list is proportional to its
  // contents - a clear of a map with many keys - cannot be sized by the caller,
  // and one transaction with one op per entry is refused as over budget. The
  // batches apply in order and converge; a failure reports its own resource. It
  // also owns the busy retry, which cannot live out here: a caller looping over
  // the whole request would re-apply the batches that already applied, and an
  // erase of an already-erased key is refused ("not found"), not ignored.
  std::optional<dataplane::TransactionEngine::Result> applied;
  try {
    applied.emplace(engine.ApplyBatched(ops));
  } catch (const std::bad_alloc &e) {
    return refused(e);
  }
  const dataplane::TransactionEngine::Result &result = *applied;
  switch (result.outcome) {
    case dataplane::TransactionEngine::Outcome::kApplied:
      break;
    case dataplane::TransactionEngine::Outcome::kBusy: {
      ControlError error = Err(EBUSY, "resources: reclamation is behind the readers; retry");
      error.code = ControlErrorCode::kResourceBusy;
      error.field = "resources";
      return std::unexpected(error);
    }
    default: {
      for (size_t i = 0; i < result.ops.size(); i++) {
        if (result.ops[i].status == dataplane::TransactionEngine::OpStatus::kFailed) {
          // ApplyBatched() returns the failing batch's results, so the resource
          // comes from there rather than from our own request order: a failure
          // in batch k would otherwise be reported against operation i of the
          // whole request, which is a different operation.
          ControlError error = Err(EINVAL, "resource '%s': %s",
                                   result.ops[i].resource.c_str(),
                                   result.ops[i].error.c_str());
          error.object = result.ops[i].resource;
          error.field = "resources";
          return std::unexpected(error);
        }
      }
      ControlError error = Err(EINVAL, "resources: the transaction was refused");
      error.field = "resources";
      return std::unexpected(error);
    }
  }
  runtime::runtime().events().Emit("bess.transaction", "control",
                                   {{"request_id", ""},
                                    {"origin", "pipeline"},
                                    {"outcome", "applied"},
                                    {"operations", std::to_string(ops.size())}},
                                   result.generation);
  return ops.size();
}

ControlResult<ApplyResult> ControlPlane::ApplyPipeline(
    const PipelineSpec &desired, const ApplyOptions &options) {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto validation_start = std::chrono::steady_clock::now();
  dataplane::TransactionEngine &engine = runtime::runtime().transactions();

  // 1. Validate. Pure, so a rejected spec cannot have touched anything.
  auto validated = bess::control::ValidatePipeline(runtime::runtime(), desired);
  if (!validated) {
    return std::unexpected(validated.error());
  }

  // 2. Optimistic concurrency: a stale writer is rejected before any effect.
  if (options.expected_generation.has_value() &&
      *options.expected_generation != runtime::runtime().generation()) {
    ControlError error = Err(ESTALE, "expected generation %llu, active generation %llu",
                             static_cast<unsigned long long>(
                                 *options.expected_generation),
                             static_cast<unsigned long long>(
                                 runtime::runtime().generation()));
    error.code = ControlErrorCode::kConflict;
    error.object = "pipeline";
    error.field = "expected_generation";
    return std::unexpected(error);
  }
  if (options.expected_resource_generation.has_value() &&
      *options.expected_resource_generation != engine.generation()) {
    ControlError error =
        Err(ESTALE, "expected resource generation %llu, active resource generation %llu",
            static_cast<unsigned long long>(*options.expected_resource_generation),
            static_cast<unsigned long long>(engine.generation()));
    error.code = ControlErrorCode::kConflict;
    error.object = "pipeline";
    error.field = "expected_resource_generation";
    return std::unexpected(error);
  }

  // 3. Diff and plan. Nothing to do means no pause and no generation change.
  const uint64_t validation_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - validation_start)
          .count());

  PipelineSnapshot before = SnapshotRuntime(runtime::runtime());
  PipelineDiff diff = Diff(before, validated->spec);
  if (diff.empty()) {
    // The topology is in place: only resource contents may change (D-129).
    auto converged = ApplyResourcesLocked(validated->spec.resources);
    if (!converged) {
      return std::unexpected(converged.error());
    }
    ApplyTiming timing;
    timing.validation_us = validation_us;
    return ApplyResult{runtime::runtime().generation(), 0, false, timing, engine.generation(),
                       *converged};
  }
  PipelinePlan plan = Plan(diff);

  // 4. Refuse what cannot be made reversible rather than hoping rollback works.
  if (auto reversible = CheckReversibility(plan); !reversible) {
    return std::unexpected(reversible.error());
  }

  // 5. Run it. D-198: memory refused inside a phase (a module's or port's
  // Init answers it as its own error; anything else unwinds out of the phase)
  // is answered like any other refusal: the abort undoes every operation that
  // completed. An operation that throws after its effect and before its undo
  // is recorded is not undone; the operations that do that allocate only
  // small control-plane objects (gates, traffic classes, an undo record), so
  // only an exhausted process heap reaches it, never a refused table.
  Transaction transaction(this, std::move(plan), std::move(before));
  if (auto prepared = RefuseOutOfMemory([&] { return transaction.Prepare(); });
      !prepared) {
    transaction.Abort();
    return std::unexpected(prepared.error());
  }
  if (auto committed = RefuseOutOfMemory([&] { return transaction.Commit(); });
      !committed) {
    transaction.Abort();
    return std::unexpected(committed.error());
  }
  // The listed resources exist now (a created module registered its own):
  // converge them in one dataplane transaction, all or nothing. Refused, the
  // topology goes back too -- the placement carries an undo for exactly this
  // case -- and nothing of the apply stays.
  ControlResult<size_t> converged = ApplyResourcesLocked(validated->spec.resources);
  if (auto injected = MaybeInjectResourceFailure(); injected.has_value()) {
    converged = std::unexpected(*injected);
  }
  if (!converged) {
    transaction.Abort();
    return std::unexpected(converged.error());
  }

  // Retirement is not undoable, so its preconditions were proven before the
  // commit (CheckReversibility). If a step still fails, the new state *is*
  // active -- the generation has to say so -- but the caller is told, because
  // returning success would claim a pipeline that does not exist. Memory
  // refused while retiring is such a failure too.
  if (auto retired = RefuseOutOfMemory([&] { return transaction.Retire(); });
      !retired) {
    runtime::runtime().BumpGeneration();

    const char *phase =
        retired.error().field == "resume" ? "resume" : "retirement";
    ControlError error = Err(retired.error().err,
                             "pipeline committed, but %s failed: %s", phase,
                             retired.error().message.c_str());
    error.code = retired.error().field == "resume"
                     ? ControlErrorCode::kInvalidArgument
                     : ControlErrorCode::kResourceFailure;
    error.object = retired.error().object;
    error.field = retired.error().field;
    return std::unexpected(error);
  }

  runtime::runtime().BumpGeneration();

  ApplyTiming timing = transaction.timing();
  timing.validation_us = validation_us;

  return ApplyResult{runtime::runtime().generation(), transaction.ops_executed(),
                     transaction.quiescence() == Quiescence::kWorkers, timing,
                     engine.generation(), *converged};
}

ControlResult<ValidatedPipeline> ControlPlane::ValidatePipeline(
    const PipelineSpec &desired) {
  std::lock_guard<std::mutex> lock(mutex_);
  return bess::control::ValidatePipeline(runtime::runtime(), desired);
}

PipelineSnapshot ControlPlane::GetPipeline() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SnapshotRuntime(runtime::runtime());
}

ControlResult<PipelineDiff> ControlPlane::DiffPipeline(
    const PipelineSpec &desired) const {
  auto diff = DiffPipelineVersioned(desired);
  if (!diff) {
    return std::unexpected(diff.error());
  }
  return std::move(diff->value);
}

ControlResult<ControlPlane::Versioned<ControlPlane::PipelineRead>> ControlPlane::ReadPipeline(
    const ResourceQuery &query) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const dataplane::TransactionEngine &engine = runtime::runtime().transactions();
  Versioned<PipelineRead> read;
  read.value.snapshot = SnapshotRuntime(runtime::runtime());
  read.generation = runtime::runtime().generation();
  read.resource_generation = engine.generation();
  if (query.include_metadata) {
    read.value.metadata = metadata::default_pipeline.report();
  }
  if (query.include) {
    auto resources = ReadResources(engine, framework::ResourceBindings::ProcessDefault(),
                                   query.names, query.max_bytes);
    if (!resources) {
      return std::unexpected(resources.error());
    }
    read.value.resources = std::move(*resources);
  }
  return read;
}

ControlResult<ControlPlane::Versioned<ResourceEntryPage>> ControlPlane::ReadResource(
    const std::string &name, const std::string &cursor, size_t max_entries,
    size_t max_bytes) const {
  if (name.empty()) {
    ControlError error = Err(EINVAL, "resource: a name is required");
    error.field = "resource";
    return std::unexpected(error);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const dataplane::TransactionEngine &engine = runtime::runtime().transactions();
  auto page = ReadResourcePage(engine, framework::ResourceBindings::ProcessDefault(), name,
                               cursor, max_entries, max_bytes);
  if (!page) {
    return std::unexpected(page.error());
  }
  Versioned<ResourceEntryPage> read;
  read.value = std::move(*page);
  read.generation = runtime::runtime().generation();
  read.resource_generation = engine.generation();
  return read;
}

ControlResult<ControlPlane::Versioned<ControlPlane::ResourceReset>>
ControlPlane::ResetResources(
    std::vector<std::string> names,
    std::optional<uint64_t> expected_resource_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  dataplane::TransactionEngine &engine = runtime::runtime().transactions();
  if (expected_resource_generation.has_value() &&
      *expected_resource_generation != engine.generation()) {
    ControlError error =
        Err(ESTALE, "expected resource generation %llu, active resource generation %llu",
            static_cast<unsigned long long>(*expected_resource_generation),
            static_cast<unsigned long long>(engine.generation()));
    error.code = ControlErrorCode::kConflict;
    error.field = "expected_resource_generation";
    return std::unexpected(error);
  }
  if (names.empty()) {
    ControlError error = Err(EINVAL, "resources: at least one name is required");
    error.field = "resources";
    return std::unexpected(error);
  }
  std::vector<std::string> unique;
  unique.reserve(names.size());
  for (std::string &name : names) {
    if (name.empty()) {
      ControlError error = Err(EINVAL, "resources: a name is required");
      error.field = "resources";
      return std::unexpected(error);
    }
    if (std::find(unique.begin(), unique.end(), name) == unique.end()) {
      unique.push_back(std::move(name));
    }
  }
  std::vector<ResourceSpec> specs;
  specs.reserve(unique.size());
  for (const std::string &name : unique) {
    ResourceSpec spec;
    spec.name = name;  // no entries: converge to empty
    specs.push_back(std::move(spec));
  }
  auto removed = ApplyResourcesLocked(specs);
  if (!removed) {
    return std::unexpected(removed.error());
  }
  Versioned<ResourceReset> reset;
  reset.value.resources = std::move(unique);
  reset.value.removed = *removed;
  reset.generation = runtime::runtime().generation();
  reset.resource_generation = engine.generation();
  return reset;
}

ControlResult<ControlPlane::Versioned<PipelineDiff>>
ControlPlane::DiffPipelineVersioned(const PipelineSpec &desired) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto validated = bess::control::ValidatePipeline(runtime::runtime(), desired);
  if (!validated) {
    return std::unexpected(validated.error());
  }
  Versioned<PipelineDiff> diff{Diff(SnapshotRuntime(runtime::runtime()), validated->spec),
                               runtime::runtime().generation(),
                               runtime::runtime().transactions().generation()};
  if (auto resources = DiffResourcesLocked(validated->spec, &diff.value); !resources) {
    return std::unexpected(resources.error());
  }
  return diff;
}

ControlResult<ControlPlane::Versioned<PipelinePlan>>
ControlPlane::PlanPipelineVersioned(const PipelineSpec &desired) const {
  auto diff = DiffPipelineVersioned(desired);
  if (!diff) {
    return std::unexpected(diff.error());
  }
  return Versioned<PipelinePlan>{Plan(diff->value), diff->generation,
                                 diff->resource_generation};
}

ControlResult<PipelinePlan> ControlPlane::PlanPipeline(
    const PipelineSpec &desired) const {
  auto diff = DiffPipeline(desired);
  if (!diff) {
    return std::unexpected(diff.error());
  }

  return Plan(*diff);
}

ControlResult<void> ControlPlane::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);

  WorkerPauser wp;

  LOG(INFO) << "*** ResetAll requested ***";

  // One transaction, one lock, one generation bump, no RPC handler calling
  // other RPC handlers.
  if (auto m = ResetModulesLocked(); !m) {
    return std::unexpected(m.error());
  }
  if (auto p = ResetPortsLocked(); !p) {
    return std::unexpected(p.error());
  }
  if (auto t = ResetTcsLocked(); !t) {
    return std::unexpected(t.error());
  }
  if (auto w = ResetWorkersLocked(); !w) {
    return std::unexpected(w.error());
  }

  runtime::runtime().BumpGeneration();
  return {};
}

}  // namespace control
}  // namespace bess
