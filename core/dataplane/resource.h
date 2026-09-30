// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_RESOURCE_H_
#define BESS_DATAPLANE_RESOURCE_H_

#include <any>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <glog/logging.h>

#include "rcu/rcu_domain.h"
#include "utils/inline_function.h"

namespace bess {
namespace dataplane {

// Dataplane resources and the operations a transaction applies to them
// (G1.2b). Decision D-021 (docs/decisions.md); design in MODERNIZATION.md
// section 14.5; prior art in D-020.
//
// A resource is a named, keyed collection of dataplane state that packets
// read: exact-match rules, routes, next hops, action objects, meters. A module
// or plugin registers its resources with the TransactionEngine; controllers
// then change several resources, across modules, in one all-or-nothing
// transaction, and the engine does the ordering, reference checks and
// reclamation that controllers otherwise hand-roll.

// A key as bytes; each resource defines its encoding. For a trivially
// copyable key type, EncodeKey/DecodeKey are the encoding.
using ResourceKey = std::string;

template <typename K>
  requires std::is_trivially_copyable_v<K>
ResourceKey EncodeKey(const K &key) {
  return ResourceKey(reinterpret_cast<const char *>(&key), sizeof(K));
}

template <typename K>
  requires std::is_trivially_copyable_v<K>
bool DecodeKey(const ResourceKey &bytes, K *key) {
  if (bytes.size() != sizeof(K)) {
    return false;
  }
  std::memcpy(key, bytes.data(), sizeof(K));
  return true;
}

enum class OpKind : uint8_t { kUpsert, kErase };

// One change to one key of one resource. `value` holds the resource's value
// type for an upsert and is empty for an erase.
struct Op {
  std::string resource;
  OpKind kind = OpKind::kUpsert;
  ResourceKey key;
  std::any value;

  static Op Upsert(std::string resource, ResourceKey key, std::any value) {
    return Op{std::move(resource), OpKind::kUpsert, std::move(key),
              std::move(value)};
  }
  static Op Erase(std::string resource, ResourceKey key) {
    return Op{std::move(resource), OpKind::kErase, std::move(key), {}};
  }
};

// A reference from a value to a key of another resource (a rule naming an
// action, a route naming a next hop). The engine refuses a transaction that
// would leave a reference to a missing key, or remove a key still referenced,
// and publishes referents before referrers.
struct Reference {
  std::string resource;
  ResourceKey key;

  friend bool operator==(const Reference &, const Reference &) = default;
  friend auto operator<=>(const Reference &, const Reference &) = default;
};

class Resource;

// Collects what a transaction's publish phase replaced, and frees it after
// one grace period for the whole transaction (so a reader that looked
// anything up before the publish finishes with it first).
class Retirer {
 public:
  // The callables a Retirer keeps live in fixed inline storage, never on
  // the heap, so handing one over during publication cannot allocate. A
  // lambda capturing more than kCallableBytes does not compile: prepare its
  // state in Reserve() (in the staged operation) and capture a pointer or
  // reference to it. (External review: std::move_only_function would
  // silently allocate for a large capture, inside noexcept Publish().)
  static constexpr size_t kCallableBytes = 48;
  using Step = utils::InlineFunction<void(Retirer &), kCallableBytes>;
  using AfterStart =
      utils::InlineFunction<void(rcu::GracePeriod), kCallableBytes>;

  // Each call during publication must fit the footprint its operation
  // declared in Reserve() (Resource::Footprint): the engine reserved exactly
  // that, so publication neither allocates nor meets an unplanned stage.
  // Exceeding it is a bug in the resource and fatal.
  template <typename T>
  void Retire(std::unique_ptr<T> object) {
    if (object == nullptr) {
      return;
    }
    CheckRoom(retire_.size(), retire_.capacity(), "Retire");
    retire_.push_back([o = std::move(object), c = outstanding_](
                          rcu::RcuDomain &domain,
                          rcu::GracePeriod token) mutable {
      domain.Retire(token, std::move(o), c);
    });
  }

  // Removes something later, in the transaction's removal cascade: `step`
  // runs once no reader can still obtain the removed key from any referrer
  // -- a grace period after the publish for the highest-rank resource, and a
  // grace period after the rank above for each lower rank (a reader that saw
  // a referrer just before it went away must be done before its referent
  // goes). The step receives a Retirer for whatever it replaces.
  // SlotResource's erase uses this to empty its slot.
  void RemoveLater(Step step) {
    CheckRoom(removals_.size(), removals_.capacity(), "RemoveLater");
    removals_.push_back({rank_, owner_, std::move(step)});
  }

  // Runs `fn(token)` once the transaction's grace period has started: for
  // bookkeeping that needs the token.
  void AfterGracePeriodStarts(AfterStart fn) {
    CheckRoom(after_.size(), after_.capacity(), "AfterGracePeriodStarts");
    after_.push_back(std::move(fn));
  }

  bool empty() const { return retire_.empty() && after_.empty(); }

 private:
  friend class TransactionEngine;

  struct Removal {
    int rank;
    const Resource *owner;  // Unregister() waits for its pending removals
    Step step;
  };

  void CheckRoom(size_t used, size_t capacity, const char *what) const {
    if (enforce_ && used >= capacity) {
      LOG(FATAL) << "resource '" << owner_name_
                 << "' exceeded the footprint it declared in Reserve() ("
                 << what << "): publication would allocate";
    }
  }

  void Finish(rcu::RcuDomain &domain, rcu::GracePeriod token) {
    for (auto &r : retire_) {
      r(domain, token);
    }
    for (auto &fn : after_) {
      fn(token);
    }
    retire_.clear();
    after_.clear();
  }

  std::vector<utils::InlineFunction<void(rcu::RcuDomain &, rcu::GracePeriod),
                                    kCallableBytes>>
      retire_;
  std::vector<AfterStart> after_;
  std::vector<Removal> removals_;
  // Set by the engine for the operation (or removal step) running now.
  int rank_ = 0;
  const Resource *owner_ = nullptr;
  const char *owner_name_ = "";
  // The owner's count of retired objects not yet destroyed (see
  // RcuDomain::Retire): Unregister() waits for it to reach zero.
  std::atomic<size_t> *outstanding_ = nullptr;
  bool enforce_ = false;  // during publication: capacity is the footprint
};

// Work a resource reserved for one operation: everything fallible is done,
// nothing is visible yet.
class StagedOp {
 public:
  virtual ~StagedOp() = default;
  // Makes the operation visible to packets. Must not fail: every check and
  // allocation happened in Reserve(). Hands replaced state to `retirer`.
  //
  // The no-allocation contract: the engine's own publication path does not
  // allocate, the Retirer cannot (its callables are inline, within the
  // declared footprint), and the built-in adapters are checked by
  // TransactionEngineTest.PublicationDoesNotAllocate. A custom resource's
  // Publish() is its author's to keep allocation-free: build objects,
  // copies and removal state in Reserve() and only move or store them
  // here; check it with the same instrumentation (a counting global
  // operator new around internal::g_publish_window_hook).
  virtual void Publish(Retirer &retirer) noexcept = 0;
  // Releases the reservation of a transaction that will not publish. Staged
  // objects themselves are freed by the destructor; override for
  // reservations held elsewhere (reserved capacity, ids).
  virtual void Abort() noexcept {}
};

class ResourceCodec;  // framework/resource_codec.h; the engine needs no protobuf.

class Resource {
 public:
  // Upper bounds on what an operation's Publish() will ask of the Retirer.
  // The engine reserves exactly the sum over a transaction before publishing,
  // and uses it for backpressure: nothing is inferred.
  struct Footprint {
    uint32_t retires = 0;    // Retirer::Retire (replaced objects)
    uint32_t removals = 0;   // Retirer::RemoveLater (deferred removal steps)
    uint32_t callbacks = 0;  // Retirer::AfterGracePeriodStarts
  };

  struct Reservation {
    std::unique_ptr<StagedOp> staged;
    // References the new value holds (upserts).
    std::vector<Reference> references;
    Footprint footprint;
    // Whether the key existed before the transaction, and the references its
    // value held then. The resource found the key anyway; reporting it here
    // saves the engine two more lookups per operation.
    bool existed = false;
    std::vector<Reference> previous_references;
  };

  // `references`: the resources this one's values may refer to. The engine
  // derives the publication order from them: a resource ranks one above the
  // highest resource it may reference. A value naming a resource not declared
  // here is refused. (Caller-assigned ranks could be wrong; a declared graph
  // cannot be.) A declared name may register later than this resource -- the
  // reference is bound when it appears, so module creation order does not
  // decide the graph -- and until it is bound a value naming it is refused.
  explicit Resource(std::string name, std::vector<std::string> references = {})
      : name_(std::move(name)), declared_(std::move(references)) {}
  virtual ~Resource() = default;

  Resource(const Resource &) = delete;
  Resource &operator=(const Resource &) = delete;

  const std::string &name() const { return name_; }

  // Module-facing typed codec; control owns the wire-envelope adapter. Null
  // means this resource is not reachable over the RPC.
  const ResourceCodec *codec() const { return codec_.get(); }
  void SetCodec(std::shared_ptr<const ResourceCodec> codec) {
    codec_ = std::move(codec);
  }

  // The resources this one may reference (declared at construction).
  const std::vector<std::string> &declared_references() const {
    return declared_;
  }
  // Publication rank, derived by the engine at registration: 0 for a
  // resource that references nothing, else 1 + the highest rank it may
  // reference. Upserts publish in ascending rank, erases in descending rank.
  int rank() const { return rank_; }

  // -- control side, called by the engine with its lock held ------------------

  // Whether `key` is present now.
  virtual bool Contains(const ResourceKey &key) const = 0;

  // The references the current value at `key` holds (none if absent).
  virtual std::vector<Reference> ReferencesOf(const ResourceKey &) const {
    return {};
  }
  // Enumerates references held by all committed values. Called only when
  // registrations change, to reconcile the ledger after order-independent
  // teardown and rebinding. Resources that declare dependencies and keep
  // values must implement this; independent resources need no extra API.
  virtual void VisitReferences(
      const std::function<void(const Reference &)> &) const {
    CHECK(declared_.empty() || LiveCount() == 0)
        << "a populated resource with dependencies must visit its references";
  }


  // Validates `op` and does all its fallible work (decoding, allocation,
  // capacity), leaving nothing visible. Called once per operation, in
  // request order; a resource that reserves capacity must account for the
  // earlier reservations of the same transaction.
  virtual std::expected<Reservation, std::string> Reserve(const Op &op) = 0;

  // Whether an erase keeps the key readable until the transaction's removal
  // cascade reaches it (Retirer::RemoveLater). Only such resources may be
  // referenced: a referrer's old value may still hand a reader the key's id
  // after the erase is published. A resource whose erase takes effect at
  // once (an rte_hash rule table) must not be a referent; the engine refuses
  // references to it.
  virtual bool DefersErase() const { return false; }

  // Called after a transaction finished (published or aborted), for
  // resources that keep per-transaction state in Reserve().
  virtual void EndTransaction() noexcept {}

  // Live (committed) keys. The engine refuses to unregister a resource that
  // still has any: their outgoing references are in its ledger, and the
  // module must erase them (in a transaction) first. It also refuses to
  // register a populated resource that may reference others, since its
  // existing references would be missing from the ledger.
  virtual size_t LiveCount() const = 0;

 private:
  friend class TransactionEngine;

  std::string name_;
  std::vector<std::string> declared_;
  int rank_ = 0;
  void *registration_ = nullptr;  // the engine's record for it
  std::shared_ptr<const ResourceCodec> codec_;
};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_RESOURCE_H_
