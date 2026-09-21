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

#include "control/pipeline_validator.h"

#include <cerrno>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "module.h"
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

ControlResult<ValidatedPipeline> ValidatePipeline(const RuntimeState &runtime,
                                                  const PipelineSpec &desired) {
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

    if (worker.core < 0 ||
        !is_cpu_present(static_cast<unsigned>(worker.core))) {
      return std::unexpected(Invalid(
          "worker", "core",
          "worker " + std::to_string(worker.wid) + ": CPU " +
              std::to_string(worker.core) + " is not present"));
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
              "' (leaf classes belong to modules, not to desired state)"));
    }

    if (tc.policy == TrafficPolicyName[POLICY_WEIGHTED_FAIR] ||
        tc.policy == TrafficPolicyName[POLICY_RATE_LIMIT]) {
      if (ResourceMap.count(tc.resource) == 0) {
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

    if (!tc.leaf_module_name.empty()) {
      if (module_names.count(tc.leaf_module_name) == 0) {
        return std::unexpected(
            NotFound("tc", "leaf_module_name",
                     "traffic class '" + tc.name + "': no module '" +
                         tc.leaf_module_name + "'"));
      }
      if (tc.leaf_module_taskid >= MAX_TASKS_PER_MODULE) {
        return std::unexpected(Invalid(
            "tc", "leaf_module_taskid",
            "traffic class '" + tc.name + "': 'taskid' must be between 0 and " +
                std::to_string(MAX_TASKS_PER_MODULE - 1)));
      }
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
