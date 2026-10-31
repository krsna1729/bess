// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_CONTROL_PLANE_H_
#define BESS_CONTROL_CONTROL_PLANE_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "control/control_error.h"
#include "control/pipeline_spec.h"
#include "control/pipeline_diff.h"
#include "control/pipeline_plan.h"
#include "control/pipeline_resources.h"
#include "control/pipeline_snapshot.h"
#include "control/pipeline_validator.h"
#include "control/transaction.h"
#include "message.h"
#include "metadata.h"
#include "module.h"
#include "port.h"
#include "traffic_class.h"
#include "utils/common.h"
#include "worker.h"

namespace bess {
namespace control {

// Requests are plain C++ values: the RPC layer converts protobuf messages into
// these and converts results back. The structural desired-state types live in
// control/pipeline_spec.h; the ones here are operation-specific.
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

  // -- modules --
  ControlResult<std::string> CreateModule(const ModuleSpec& spec);
  ControlResult<void> DestroyModule(const std::string& name);
  ControlResult<void> ConnectModules(const ConnectionSpec& spec);
  ControlResult<void> DisconnectModules(const DisconnectionSpec& spec);
  // Runs module `module`'s action `command` with `arg` (GetSchema names each
  // action's argument type). A command not marked thread-safe is refused
  // (EBUSY) while a worker runs: pause first. The command's own failure is the
  // response's error; the result's error is only "no such module". An action
  // that edits the resource it owns (edits_resources) moves the resource
  // generation, so an apply built on an older read conflicts instead of
  // undoing the edit (D-129, D-140).
  ControlResult<bess::pb::CommandResponse> RunModuleCommand(
      const std::string& module, const std::string& command,
      const google::protobuf::Any& arg);

  // -- workers --
  // No core: automatic -- Placement::NextWorkerCpu over the live workers'
  // CPUs; ENOSPC when none is free. An explicit core must be in ProcessCpus()
  // (EINVAL otherwise); anywhere else in it is honoured. Either way, what
  // DescribeWorkerCpu finds (another node, a control CPU, a shared core) is
  // logged once as a warning, never refused (D-123).
  ControlResult<void> AddWorker(uint64_t wid, std::optional<uint64_t> core,
                                const std::string& scheduler);
  ControlResult<void> DestroyWorker(uint64_t wid);
  ControlResult<void> ResetWorkers();
  ControlResult<void> PauseAll();
  ControlResult<void> PauseWorker(uint64_t wid);
  ControlResult<void> ResumeAll();
  ControlResult<void> ResumeWorker(uint64_t wid);

  // -- traffic classes --
  ControlResult<void> AddTc(const TrafficClassSpec& spec);
  ControlResult<void> UpdateTcParent(const TrafficClassSpec& spec);
  ControlResult<SchedulingConstraintsReport> CheckSchedulingConstraints();

  // -- gate hooks --
  ControlResult<std::string> ConfigureGateHook(const GateHookSpec& spec,
                                               bool enable);

  // -- plugins --
  ControlResult<void> ImportPlugin(const std::string& path);
  ControlResult<void> UnloadPlugin(const std::string& path);

  // -- desired state --
  // Validates a desired pipeline without touching the runtime, and snapshots
  // the active one. ApplyPipeline (the transactional path) lands with the
  // planner and transaction engine; these two are its read-only half.
  ControlResult<ValidatedPipeline> ValidatePipeline(const PipelineSpec& desired);
  PipelineSnapshot GetPipeline() const;

  // Validated desired state -> what has to change -> dependency-ordered plan.
  // Pure with respect to the runtime; nothing here mutates anything. The
  // diff's `resources` count what converging each listed resource changes
  // (D-129); a key that does not decode is INVALID_ARGUMENT here already.
  ControlResult<PipelineDiff> DiffPipeline(const PipelineSpec& desired) const;
  ControlResult<PipelinePlan> PlanPipeline(const PipelineSpec& desired) const;

  // The same reads, paired with the generations they observed (the
  // pipeline's, and the dataplane transaction generation resource contents
  // are at), taken under one lock acquisition -- what a client needs to
  // follow up with an ApplyPipeline(expected_generation) (G1).
  template <typename T>
  struct Versioned {
    T value;
    uint64_t generation = 0;
    uint64_t resource_generation = 0;
  };
  // Which resources' contents a read includes (D-129).
  struct ResourceQuery {
    bool include = false;
    std::vector<std::string> names;  // with include: only these (empty: all)
    bool include_metadata = false;   // also snapshot the metadata report
    // With include: contents above this many serialized bytes are refused
    // (RESOURCE_EXHAUSTED) without building them (ReadResources).
    size_t max_bytes = std::numeric_limits<size_t>::max();
  };
  struct PipelineRead {
    PipelineSnapshot snapshot;
    std::vector<ResourceSpec> resources;  // by name
    // Copied under this call's mutex when include_metadata is set, so a
    // concurrent apply/resume cannot change it after the lock is released.
    std::optional<metadata::MetadataReport> metadata;
  };
  ControlResult<Versioned<PipelineRead>> ReadPipeline(const ResourceQuery& query) const;
  // One page of one resource's contents, for the streamed read: the entries
  // from `cursor` (empty: the first page), bounded as ReadResourcePage bounds
  // them, the next page's cursor, and the generations they were read at.
  // Each page is its own lock hold. Unknown names are NOT_FOUND, as
  // ReadPipeline's are.
  ControlResult<Versioned<ResourceEntryPage>> ReadResource(
      const std::string& name, const std::string& cursor, size_t max_entries,
      size_t max_bytes) const;
  // Empty the named map resources without sending their keys to the client:
  // each is converged to no entries and the erases are applied in batches,
  // with the same pause rule as a desired-state apply. Duplicate names are
  // removed; a singleton is a no-op (it has no entries); an unknown name is
  // NOT_FOUND and nothing is erased. A stale expected resource generation is
  // a conflict before any effect.
  struct ResourceReset {
    std::vector<std::string> resources;  // deduped, request order
    size_t removed = 0;                  // entries erased
  };
  ControlResult<Versioned<ResourceReset>> ResetResources(
      std::vector<std::string> names,
      std::optional<uint64_t> expected_resource_generation);
  ControlResult<Versioned<PipelineDiff>> DiffPipelineVersioned(
      const PipelineSpec& desired) const;
  ControlResult<Versioned<PipelinePlan>> PlanPipelineVersioned(
      const PipelineSpec& desired) const;

  // -- transactions --
  // The transactional path (section 9.7): validate, check the expected
  // generations, diff, plan, then run the plan as a transaction: prepare,
  // commit, the listed resources' contents in one dataplane transaction
  // (D-129; its refusal aborts the whole apply), retire. A failure leaves the
  // previous runtime active; a no-op apply neither pauses workers nor bumps
  // a generation, and one that changes only resource contents bumps only the
  // dataplane transaction generation.
  ControlResult<ApplyResult> ApplyPipeline(const PipelineSpec& desired,
                                           const ApplyOptions& options);

  // -- composition --
  // Today's ResetAll: modules, then ports, then TCs, then workers. Composed
  // here rather than by one RPC handler calling four others.
  ControlResult<void> Reset();

 private:
  friend class Transaction;

  // Lock-free bodies used by composed operations (Reset) and by the transaction
  // engine, both of which already hold the control-plane lock. Public wrappers
  // take the lock and call these.
  ControlResult<PortInfo> CreatePortLocked(const PortSpec& spec);
  ControlResult<void> DestroyPortLocked(const std::string& name);
  ControlResult<std::string> CreateModuleLocked(const ModuleSpec& spec);
  ControlResult<void> DestroyModuleLocked(const std::string& name);
  ControlResult<void> ConnectModulesLocked(const ConnectionSpec& spec);
  ControlResult<void> DisconnectModulesLocked(const DisconnectionSpec& spec);
  ControlResult<void> AddWorkerLocked(uint64_t wid,
                                      std::optional<uint64_t> core,
                                      const std::string& scheduler);
  ControlResult<void> DestroyWorkerLocked(uint64_t wid);
  ControlResult<void> AddTcLocked(const TrafficClassSpec& spec);
  ControlResult<void> UpdateTcParentLocked(const TrafficClassSpec& spec);
  ControlResult<void> UpdateTcParamsLocked(const TrafficClassSpec& spec);
  // Moves a traffic class to a new parent/priority/share. Unlike the legacy
  // UpdateTcParent this works for a class that is currently attached to a
  // worker: the move purges the subtree from the scheduler wakeup queues first.
  ControlResult<void> ReparentTcLocked(const TrafficClassSpec& spec);
  // Places module tasks' leaf classes (each spec names one by
  // leaf_module_name and leaf_module_taskid) under its parent or at its
  // worker, in two phases: every task is detached, then each attached, so
  // tasks may trade places. The tasks of the `leaving` modules leave their
  // classes too (orphaned until their module is removed), making room. All or
  // nothing: a refused attach puts every task back. For the transaction,
  // whose workers are paused.
  // `previous_out`, when given, receives where each class this call moves sat
  // before it moved, in the same order: the transaction's undo for a placement
  // is to run this again with those specs.
  ControlResult<void> PlaceTasksLocked(std::span<const TrafficClassSpec> leaves,
                                       std::span<const std::string> leaving,
                                       std::vector<TrafficClassSpec> *previous_out = nullptr);
  void RemoveSubtreeFromWakeupQueues(bess::TrafficClass* c);
  ControlResult<void> RemoveTcLocked(const std::string& name);

  ControlResult<void> ResetModulesLocked();
  ControlResult<void> ResetPortsLocked();
  ControlResult<void> ResetTcsLocked();
  ControlResult<void> ResetWorkersLocked();

  // Fills `diff->resources` for the validated `desired` (D-129). A resource
  // whose module the diff creates or replaces does not exist yet: unchecked.
  ControlResult<void> DiffResourcesLocked(const PipelineSpec& desired,
                                          PipelineDiff* diff) const;
  // Converges the listed resources in one dataplane transaction, pausing the
  // workers if a resource that changes requires it. Returns the operations
  // applied.
  ControlResult<size_t> ApplyResourcesLocked(std::span<const ResourceSpec> resources);

  ControlResult<void> AttachTc(bess::TrafficClass* c_,
                               const TrafficClassSpec& spec);
  // Attaches a class that already exists. Never unregisters or destroys it on
  // failure: the caller owns that decision (creation destroys, reparenting puts
  // the class back where it was).
  ControlResult<void> AttachExistingTcLocked(bess::TrafficClass* c,
                                             const TrafficClassSpec& spec);
  ControlResult<bess::TrafficClass*> FindTc(const TrafficClassSpec& spec);

  // Locked by every public method, including the const readers (the snapshot
  // and validation paths), hence mutable.
  mutable std::mutex mutex_;
};

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_CONTROL_PLANE_H_
