// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_CONNTRACK_H_
#define BESS_MODULES_CONNTRACK_H_

#include <array>
#include <memory>
#include <optional>
#include <string>

#include "conntrack/conntrack.h"
#include "conntrack/shared_conntrack.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "worker.h"

// Connection tracking (TP8, D-080): the conntrack library (M17) as a module,
// a stateful filter. igate 0 may start connections (the inside), igate 1 may
// not (the outside); ogate 0 gets packets of tracked connections (new,
// existing, related), ogate 1 the rest (invalid, untracked, table full).
//
// Modes (table_policy.md 5.3):
//   OWNED       one worker, one table.
//   PER_WORKER  one table per worker, no shared writes, no lock. Both
//               directions of a connection must reach the same worker: every
//               input must be a port queue with a symmetric hash, queue q of
//               each port on the same worker (checked before every resume).
//               Unverified, the module fails closed (everything to ogate 1),
//               or with fallback_shared tracks in SHARED mode.
//   SHARED      one table for every worker (SharedConntrack): lookups
//               lock-free, a per-connection lock for the state machine.
class ConnTrack final : public Module {
 public:
  static const gate_idx_t kNumIGates = 2;
  static const gate_idx_t kNumOGates = 2;

  static const Commands cmds;

  ConnTrack() : Module() { max_allowed_workers_ = 1; }

  CommandResponse Init(const bess::pb::ConnTrackArg &arg);
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  int OnEvent(bess::Event event) override;
  std::string GetDesc() const override;

 private:
  using Ct = bess::conntrack::Conntrack<>;
  using SharedCt = bess::conntrack::SharedConntrack<>;
  template <typename T>
  void Track(T &table, Context *ctx, bess::PacketBatch *batch);

  std::unique_ptr<Ct> MakeTable() const;

  bess::pb::ConnTrackArg::Mode mode_ = bess::pb::ConnTrackArg::OWNED;
  size_t capacity_ = 0;
  bess::conntrack::TimeoutPolicy policy_;
  // OWNED: tables_[0]. PER_WORKER: one per worker that runs the module,
  // allocated before resume (never on the packet path).
  std::array<std::unique_ptr<Ct>, Worker::kMaxWorkers> tables_;
  // SHARED, and PER_WORKER with fallback_shared.
  std::unique_ptr<SharedCt> shared_;
  bool fallback_shared_ = false;
  // PER_WORKER: why the inputs are not symmetric (fail closed, or shared_
  // with fallback_shared), or nullopt.
  std::optional<std::string> refused_;
};

#endif  // BESS_MODULES_CONNTRACK_H_
