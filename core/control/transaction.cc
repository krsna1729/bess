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

#include "control/transaction.h"

#include <cerrno>
#include <memory>
#include <string>
#include <utility>

#include <glog/logging.h>

#include "control/control_plane.h"
#include "control/runtime_state.h"
#include "worker.h"

namespace bess {
namespace control {

namespace {

const ConnectionSnapshot *FindEdge(const PipelineSnapshot &snapshot,
                                   const std::string &upstream,
                                   gate_idx_t ogate) {
  for (const ConnectionSnapshot &edge : snapshot.connections) {
    if (edge.upstream == upstream && edge.ogate == ogate) {
      return &edge;
    }
  }
  return nullptr;
}

const TrafficClassSnapshot *FindTc(const PipelineSnapshot &snapshot,
                                   const std::string &name) {
  for (const TrafficClassSnapshot &tc : snapshot.traffic_classes) {
    if (tc.name == name) {
      return &tc;
    }
  }
  return nullptr;
}

ControlError Unsupported(const std::string &object, const std::string &name,
                         const std::string &what) {
  ControlError error =
      Err(EOPNOTSUPP, "%s '%s': %s", object.c_str(), name.c_str(),
          what.c_str());
  error.code = ControlErrorCode::kUnsupportedTransaction;
  error.object = object;
  error.field = "transaction";
  return error;
}

}  // namespace

Quiescence RequiredQuiescence(const PipelinePlan &plan) {
  // Setup alone can run while workers run; anything that rewires the graph or
  // tears down objects the dataplane can see cannot.
  if (!plan.commit_ops.empty() || !plan.retire_ops.empty()) {
    return Quiescence::kWorkers;
  }
  return Quiescence::kNone;
}

ControlResult<void> CheckReversibility(const PipelinePlan &plan) {
  const RuntimeState &state = runtime();

  for (const PlanOperation &op : plan.prepare_ops) {
    if (const auto *create = std::get_if<CreatePortOp>(&op)) {
      if (state.ports().Contains(create->spec.name)) {
        return std::unexpected(Unsupported(
            "port", create->spec.name,
            "transactional replacement is not supported (the device would have "
            "to be opened twice); remove it first"));
      }
    } else if (const auto *update = std::get_if<UpdatePortOp>(&op)) {
      return std::unexpected(Unsupported(
          "port", update->spec.name,
          "transactional reconfiguration is not supported (queue shape and "
          "driver arguments are fixed at creation); remove and recreate it"));
    } else if (const auto *create = std::get_if<CreateModuleOp>(&op)) {
      if (state.modules().Contains(create->spec.name)) {
        return std::unexpected(Unsupported(
            "module", create->spec.name,
            "transactional replacement is not supported (a module's argument is "
            "applied at construction); remove it first"));
      }
    }
  }

  for (const PlanOperation &op : plan.commit_ops) {
    if (const auto *create = std::get_if<CreateTcOp>(&op)) {
      if (state.traffic_classes().Contains(create->spec.name)) {
        return std::unexpected(Unsupported(
            "tc", create->spec.name,
            "transactional replacement is not supported (a traffic class "
            "cannot change policy in place); remove it first"));
      }
    }
  }

  return {};
}

Transaction::Transaction(ControlPlane *plane, PipelinePlan plan,
                         PipelineSnapshot before)
    : plane_(plane), plan_(std::move(plan)), before_(std::move(before)) {}

ControlResult<void> Transaction::Prepare() {
  for (const PlanOperation &op : plan_.prepare_ops) {
    if (auto result = ExecutePrepareOp(op); !result) {
      return std::unexpected(result.error());
    }
  }
  return {};
}

ControlResult<void> Transaction::Commit() {
  quiescence_ = RequiredQuiescence(plan_);

  std::optional<WorkerPauser> pauser;
  if (quiescence_ == Quiescence::kWorkers) {
    pauser.emplace();
  }

  for (const PlanOperation &op : plan_.commit_ops) {
    if (auto result = ExecuteCommitOp(op); !result) {
      return std::unexpected(result.error());
    }
  }

  return {};
}

void Transaction::Retire() {
  if (plan_.retire_ops.empty()) {
    return;
  }

  // Teardown destroys traffic classes, modules and ports the dataplane can
  // see, so it happens quiesced. It is not undoable by construction: by now the
  // new state is active, so a failure here is reported, not rolled back.
  WorkerPauser pauser;
  for (const PlanOperation &op : plan_.retire_ops) {
    ExecuteRetireOp(op);
  }
}

void Transaction::Abort() noexcept {
  for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) {
    const Undo &undo = *it;
    ControlResult<void> result;

    switch (undo.kind) {
      case Undo::Kind::kDestroyPort:
        result = plane_->DestroyPortLocked(undo.name);
        break;
      case Undo::Kind::kDestroyModule:
        result = plane_->DestroyModuleLocked(undo.name);
        break;
      case Undo::Kind::kRemoveWorker:
        result = plane_->DestroyWorkerLocked(undo.wid);
        break;
      case Undo::Kind::kReconnect:
        result = plane_->ConnectModulesLocked(undo.connection);
        break;
      case Undo::Kind::kDisconnect:
        result = plane_->DisconnectModulesLocked({undo.name, undo.connection.ogate});
        break;
      case Undo::Kind::kRemoveTc:
        result = plane_->RemoveTcLocked(undo.name);
        break;
      case Undo::Kind::kReparentTc:
        result = plane_->UpdateTcParentLocked(undo.tc);
        break;
    }

    if (!result) {
      // Never pretend the rollback succeeded: this is the one case where the
      // runtime may have been left in a state the transaction did not intend.
      LOG(ERROR) << "transaction rollback step failed for " << undo.name << ": "
                 << result.error().message;
    }
  }

  undo_.clear();
}

ControlResult<void> Transaction::ExecutePrepareOp(const PlanOperation &op) {
  if (const auto *create = std::get_if<CreatePortOp>(&op)) {
    auto result = plane_->CreatePortLocked(create->spec);
    if (!result) {
      return std::unexpected(result.error());
    }
    undo_.push_back(Undo{Undo::Kind::kDestroyPort, create->spec.name, {}, -1,
                         {}});
    ops_executed_++;
    return {};
  }

  if (const auto *create = std::get_if<CreateModuleOp>(&op)) {
    auto result = plane_->CreateModuleLocked(create->spec);
    if (!result) {
      return std::unexpected(result.error());
    }
    undo_.push_back(Undo{Undo::Kind::kDestroyModule, create->spec.name, {}, -1,
                         {}});
    ops_executed_++;
    return {};
  }

  if (const auto *worker = std::get_if<AddWorkerOp>(&op)) {
    auto result = plane_->AddWorkerLocked(worker->spec.wid, worker->spec.core,
                                          worker->spec.scheduler);
    if (!result) {
      return std::unexpected(result.error());
    }
    undo_.push_back(Undo{Undo::Kind::kRemoveWorker, "", {}, worker->spec.wid,
                         {}});
    ops_executed_++;
    return {};
  }

  return std::unexpected(Err(EINVAL, "unexpected prepare operation"));
}

ControlResult<void> Transaction::ExecuteCommitOp(const PlanOperation &op) {
  if (const auto *disconnect = std::get_if<DisconnectOp>(&op)) {
    const ConnectionSnapshot *edge =
        FindEdge(before_, disconnect->connection.name,
                 disconnect->connection.ogate);

    auto result = plane_->DisconnectModulesLocked(disconnect->connection);
    if (!result) {
      return std::unexpected(result.error());
    }

    Undo undo{Undo::Kind::kReconnect, disconnect->connection.name, {}, -1,
              {}};
    if (edge != nullptr) {
      undo.connection.upstream = edge->upstream;
      undo.connection.ogate = edge->ogate;
      undo.connection.downstream = edge->downstream;
      undo.connection.igate = edge->igate;
    }
    undo_.push_back(undo);
    ops_executed_++;
    return {};
  }

  if (const auto *connect = std::get_if<ConnectOp>(&op)) {
    auto result = plane_->ConnectModulesLocked(connect->connection);
    if (!result) {
      return std::unexpected(result.error());
    }
    undo_.push_back(Undo{Undo::Kind::kDisconnect, connect->connection.upstream,
                         connect->connection, -1, {}});
    ops_executed_++;
    return {};
  }

  if (const auto *create = std::get_if<CreateTcOp>(&op)) {
    auto result = plane_->AddTcLocked(create->spec);
    if (!result) {
      return std::unexpected(result.error());
    }
    undo_.push_back(Undo{Undo::Kind::kRemoveTc, create->spec.name, {}, -1,
                         {}});
    ops_executed_++;
    return {};
  }

  if (const auto *reparent = std::get_if<ReparentTcOp>(&op)) {
    const TrafficClassSnapshot *before = FindTc(before_, reparent->spec.name);

    auto result = plane_->UpdateTcParentLocked(reparent->spec);
    if (!result) {
      return std::unexpected(result.error());
    }

    Undo undo{Undo::Kind::kReparentTc, reparent->spec.name, {}, -1, {}};
    if (before != nullptr) {
      undo.tc = reparent->spec;
      undo.tc.parent = before->parent;
      if (before->parent.empty()) {
        undo.tc.wid = before->wid;
      }
    }
    undo_.push_back(undo);
    ops_executed_++;
    return {};
  }

  if (std::get_if<RemoveTcOp>(&op)) {
    // A policy change is expressed as remove+create; the remove half is
    // destructive, which is why CheckReversibility refuses such plans.
    return std::unexpected(Unsupported("tc", "", "destructive commit"));
  }

  return std::unexpected(Err(EINVAL, "unexpected commit operation"));
}

void Transaction::ExecuteRetireOp(const PlanOperation &op) {
  ControlResult<void> result;

  if (const auto *remove = std::get_if<RemoveTcOp>(&op)) {
    result = plane_->RemoveTcLocked(remove->name);
  } else if (const auto *remove = std::get_if<RemoveModuleOp>(&op)) {
    result = plane_->DestroyModuleLocked(remove->name);
  } else if (const auto *remove = std::get_if<RemovePortOp>(&op)) {
    result = plane_->DestroyPortLocked(remove->name);
  } else if (const auto *remove = std::get_if<RemoveWorkerOp>(&op)) {
    result = plane_->DestroyWorkerLocked(remove->wid);
  } else {
    LOG(ERROR) << "unexpected retire operation";
    return;
  }

  if (!result) {
    // Retirement happens after the transition succeeded; report, do not
    // pretend the transaction failed (it did not).
    LOG(ERROR) << "retire step failed: " << result.error().message;
  }
  ops_executed_++;
}

}  // namespace control
}  // namespace bess
