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

#ifndef BESS_CONTROL_CONTROL_PLANE_H_
#define BESS_CONTROL_CONTROL_PLANE_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "control/control_error.h"
#include "message.h"
#include "port.h"
#include "traffic_class.h"
#include "utils/common.h"
#include "worker.h"

namespace bess {
namespace control {

// Requests are plain C++ values: the RPC layer converts protobuf messages into
// these and converts results back. Phase G0 commit 1 keeps the request shape of
// today's RPCs; the desired-state PipelineSpec (see MODERNIZATION.md section 9)
// is what these requests will be derived from.
struct PortSpec {
  std::string name;
  std::string driver;
  queue_t num_inc_q = 0;
  queue_t num_out_q = 0;
  uint64_t size_inc_q = 0;
  uint64_t size_out_q = 0;
  google::protobuf::Any arg;
};

struct PortInfo {
  std::string name;
  std::string driver;
  std::string mac_addr;
};

// Port configuration as it arrives from a client; the control plane validates
// and converts it, so malformed input is rejected with the same error ordering
// as before this extraction.
struct PortConfSpec {
  std::string mac_addr;
  uint32_t mtu = 1500;
  bool admin_up = true;
};

struct ModuleSpec {
  std::string name;
  std::string mclass;
  google::protobuf::Any arg;
};

struct ConnectionSpec {
  std::string m1;
  std::string m2;
  gate_idx_t ogate = 0;
  gate_idx_t igate = 0;
  bool skip_default_hooks = false;
};

struct DisconnectionSpec {
  std::string name;
  gate_idx_t ogate = 0;
};

struct TrafficClassSpec {
  std::string name;
  std::string parent;
  std::string policy;
  std::string resource;
  int wid = Worker::kAnyWorker;
  bool has_priority = false;
  int64_t priority = 0;
  bool has_share = false;
  int64_t share = 0;
  std::map<std::string, int64_t> limit;
  std::map<std::string, int64_t> max_burst;
  std::string leaf_module_name;
  uint64_t leaf_module_taskid = 0;
};

struct SchedulingConstraintViolation {
  std::string name;
  int constraint = 0;
  int assigned_node = 0;
  int assigned_core = 0;
};

struct ModuleConstraintViolation {
  std::string name;
};

struct SchedulingConstraintsReport {
  std::vector<SchedulingConstraintViolation> violations;
  std::vector<ModuleConstraintViolation> modules;
  bool fatal = false;
};

struct GateHookSpec {
  std::string class_name;
  std::string hook_name;
  std::string module_name;
  bool is_igate = false;
  int64_t gate_idx = 0;
  bool use_gate = false;
  google::protobuf::Any arg;
};

// Owns BESS control-plane semantics: validation, resource lifetime decisions,
// composition of multi-object operations, and (as Phase G0 progresses) the
// transaction engine. It must not contain gRPC concerns.
//
// Single-writer discipline: every public method takes the control-plane lock,
// which is non-recursive -- control-plane methods never call each other's
// public API. The RPC service holds no lock of its own.
class ControlPlane {
 public:
  ControlPlane() = default;

  ControlPlane(const ControlPlane&) = delete;
  ControlPlane& operator=(const ControlPlane&) = delete;

  // Acquires the control-plane lock for callers that still read runtime state
  // directly (the read-only RPC handlers). Temporary: reads move behind
  // snapshot-backed accessors as G0 progresses.
  std::unique_lock<std::mutex> AcquireLock() {
    return std::unique_lock<std::mutex>(mutex_);
  }

  // -- ports --
  ControlResult<PortInfo> CreatePort(const PortSpec& spec);
  ControlResult<void> DestroyPort(const std::string& name);
  ControlResult<bess::pb::CommandResponse> SetPortConf(
      const std::string& name, const PortConfSpec& spec);
  ControlResult<void> ResetPorts();

  // -- modules --
  ControlResult<std::string> CreateModule(const ModuleSpec& spec);
  ControlResult<void> DestroyModule(const std::string& name);
  ControlResult<void> ResetModules();
  ControlResult<void> ConnectModules(const ConnectionSpec& spec);
  ControlResult<void> DisconnectModules(const DisconnectionSpec& spec);

  // -- workers --
  ControlResult<void> AddWorker(uint64_t wid, uint64_t core,
                                const std::string& scheduler);
  ControlResult<void> DestroyWorker(uint64_t wid);
  ControlResult<void> ResetWorkers();
  ControlResult<void> PauseAll();
  ControlResult<void> PauseWorker(uint64_t wid);
  ControlResult<void> ResumeAll();
  ControlResult<void> ResumeWorker(uint64_t wid);

  // -- traffic classes --
  ControlResult<void> AddTc(const TrafficClassSpec& spec);
  ControlResult<void> UpdateTcParams(const TrafficClassSpec& spec);
  ControlResult<void> UpdateTcParent(const TrafficClassSpec& spec);
  ControlResult<void> ResetTcs();
  ControlResult<SchedulingConstraintsReport> CheckSchedulingConstraints();

  // -- gate hooks --
  ControlResult<std::string> ConfigureGateHook(const GateHookSpec& spec,
                                               bool enable);

  // -- resume hooks --
  ControlResult<bess::pb::CommandResponse> ConfigureResumeHook(
      const std::string& hook_name, bool enable,
      const google::protobuf::Any& arg);

  // -- plugins --
  ControlResult<void> ImportPlugin(const std::string& path);
  ControlResult<void> UnloadPlugin(const std::string& path);

  // -- composition --
  // Today's ResetAll: modules, then ports, then TCs, then workers. Composed
  // here rather than by one RPC handler calling four others.
  ControlResult<void> Reset();

 private:
  // Lock-free bodies used by composed operations (Reset) that already hold the
  // control-plane lock. Public wrappers take the lock and call these.
  ControlResult<void> ResetModulesLocked();
  ControlResult<void> ResetPortsLocked();
  ControlResult<void> ResetTcsLocked();
  ControlResult<void> ResetWorkersLocked();

  ControlResult<void> AttachTc(bess::TrafficClass* c_,
                               const TrafficClassSpec& spec);
  ControlResult<bess::TrafficClass*> FindTc(const TrafficClassSpec& spec);

  std::mutex mutex_;
};

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_CONTROL_PLANE_H_
