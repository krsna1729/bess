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

#include "control/control_plane.h"

#include "control/worker_manager.h"

#include <memory>

#include <cerrno>
#include <cstdarg>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "bessd.h"
#include "gate.h"
#include "message.h"
#include "module.h"
#include "module_graph.h"
#include "opts.h"
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

}  // namespace

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

ControlResult<PortInfo> ControlPlane::CreatePort(const PortSpec& spec) {
  std::lock_guard<std::mutex> lock(mutex_);
  return CreatePortLocked(spec);
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

  PortRegistry &ports = runtime().ports();
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

  CommandResponse ret = p->InitWithGenericArg(spec.arg);

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

ControlResult<void> ControlPlane::DestroyPort(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  return DestroyPortLocked(name);
}

ControlResult<void> ControlPlane::DestroyPortLocked(const std::string& name) {

  if (name.length() == 0) {
    return std::unexpected(
        Err(EINVAL, "Argument must be a name in str"));
  }

  int ret = runtime().ports().Destroy(name);
  if (ret == -ENOENT) {
    return std::unexpected(Err(ENOENT, "No port `%s' found", name.c_str()));
  }
  if (ret) {
    return std::unexpected(Errno(-ret));
  }

  return {};
}

ControlResult<bess::pb::CommandResponse> ControlPlane::SetPortConf(
    const std::string& name, const PortConfSpec& spec) {
  std::lock_guard<std::mutex> lock(mutex_);
  return SetPortConfLocked(name, spec);
}

ControlResult<bess::pb::CommandResponse> ControlPlane::SetPortConfLocked(const std::string& name, const PortConfSpec& spec) {

  if (!name.length()) {
    return std::unexpected(Err(EINVAL, "Port name is not given"));
  }

  Port *port = runtime().ports().Find(name);
  if (!port) {
    return std::unexpected(Err(ENOENT, "No port `%s' found", name.c_str()));
  }

  Port::Conf conf;
  conf.mtu = spec.mtu;
  conf.admin_up = spec.admin_up;

  if (!conf.mac_addr.FromString(spec.mac_addr)) {
    return std::unexpected(
        Err(EINVAL, "MAC address should be formatted xx:xx:xx:xx:xx:xx"));
  }

  WorkerPauser wp;
  return port->UpdateConf(conf);
}

ControlResult<void> ControlPlane::ResetPorts() {
  std::lock_guard<std::mutex> lock(mutex_);
  return ResetPortsLocked();
}

ControlResult<void> ControlPlane::ResetPortsLocked() {
  WorkerPauser wp;

  PortRegistry &ports = runtime().ports();

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
  return CreateModuleLocked(spec);
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
    if (runtime().modules().Contains(spec.name)) {
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

  return module->name();
}

ControlResult<void> ControlPlane::DestroyModule(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  return DestroyModuleLocked(name);
}

ControlResult<void> ControlPlane::DestroyModuleLocked(const std::string& name) {

  WorkerPauser wp;

  if (name.length() == 0) {
    return std::unexpected(Err(EINVAL, "Argument must be a name in str"));
  }

  Module* m = runtime().modules().Find(name);
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

ControlResult<void> ControlPlane::ResetModules() {
  std::lock_guard<std::mutex> lock(mutex_);
  return ResetModulesLocked();
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
  return ConnectModulesLocked(spec);
}

ControlResult<void> ControlPlane::ConnectModulesLocked(const ConnectionSpec& spec) {

  VLOG(1) << "ConnectModules " << spec.upstream << ":" << spec.ogate << " -> "
          << spec.igate << ":" << spec.downstream;

  if (spec.upstream.empty() || spec.downstream.empty()) {
    return std::unexpected(Err(EINVAL, "Missing 'm1' or 'm2' field"));
  }

  Module* m1 = runtime().modules().Find(spec.upstream);
  if (!m1) {
    return std::unexpected(
        Err(ENOENT, "No module '%s' found", spec.upstream.c_str()));
  }

  Module* m2 = runtime().modules().Find(spec.downstream);
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
  return DisconnectModulesLocked(spec);
}

ControlResult<void> ControlPlane::DisconnectModulesLocked(const DisconnectionSpec& spec) {

  WorkerPauser wp;

  if (spec.name.length() == 0) {
    return std::unexpected(Err(EINVAL, "Missing 'name' field"));
  }

  Module* m = runtime().modules().Find(spec.name);
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

ControlResult<void> ControlPlane::AddWorker(uint64_t wid, uint64_t core,
                                            const std::string& scheduler) {
  std::lock_guard<std::mutex> lock(mutex_);
  return AddWorkerLocked(wid, core, scheduler);
}

ControlResult<void> ControlPlane::AddWorkerLocked(uint64_t wid, uint64_t core, const std::string& scheduler) {

  if (wid >= Worker::kMaxWorkers) {
    return std::unexpected(Err(EINVAL, "Invalid worker id"));
  }
  if (!is_cpu_present(core)) {
    return std::unexpected(Err(EINVAL, "Invalid core %d", static_cast<int>(core)));
  }
  if (is_worker_active(wid)) {
    return std::unexpected(Err(EEXIST, "worker:%d is already active", static_cast<int>(wid)));
  }
  if (scheduler != "" && scheduler != "experimental") {
    return std::unexpected(Err(EINVAL, "Invalid scheduler %s",
                               scheduler.c_str()));
  }

  launch_worker(wid, core, scheduler);
  return {};
}

ControlResult<void> ControlPlane::DestroyWorker(uint64_t wid) {
  std::lock_guard<std::mutex> lock(mutex_);
  return DestroyWorkerLocked(wid);
}

ControlResult<void> ControlPlane::DestroyWorkerLocked(uint64_t wid) {

  if (wid >= Worker::kMaxWorkers) {
    return std::unexpected(Err(EINVAL, "Invalid worker id"));
  }
  Worker* worker = runtime().workers().Get(wid);
  if (!worker) {
    return std::unexpected(Err(ENOENT, "Worker %d is not active", static_cast<int>(wid)));
  }

  bess::TrafficClass* root = worker->scheduler()->root();
  if (root) {
    for (const auto& it : TrafficClassBuilder::all_tcs()) {
      bess::TrafficClass* c = it.second.get();
      if (c->policy() == bess::POLICY_LEAF && c->Root() == root) {
        return std::unexpected(Err(EBUSY, "Worker %d has active tasks: %s", static_cast<int>(wid),
                                   c->name().c_str()));
      }
    }
  }

  destroy_worker(wid);
  return {};
}

ControlResult<void> ControlPlane::ResetWorkers() {
  std::lock_guard<std::mutex> lock(mutex_);
  return ResetWorkersLocked();
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

  // TODO: It should be made harder to wreak havoc on the rest of the daemon
  // when using PauseWorker(). For now a warning and suggestion that this is
  // for experts only is sufficient.
  LOG(WARNING) << "PauseWorker() is an experimental operation and should be"
               << " used with care. Long-term support not guaranteed.";
  pause_worker(wid);
  LOG(INFO) << "*** Worker " << wid << " has been paused ***";
  return {};
}

ControlResult<void> ControlPlane::ResumeAll() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!is_any_worker_running()) {
    attach_orphans();
  }

  bess::run_global_resume_hooks();

  LOG(INFO) << "*** Resuming ***";
  resume_all_workers();
  return {};
}

ControlResult<void> ControlPlane::ResumeWorker(uint64_t wid) {
  std::lock_guard<std::mutex> lock(mutex_);

  LOG(INFO) << "*** Resuming worker " << wid << " ***";
  resume_worker(wid);
  return {};
}

// ---------------------------------------------------------------------------
// Traffic classes
// ---------------------------------------------------------------------------

ControlResult<void> ControlPlane::AddTc(const TrafficClassSpec& spec) {
  std::lock_guard<std::mutex> lock(mutex_);
  return AddTcLocked(spec);
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
    if (bess::ResourceMap.count(spec.resource) == 0) {
      return std::unexpected(Err(EINVAL, "Invalid resource"));
    }
    c = reinterpret_cast<bess::TrafficClass*>(
        TrafficClassBuilder::CreateTrafficClass<
            bess::WeightedFairTrafficClass>(tc_name,
                                            bess::ResourceMap.at(
                                                spec.resource)));
  } else if (policy == bess::TrafficPolicyName[bess::POLICY_ROUND_ROBIN]) {
    c = reinterpret_cast<bess::TrafficClass*>(
        TrafficClassBuilder::CreateTrafficClass<bess::RoundRobinTrafficClass>(
            tc_name));
  } else if (policy == bess::TrafficPolicyName[bess::POLICY_RATE_LIMIT]) {
    uint64_t limit = 0;
    uint64_t max_burst = 0;
    if (bess::ResourceMap.count(spec.resource) == 0) {
      return std::unexpected(Err(EINVAL, "Invalid resource"));
    }
    if (spec.limit.find(spec.resource) != spec.limit.end()) {
      limit = spec.limit.at(spec.resource);
    }
    if (spec.max_burst.find(spec.resource) != spec.max_burst.end()) {
      max_burst = spec.max_burst.at(spec.resource);
    }
    c = reinterpret_cast<bess::TrafficClass*>(
        TrafficClassBuilder::CreateTrafficClass<bess::RateLimitTrafficClass>(
            tc_name, bess::ResourceMap.at(spec.resource), limit, max_burst));
  } else if (policy == bess::TrafficPolicyName[bess::POLICY_LEAF]) {
    return std::unexpected(Err(EINVAL,
                               "Cannot create leaf TC. Use "
                               "UpdateTcParentRequest message"));
  } else {
    return std::unexpected(Err(EINVAL, "Invalid traffic policy"));
  }

  if (!c) {
    return std::unexpected(Err(ENOMEM, "CreateTrafficClass failed"));
  }

  return AttachTc(c, spec);
}

ControlResult<void> ControlPlane::UpdateTcParams(const TrafficClassSpec& spec) {
  std::lock_guard<std::mutex> lock(mutex_);

  WorkerPauser wp;

  ControlResult<bess::TrafficClass*> found = FindTc(spec);
  if (!found) {
    return std::unexpected(found.error());
  }
  bess::TrafficClass* c = *found;

  if (c->policy() == bess::POLICY_RATE_LIMIT) {
    bess::RateLimitTrafficClass* tc =
        reinterpret_cast<bess::RateLimitTrafficClass*>(c);
    if (bess::ResourceMap.count(spec.resource) == 0) {
      return std::unexpected(Err(EINVAL, "Invalid resource"));
    }
    tc->set_resource(bess::ResourceMap.at(spec.resource));
    if (spec.limit.find(spec.resource) != spec.limit.end()) {
      tc->set_limit(spec.limit.at(spec.resource));
    }
    if (spec.max_burst.find(spec.resource) != spec.max_burst.end()) {
      tc->set_max_burst(spec.max_burst.at(spec.resource));
    }
  } else if (c->policy() == bess::POLICY_WEIGHTED_FAIR) {
    bess::WeightedFairTrafficClass* tc =
        reinterpret_cast<bess::WeightedFairTrafficClass*>(c);
    if (bess::ResourceMap.count(spec.resource) == 0) {
      return std::unexpected(Err(EINVAL, "Invalid resource"));
    }
    tc->set_resource(bess::ResourceMap.at(spec.resource));
  } else {
    return std::unexpected(Err(EINVAL,
                               "Only 'rate_limit' and"
                               " 'weighted_fair' can be updated"));
  }

  return {};
}

ControlResult<void> ControlPlane::UpdateTcParent(const TrafficClassSpec& spec) {
  std::lock_guard<std::mutex> lock(mutex_);
  return UpdateTcParentLocked(spec);
}

ControlResult<void> ControlPlane::UpdateTcParentLocked(const TrafficClassSpec& spec) {

  WorkerPauser wp;

  ControlResult<bess::TrafficClass*> found = FindTc(spec);
  if (!found) {
    return std::unexpected(found.error());
  }
  bess::TrafficClass* c = *found;

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

  return AttachTc(c, spec);
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
  if (!detach_tc(c)) {
    return std::unexpected(
        Err(EBUSY, "Cannot detach '%s' while it is part of a worker",
            name.c_str()));
  }

  runtime().traffic_classes().Release(c);
  delete c;
  return {};
}

ControlResult<void> ControlPlane::ResetTcs() {
  std::lock_guard<std::mutex> lock(mutex_);
  return ResetTcsLocked();
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
  for (int i = 0; i < Worker::kMaxWorkers; i++) {
    Worker* worker = runtime().workers().Get(i);
    if (worker == nullptr) {
      continue;
    }
    int socket = 1ull << worker->socket();
    int core = worker->core();
    bess::TrafficClass* root = worker->scheduler()->root();

    for (const auto& tc_pair : TrafficClassBuilder::all_tcs()) {
      bess::TrafficClass* c = tc_pair.second.get();
      if (c->policy() == bess::POLICY_LEAF && root == c->Root()) {
        auto leaf = static_cast<bess::LeafTrafficClass*>(c);
        int constraints = leaf->task()->GetSocketConstraints();
        if ((constraints & socket) == 0) {
          LOG(WARNING) << "Scheduler constraints are violated for wid " << i
                       << " socket " << socket << " constraint "
                       << constraints;
          report.violations.push_back(SchedulingConstraintViolation{
              c->name(), constraints, worker->socket(), core});
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
    Module* m = runtime().modules().Find(module_name);
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

ControlResult<void> ControlPlane::AttachTc(bess::TrafficClass* c_,
                                           const TrafficClassSpec& spec) {
  std::unique_ptr<bess::TrafficClass> c(c_);

  // The class is already registered (it was created through
  // TrafficClassBuilder), so a failure here has to give the registry entry back
  // before the unique_ptr destroys the object -- otherwise the registry would
  // keep a pointer to freed memory.
  auto fail = [&](ControlError error) -> ControlResult<void> {
    runtime().traffic_classes().Release(c.get());
    return std::unexpected(error);
  };

  int wid = spec.wid;

  if (spec.parent == "") {
    if (wid != Worker::kAnyWorker && (wid < 0 || wid >= Worker::kMaxWorkers)) {
      return fail(Err(EINVAL, "'wid' must be %d or between 0 and %d",
                      Worker::kAnyWorker, Worker::kMaxWorkers - 1));
    }

    int active_workers = runtime().workers().num_workers();
    if ((wid != Worker::kAnyWorker && !is_worker_active(wid)) ||
        (wid == Worker::kAnyWorker && active_workers == 0)) {
      if (active_workers == 0 && (wid == 0 || wid == Worker::kAnyWorker)) {
        launch_worker(0, FLAGS_c);
      } else {
        return fail(Err(EINVAL, "worker:%d does not exist",
                        static_cast<int>(wid)));
      }
    }

    add_tc_to_orphan(c.release(), wid);
    return {};
  }

  if (wid != Worker::kAnyWorker) {
    return fail(Err(EINVAL,
                    "Both 'parent' and 'wid'"
                    "have been specified"));
  }

  bess::TrafficClass* parent = TrafficClassBuilder::Find(spec.parent);
  if (!parent) {
    return fail(
        Err(ENOENT, "Parent TC '%s' not found", spec.parent.c_str()));
  }

  bool fail_add = false;
  switch (parent->policy()) {
    case bess::POLICY_PRIORITY: {
      if (!spec.has_priority) {
        return fail(Err(EINVAL, "No priority specified"));
      }
      bess::priority_t pri = spec.priority;
      if (pri == DEFAULT_PRIORITY) {
        return fail(Err(EINVAL, "Priority %d is reserved", DEFAULT_PRIORITY));
      }
      fail_add = !static_cast<bess::PriorityTrafficClass*>(parent)->AddChild(
          c.get(), pri);
      break;
    }
    case bess::POLICY_WEIGHTED_FAIR:
      if (!spec.has_share) {
        return fail(Err(EINVAL, "No share specified"));
      }
      fail_add = !static_cast<bess::WeightedFairTrafficClass*>(parent)->AddChild(
          c.get(), spec.share);
      break;
    case bess::POLICY_ROUND_ROBIN:
      fail_add = !static_cast<bess::RoundRobinTrafficClass*>(parent)->AddChild(
          c.get());
      break;
    case bess::POLICY_RATE_LIMIT:
      fail_add = !static_cast<bess::RateLimitTrafficClass*>(parent)->AddChild(
          c.get());
      break;
    default:
      return fail(Err(EPERM, "Parent tc doesn't support children"));
  }
  if (fail_add) {
    return fail(Err(EINVAL, "AddChild() failed"));
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

  std::vector<std::string> created_hook_names;

  if (spec.is_igate) {
    for (auto& gate : m->igates()) {
      if (!gate) {
        continue;
      }
      pb_error_t error;
      bess::GateHook* hook = gate->CreateGateHook(&builder, gate,
                                                  spec.hook_name, spec.arg,
                                                  &error);
      if (error.code() != 0 || !hook) {
        // in case of failed creating gate hook, remove previously created
        // before return
        for (const auto& name : created_hook_names) {
          gate->RemoveHook(name);
        }
        return std::unexpected(ErrorFromLegacy(error.code(), error.errmsg()));
      }
      created_hook_names.push_back(hook->name());
    }
  } else {
    for (auto& gate : m->ogates()) {
      if (!gate) {
        continue;
      }
      pb_error_t error;
      bess::GateHook* hook = gate->CreateGateHook(&builder, gate,
                                                  spec.hook_name, spec.arg,
                                                  &error);
      if (error.code() != 0 || !hook) {
        // in case of failed creating gate hook, remove previously created
        // before return
        for (const auto& name : created_hook_names) {
          gate->RemoveHook(name);
        }
        return std::unexpected(ErrorFromLegacy(error.code(), error.errmsg()));
      }
      created_hook_names.push_back(hook->name());
    }
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
  Module *m = runtime().modules().Find(spec.module_name);
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
// Resume hooks
// ---------------------------------------------------------------------------

ControlResult<bess::pb::CommandResponse> ControlPlane::ConfigureResumeHook(
    const std::string& hook_name, bool enable,
    const google::protobuf::Any& arg) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto& hooks = bess::global_resume_hooks;
  auto hook_it = hooks.end();
  for (auto it = hooks.begin(); it != hooks.end(); ++it) {
    if (it->get()->name() == hook_name) {
      hook_it = it;
      break;
    }
  }

  if (!enable) {
    if (hook_it != hooks.end()) {
      hooks.erase(hook_it);
    }
    return bess::pb::CommandResponse();
  }

  if (hook_it != hooks.end()) {
    return std::unexpected(
        Err(EEXIST, "Resume hook '%s' is already installed", hook_name.c_str()));
  }

  const auto builder = bess::ResumeHookBuilder::all_resume_hook_builders().find(
      hook_name);
  if (builder == bess::ResumeHookBuilder::all_resume_hook_builders().end()) {
    return std::unexpected(
        Err(ENOENT, "No such resume hook '%s'", hook_name.c_str()));
  }
  auto hook = builder->second.CreateResumeHook();
  bess::pb::CommandResponse init_response =
      builder->second.InitResumeHook(hook.get(), arg);
  if (init_response.has_error()) {
    return init_response;
  }
  hooks.insert(std::move(hook));

  return init_response;
}

// ---------------------------------------------------------------------------
// Plugins
// ---------------------------------------------------------------------------

ControlResult<void> ControlPlane::ImportPlugin(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);

  WorkerPauser wp;
  VLOG(1) << "Loading plugin: " << path;
  if (!bess::bessd::LoadPlugin(path)) {
    return std::unexpected(
        Err(-1, "Failed loading plugin %s", path.c_str()));
  }
  return {};
}

ControlResult<void> ControlPlane::UnloadPlugin(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);

  WorkerPauser wp;

  VLOG(1) << "Unloading plugin: " << path;
  if (!bess::bessd::UnloadPlugin(path)) {
    return std::unexpected(
        Err(-1, "Failed unloading plugin %s", path.c_str()));
  }
  return {};
}

// ---------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------

ControlResult<ApplyResult> ControlPlane::ApplyPipeline(
    const PipelineSpec &desired, const ApplyOptions &options) {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto validation_start = std::chrono::steady_clock::now();

  // 1. Validate. Pure, so a rejected spec cannot have touched anything.
  auto validated = bess::control::ValidatePipeline(runtime(), desired);
  if (!validated) {
    return std::unexpected(validated.error());
  }

  // 2. Optimistic concurrency: a stale writer is rejected before any effect.
  if (options.expected_generation.has_value() &&
      *options.expected_generation != runtime().generation()) {
    ControlError error = Err(ESTALE, "expected generation %llu, active generation %llu",
                             static_cast<unsigned long long>(
                                 *options.expected_generation),
                             static_cast<unsigned long long>(
                                 runtime().generation()));
    error.code = ControlErrorCode::kConflict;
    error.object = "pipeline";
    error.field = "expected_generation";
    return std::unexpected(error);
  }

  // 3. Diff and plan. Nothing to do means no pause and no generation change.
  const uint64_t validation_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - validation_start)
          .count());

  PipelineSnapshot before = SnapshotRuntime(runtime());
  PipelineDiff diff = Diff(before, validated->spec);
  if (diff.empty()) {
    ApplyTiming timing;
    timing.validation_us = validation_us;
    return ApplyResult{runtime().generation(), 0, false, timing};
  }
  PipelinePlan plan = Plan(diff);

  // 4. Refuse what cannot be made reversible rather than hoping rollback works.
  if (auto reversible = CheckReversibility(plan); !reversible) {
    return std::unexpected(reversible.error());
  }

  // 5. Run it.
  Transaction transaction(this, std::move(plan), std::move(before));
  if (auto prepared = transaction.Prepare(); !prepared) {
    transaction.Abort();
    return std::unexpected(prepared.error());
  }
  if (auto committed = transaction.Commit(); !committed) {
    transaction.Abort();
    return std::unexpected(committed.error());
  }
  transaction.Retire();

  runtime().BumpGeneration();

  ApplyTiming timing = transaction.timing();
  timing.validation_us = validation_us;

  return ApplyResult{runtime().generation(), transaction.ops_executed(),
                     transaction.quiescence() == Quiescence::kWorkers, timing};
}

ControlResult<ValidatedPipeline> ControlPlane::ValidatePipeline(
    const PipelineSpec &desired) {
  std::lock_guard<std::mutex> lock(mutex_);
  return bess::control::ValidatePipeline(runtime(), desired);
}

PipelineSnapshot ControlPlane::GetPipeline() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SnapshotRuntime(runtime());
}

ControlResult<PipelineDiff> ControlPlane::DiffPipeline(
    const PipelineSpec &desired) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto validated = bess::control::ValidatePipeline(runtime(), desired);
  if (!validated) {
    return std::unexpected(validated.error());
  }

  return Diff(SnapshotRuntime(runtime()), validated->spec);
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

  // One transaction, one lock, no RPC handler calling other RPC handlers.
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

  return {};
}

}  // namespace control
}  // namespace bess
