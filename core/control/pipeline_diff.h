// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_PIPELINE_DIFF_H_
#define BESS_CONTROL_PIPELINE_DIFF_H_

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
  TrafficClassSpec desired;
};

struct PipelineDiff {
  std::vector<PortChange> ports;
  std::vector<ModuleChange> modules;
  std::vector<ConnectionChange> connections;
  std::vector<WorkerChange> workers;
  std::vector<TrafficClassChange> traffic_classes;

  // True when the desired state is already active. A no-op apply must not
  // pause workers and must not bump the generation.
  bool empty() const {
    return ports.empty() && modules.empty() && connections.empty() &&
           workers.empty() && traffic_classes.empty();
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
