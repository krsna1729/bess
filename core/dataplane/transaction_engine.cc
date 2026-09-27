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

namespace bess {
namespace dataplane {

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

bool TransactionEngine::Register(Resource *resource) {
  std::lock_guard<std::mutex> lock(mutex_);
  return resources_.emplace(resource->name(), resource).second;
}

bool TransactionEngine::Unregister(const std::string &name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto res = resources_.find(name);
  if (res == resources_.end()) {
    return false;
  }
  auto it = references_.lower_bound(Reference{name, {}});
  if (it != references_.end() && it->first.resource == name) {
    return false;
  }
  // Pending removal steps capture this resource's tables: unregistering
  // (after which the module may destroy them) must wait until they ran.
  ReclaimRetiredLocked();
  if (pending_removals_.contains(res->second)) {
    return false;
  }
  resources_.erase(res);
  return true;
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
  size_t kept = 0;
  for (size_t c = 0; c < cascades_.size(); c++) {
    Cascade &cascade = cascades_[c];
    while (cascade.next < cascade.stages.size() &&
           domain_.IsComplete(cascade.token)) {
      Retirer retirer;
      for (Step &step : cascade.stages[cascade.next]) {
        step.fn(retirer);
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
    staged.push_back(Staged{i, std::move(reserved->staged)});
  }

  // -- references the transaction leaves behind -----------------------------
  for (const auto &[ref, op_index] : added_by) {
    auto target = resources_.find(ref.resource);
    if (target == resources_.end()) {
      return Reject(n, op_index,
                    "references unknown resource '" + ref.resource + "'");
    }
    // Publication order is by rank, so a referrer must rank strictly above
    // what it references -- or its upsert could publish first and a packet
    // resolve a missing key. Ranks are declared, so check them.
    const Resource *referrer = resource[op_index];
    if (target->second->rank() >= referrer->rank()) {
      return Reject(n, op_index,
                    "rank violation: '" + referrer->name() + "' (rank " +
                        std::to_string(referrer->rank()) + ") references '" +
                        ref.resource + "' (rank " +
                        std::to_string(target->second->rank()) +
                        "); a referrer must rank strictly higher");
    }
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

  // -- publish (infallible), dependency-ordered ------------------------------
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

  Retirer retirer;
  for (size_t i : order) {
    retirer.rank_ = resource[staged[i].op]->rank();
    retirer.owner_ = resource[staged[i].op];
    staged[i].work->Publish(retirer);
  }
  abort.Dismiss();
  for (Resource *r : touched) {
    r->EndTransaction();
  }

  for (const auto &[ref, d] : delta) {
    const int64_t after = static_cast<int64_t>(references_[ref]) + d;
    if (after <= 0) {
      references_.erase(ref);
    } else {
      references_[ref] = static_cast<size_t>(after);
    }
  }
  generation_++;

  if (!retirer.empty() || !retirer.removals_.empty()) {
    const rcu::GracePeriod token = domain_.StartGracePeriod();
    if (!retirer.removals_.empty()) {
      // Stages by rank, highest (referrers) first.
      std::stable_sort(retirer.removals_.begin(), retirer.removals_.end(),
                       [](const Retirer::Removal &a, const Retirer::Removal &b) {
                         return a.rank > b.rank;
                       });
      Cascade cascade{token, {}, 0};
      for (size_t i = 0; i < retirer.removals_.size(); i++) {
        if (i == 0 ||
            retirer.removals_[i].rank != retirer.removals_[i - 1].rank) {
          cascade.stages.emplace_back();
        }
        cascade.stages.back().push_back(
            Step{retirer.removals_[i].owner,
                 std::move(retirer.removals_[i].step)});
        pending_removals_[retirer.removals_[i].owner]++;
      }
      retirer.removals_.clear();
      cascades_.push_back(std::move(cascade));
    }
    retirer.Finish(domain_, token);
    domain_.ReclaimReady();
  }

  Result result;
  result.outcome = Outcome::kApplied;
  result.generation = generation_;
  result.ops.assign(n, OpResult{OpStatus::kApplied, {}});
  return result;
}

}  // namespace dataplane
}  // namespace bess
