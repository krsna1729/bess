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

#ifndef BESS_CONTROL_PIPELINE_PLAN_H_
#define BESS_CONTROL_PIPELINE_PLAN_H_

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "control/pipeline_diff.h"
#include "control/pipeline_spec.h"

namespace bess {
namespace control {

// Explicit, inspectable plan operations (MODERNIZATION.md section 9.6). Typed
// operations rather than closures: they can be tested, logged, serialized and
// reasoned about during rollback.
struct CreatePortOp {
  PortSpec spec;
};
struct UpdatePortOp {
  PortSpec spec;
};
struct RemovePortOp {
  std::string name;
};
struct CreateModuleOp {
  ModuleSpec spec;
};
struct RemoveModuleOp {
  std::string name;
};
struct ConnectOp {
  ConnectionSpec connection;
};
struct DisconnectOp {
  DisconnectionSpec connection;
};
struct AddWorkerOp {
  WorkerSpec spec;
};
struct RemoveWorkerOp {
  int wid = -1;
};
struct CreateTcOp {
  TrafficClassSpec spec;
};
struct ReparentTcOp {
  TrafficClassSpec spec;
};
struct RemoveTcOp {
  std::string name;
};

using PlanOperation = std::variant<CreatePortOp, UpdatePortOp, RemovePortOp,
                                   CreateModuleOp, RemoveModuleOp, ConnectOp,
                                   DisconnectOp, AddWorkerOp, RemoveWorkerOp,
                                   CreateTcOp, ReparentTcOp, RemoveTcOp>;

// A dependency-ordered plan in three phases:
//
//   prepare -- reversible setup that can happen while workers run
//              (workers, then ports, then modules, since module Init may
//              resolve the ports it references)
//   commit  -- the structural transition, in dependency order
//              (disconnect old edges, connect new ones, create/reparent TCs;
//              module tasks must exist before leaf TCs attach to them, and
//              workers must exist before a scheduler root is attached)
//   retire  -- teardown, in reverse dependency order
//              (TCs detach before their modules go, modules release port
//              queues before ports are destroyed, workers last)
//
// Each phase is ordered deterministically (by name, or by wid for workers).
struct PipelinePlan {
  std::vector<PlanOperation> prepare_ops;
  std::vector<PlanOperation> commit_ops;
  std::vector<PlanOperation> retire_ops;

  bool empty() const {
    return prepare_ops.empty() && commit_ops.empty() && retire_ops.empty();
  }
};

// Turns a validated diff into a plan. Pure: it computes the operations and
// their order, and touches no runtime state.
PipelinePlan Plan(const PipelineDiff &diff);

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_PIPELINE_PLAN_H_
