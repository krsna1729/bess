// SPDX-License-Identifier: BSD-3-Clause

#include "control/pipeline_validator.h"
#include "runtime/thread_placement.h"
#include <cerrno>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "module.h"
#include <limits>

#include "port.h"
#include "traffic_class.h"
#include "worker.h"

namespace bess {
namespace control {

namespace {

ControlError Invalid(const std::string &object, const std::string &field,
                     const std::string &message) {
  ControlError error = Err(EINVAL, "%s", message.c_str());
  error.object = object;
  error.field = field;
  return error;
}

ControlError NotFound(const std::string &object, const std::string &field,
                      const std::string &message) {
  ControlError error = Err(ENOENT, "%s", message.c_str());
  error.object = object;
  error.field = field;
  return error;
}

ControlError Duplicate(const std::string &object, const std::string &field,
                       const std::string &message) {
  ControlError error = Err(EEXIST, "%s", message.c_str());
  error.object = object;
  error.field = field;
  return error;
}

bool IsSupportedPolicy(const std::string &policy) {
  return policy == TrafficPolicyName[POLICY_PRIORITY] ||
         policy == TrafficPolicyName[POLICY_WEIGHTED_FAIR] ||
         policy == TrafficPolicyName[POLICY_ROUND_ROBIN] ||
         policy == TrafficPolicyName[POLICY_RATE_LIMIT];
}

const char *ParentOf(const PipelineSpec &spec, const std::string &name) {
  for (const TrafficClassSpec &tc : spec.traffic_classes) {
    if (tc.name == name) {
      return tc.parent.c_str();
    }
  }
  return nullptr;
}

}  // namespace

ControlResult<ValidatedPipeline> ValidatePipeline(
    const runtime::RuntimeState &runtime, const PipelineSpec &desired) {
  // Only type registries and the CPU topology are consulted; the runtime's
  // instance registries are deliberately not touched, so validation cannot
  // have side effects on the active pipeline.
  (void)runtime;

  PipelineSpec spec = desired;

  // -- ports ----------------------------------------------------------------
  std::set<std::string> port_names;
  for (const PortSpec &port : spec.ports) {
    if (port.name.empty()) {
      return std::unexpected(
          Invalid("port", "name", "port: 'name' must not be empty"));
    }
    if (!port_names.insert(port.name).second) {
      return std::unexpected(Duplicate("port", "name",
                                       "port '" + port.name +
                                           "' is defined twice"));
    }

    const auto &drivers = PortBuilder::all_port_builders();
    if (drivers.find(port.driver) == drivers.end()) {
      return std::unexpected(
          NotFound("port", "driver", "port '" + port.name + "': no port driver '" +
                                          port.driver + "'"));
    }

    if (port.num_rx_queues > MAX_QUEUES_PER_DIR ||
        port.num_tx_queues > MAX_QUEUES_PER_DIR) {
      return std::unexpected(Invalid(
          "port", "queues",
          "port '" + port.name + "': more than " +
              std::to_string(MAX_QUEUES_PER_DIR) + " queues in a direction"));
    }

    if (port.rx_queue_size > MAX_QUEUE_SIZE ||
        port.tx_queue_size > MAX_QUEUE_SIZE) {
      return std::unexpected(Invalid(
          "port", "queue_size", "port '" + port.name + "': queue size above " +
                                    std::to_string(MAX_QUEUE_SIZE)));
    }
  }

  // -- modules --------------------------------------------------------------
  std::set<std::string> module_names;
  std::unordered_map<std::string, const ModuleBuilder *> builders;
  for (const ModuleSpec &module : spec.modules) {
    if (module.name.empty()) {
      return std::unexpected(
          Invalid("module", "name", "module: 'name' must not be empty"));
    }
    if (!module_names.insert(module.name).second) {
      return std::unexpected(Duplicate(
          "module", "name", "module '" + module.name + "' is defined twice"));
    }

    const auto &all_builders = ModuleBuilder::all_module_builders();
    const auto &it = all_builders.find(module.mclass);
    if (it == all_builders.end()) {
      return std::unexpected(NotFound(
          "module", "mclass",
          "module '" + module.name + "': no mclass '" + module.mclass + "'"));
    }
    builders[module.name] = &it->second;
  }

  // -- connections ----------------------------------------------------------
  std::set<std::pair<std::string, gate_idx_t>> connected_ogates;
  for (const ConnectionSpec &connection : spec.connections) {
    const auto up = builders.find(connection.upstream);
    if (up == builders.end()) {
      return std::unexpected(
          NotFound("connection", "upstream",
                   "connection: no module '" + connection.upstream + "'"));
    }
    const auto down = builders.find(connection.downstream);
    if (down == builders.end()) {
      return std::unexpected(
          NotFound("connection", "downstream",
                   "connection: no module '" + connection.downstream + "'"));
    }

    if (connection.ogate >= up->second->NumOGates() ||
        connection.ogate >= MAX_GATES) {
      return std::unexpected(Invalid(
          "connection", "ogate",
          "connection " + connection.upstream + ":" +
              std::to_string(connection.ogate) +
              ": output gate does not exist"));
    }
    if (connection.igate >= down->second->NumIGates() ||
        connection.igate >= MAX_GATES) {
      return std::unexpected(Invalid(
          "connection", "igate",
          "connection " + connection.downstream + ":" +
              std::to_string(connection.igate) +
              ": input gate does not exist"));
    }

    if (!connected_ogates.insert({connection.upstream, connection.ogate})
             .second) {
      return std::unexpected(Duplicate(
          "connection", "ogate",
          "connection: output gate " + connection.upstream + ":" +
              std::to_string(connection.ogate) + " is connected twice"));
    }
  }

  // -- workers --------------------------------------------------------------
  std::set<int> worker_ids;
  std::set<int> worker_cores;
  for (const WorkerSpec &worker : spec.workers) {
    if (worker.wid < 0 || worker.wid >= Worker::kMaxWorkers) {
      return std::unexpected(
          Invalid("worker", "wid", "worker: 'wid' must be between 0 and " +
                                       std::to_string(Worker::kMaxWorkers - 1)));
    }
    if (!worker_ids.insert(worker.wid).second) {
      return std::unexpected(Duplicate("worker", "wid",
                                       "worker " + std::to_string(worker.wid) +
                                           " is defined twice"));
    }

    const std::optional<runtime::CpuId> cpu = runtime::CpuIdFrom(worker.core);
    if (!cpu || !runtime::CpuPresent(*cpu)) {
      return std::unexpected(Invalid(
          "worker", "core",
          "worker " + std::to_string(worker.wid) + ": CPU " +
              std::to_string(worker.core) + " is not present"));
    }
    if (!runtime::CpuAllowed(*cpu)) {
      return std::unexpected(Invalid(
          "worker", "core",
          "worker " + std::to_string(worker.wid) + ": CPU " +
              std::to_string(worker.core) + " is not in bessd's CPU set (" +
              runtime::CpuList(runtime::ProcessCpus()) + ")"));
    }
    if (!worker_cores.insert(worker.core).second) {
      return std::unexpected(Invalid(
          "worker", "core",
          "worker " + std::to_string(worker.wid) + ": CPU " +
              std::to_string(worker.core) + " is used by another worker"));
    }

    if (worker.scheduler != "" && worker.scheduler != "experimental") {
      return std::unexpected(Invalid(
          "worker", "scheduler",
          "worker " + std::to_string(worker.wid) + ": unknown scheduler '" +
              worker.scheduler + "'"));
    }
  }

  // -- traffic classes ------------------------------------------------------
  std::set<std::string> tc_names;
  for (const TrafficClassSpec &tc : spec.traffic_classes) {
    if (tc.name.empty()) {
      return std::unexpected(
          Invalid("tc", "name", "traffic class: 'name' must not be empty"));
    }
    if (tc.name[0] == '!') {
      return std::unexpected(
          Invalid("tc", "name", "traffic class '" + tc.name +
                                    "': names starting with '!' are reserved"));
    }
    if (!tc_names.insert(tc.name).second) {
      return std::unexpected(Duplicate(
          "tc", "name", "traffic class '" + tc.name + "' is defined twice"));
    }

    if (!IsSupportedPolicy(tc.policy)) {
      return std::unexpected(Invalid(
          "tc", "policy",
          "traffic class '" + tc.name + "': invalid policy '" + tc.policy +
              "' (a module's own leaf class is placed with 'tasks')"));
    }

    if (tc.policy == TrafficPolicyName[POLICY_WEIGHTED_FAIR] ||
        tc.policy == TrafficPolicyName[POLICY_RATE_LIMIT]) {
      if (!ResourceFromName(tc.resource)) {
        return std::unexpected(Invalid(
            "tc", "resource",
            "traffic class '" + tc.name + "': invalid resource '" +
                tc.resource + "'"));
      }
    }

    if (tc.parent.empty()) {
      if (tc.wid != Worker::kAnyWorker &&
          (tc.wid < 0 || tc.wid >= Worker::kMaxWorkers)) {
        return std::unexpected(Invalid(
            "tc", "wid",
            "traffic class '" + tc.name + "': 'wid' must be " +
                std::to_string(Worker::kAnyWorker) + " or between 0 and " +
                std::to_string(Worker::kMaxWorkers - 1)));
      }
    } else if (tc_names.count(tc.parent) == 0) {
      // Parents may be defined later in the same spec, so accept anything the
      // spec defines at all and reject only names it never mentions.
      bool defined = false;
      for (const TrafficClassSpec &other : spec.traffic_classes) {
        if (other.name == tc.parent) {
          defined = true;
          break;
        }
      }
      if (!defined) {
        return std::unexpected(
            NotFound("tc", "parent", "traffic class '" + tc.name +
                                         "': no parent '" + tc.parent + "'"));
      }
    }

    // The spec carries these as int64 (the wire type); the scheduler takes
    // narrower types. Out-of-range values are refused here, not narrowed.
    if (tc.has_priority &&
        (tc.priority < 0 ||
         tc.priority > std::numeric_limits<bess::priority_t>::max())) {
      return std::unexpected(Invalid(
          "tc", "priority",
          "traffic class '" + tc.name + "': priority " +
              std::to_string(tc.priority) + " is out of range (0 to " +
              std::to_string(std::numeric_limits<bess::priority_t>::max()) +
              ")"));
    }
    if (tc.has_share &&
        (tc.share < 1 ||
         tc.share > std::numeric_limits<bess::resource_share_t>::max())) {
      return std::unexpected(Invalid(
          "tc", "share",
          "traffic class '" + tc.name + "': share " +
              std::to_string(tc.share) + " is out of range (1 to " +
              std::to_string(
                  std::numeric_limits<bess::resource_share_t>::max()) +
              ")"));
    }
    for (const auto *map : {&tc.limit, &tc.max_burst}) {
      for (const auto &[resource, value] : *map) {
        if (value < 0) {
          return std::unexpected(Invalid(
              "tc", map == &tc.limit ? "limit" : "max_burst",
              "traffic class '" + tc.name + "': " +
                  (map == &tc.limit ? "limit" : "max_burst") + " for '" +
                  resource + "' must not be negative"));
        }
      }
    }
  }

  // -- task placements --------------------------------------------------------
  std::set<std::pair<std::string, uint64_t>> placed;
  for (const TaskPlacementSpec &task : spec.tasks) {
    const std::string what =
        "task '" + task.module + ":" + std::to_string(task.task) + "'";
    if (module_names.count(task.module) == 0) {
      return std::unexpected(
          NotFound("task", "module", what + ": no module '" + task.module + "'"));
    }
    if (task.task >= MAX_TASKS_PER_MODULE) {
      return std::unexpected(Invalid(
          "task", "task",
          what + ": the task index must be below " +
              std::to_string(MAX_TASKS_PER_MODULE)));
    }
    if (!placed.insert({task.module, task.task}).second) {
      return std::unexpected(
          Duplicate("task", "task", what + " is placed twice"));
    }
    if (task.parent.empty()) {
      if (task.wid < 0 || task.wid >= Worker::kMaxWorkers) {
        return std::unexpected(Invalid(
            "task", "wid",
            what + ": needs a parent or a 'wid' between 0 and " +
                std::to_string(Worker::kMaxWorkers - 1)));
      }
      if (task.has_priority || task.has_share) {
        return std::unexpected(Invalid(
            "task", "parent",
            what + ": a priority or share needs a parent"));
      }
    } else {
      if (task.wid != Worker::kAnyWorker) {
        return std::unexpected(Invalid(
            "task", "wid", what + ": give a parent or a 'wid', not both"));
      }
      if (tc_names.count(task.parent) == 0) {
        return std::unexpected(NotFound(
            "task", "parent", what + ": no traffic class '" + task.parent + "'"));
      }
    }
    if (task.has_priority &&
        (task.priority < 0 ||
         task.priority > std::numeric_limits<bess::priority_t>::max())) {
      return std::unexpected(Invalid(
          "task", "priority",
          what + ": priority " + std::to_string(task.priority) +
              " is out of range (0 to " +
              std::to_string(std::numeric_limits<bess::priority_t>::max()) +
              ")"));
    }
    if (task.has_share &&
        (task.share < 1 ||
         task.share > std::numeric_limits<bess::resource_share_t>::max())) {
      return std::unexpected(Invalid(
          "task", "share",
          what + ": share " + std::to_string(task.share) +
              " is out of range (1 to " +
              std::to_string(
                  std::numeric_limits<bess::resource_share_t>::max()) +
              ")"));
    }
  }

  // Attachment constraints: what a parent's policy requires of a child, a
  // traffic class or a task. These are the constraints the attach path
  // enforces at commit time; catching them here means an impossible desired
  // tree is refused before anything moves.
  std::map<std::string, const TrafficClassSpec *> by_name;
  for (const TrafficClassSpec &tc : spec.traffic_classes) {
    by_name[tc.name] = &tc;
  }

  struct Child {
    const char *object;  // "tc" or "task"
    std::string what;
    const std::string &parent;
    bool has_priority;
    int64_t priority;
    bool has_share;
    int64_t share;
  };
  std::vector<Child> children;
  children.reserve(spec.traffic_classes.size() + spec.tasks.size());
  for (const TrafficClassSpec &tc : spec.traffic_classes) {
    children.push_back({"tc", "traffic class '" + tc.name + "'", tc.parent,
                        tc.has_priority, tc.priority, tc.has_share, tc.share});
  }
  for (const TaskPlacementSpec &task : spec.tasks) {
    children.push_back(
        {"task", "task '" + task.module + ":" + std::to_string(task.task) + "'",
         task.parent, task.has_priority, task.priority, task.has_share,
         task.share});
  }

  std::set<std::pair<std::string, int64_t>> priority_slots;
  std::map<std::string, size_t> children_per_parent;

  for (const Child &child : children) {
    if (child.parent.empty()) {
      continue;
    }

    const auto parent_it = by_name.find(child.parent);
    if (parent_it == by_name.end()) {
      continue;  // existence is reported above
    }
    const std::string &parent_policy = parent_it->second->policy;

    if (parent_policy == TrafficPolicyName[POLICY_PRIORITY]) {
      if (!child.has_priority) {
        return std::unexpected(
            Invalid(child.object, "priority",
                    child.what + ": a child of priority class '" +
                        child.parent + "' must specify a priority"));
      }
      if (child.priority == static_cast<int64_t>(DEFAULT_PRIORITY)) {
        return std::unexpected(
            Invalid(child.object, "priority",
                    child.what + ": priority " +
                        std::to_string(DEFAULT_PRIORITY) + " is reserved"));
      }
      if (!priority_slots.insert({child.parent, child.priority}).second) {
        return std::unexpected(Invalid(
            child.object, "priority",
            child.what + ": priority " + std::to_string(child.priority) +
                " is already taken under '" + child.parent + "'"));
      }
      if (child.has_share) {
        return std::unexpected(Invalid(
            child.object, "share",
            child.what +
                ": a share is only meaningful under a weighted_fair parent"));
      }
    } else if (parent_policy == TrafficPolicyName[POLICY_WEIGHTED_FAIR]) {
      if (!child.has_share) {
        return std::unexpected(
            Invalid(child.object, "share",
                    child.what + ": a child of weighted_fair class '" +
                        child.parent + "' must specify a share"));
      }
      if (child.share <= 0) {
        return std::unexpected(Invalid(
            child.object, "share",
            child.what + ": share must be greater than zero"));
      }
      if (child.has_priority) {
        return std::unexpected(
            Invalid(child.object, "priority",
                    child.what + ": a priority is only meaningful under a "
                                 "priority parent"));
      }
    } else {
      // round_robin and rate_limit take no attachment parameter.
      if (child.has_priority || child.has_share) {
        return std::unexpected(Invalid(
            child.object, "parent",
            child.what + ": parent '" + child.parent + "' (" + parent_policy +
                ") takes no attachment parameter"));
      }
    }

    children_per_parent[child.parent]++;
  }

  for (const auto &entry : children_per_parent) {
    const TrafficClassSpec *parent = by_name.at(entry.first);
    if (parent->policy == TrafficPolicyName[POLICY_RATE_LIMIT] &&
        entry.second > 1) {
      return std::unexpected(Invalid(
          "tc", "parent",
          "rate_limit class '" + entry.first + "' takes a single child, but " +
              std::to_string(entry.second) + " are defined"));
    }
  }

  // Parent cycles make a hierarchy unattachable: walk each parent chain.
  for (const TrafficClassSpec &tc : spec.traffic_classes) {
    std::unordered_set<std::string> seen;
    std::string current = tc.name;
    while (!current.empty()) {
      if (!seen.insert(current).second) {
        return std::unexpected(
            Invalid("tc", "parent", "traffic class '" + tc.name +
                                        "': parent chain contains a cycle"));
      }
      const char *parent = ParentOf(spec, current);
      current = parent != nullptr ? parent : "";
    }
  }

  Normalize(&spec);

  ValidatedPipeline validated;
  validated.spec = std::move(spec);
  return validated;
}

}  // namespace control
}  // namespace bess
