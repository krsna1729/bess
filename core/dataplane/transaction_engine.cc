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

#include "dataplane/transaction_engine.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <utility>

#include <glog/logging.h>

namespace bess {
namespace dataplane {

namespace internal {
void (*g_publish_window_hook)(bool entering) = nullptr;
}  // namespace internal

namespace {

std::string Describe(const std::string &resource, const ResourceKey &key) {
  std::string out = resource + "/";
  char byte[4];
  for (unsigned char c : key) {
    std::snprintf(byte, sizeof(byte), "%02x", c);
    out += byte;
  }
  return out;
}

// Runs `fn` on scope exit unless dismissed (the scope_fail idiom): the abort
// path of a transaction cannot be skipped by an early return.
template <typename Fn>
class AbortGuard {
 public:
  explicit AbortGuard(Fn fn) : fn_(std::move(fn)) {}
  ~AbortGuard() {
    if (armed_) {
      fn_();
    }
  }
  AbortGuard(const AbortGuard &) = delete;
  AbortGuard &operator=(const AbortGuard &) = delete;
  void Dismiss() { armed_ = false; }

 private:
  Fn fn_;
  bool armed_ = true;
};

}  // namespace

std::expected<void, std::string> TransactionEngine::Register(
    Resource *resource) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (resources_.contains(resource->name())) {
    return std::unexpected("resource '" + resource->name() +
                           "' is already registered");
  }
  int rank = 0;
  for (const std::string &dep : resource->declared_references()) {
    if (dep == resource->name()) {
      return std::unexpected("resource '" + dep + "' cannot reference itself");
    }
    auto it = resources_.find(dep);
    if (it == resources_.end()) {
      return std::unexpected("resource '" + resource->name() +
                             "' references '" + dep +
                             "', which is not registered (register it first)");
    }
    rank = std::max(rank, it->second->rank() + 1);
  }
  if (!resource->declared_references().empty() && resource->LiveCount() != 0) {
    return std::unexpected("resource '" + resource->name() +
                           "' is populated and may reference others: its "
                           "existing references are unknown to the engine");
  }
  resource->rank_ = rank;
  resources_.emplace(resource->name(), resource);
  outstanding_.emplace(resource, std::make_unique<std::atomic<size_t>>(0));
  return {};
}

std::expected<void, std::string> TransactionEngine::Unregister(
    const std::string &name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto res = resources_.find(name);
  if (res == resources_.end()) {
    return std::unexpected("resource '" + name + "' is not registered");
  }
  for (const auto &[other_name, other] : resources_) {
    for (const std::string &dep : other->declared_references()) {
      if (dep == name) {
        return std::unexpected("resource '" + other_name +
                               "' may reference '" + name +
                               "' (unregister it first)");
      }
    }
  }
  if (res->second->LiveCount() != 0) {
    return std::unexpected("resource '" + name + "' still has " +
                           std::to_string(res->second->LiveCount()) +
                           " live key(s)");
  }
  auto it = references_.lower_bound(Reference{name, {}});
  if (it != references_.end() && it->first.resource == name) {
    return std::unexpected("keys of '" + name + "' are still referenced");
  }
  // Pending removal steps capture this resource's tables: unregistering
  // (after which the module may destroy them) must wait until they ran.
  ReclaimRetiredLocked();
  if (pending_removals_.contains(res->second)) {
    return std::unexpected("removals of '" + name +
                           "' are still waiting for readers");
  }
  // Objects of this resource already handed to RCU are destroyed by code in
  // the resource's module (the deleter it instantiated); it must not go
  // before the last of them has run.
  domain_.ReclaimReady();
  std::atomic<size_t> &outstanding = *outstanding_.at(res->second);
  if (const size_t left = outstanding.load(std::memory_order_acquire);
      left != 0) {
    return std::unexpected(std::to_string(left) + " retired object(s) of '" +
                           name + "' are still waiting for readers");
  }
  outstanding_.erase(res->second);
  resources_.erase(res);
  return {};
}

uint64_t TransactionEngine::generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}

size_t TransactionEngine::ReferenceCount(const std::string &resource,
                                         const ResourceKey &key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = references_.find(Reference{resource, key});
  return it == references_.end() ? 0 : it->second;
}

size_t TransactionEngine::ReclaimRetired() {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReclaimRetiredLocked();
}

size_t TransactionEngine::ReclaimRetiredLocked() {
  const size_t retire_budget = domain_.retire_high_water() / 2;
  size_t kept = 0;
  for (size_t c = 0; c < cascades_.size(); c++) {
    Cascade &cascade = cascades_[c];
    while (cascade.next < cascade.stages.size() &&
           domain_.IsComplete(cascade.token)) {
      std::vector<Step> &stage = cascade.stages[cascade.next];
      // Each step retires at most one object. Run a stage only if that keeps
      // the RCU queue within budget, so reclamation never pushes it to the
      // point where RetireErased() waits for readers under this lock; a
      // stage that does not fit waits for readers to drain the queue.
      if (domain_.pending_retired() + stage.size() > retire_budget) {
        break;
      }
      domain_.ReserveRetirements(stage.size());
      Retirer retirer;
      retirer.retire_.reserve(stage.size());
      for (Step &step : stage) {
        retirer.owner_ = step.owner;
        retirer.outstanding_ = outstanding_.at(step.owner).get();
        step.fn(retirer);
        deferred_objects_--;
        if (--pending_removals_[step.owner] == 0) {
          pending_removals_.erase(step.owner);
        }
      }
      cascade.next++;
      // The next stage waits a grace period from here, and what this stage
      // emptied is freed after it.
      cascade.token = domain_.StartGracePeriod();
      retirer.Finish(domain_, cascade.token);
    }
    if (cascade.next < cascade.stages.size()) {
      if (kept != c) {
        cascades_[kept] = std::move(cascade);
      }
      kept++;
    }
  }
  cascades_.resize(kept);
  domain_.ReclaimReady();
  return kept;
}

TransactionEngine::Result TransactionEngine::Reject(size_t n_ops,
                                                    size_t failed,
                                                    std::string error) const {
  Result result;
  result.outcome = Outcome::kRejected;
  result.generation = generation_;
  result.ops.resize(n_ops);
  result.ops[failed] = OpResult{OpStatus::kFailed, std::move(error)};
  return result;
}

TransactionEngine::Result TransactionEngine::Apply(
    std::span<const Op> ops, std::optional<uint64_t> expected_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  ReclaimRetiredLocked();  // frees ids whose removal has completed
  const size_t n = ops.size();

  if (expected_generation && *expected_generation != generation_) {
    Result result;
    result.outcome = Outcome::kConflict;
    result.generation = generation_;
    result.ops.resize(n);
    return result;
  }

  // -- structure: known resources, one operation per key --------------------
  std::vector<Resource *> resource(n);
  std::map<Reference, size_t> op_on;  // (resource, key) -> operation index
  for (size_t i = 0; i < n; i++) {
    const Op &op = ops[i];
    auto it = resources_.find(op.resource);
    if (it == resources_.end()) {
      return Reject(n, i, "unknown resource '" + op.resource + "'");
    }
    resource[i] = it->second;
    if (op.kind == OpKind::kUpsert && !op.value.has_value()) {
      return Reject(n, i, "upsert without a value");
    }
    if (!op_on.emplace(Reference{op.resource, op.key}, i).second) {
      return Reject(n, i,
                    "second operation on " + Describe(op.resource, op.key) +
                        " in one transaction");
    }
  }

  // -- reserve: all fallible work, nothing visible ---------------------------
  struct Staged {
    size_t op;
    std::unique_ptr<StagedOp> work;
  };
  std::vector<Staged> staged;
  staged.reserve(n);
  std::set<Resource *> touched;
  AbortGuard abort([&] {
    for (auto it = staged.rbegin(); it != staged.rend(); ++it) {
      it->work->Abort();
    }
    for (Resource *r : touched) {
      r->EndTransaction();
    }
  });

  Resource::Footprint footprint;          // summed over the transaction
  std::vector<uint32_t> removals_by_op(n, 0);
  std::map<Reference, int64_t> delta;     // referent -> reference change
  std::map<Reference, size_t> added_by;   // referent -> first op naming it
  for (size_t i = 0; i < n; i++) {
    const Op &op = ops[i];
    touched.insert(resource[i]);
    auto reserved = resource[i]->Reserve(op);
    if (!reserved) {
      return Reject(n, i, std::move(reserved.error()));
    }
    if (op.kind == OpKind::kErase) {
      // Checked below even if no reference to it changes in this transaction.
      delta.try_emplace(Reference{op.resource, op.key}, 0);
    }
    if (resource[i]->Contains(op.key)) {
      for (const Reference &ref : resource[i]->ReferencesOf(op.key)) {
        delta[ref]--;
      }
    }
    if (op.kind == OpKind::kUpsert) {
      for (const Reference &ref : reserved->references) {
        delta[ref]++;
        added_by.emplace(ref, i);
      }
    }
    footprint.retires += reserved->footprint.retires;
    footprint.removals += reserved->footprint.removals;
    footprint.callbacks += reserved->footprint.callbacks;
    removals_by_op[i] = reserved->footprint.removals;
    staged.push_back(Staged{i, std::move(reserved->staged)});
  }

  // -- bounded backpressure -------------------------------------------------
  // Retirement runs behind readers. Never let the RCU retire queue approach
  // the point where RetireErased() waits for readers (while this lock is
  // held): count what is queued, what pending removal cascades will still
  // retire, and what this transaction declares. Refuse retriably -- the
  // reservations are aborted, so nothing becomes visible.
  const size_t may_retire =
      size_t{footprint.retires} + size_t{footprint.removals};
  const size_t retire_budget = domain_.retire_high_water() / 2;
  if (may_retire > retire_budget) {
    return Reject(n, 0,
                  "transaction retires up to " + std::to_string(may_retire) +
                      " objects, more than one batch allows (" +
                      std::to_string(retire_budget) + "); split it");
  }
  if (domain_.pending_retired() + deferred_objects_ + may_retire >
          retire_budget ||
      cascades_.size() >= kMaxPendingCascades) {
    Result result;
    result.outcome = Outcome::kBusy;
    result.generation = generation_;
    result.ops.resize(n);
    return result;
  }

  // -- references the transaction leaves behind -----------------------------
  for (const auto &[ref, op_index] : added_by) {
    auto target = resources_.find(ref.resource);
    if (target == resources_.end()) {
      return Reject(n, op_index,
                    "references unknown resource '" + ref.resource + "'");
    }
    // Only declared references: the publication order is derived from the
    // declarations, so an undeclared one could publish before its referent.
    const Resource *referrer = resource[op_index];
    const auto &declared = referrer->declared_references();
    if (std::find(declared.begin(), declared.end(), ref.resource) ==
        declared.end()) {
      return Reject(n, op_index,
                    "undeclared reference: '" + referrer->name() +
                        "' may not reference '" + ref.resource +
                        "' (declare it when constructing the resource)");
    }
    // Holds by construction (declared references are registered first and
    // rank below); checked because publication order depends on it.
    DCHECK_LT(target->second->rank(), referrer->rank());
    if (!target->second->DefersErase()) {
      return Reject(n, op_index,
                    "resource '" + ref.resource +
                        "' cannot be referenced: its erases take effect at "
                        "once (Resource::DefersErase)");
    }
  }
  for (const auto &[ref, d] : delta) {
    auto current = references_.find(ref);
    const int64_t after =
        (current == references_.end() ? 0
                                      : static_cast<int64_t>(current->second)) +
        d;
    if (after <= 0) {
      continue;
    }
    auto target = resources_.find(ref.resource);
    auto op = op_on.find(ref);
    bool exists_after;
    if (op != op_on.end()) {
      exists_after = ops[op->second].kind == OpKind::kUpsert;
    } else {
      exists_after =
          target != resources_.end() && target->second->Contains(ref.key);
    }
    if (exists_after) {
      continue;
    }
    if (op != op_on.end()) {
      // The transaction erases a key that is still referenced afterwards.
      return Reject(n, op->second,
                    Describe(ref.resource, ref.key) + " is still referenced " +
                        std::to_string(after) + " time(s)");
    }
    return Reject(n, added_by.at(ref),
                  "references missing " + Describe(ref.resource, ref.key));
  }

  // -- commit bookkeeping, allocated before anything is visible -------------
  std::vector<size_t> order(staged.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    const Op &x = ops[staged[a].op];
    const Op &y = ops[staged[b].op];
    const bool xu = x.kind == OpKind::kUpsert;
    const bool yu = y.kind == OpKind::kUpsert;
    if (xu != yu) {
      return xu;  // upserts before erases
    }
    const int rx = resource[staged[a].op]->rank();
    const int ry = resource[staged[b].op]->rank();
    return xu ? rx < ry : rx > ry;
  });

  // Exactly what the resources declared (their Reserve() footprints): the
  // retirer's storage, and room in the RCU retire queue for what Finish()
  // hands over after commit, so neither publication nor the handoff
  // allocates.
  Retirer retirer;
  retirer.retire_.reserve(footprint.retires);
  retirer.after_.reserve(footprint.callbacks);
  retirer.removals_.reserve(footprint.removals);
  domain_.ReserveRetirements(footprint.retires);
  // Ledger nodes for new references, so publishing only updates counts.
  for (const auto &[ref, d] : delta) {
    if (d > 0) {
      references_.try_emplace(ref, 0);
    }
  }
  // The removal cascade's skeleton, from the declared removals: one stage
  // per rank, highest first, sized exactly; a pending-removal counter per
  // owning resource.
  std::vector<int> stage_ranks;
  Cascade cascade{0, {}, 0};
  if (footprint.removals != 0) {
    for (size_t i = 0; i < n; i++) {
      if (removals_by_op[i] != 0) {
        stage_ranks.push_back(resource[i]->rank());
        pending_removals_.try_emplace(resource[i], 0);
      }
    }
    std::sort(stage_ranks.begin(), stage_ranks.end(), std::greater<int>());
    stage_ranks.erase(std::unique(stage_ranks.begin(), stage_ranks.end()),
                      stage_ranks.end());
    cascade.stages.resize(stage_ranks.size());
    for (size_t s_i = 0; s_i < stage_ranks.size(); s_i++) {
      size_t steps = 0;
      for (size_t i = 0; i < n; i++) {
        if (resource[i]->rank() == stage_ranks[s_i]) {
          steps += removals_by_op[i];
        }
      }
      cascade.stages[s_i].reserve(steps);
    }
    cascades_.reserve(cascades_.size() + 1);
  }

  // -- publish: from here on nothing may fail, and nothing allocates --------
  if (internal::g_publish_window_hook != nullptr) {
    internal::g_publish_window_hook(true);
  }
  retirer.enforce_ = true;
  for (size_t i : order) {
    Resource *owner = resource[staged[i].op];
    retirer.rank_ = owner->rank();
    retirer.owner_ = owner;
    retirer.owner_name_ = owner->name().c_str();
    retirer.outstanding_ = outstanding_.at(owner).get();
    staged[i].work->Publish(retirer);
  }
  retirer.enforce_ = false;
  abort.Dismiss();
  for (const auto &[ref, d] : delta) {
    auto it = references_.find(ref);
    if (it == references_.end()) {
      continue;  // d <= 0 for a key with no references: nothing to record
    }
    const int64_t after = static_cast<int64_t>(it->second) + d;
    if (after <= 0) {
      references_.erase(it);
    } else {
      it->second = static_cast<size_t>(after);
    }
  }
  for (Retirer::Removal &removal : retirer.removals_) {
    // Every removal was declared by its operation, whose rank has a stage
    // sized for it (Retirer enforced the count).
    size_t s_i = 0;
    while (stage_ranks[s_i] != removal.rank) {
      s_i++;
    }
    cascade.stages[s_i].push_back(Step{removal.owner, std::move(removal.step)});
    pending_removals_.find(removal.owner)->second++;
    deferred_objects_++;
  }
  retirer.removals_.clear();
  generation_++;

  // -- committed: resources' own cleanup and asynchronous reclamation --------
  for (Resource *r : touched) {
    r->EndTransaction();
  }
  for (auto it = pending_removals_.begin(); it != pending_removals_.end();) {
    it = it->second == 0 ? pending_removals_.erase(it) : std::next(it);
  }
  std::erase_if(cascade.stages, [](const auto &stage) { return stage.empty(); });
  if (!retirer.empty() || !cascade.stages.empty()) {
    const rcu::GracePeriod token = domain_.StartGracePeriod();
    if (!cascade.stages.empty()) {
      cascade.token = token;
      cascades_.push_back(std::move(cascade));
    }
    retirer.Finish(domain_, token);  // into reserved room: no allocation
  }
  if (internal::g_publish_window_hook != nullptr) {
    internal::g_publish_window_hook(false);
  }
  domain_.ReclaimReady();

  Result result;
  result.outcome = Outcome::kApplied;
  result.generation = generation_;
  result.ops.assign(n, OpResult{OpStatus::kApplied, {}});
  return result;
}

}  // namespace dataplane
}  // namespace bess
