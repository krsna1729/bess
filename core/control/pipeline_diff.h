// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_PIPELINE_DIFF_H_
#define BESS_CONTROL_PIPELINE_DIFF_H_

#include <cstdint>
#include <string>
#include <vector>

#include "control/pipeline_snapshot.h"
#include "control/pipeline_spec.h"
#include "gate.h"

namespace bess {
namespace control {

// How one resource differs between the active runtime and the desired state
// (MODERNIZATION.md section 9.6). `kReplace` means the object cannot be
// reconfigured in place and has to be recreated; `kUpdate` means it can.
enum class ChangeKind {
  kCreate,
  kRemove,
  kReplace,       // cannot be changed in place: rebuild it
  kUpdate,        // reattach (parent, priority or share)
  kUpdateParams,  // change parameters in place (rate limit, burst, resource)
  kUnchanged,
};

struct PortChange {
  std::string name;
  ChangeKind kind = ChangeKind::kUnchanged;
  PortSpec desired;  // the desired configuration, for create/replace/update
};

struct ModuleChange {
  std::string name;
  ChangeKind kind = ChangeKind::kUnchanged;
  ModuleSpec desired;
};

struct ConnectionChange {
  std::string upstream;
  gate_idx_t ogate = 0;
  std::string downstream;
  gate_idx_t igate = 0;
  ChangeKind kind = ChangeKind::kUnchanged;  // kCreate == connect, kRemove == disconnect
};

struct WorkerChange {
  int wid = -1;
  ChangeKind kind = ChangeKind::kUnchanged;  // kReplace == move to another CPU
  WorkerSpec desired;
};

struct TrafficClassChange {
  std::string name;
  ChangeKind kind = ChangeKind::kUnchanged;
  TrafficClassSpec desired;  // kRemove: only name and the current parent
  // kUpdate (moved): its parameters (resource, limit, max_burst) change too.
  bool params_changed = false;
};

// A task to place where desired state names (always kUpdate: a task is never
// created or removed by placement, only moved).
struct TaskChange {
  TaskPlacementSpec desired;
  ChangeKind kind = ChangeKind::kUpdate;
};

// What converging one listed resource changes, by entries (D-129).
struct ResourceDelta {
  std::string name;
  uint64_t created = 0;
  uint64_t updated = 0;
  uint64_t removed = 0;
  // False: the resource does not exist yet (the apply creates or replaces its
  // module); every entry counts as created and is checked at the apply.
  bool checked = true;

  bool empty() const { return created == 0 && updated == 0 && removed == 0; }
};

struct PipelineDiff {
  std::vector<PortChange> ports;
  std::vector<ModuleChange> modules;
  std::vector<ConnectionChange> connections;
  std::vector<WorkerChange> workers;
  std::vector<TrafficClassChange> traffic_classes;
  std::vector<TaskChange> tasks;  // in (module, task) order
  // Listed resources whose contents change, by name. Diff() leaves it empty:
  // the control plane fills it from the resources themselves
  // (pipeline_resources.h).
  std::vector<ResourceDelta> resources;

  // True when the desired topology is already active (resources aside). A
  // no-op apply must not pause workers and must not bump the generation.
  bool empty() const {
    return ports.empty() && modules.empty() && connections.empty() &&
           workers.empty() && traffic_classes.empty() && tasks.empty();
  }
};

// Deterministic classification of every change between the active runtime and
// a desired pipeline. Comparison is by name and normalized value -- never by
// pointer -- and "0 means driver default" in a spec matches whatever the
// runtime resolved, so re-applying an equivalent description produces an empty
// diff instead of churn.
//
// `desired` is expected to be validated/normalized (see ValidatePipeline).
PipelineDiff Diff(const PipelineSnapshot &current, const PipelineSpec &desired);

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_PIPELINE_DIFF_H_
