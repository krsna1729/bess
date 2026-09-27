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

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
namespace internal {
// Test-only: called with true as the publication phase starts and false as it
// ends (a test counts allocations in between; none are allowed). Null in
// production.
extern void (*g_publish_window_hook)(bool entering);
}  // namespace internal

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
    kBusy,      // reclamation is behind (readers slow to quiesce); nothing
                // was attempted -- retry later
  };

  struct Result {
    Outcome outcome = Outcome::kRejected;
    uint64_t generation = 0;      // after the transaction
    std::vector<OpResult> ops;    // one per operation, in request order
  };

  explicit TransactionEngine(rcu::RcuDomain &domain) : domain_(domain) {}

  TransactionEngine(const TransactionEngine &) = delete;
  TransactionEngine &operator=(const TransactionEngine &) = delete;

  // Registers `resource` (not owned; it must outlive its registration) and
  // derives its rank from its declared references, which must already be
  // registered. Refused: a taken name, an undeclared or unregistered
  // dependency, a self-dependency, or a populated resource that may
  // reference others (its existing references would be missing from the
  // ledger).
  std::expected<void, std::string> Register(Resource *resource);

  // Refused while: another registered resource declares this one as a
  // reference; it has live keys and may reference others (erase them in a
  // transaction first -- their outgoing references are in the ledger; a
  // resource that references nothing may go with its keys, D-022); keys of
  // it are referenced; or its
  // removal cascade still holds steps for it (they capture its tables, which
  // the module must not destroy before). It advances the cascade first, so
  // with no reader online -- workers paused or stopped, as at module
  // teardown -- that last condition clears in the same call.
  std::expected<void, std::string> Unregister(const std::string &name);
  // Several resources at once. Resources that reference only each other
  // (routes and the next hops they name) may leave together with their
  // keys: their references are all inside the group. D-023.
  std::expected<void, std::string> Unregister(
      std::span<const std::string> names);

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
  // More pending removal cascades than this and Apply() answers kBusy.
  static constexpr size_t kMaxPendingCascades = 4096;

  Result Reject(size_t n_ops, size_t failed, std::string error) const;
  size_t ReclaimRetiredLocked();

  // Hash maps keyed by strings, looked up by string_view without a copy.
  struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept {
      return std::hash<std::string_view>{}(s);
    }
  };
  template <typename V>
  using StringMap =
      std::unordered_map<std::string, V, StringHash, std::equal_to<>>;

  // Everything the engine keeps about one registered resource.
  struct Registration {
    Resource *resource = nullptr;
    // Its declared references, resolved once at registration.
    std::vector<std::pair<std::string, Registration *>> deps;
    // References to its keys held by other resources' values: key -> count.
    StringMap<size_t> incoming;
    // Retired objects of it handed to RCU and not yet destroyed.
    std::atomic<size_t> outstanding{0};
    size_t pending_removals = 0;  // cascade steps not yet run
    bool touched = false;         // during Apply: needs EndTransaction()
  };
  static Registration &RegistrationOf(const Resource *resource) {
    return *static_cast<Registration *>(resource->registration_);
  }
  // The registration a reference from `from` names, if `from` declared it.
  static Registration *Resolve(const Registration &from,
                               std::string_view resource);

  // One transaction's removals, stage by stage (ranks, highest first); the
  // next stage runs once `token` completes.
  struct Step {
    const Resource *owner;
    Retirer::Step fn;
  };
  struct Cascade {
    rcu::GracePeriod token;
    std::vector<std::vector<Step>> stages;
    size_t next = 0;
  };

  // Per-transaction working storage, kept across transactions so its
  // capacity is reused (no allocation per Apply in the steady state).
  struct KeyRef {
    Registration *reg;
    std::string_view key;
    uint32_t op;
  };
  struct Delta {
    Registration *target;
    std::string_view key;
    int64_t change;
    uint32_t added_by;  // first operation adding a reference; kNone if none
  };
  static constexpr uint32_t kNone = ~uint32_t{0};
  struct Staged {
    size_t op;
    std::unique_ptr<StagedOp> work;
  };
  struct Scratch {
    std::vector<Registration *> reg;
    std::vector<KeyRef> keys;
    std::vector<Resource::Reservation> reservations;
    std::vector<Staged> staged;
    std::vector<uint32_t> removals_by_op;
    std::vector<Delta> deltas;
    std::vector<size_t> order;
    std::vector<Registration *> touched;
  };

  rcu::RcuDomain &domain_;
  mutable std::mutex mutex_;
  StringMap<std::unique_ptr<Registration>> resources_;
  std::vector<Cascade> cascades_;
  // Objects that pending removal cascades will still retire (one per step).
  size_t deferred_objects_ = 0;
  uint64_t generation_ = 0;
  Scratch scratch_;
};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_TRANSACTION_ENGINE_H_
