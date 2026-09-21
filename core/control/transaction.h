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

#ifndef BESS_CONTROL_TRANSACTION_H_
#define BESS_CONTROL_TRANSACTION_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "control/control_error.h"
#include "control/pipeline_plan.h"
#include "control/pipeline_snapshot.h"
#include "control/pipeline_spec.h"

namespace bess {
namespace control {

class ControlPlane;

// Who has to be quiesced for a commit. The transaction engine decides; the
// client never calls PauseAll (MODERNIZATION.md section 9.7).
enum class Quiescence {
  kNone,
  kWorkers,
};

// What the planner can promise about undoing an operation. Correct refusal
// beats false atomicity (section 9.7).
enum class Reversibility {
  kReversible,
  kRequiresDestructiveCommit,
  kUnsupportedTransactionally,
};

struct ApplyOptions {
  // Optimistic concurrency: if set, the apply is rejected with a conflict
  // before any side effect when the active generation differs.
  std::optional<uint64_t> expected_generation;
};

// Where a transaction spent its time. Recorded so that pause duration is
// observable from the start (MODERNIZATION.md section 9.10); it is not an
// optimization target yet.
struct ApplyTiming {
  uint64_t validation_us = 0;
  uint64_t prepare_us = 0;
  uint64_t paused_commit_us = 0;
  uint64_t retire_us = 0;
};

struct ApplyResult {
  uint64_t generation = 0;   // generation after a successful apply
  size_t applied_ops = 0;    // operations executed (prepare + commit + retire)
  bool workers_paused = false;
  ApplyTiming timing;
};

enum class TransactionPhase {
  kPrepare,
  kCommit,
  kRetire,
};

// Test-only failure injection: when set, the engine asks before executing each
// operation and fails the transaction if an error comes back. Empty by default,
// so production behavior is unaffected -- there is deliberately no
// environment-variable switch.
using FailureInjector = std::function<std::optional<ControlError>(
    TransactionPhase phase, const PlanOperation &op)>;

void SetFailureInjector(FailureInjector injector);
void ClearFailureInjector();

// Transaction state machine (section 9.7):
//
//   Created -> Validated -> Prepared -> Committing -> Committed -> Retired
//
// A failure at any stage aborts: Prepared/Committing failures run Abort(), and
// a transaction that never committed leaves the previous runtime active.
//
// Prepare does the reversible setup, Commit performs the structural transition
// in the smallest possible quiesced window, Retire destroys what the new state
// replaced, and Abort undoes everything that was done.
class Transaction {
 public:
  Transaction(ControlPlane *plane, PipelinePlan plan, PipelineSnapshot before);

  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;

  // Reversible setup. Runs with workers running.
  ControlResult<void> Prepare();

  // Structural transition; the engine pauses workers only when the plan needs
  // it. On failure the transaction is aborted (see Abort()).
  ControlResult<void> Commit();

  // Destroys what the new state replaced. Not undoable -- by the time it runs
  // the new state is active -- so the planner proves its preconditions up front
  // (see CheckReversibility) and a failure here is a contract violation that
  // the caller must hear about rather than a step to ignore.
  ControlResult<void> Retire();

  // Undoes everything Prepare and Commit did, in reverse order.
  void Abort() noexcept;

  Quiescence quiescence() const { return quiescence_; }
  size_t ops_executed() const { return ops_executed_; }
  const ApplyTiming &timing() const { return timing_; }

 private:
  // One undoable step: how to put back what an executed operation changed.
  struct Undo {
    enum class Kind {
      kDestroyPort,
      kDestroyModule,
      kRemoveWorker,
      kReconnect,   // restore the connection that Disconnect removed
      kDisconnect,  // undo Connect
      kRemoveTc,        // undo a TC that this transaction created
      kReparentTc,      // put a TC back under its previous parent
      kRestoreTcParams, // put a TC's parameters back
    };

    Kind kind;
    std::string name;
    ConnectionSpec connection;
    int wid = -1;
    TrafficClassSpec tc;
  };

  ControlResult<void> ExecutePrepareOp(const PlanOperation &op);
  ControlResult<void> ExecuteCommitOp(const PlanOperation &op);
  ControlResult<void> ExecuteRetireOp(const PlanOperation &op);

  ControlPlane *plane_;
  PipelinePlan plan_;
  PipelineSnapshot before_;
  std::vector<Undo> undo_;
  Quiescence quiescence_ = Quiescence::kNone;
  size_t ops_executed_ = 0;
  ApplyTiming timing_;
};

// Decides whether a plan needs worker quiescence: anything that touches the
// graph or tears down objects the dataplane can see does; pure setup does not.
Quiescence RequiredQuiescence(const PipelinePlan &plan);

// Rejects plans the runtime cannot stage reversibly, before anything runs.
ControlResult<void> CheckReversibility(const PipelinePlan &plan);

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_TRANSACTION_H_
