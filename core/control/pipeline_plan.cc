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

#include "control/pipeline_plan.h"

#include <algorithm>
#include <string>
#include <vector>

namespace bess {
namespace control {

PipelinePlan Plan(const PipelineDiff &diff) {
  PipelinePlan plan;

  // -- prepare: stageable, reversible setup --------------------------------
  // Workers first: a traffic class may be attached to a worker that does not
  // exist yet. Then ports, then modules -- module Init resolves the ports it
  // references, so those must be in place.
  for (const WorkerChange &change : diff.workers) {
    if (change.kind == ChangeKind::kCreate ||
        change.kind == ChangeKind::kReplace) {
      plan.prepare_ops.push_back(AddWorkerOp{change.desired});
    }
  }

  for (const PortChange &change : diff.ports) {
    switch (change.kind) {
      case ChangeKind::kCreate:
      case ChangeKind::kReplace:
        plan.prepare_ops.push_back(CreatePortOp{change.desired});
        break;
      case ChangeKind::kUpdate:
        plan.prepare_ops.push_back(UpdatePortOp{change.desired});
        break;
      case ChangeKind::kRemove:
      case ChangeKind::kUnchanged:
        break;  // removal is a retire operation
    }
  }

  for (const ModuleChange &change : diff.modules) {
    if (change.kind == ChangeKind::kCreate ||
        change.kind == ChangeKind::kReplace) {
      plan.prepare_ops.push_back(CreateModuleOp{change.desired});
    }
  }

  // -- commit: the structural transition ------------------------------------
  // Disconnect first: a replace of the module at one end of an edge needs the
  // old edge gone, and an ogate can only carry one connection.
  for (const ConnectionChange &change : diff.connections) {
    if (change.kind == ChangeKind::kRemove) {
      plan.commit_ops.push_back(DisconnectOp{
          DisconnectionSpec{change.upstream, change.ogate}});
    }
  }

  for (const ConnectionChange &change : diff.connections) {
    if (change.kind == ChangeKind::kCreate) {
      ConnectionSpec connection;
      connection.upstream = change.upstream;
      connection.ogate = change.ogate;
      connection.downstream = change.downstream;
      connection.igate = change.igate;
      plan.commit_ops.push_back(ConnectOp{connection});
    }
  }

  // Modules that are replaced have to be rebuilt before anything connects to
  // them; that is expressed by the prepare phase, so here the remaining work is
  // traffic classes: a leaf class attaches to a module task that now exists.
  // Parent before child: a child attaches to a parent that must already
  // exist. `traffic_classes` is sorted by name, so this is a separate stable
  // sort by hierarchy depth.
  std::vector<const TrafficClassChange *> tc_creates;
  for (const TrafficClassChange &change : diff.traffic_classes) {
    if (change.kind == ChangeKind::kCreate) {
      tc_creates.push_back(&change);
    }
  }
  auto depth_of = [&diff](const std::string &name) {
    int depth = 0;
    std::string current = name;
    for (size_t guard = 0; guard < diff.traffic_classes.size() + 1; guard++) {
      const std::string *parent = nullptr;
      for (const TrafficClassChange &change : diff.traffic_classes) {
        if (change.name == current) {
          parent = &change.desired.parent;
          break;
        }
      }
      if (parent == nullptr || parent->empty()) {
        break;
      }
      depth++;
      current = *parent;
    }
    return depth;
  };
  std::stable_sort(tc_creates.begin(), tc_creates.end(),
                   [&depth_of](const TrafficClassChange *a,
                               const TrafficClassChange *b) {
                     return depth_of(a->name) < depth_of(b->name);
                   });

  for (const TrafficClassChange *change : tc_creates) {
    plan.commit_ops.push_back(CreateTcOp{change->desired});
  }

  for (const TrafficClassChange &change : diff.traffic_classes) {
    switch (change.kind) {
      case ChangeKind::kCreate:
        break;  // handled above, in hierarchy order
      case ChangeKind::kUpdate:
        plan.commit_ops.push_back(ReparentTcOp{change.desired});
        break;
      case ChangeKind::kReplace:
        // No in-place policy change: detach now, recreate in prepare-time
        // fashion is not possible for TCs, so this is an explicit
        // remove+create pair in the same commit phase, in that order.
        plan.commit_ops.push_back(RemoveTcOp{change.name});
        plan.commit_ops.push_back(CreateTcOp{change.desired});
        break;
      case ChangeKind::kRemove:
        break;  // retire
      case ChangeKind::kUnchanged:
        break;
    }
  }

  // -- retire: teardown, reverse dependency order ---------------------------
  // Children before parents, the reverse of how they were created.
  std::vector<const TrafficClassChange *> tc_removes;
  for (const TrafficClassChange &change : diff.traffic_classes) {
    if (change.kind == ChangeKind::kRemove) {
      tc_removes.push_back(&change);
    }
  }
  std::stable_sort(tc_removes.begin(), tc_removes.end(),
                   [&depth_of](const TrafficClassChange *a,
                               const TrafficClassChange *b) {
                     return depth_of(a->name) > depth_of(b->name);
                   });
  for (const TrafficClassChange *change : tc_removes) {
    plan.retire_ops.push_back(RemoveTcOp{change->name});
  }

  // Modules before ports: a module releases the port queues it acquired.
  for (const ModuleChange &change : diff.modules) {
    if (change.kind == ChangeKind::kRemove ||
        change.kind == ChangeKind::kReplace) {
      plan.retire_ops.push_back(RemoveModuleOp{change.name});
    }
  }

  for (const PortChange &change : diff.ports) {
    if (change.kind == ChangeKind::kRemove ||
        change.kind == ChangeKind::kReplace) {
      plan.retire_ops.push_back(RemovePortOp{change.name});
    }
  }

  // Workers last: their scheduler roots hold the traffic classes above.
  for (const WorkerChange &change : diff.workers) {
    if (change.kind == ChangeKind::kRemove ||
        change.kind == ChangeKind::kReplace) {
      plan.retire_ops.push_back(RemoveWorkerOp{change.wid});
    }
  }

  return plan;
}

}  // namespace control
}  // namespace bess
