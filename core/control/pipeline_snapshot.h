// SPDX-License-Identifier: BSD-3-Clause

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

// A traffic class as the runtime holds it. Beyond identity and placement this
// carries the *parameters* that decide behaviour -- the resource a weighted-fair
// or rate-limit class regulates, its limit and burst, and the priority or share
// with which it hangs off its parent -- so that desired state which changes only
// a parameter is not mistaken for "unchanged".
struct TrafficClassSnapshot {
  std::string name;
  std::string parent;
  std::string policy;
  int wid = -1;

  // Own parameters (weighted_fair, rate_limit).
  std::string resource;
  uint64_t limit = 0;
  uint64_t max_burst = 0;

  // Attachment parameters, held by the parent: priority under a priority
  // class, share under a weighted-fair one.
  bool has_priority = false;
  int64_t priority = 0;
  bool has_share = false;
  int64_t share = 0;

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
