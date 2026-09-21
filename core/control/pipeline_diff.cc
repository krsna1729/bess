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

#include "control/pipeline_diff.h"

#include <algorithm>
#include <map>
#include <string>

namespace bess {
namespace control {

namespace {

bool SameAny(const google::protobuf::Any &a, const google::protobuf::Any &b) {
  return a.SerializeAsString() == b.SerializeAsString();
}

// "0" in a desired port means "whatever the driver default is"; the runtime
// stores the resolved value, so a zero must match anything.
bool QueueMatches(uint64_t desired, uint64_t current) {
  return desired == 0 || desired == current;
}

// A class that hangs off a scheduler's internal default round-robin wrapper is
// a root as far as desired state is concerned: the wrapper is where the
// scheduler put it, not where the client asked for it.
const std::string &EffectiveParent(const TrafficClassSnapshot &tc) {
  static const std::string kNoParent;
  if (tc.parent.empty() || tc.parent[0] == '!') {
    return kNoParent;
  }
  return tc.parent;
}

// The legacy API carries rate-limit parameters as maps keyed by resource name.
uint64_t DesiredLimit(const TrafficClassSpec &spec) {
  auto it = spec.limit.find(spec.resource);
  return it == spec.limit.end() ? 0 : static_cast<uint64_t>(it->second);
}

uint64_t DesiredMaxBurst(const TrafficClassSpec &spec) {
  auto it = spec.max_burst.find(spec.resource);
  return it == spec.max_burst.end() ? 0 : static_cast<uint64_t>(it->second);
}

const PortSnapshot *FindPort(const PipelineSnapshot &snapshot,
                             const std::string &name) {
  for (const PortSnapshot &port : snapshot.ports) {
    if (port.name == name) {
      return &port;
    }
  }
  return nullptr;
}

const ModuleSnapshot *FindModule(const PipelineSnapshot &snapshot,
                                 const std::string &name) {
  for (const ModuleSnapshot &module : snapshot.modules) {
    if (module.name == name) {
      return &module;
    }
  }
  return nullptr;
}

bool SameConnection(const ConnectionSnapshot &current,
                    const ConnectionSpec &desired) {
  return current.upstream == desired.upstream && current.ogate == desired.ogate &&
         current.downstream == desired.downstream &&
         current.igate == desired.igate;
}

}  // namespace

PipelineDiff Diff(const PipelineSnapshot &current,
                  const PipelineSpec &desired) {
  PipelineDiff diff;

  // -- ports ----------------------------------------------------------------
  std::map<std::string, const PortSpec *> desired_ports;
  for (const PortSpec &port : desired.ports) {
    desired_ports[port.name] = &port;

    const PortSnapshot *active = FindPort(current, port.name);
    if (active == nullptr) {
      diff.ports.push_back(PortChange{port.name, ChangeKind::kCreate, port});
      continue;
    }

    if (active->driver != port.driver) {
      // A different driver is a different device, not a reconfiguration.
      diff.ports.push_back(PortChange{port.name, ChangeKind::kReplace, port});
      continue;
    }

    const bool same_shape =
        QueueMatches(port.num_rx_queues, active->num_rx_queues) &&
        QueueMatches(port.num_tx_queues, active->num_tx_queues) &&
        QueueMatches(port.rx_queue_size, active->rx_queue_size) &&
        QueueMatches(port.tx_queue_size, active->tx_queue_size);
    if (same_shape && SameAny(port.arg, active->driver_arg)) {
      diff.ports.push_back(
          PortChange{port.name, ChangeKind::kUnchanged, port});
    } else {
      diff.ports.push_back(PortChange{port.name, ChangeKind::kUpdate, port});
    }
  }

  for (const PortSnapshot &port : current.ports) {
    if (desired_ports.find(port.name) == desired_ports.end()) {
      diff.ports.push_back(PortChange{port.name, ChangeKind::kRemove, {}});
    }
  }

  // -- modules --------------------------------------------------------------
  std::map<std::string, const ModuleSpec *> desired_modules;
  for (const ModuleSpec &module : desired.modules) {
    desired_modules[module.name] = &module;

    const ModuleSnapshot *active = FindModule(current, module.name);
    if (active == nullptr) {
      diff.modules.push_back(
          ModuleChange{module.name, ChangeKind::kCreate, module});
    } else if (active->mclass != module.mclass ||
               !SameAny(module.arg, active->arg)) {
      // A module's argument is applied at construction; changing it means
      // building the module again.
      diff.modules.push_back(
          ModuleChange{module.name, ChangeKind::kReplace, module});
    } else {
      diff.modules.push_back(
          ModuleChange{module.name, ChangeKind::kUnchanged, module});
    }
  }

  for (const ModuleSnapshot &module : current.modules) {
    if (desired_modules.find(module.name) == desired_modules.end()) {
      diff.modules.push_back(ModuleChange{module.name, ChangeKind::kRemove, {}});
    }
  }

  // -- connections ----------------------------------------------------------
  for (const ConnectionSpec &connection : desired.connections) {
    bool present = false;
    for (const ConnectionSnapshot &active : current.connections) {
      if (SameConnection(active, connection)) {
        present = true;
        break;
      }
    }
    if (!present) {
      diff.connections.push_back(ConnectionChange{
          connection.upstream, connection.ogate, connection.downstream,
          connection.igate, ChangeKind::kCreate});
    }
  }

  for (const ConnectionSnapshot &active : current.connections) {
    bool wanted = false;
    for (const ConnectionSpec &connection : desired.connections) {
      if (SameConnection(active, connection)) {
        wanted = true;
        break;
      }
    }
    if (!wanted) {
      diff.connections.push_back(ConnectionChange{
          active.upstream, active.ogate, active.downstream, active.igate,
          ChangeKind::kRemove});
    }
  }

  // -- workers --------------------------------------------------------------
  std::map<int, const WorkerSpec *> desired_workers;
  for (const WorkerSpec &worker : desired.workers) {
    desired_workers[worker.wid] = &worker;

    const WorkerSnapshot *active = nullptr;
    for (const WorkerSnapshot &snapshot_worker : current.workers) {
      if (snapshot_worker.wid == worker.wid) {
        active = &snapshot_worker;
        break;
      }
    }

    if (active == nullptr) {
      diff.workers.push_back(
          WorkerChange{worker.wid, ChangeKind::kCreate, worker});
    } else if (active->core != worker.core ||
               active->scheduler != worker.scheduler) {
      diff.workers.push_back(
          WorkerChange{worker.wid, ChangeKind::kReplace, worker});
    } else {
      diff.workers.push_back(
          WorkerChange{worker.wid, ChangeKind::kUnchanged, worker});
    }
  }

  for (const WorkerSnapshot &active : current.workers) {
    if (desired_workers.find(active.wid) == desired_workers.end()) {
      diff.workers.push_back(WorkerChange{active.wid, ChangeKind::kRemove, {}});
    }
  }

  // -- traffic classes ------------------------------------------------------
  std::map<std::string, const TrafficClassSpec *> desired_tcs;
  for (const TrafficClassSpec &tc : desired.traffic_classes) {
    if (!tc.name.empty() && tc.name[0] == '!') {
      continue;  // internal, not desired state
    }
    desired_tcs[tc.name] = &tc;

    const TrafficClassSnapshot *active = nullptr;
    for (const TrafficClassSnapshot &snapshot_tc : current.traffic_classes) {
      if (snapshot_tc.name == tc.name) {
        active = &snapshot_tc;
        break;
      }
    }

    if (active == nullptr) {
      diff.traffic_classes.push_back(
          TrafficClassChange{tc.name, ChangeKind::kCreate, tc});
    } else if (active->policy != tc.policy) {
      // A different policy is a different class: it cannot be changed in place.
      diff.traffic_classes.push_back(
          TrafficClassChange{tc.name, ChangeKind::kReplace, tc});
    } else if (EffectiveParent(*active) != tc.parent ||
               active->has_priority != tc.has_priority ||
               active->priority != tc.priority ||
               active->has_share != tc.has_share ||
               active->share != tc.share) {
      // Attachment is reversible: detach and reattach.
      diff.traffic_classes.push_back(
          TrafficClassChange{tc.name, ChangeKind::kUpdate, tc});
    } else if (active->resource != tc.resource ||
               active->limit != DesiredLimit(tc) ||
               active->max_burst != DesiredMaxBurst(tc)) {
      // Parameters change in place, and are undone by restoring the old ones.
      diff.traffic_classes.push_back(
          TrafficClassChange{tc.name, ChangeKind::kUpdateParams, tc});
    } else {
      diff.traffic_classes.push_back(
          TrafficClassChange{tc.name, ChangeKind::kUnchanged, tc});
    }
  }

  for (const TrafficClassSnapshot &active : current.traffic_classes) {
    if (!active.name.empty() && active.name[0] == '!') {
      continue;  // module leaf classes and scheduler defaults
    }
    if (desired_tcs.find(active.name) == desired_tcs.end()) {
      diff.traffic_classes.push_back(
          TrafficClassChange{active.name, ChangeKind::kRemove, {}});
    }
  }

  // Drop the unchanged entries: the diff is the set of things to do.
  auto drop_unchanged = [](auto &changes) {
    changes.erase(std::remove_if(changes.begin(), changes.end(),
                                 [](const auto &change) {
                                   return change.kind == ChangeKind::kUnchanged;
                                 }),
                  changes.end());
  };
  drop_unchanged(diff.ports);
  drop_unchanged(diff.modules);
  drop_unchanged(diff.workers);
  drop_unchanged(diff.traffic_classes);

  // Stable order for every collection, independent of how the two sides were
  // assembled.
  std::sort(diff.ports.begin(), diff.ports.end(),
            [](const PortChange &a, const PortChange &b) { return a.name < b.name; });
  std::sort(diff.modules.begin(), diff.modules.end(),
            [](const ModuleChange &a, const ModuleChange &b) {
              return a.name < b.name;
            });
  std::sort(diff.connections.begin(), diff.connections.end(),
            [](const ConnectionChange &a, const ConnectionChange &b) {
              if (a.upstream != b.upstream) {
                return a.upstream < b.upstream;
              }
              return a.ogate < b.ogate;
            });
  std::sort(diff.workers.begin(), diff.workers.end(),
            [](const WorkerChange &a, const WorkerChange &b) {
              return a.wid < b.wid;
            });
  std::sort(diff.traffic_classes.begin(), diff.traffic_classes.end(),
            [](const TrafficClassChange &a, const TrafficClassChange &b) {
              return a.name < b.name;
            });

  return diff;
}

}  // namespace control
}  // namespace bess
