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
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <glog/logging.h>

#include "control/control_plane.h"
#include "control/runtime_state.h"
#include "control/worker_manager.h"
#include "module.h"
#include "scheduler.h"
#include "worker.h"

namespace bess {
namespace control {

namespace {

// Test-only injection point; empty in production.
FailureInjector &Injector() {
  static FailureInjector injector;
  return injector;
}

std::optional<ControlError> MaybeInjectFailure(TransactionPhase phase,
                                             const PlanOperation &op) {
  FailureInjector &injector = Injector();
  if (!injector) {
    return std::nullopt;
  }
  return injector(phase, op);
}

uint64_t MicrosSince(std::chrono::steady_clock::time_point start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
}

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

void SetFailureInjector(FailureInjector injector) {
  Injector() = std::move(injector);
}

void ClearFailureInjector() {
  Injector() = nullptr;
}

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

  // Retirement is not undoable, so prove its preconditions now, while nothing
  // has changed: a plan whose retire phase could fail is refused instead of
  // committing and then discovering it cannot finish the job.
  std::set<std::string> removed_modules;
  for (const PlanOperation &op : plan.retire_ops) {
    if (const auto *remove = std::get_if<RemoveModuleOp>(&op)) {
      removed_modules.insert(remove->name);
    }
  }

  for (const PlanOperation &op : plan.retire_ops) {
    if (const auto *remove = std::get_if<RemovePortOp>(&op)) {
      const Port *port = state.ports().Find(remove->name);
      if (port == nullptr) {
        continue;  // already gone; nothing to retire
      }
      for (packet_dir_t dir : {PACKET_DIR_INC, PACKET_DIR_OUT}) {
        for (queue_t qid = 0; qid < port->num_queues[dir]; qid++) {
          const module *user = port->users[dir][qid];
          if (user == nullptr) {
            continue;
          }
          const auto *user_module = reinterpret_cast<const Module *>(user);
          if (removed_modules.count(user_module->name()) == 0) {
            return std::unexpected(Unsupported(
                "port", remove->name,
                "still in use by module '" + user_module->name() +
                    "', which this plan keeps"));
          }
        }
      }
    }

    if (const auto *remove = std::get_if<RemoveWorkerOp>(&op)) {
      const Worker *worker = state.workers().Get(remove->wid);
      if (worker == nullptr) {
        continue;  // already gone
      }
      const TrafficClass *root = worker->scheduler()->root();
      if (root == nullptr) {
        continue;
      }
      for (const auto &pair : state.traffic_classes().All()) {
        TrafficClass *c = pair.second.get();
        if (c->policy() != POLICY_LEAF || c->Root() != root) {
          continue;
        }
        const Module *owner =
            static_cast<LeafTrafficClass *>(c)->task()->module();
        if (removed_modules.count(owner->name()) == 0) {
          return std::unexpected(Unsupported(
              "worker", std::to_string(remove->wid),
              "still runs tasks of module '" + owner->name() +
                  "', which this plan keeps"));
        }
      }
    }
  }

  return {};
}

Transaction::Transaction(ControlPlane *plane, PipelinePlan plan,
                         PipelineSnapshot before)
    : plane_(plane), plan_(std::move(plan)), before_(std::move(before)) {}

ControlResult<void> Transaction::Prepare() {
  const auto start = std::chrono::steady_clock::now();

  for (const PlanOperation &op : plan_.prepare_ops) {
    if (auto injected = MaybeInjectFailure(TransactionPhase::kPrepare, op);
        injected.has_value()) {
      return std::unexpected(*injected);
    }
    if (auto result = ExecutePrepareOp(op); !result) {
      return std::unexpected(result.error());
    }
  }

  timing_.prepare_us = MicrosSince(start);
  return {};
}

ControlResult<void> Transaction::Commit() {
  quiescence_ = RequiredQuiescence(plan_);

  const auto start = std::chrono::steady_clock::now();

  std::optional<WorkerPauser> pauser;
  if (quiescence_ == Quiescence::kWorkers) {
    pauser.emplace();
  }

  for (const PlanOperation &op : plan_.commit_ops) {
    if (auto injected = MaybeInjectFailure(TransactionPhase::kCommit, op);
        injected.has_value()) {
      return std::unexpected(*injected);
    }
    if (auto result = ExecuteCommitOp(op); !result) {
      return std::unexpected(result.error());
    }
  }

  // The quiesced window ends when the pauser goes out of scope; measuring to
  // here is the closest honest number for "time spent with workers paused".
  timing_.paused_commit_us = MicrosSince(start);
  return {};
}

ControlResult<void> Transaction::Retire() {
  if (plan_.retire_ops.empty()) {
    return {};
  }

  const auto start = std::chrono::steady_clock::now();

  // Teardown destroys traffic classes, modules and ports the dataplane can
  // see, so it happens quiesced. It is not undoable by construction: by now the
  // new state is active, so a failure here is reported, not rolled back.
  WorkerPauser pauser;
  for (const PlanOperation &op : plan_.retire_ops) {
    if (auto injected = MaybeInjectFailure(TransactionPhase::kRetire, op);
        injected.has_value()) {
      return std::unexpected(*injected);
    }
    if (auto result = ExecuteRetireOp(op); !result) {
      return std::unexpected(result.error());
    }
  }

  timing_.retire_us = MicrosSince(start);
  return {};
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
        result = plane_->ReparentTcLocked(undo.tc);
        break;
      case Undo::Kind::kRestoreTcParams:
        result = plane_->UpdateTcParamsLocked(undo.tc);
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

  // Undoing the operations is not quite enough: leaving the quiesced window
  // attaches orphan traffic classes, and a scheduler that briefly held two
  // roots keeps a default round-robin wrapper for them. Collapse those, so a
  // failed transaction leaves no trace at all -- including in the structural
  // snapshot.
  runtime().workers().AdjustSchedulerDefaults();
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

    auto result = plane_->ReparentTcLocked(reparent->spec);
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

  if (const auto *params = std::get_if<UpdateTcParamsOp>(&op)) {
    const TrafficClassSnapshot *before = FindTc(before_, params->spec.name);

    auto result = plane_->UpdateTcParamsLocked(params->spec);
    if (!result) {
      return std::unexpected(result.error());
    }

    Undo undo{Undo::Kind::kRestoreTcParams, params->spec.name, {}, -1, {}};
    if (before != nullptr) {
      // Restore what the class had: same identity, previous parameters.
      undo.tc = params->spec;
      undo.tc.resource = before->resource;
      undo.tc.limit.clear();
      undo.tc.max_burst.clear();
      if (before->limit != 0) {
        undo.tc.limit[before->resource] = static_cast<int64_t>(before->limit);
      }
      if (before->max_burst != 0) {
        undo.tc.max_burst[before->resource] =
            static_cast<int64_t>(before->max_burst);
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

ControlResult<void> Transaction::ExecuteRetireOp(const PlanOperation &op) {
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
    return std::unexpected(Err(EINVAL, "unexpected retire operation"));
  }

  ops_executed_++;
  return result;
}

}  // namespace control
}  // namespace bess
