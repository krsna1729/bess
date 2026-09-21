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

#ifndef BESS_CONTROL_PIPELINE_SNAPSHOT_H_
#define BESS_CONTROL_PIPELINE_SNAPSHOT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "control/pipeline_spec.h"
#include "control/runtime_state.h"
#include "gate.h"
#include "message.h"
#include "port.h"

namespace bess {
namespace control {

// Structural snapshot of the active runtime (MODERNIZATION.md section 9.4).
// It carries no transient statistics and no worker paused/running transitions,
// and it is deterministic: registries are ordered maps, connections are derived
// from module gates in (upstream, ogate) order, workers ascend by wid, and no
// pointer address is ever recorded. Desired-state *and* active-state arguments
// are compared by serialized bytes, since protobuf messages have no value
// equality.

struct PortSnapshot {
  std::string name;
  std::string driver;
  std::string mac_addr;
  queue_t num_rx_queues = 0;
  queue_t num_tx_queues = 0;
  uint64_t rx_queue_size = 0;
  uint64_t tx_queue_size = 0;
  google::protobuf::Any driver_arg;

  bool operator==(const PortSnapshot &other) const;
};

struct ModuleSnapshot {
  std::string name;
  std::string mclass;
  google::protobuf::Any arg;

  bool operator==(const ModuleSnapshot &other) const;
};

struct ConnectionSnapshot {
  std::string upstream;
  gate_idx_t ogate = 0;
  std::string downstream;
  gate_idx_t igate = 0;

  bool operator==(const ConnectionSnapshot &other) const = default;
};

struct WorkerSnapshot {
  int wid = -1;
  int core = -1;
  std::string scheduler;

  bool operator==(const WorkerSnapshot &other) const = default;
};

struct TrafficClassSnapshot {
  std::string name;
  std::string parent;
  std::string policy;
  int wid = -1;
  std::string leaf_module_name;
  uint64_t leaf_module_taskid = 0;

  bool operator==(const TrafficClassSnapshot &other) const = default;
};

struct PipelineSnapshot {
  std::vector<PortSnapshot> ports;
  std::vector<ModuleSnapshot> modules;
  std::vector<ConnectionSnapshot> connections;
  std::vector<WorkerSnapshot> workers;
  std::vector<TrafficClassSnapshot> traffic_classes;

  bool operator==(const PipelineSnapshot &other) const = default;
};

// Reads the active runtime into a snapshot. Pure: it never mutates the state it
// is given.
PipelineSnapshot SnapshotRuntime(const RuntimeState &runtime);

// Reconstructs the desired-state description of an active runtime. Internal
// traffic classes (module leaf classes and scheduler defaults, whose names
// start with '!') are left out: they are consequences of the modules and
// workers that own them, not desired-state objects. This is what makes
// "apply the same pipeline again" a no-op.
PipelineSpec SpecFromSnapshot(const PipelineSnapshot &snapshot);

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_PIPELINE_SNAPSHOT_H_
