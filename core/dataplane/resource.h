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

#ifndef BESS_DATAPLANE_RESOURCE_H_
#define BESS_DATAPLANE_RESOURCE_H_

#include <any>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "rcu/rcu_domain.h"

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
  template <typename T>
  void Retire(std::unique_ptr<T> object) {
    if (object == nullptr) {
      return;
    }
    retire_.push_back([o = std::move(object)](rcu::RcuDomain &domain,
                                              rcu::GracePeriod token) mutable {
      domain.Retire(token, std::move(o));
    });
  }

  // Removes something later, in the transaction's removal cascade: `step`
  // runs once no reader can still obtain the removed key from any referrer
  // -- a grace period after the publish for the highest-rank resource, and a
  // grace period after the rank above for each lower rank (a reader that saw
  // a referrer just before it went away must be done before its referent
  // goes). The step receives a Retirer for whatever it replaces.
  // SlotResource's erase uses this to empty its slot.
  void RemoveLater(std::move_only_function<void(Retirer &)> step) {
    removals_.push_back({rank_, owner_, std::move(step)});
  }

  // Runs `fn(token)` once the transaction's grace period has started: for
  // bookkeeping that needs the token.
  void AfterGracePeriodStarts(std::move_only_function<void(rcu::GracePeriod)> fn) {
    after_.push_back(std::move(fn));
  }

  bool empty() const { return retire_.empty() && after_.empty(); }

 private:
  friend class TransactionEngine;

  struct Removal {
    int rank;
    const Resource *owner;  // Unregister() waits for its pending removals
    std::move_only_function<void(Retirer &)> step;
  };

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

  std::vector<std::move_only_function<void(rcu::RcuDomain &, rcu::GracePeriod)>>
      retire_;
  std::vector<std::move_only_function<void(rcu::GracePeriod)>> after_;
  std::vector<Removal> removals_;
  int rank_ = 0;                     // of the operation publishing now
  const Resource *owner_ = nullptr;  // (both set by the engine)
};

// Work a resource reserved for one operation: everything fallible is done,
// nothing is visible yet.
class StagedOp {
 public:
  virtual ~StagedOp() = default;
  // Makes the operation visible to packets. Must not fail: every check and
  // allocation happened in Reserve(). Hands replaced state to `retirer`.
  virtual void Publish(Retirer &retirer) noexcept = 0;
  // Releases the reservation of a transaction that will not publish. Staged
  // objects themselves are freed by the destructor; override for
  // reservations held elsewhere (reserved capacity, ids).
  virtual void Abort() noexcept {}
};

class Resource {
 public:
  struct Reservation {
    std::unique_ptr<StagedOp> staged;
    // References the new value holds (upserts).
    std::vector<Reference> references;
  };

  // `rank` orders publication: a resource whose values reference another
  // resource must have a higher rank than it. Upserts publish in ascending
  // rank (referents first), erases in descending rank (referrers first).
  Resource(std::string name, int rank) : name_(std::move(name)), rank_(rank) {}
  virtual ~Resource() = default;

  Resource(const Resource &) = delete;
  Resource &operator=(const Resource &) = delete;

  const std::string &name() const { return name_; }
  int rank() const { return rank_; }

  // -- control side, called by the engine with its lock held ------------------

  // Whether `key` is present now.
  virtual bool Contains(const ResourceKey &key) const = 0;

  // The references the current value at `key` holds (none if absent).
  virtual std::vector<Reference> ReferencesOf(const ResourceKey &) const {
    return {};
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

 private:
  std::string name_;
  int rank_;
};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_RESOURCE_H_
