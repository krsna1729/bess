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

#ifndef BESS_DATAPLANE_TRANSACTION_ENGINE_H_
#define BESS_DATAPLANE_TRANSACTION_ENGINE_H_

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dataplane/resource.h"
#include "rcu/rcu_domain.h"

namespace bess {
namespace dataplane {

// Applies transactions over registered resources (G1.2b). Decision D-021
// (docs/decisions.md); prior art D-020.
//
// Apply(ops) is all-or-nothing and never visible in part to a failed
// transaction:
//
//   1. check expected_generation (optimistic concurrency);
//   2. reserve: every operation, in request order, does all its fallible work
//      (Resource::Reserve); then every reference the transaction leaves is
//      checked -- a value may only name a key that exists afterwards, and a
//      key may only be erased if nothing references it afterwards. Any
//      failure aborts every reservation: nothing was published.
//   3. publish (infallible): upserts in ascending resource rank (referents
//      first), then erases in descending rank (referrers first), so a packet
//      never resolves a reference to a missing key;
//   4. one grace period for the whole transaction retires what it replaced;
//   5. erased keys that may still be referenced by an in-flight reader stay
//      readable and leave in a removal cascade: the highest-rank resource's
//      erased keys are emptied one grace period after the publish, each lower
//      rank one grace period after the rank above. A reader that saw a
//      referrer just before it went away is done before its referent goes.
//      ReclaimRetired() (also run at the start of every Apply) advances it.
//
// Transactions are strictly serializable: one at a time, in arrival order
// (the engine is the single writer of every registered resource). By
// default packets may see the operations of a successful transaction take
// effect one by one, in the order above; all-at-once visibility per scope
// (the scope cell of section 14.5) is a later increment.
class TransactionEngine {
 public:
  enum class OpStatus : uint8_t {
    kApplied,
    kFailed,        // this operation caused the rejection
    kNotApplied,    // the transaction was rejected for another operation
  };

  struct OpResult {
    OpStatus status = OpStatus::kNotApplied;
    std::string error;  // for kFailed
  };

  enum class Outcome : uint8_t {
    kApplied,
    kRejected,  // an operation failed; results say which and why
    kConflict,  // expected_generation did not match; nothing was attempted
  };

  struct Result {
    Outcome outcome = Outcome::kRejected;
    uint64_t generation = 0;      // after the transaction
    std::vector<OpResult> ops;    // one per operation, in request order
  };

  explicit TransactionEngine(rcu::RcuDomain &domain) : domain_(domain) {}

  TransactionEngine(const TransactionEngine &) = delete;
  TransactionEngine &operator=(const TransactionEngine &) = delete;

  // Registers `resource` (not owned; it must outlive its registration).
  // Fails if the name is taken.
  bool Register(Resource *resource);
  // Fails while other resources reference keys of it, or while its removal
  // cascade still holds steps for it (they capture the resource's tables;
  // a module must not destroy them before). It advances the cascade first,
  // so with no reader online -- workers paused or stopped, as at module
  // teardown -- one call is enough.
  bool Unregister(const std::string &name);

  Result Apply(std::span<const Op> ops,
               std::optional<uint64_t> expected_generation = std::nullopt);

  // Advances every removal cascade whose grace period has completed, and
  // returns how many cascades are still pending. Control thread only.
  size_t ReclaimRetired();

  uint64_t generation() const;
  // References to `key` of `resource` held by other resources' values.
  size_t ReferenceCount(const std::string &resource,
                        const ResourceKey &key) const;

 private:
  Result Reject(size_t n_ops, size_t failed, std::string error) const;
  size_t ReclaimRetiredLocked();

  // One transaction's removals, stage by stage (ranks, highest first); the
  // next stage runs once `token` completes.
  struct Step {
    const Resource *owner;
    std::move_only_function<void(Retirer &)> fn;
  };
  struct Cascade {
    rcu::GracePeriod token;
    std::vector<std::vector<Step>> stages;
    size_t next = 0;
  };

  rcu::RcuDomain &domain_;
  mutable std::mutex mutex_;
  std::map<std::string, Resource *> resources_;
  std::map<Reference, size_t> references_;  // referent -> count
  std::vector<Cascade> cascades_;
  std::map<const Resource *, size_t> pending_removals_;
  uint64_t generation_ = 0;
};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_TRANSACTION_ENGINE_H_
