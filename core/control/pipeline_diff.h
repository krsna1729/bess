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
