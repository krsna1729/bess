// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_RESOURCE_REGISTRY_H_
#define BESS_DATAPLANE_RESOURCE_REGISTRY_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include "dataplane/resource.h"

namespace bess::dataplane {

class TransactionEngine;

// What a module or library may do with the process's transactional resources
// (D-021, D-022): register the resources it owns, release them, read
// reference counts, and apply transactions to its own resources. The public
// face of the transaction engine (consolidation review, 2026-10-04): applying
// transactions to any resource, reclamation and the engine's internals stay
// with the control plane; an owner applies only to resources it registered
// (ApplyOwned, D-200). Callers hold the control-plane lock, as module commands
// do. Obtained from ModuleInitContext::resources() or, in tests,
// TransactionEngine::registry().
class ResourceRegistry {
 public:
  ResourceRegistry(const ResourceRegistry &) = delete;
  ResourceRegistry &operator=(const ResourceRegistry &) = delete;

  // See TransactionEngine::Register / Unregister / ReleaseForTeardown.
  std::expected<void, std::string> Register(Resource *resource);
  std::expected<void, std::string> Unregister(const std::string &name);
  std::expected<void, std::string> Unregister(std::span<const std::string> names);
  std::expected<void, std::string> ReleaseForTeardown(std::span<const std::string> names);

  // References to `key` of `resource` held by other resources' values.
  size_t ReferenceCount(const std::string &resource, const ResourceKey &key) const;
  // The resource generation: it advances on every applied transaction and on
  // an edit made outside one (TransactionEngine::AdvanceGeneration, D-129).
  uint64_t generation() const;

  // The answer to ApplyOwned(): the engine's answer for the transaction,
  // narrowed to what an owner's request can get. The request names no expected
  // generation and asks for referential consistency, so the engine's
  // kConflict and kUnsupported cannot occur.
  struct OwnedApplyResult {
    enum class Outcome : uint8_t {
      kApplied,
      kRejected,  // an operation failed: `failed_op` and `error` say which and why
      kBusy,      // reclamation is behind; nothing was attempted -- retry later
    };
    Outcome outcome = Outcome::kRejected;
    uint64_t generation = 0;  // after the transaction
    size_t failed_op = 0;     // kRejected: the operation's index in `ops`
    std::string error;        // kRejected: why
    // kRejected by the engine's retire budget: the operations are well-formed
    // but would retire more than one transaction may, so send fewer at once.
    bool over_budget = false;
  };

  // Applies `ops` as one transaction exactly as the control plane applies one
  // (TransactionEngine::Apply: all-or-nothing, the same retire budget and
  // refusals, counted in the same outcome metrics, kBusy answered rather than
  // waited out), provided every operation names a resource in `owned`.
  //
  // `owned` is the caller's own Resource objects, the pointers it passed to
  // Register(): holding the object is the proof of ownership, since an Op's
  // resource name is a string that can name anyone's resource. An operation on
  // a resource registered by someone else rejects the whole transaction before
  // anything is reserved: kRejected, `failed_op` that operation, `error`
  // "resource '<name>' is not owned by the caller". A name nobody registered is
  // refused as Apply refuses it ("unknown resource"). A pointer in `owned` that
  // is not registered matches no operation.
  //
  // Threading: the control side, under the control-plane lock -- a module
  // command, a TickEndpoint callback, a RequestEndpoint handler, Init() or
  // DeInit() -- the context the Router module publishes its resolver's
  // updates from. Never on a worker: it allocates (the result, and the
  // engine's working storage on growth) and starts a grace period. The engine's
  // own mutex orders it with every other transaction; the control-plane lock is
  // what keeps the caller's resources registered and the control plane's
  // planning from interleaving with it. Not from inside a Resource callback
  // (Reserve, Prepare, Publish, a retire step): the engine's mutex is held
  // there and is not recursive. It never waits for readers.
  OwnedApplyResult ApplyOwned(std::span<const Resource *const> owned,
                              std::span<const Op> ops);

 private:
  friend class TransactionEngine;
  explicit ResourceRegistry(TransactionEngine &engine) noexcept : engine_(engine) {}

  TransactionEngine &engine_;
};

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_RESOURCE_REGISTRY_H_
