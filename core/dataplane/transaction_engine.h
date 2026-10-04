// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_TRANSACTION_ENGINE_H_
#define BESS_DATAPLANE_TRANSACTION_ENGINE_H_

#include <array>
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
#include "dataplane/resource_registry.h"
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
// (the engine is the single writer of every registered resource).
//
// Visibility to packets is requested per transaction (Consistency, M8, D-050)
// and is never silently downgraded:
//
//   kReferential (default): packets may see the operations of a successful
//     transaction take effect one by one, in the order above, and nothing of
//     a failed one. A packet that can name a key finds it.
//   kScopeSnapshot: only for transactions whose every operation is on a
//     resource that provides it (Resource::ProvidedConsistency, ScopeResource).
//     Each scope switches from its whole old version to its whole new one in
//     a single store. Any other operation makes the engine answer
//     Outcome::kUnsupported before reserving anything.
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
    kUnsupported,  // the requested consistency cannot be provided by an
                   // operation's resource (ops say which); nothing was
                   // attempted and the request will not succeed as it stands
  };

  struct Result {
    Outcome outcome = Outcome::kRejected;
    uint64_t generation = 0;      // after the transaction
    std::vector<OpResult> ops;    // one per operation, in request order
  };

  explicit TransactionEngine(rcu::RcuDomain &domain) : domain_(domain) {}

  // The public face given to modules and libraries (resource_registry.h).
  ResourceRegistry &registry() noexcept { return registry_; }

  TransactionEngine(const TransactionEngine &) = delete;
  TransactionEngine &operator=(const TransactionEngine &) = delete;

  // Registers `resource` (not owned; it must outlive its registration).
  // Refused: a taken name, a self-dependency, or a populated resource that
  // may reference others (its existing references would be missing from the
  // ledger).
  //
  // A declared reference to a resource that is not registered yet is kept
  // unresolved and bound when that resource registers: modules are created in
  // an order the graph does not control (the desired-state planner creates
  // them by name), so a referrer may legitimately appear before its referent.
  // Until the reference is bound, a transaction value that names it is
  // refused ("undeclared reference"), so no key can hold an outgoing
  // reference the ledger does not know about. Ranks are derived from the
  // bound graph before each Apply.
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

  // Teardown release: a module being destroyed hands its resources back while
  // the module graph is being disconnected around it, so the order modules
  // are destroyed in (the planner's, by name) must not decide whether
  // teardown works. Same as Unregister() except two checks that only make
  // sense while the resource stays in a live graph:
  //
  //  - a declaration naming a released resource is left declared but unbound
  //    (so a replacement module with the same name binds it again, and until
  //    then a value naming it is refused as undeclared);
  //  - a resource may leave with live keys, and with its keys still named by
  //    other resources' live keys: those references become dangling and are
  //    logged. Nothing may resolve through the released table any more (the
  //    graph edges that reached it are gone), and the ledger no longer counts
  //    its keys.
  //
  // What still blocks: pending removal steps and retired objects not yet
  // destroyed (they capture the resource's tables), as for Unregister().
  // Missing names are ignored: releasing is idempotent.
  std::expected<void, std::string> ReleaseForTeardown(
      std::span<const std::string> names);

  // `consistency` is a request, not a hint: if any operation's resource
  // cannot provide it the answer is Outcome::kUnsupported (the failed
  // operation carries the reason), never a weaker transaction.
  Result Apply(std::span<const Op> ops,
               std::optional<uint64_t> expected_generation = std::nullopt,
               Consistency consistency = Consistency::kReferential);

  // How many Apply() calls ended in each Outcome, indexed by the enum's value
  // (operational metrics, M25).
  static constexpr size_t kOutcomes = 8;
  using OutcomeCounts = std::array<uint64_t, kOutcomes>;
  OutcomeCounts outcome_counts() const;
  // Removal cascades still waiting for a grace period.
  size_t pending_cascades() const;

  // Advances every removal cascade whose grace period has completed, and
  // returns how many cascades are still pending. Control thread only.
  size_t ReclaimRetired();

  uint64_t generation() const;
  // The registered resource with this name, or null. The pointer is valid
  // while it stays registered (callers hold the control-plane lock).
  Resource *FindResource(std::string_view name) const;
  // Registered resource names, sorted.
  std::vector<std::string> ResourceNames() const;
  // References to `key` of `resource` held by other resources' values.
  size_t ReferenceCount(const std::string &resource,
                        const ResourceKey &key) const;

 private:
  // More pending removal cascades than this and Apply() answers kBusy.
  static constexpr size_t kMaxPendingCascades = 4096;

  Result Reject(size_t n_ops, size_t failed, std::string error) const;
  Result ApplyImpl(std::span<const Op> ops, std::optional<uint64_t> expected_generation,
                   Consistency consistency);
  size_t ReclaimRetiredLocked();

  struct Registration;

  // Binds `reg`'s unresolved references to newly registered resources, and
  // derives every rank from the bound graph (a referrer ranks one above the
  // highest resource it may reference). Cheap and idempotent; called when a
  // registration changes the graph and before each Apply.
  void RebindDependenciesLocked(Registration &reg);
  void RecomputeRanksLocked();
  // Registration/teardown are rare; rebuild counts from the live values
  // rather than keep references to a removed registration or duplicate the
  // outgoing ledger on every transaction.
  void RebuildIncomingLocked();


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
    // Its declared references that are bound (the referent is registered).
    std::vector<std::pair<std::string, Registration *>> deps;
    // Declared references whose resource has not registered yet.
    std::vector<std::string> unresolved;
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
  // Ranks need deriving from the bound dependency graph (a registration
  // changed it, or a reference bound late).
  bool ranks_dirty_ = false;
  // The bound graph contains a cycle: no publication order exists, so Apply
  // refuses every transaction until the graph changes (a registration bug).
  bool reference_cycle_ = false;
  ResourceRegistry registry_{*this};
  std::array<std::atomic<uint64_t>, kOutcomes> outcome_counts_{};
};

// The engine behind a registry: for the control plane and in-tree code that
// applies transactions (a module's legacy commands), never for plugins.
inline TransactionEngine &EngineOf(ResourceRegistry &registry) noexcept {
  return registry.engine_;
}

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_TRANSACTION_ENGINE_H_
