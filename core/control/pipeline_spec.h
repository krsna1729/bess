// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_PIPELINE_SPEC_H_
#define BESS_CONTROL_PIPELINE_SPEC_H_

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "gate.h"
#include "message.h"
#include "port.h"

namespace bess {
namespace control {

// Desired-state representation of a pipeline (MODERNIZATION.md section 9.3).
// This is the in-process domain model: protobuf is a wire format that the RPC
// adapter converts to and from these types, never the thing the control plane
// reasons about.
//
// Desired state requires explicit stable names -- unlike the legacy imperative
// RPCs, which may generate one -- because diff, reapply, idempotency,
// diagnostics and generation comparisons all depend on desired-state identity.

struct PortSpec {
  std::string name;
  std::string driver;
  queue_t num_rx_queues = 0;  // 0 means "one queue" / "driver default"
  queue_t num_tx_queues = 0;
  uint64_t rx_queue_size = 0;  // 0 means "driver default"
  uint64_t tx_queue_size = 0;
  google::protobuf::Any arg;

  // Protobuf messages have no value equality, so arguments are compared by
  // their serialized bytes.
  bool operator==(const PortSpec &other) const;
};

struct ModuleSpec {
  std::string name;
  std::string mclass;
  google::protobuf::Any arg;

  bool operator==(const ModuleSpec &other) const;
};

struct ConnectionSpec {
  std::string upstream;
  gate_idx_t ogate = 0;
  std::string downstream;
  gate_idx_t igate = 0;
  bool skip_default_hooks = false;

  bool operator==(const ConnectionSpec &other) const = default;
};

// One end of a disconnect: the upstream module and the output gate to clear.
struct DisconnectionSpec {
  std::string name;
  gate_idx_t ogate = 0;

  bool operator==(const DisconnectionSpec &other) const = default;
};

struct WorkerSpec {
  int wid = -1;
  // nullopt: automatic -- a free worker CPU of the memory node in SMT-striped
  // order (Placement::NextWorkerCpu), chosen at creation and kept for the
  // worker's life (D-123).
  std::optional<int> core;
  std::string scheduler;

  bool operator==(const WorkerSpec &other) const = default;
};

struct TrafficClassSpec {
  std::string name;
  std::string parent;
  std::string policy;
  std::string resource;
  int wid = -1;
  bool has_priority = false;
  int64_t priority = 0;
  bool has_share = false;
  int64_t share = 0;
  std::map<std::string, int64_t> limit;
  std::map<std::string, int64_t> max_burst;
  std::string leaf_module_name;
  uint64_t leaf_module_taskid = 0;

  bool operator==(const TrafficClassSpec &other) const = default;
};

// Where a module's task is scheduled: under `parent`, or (no parent) at the
// root of worker `wid`. Desired state names only the tasks it places; the
// leaf class itself belongs to the module.
struct TaskPlacementSpec {
  std::string module;
  uint64_t task = 0;
  std::string parent;
  int wid = -1;  // with no parent: the worker; -1: none
  bool has_priority = false;
  int64_t priority = 0;
  bool has_share = false;
  int64_t share = 0;

  // The legacy attach request this placement is (FindTc by module and task).
  TrafficClassSpec AsLeafSpec() const;

  bool operator==(const TaskPlacementSpec &other) const = default;
};

struct PipelineSpec {
  std::vector<PortSpec> ports;
  std::vector<ModuleSpec> modules;
  std::vector<ConnectionSpec> connections;
  std::vector<WorkerSpec> workers;
  std::vector<TrafficClassSpec> traffic_classes;
  std::vector<TaskPlacementSpec> tasks;

  bool operator==(const PipelineSpec &other) const = default;
};

// Puts a spec into canonical form: stable ordering and normalized defaults, so
// that two descriptions of the same pipeline compare equal and diffs do not
// churn (section 9.6). Sorting is by name (and by (upstream, ogate) for
// connections, by wid for workers, by (module, task) for tasks); queue counts default to one queue, since
// that is what the runtime resolves 0 to.
void Normalize(PipelineSpec *spec);

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_PIPELINE_SPEC_H_
