// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_SCOPE_H_
#define BESS_DATAPLANE_SCOPE_H_

#include <cstdint>

#include "dataplane/resource.h"
#include "dataplane/slot_resource.h"
#include "dataplane/slot_table.h"
#include "dataplane/strong_id.h"

namespace bess::dataplane {

// Scope snapshots (M8, Decision D-050; roadmap "ScopeCell -> immutable
// ScopeVersion").
//
// A scope is whatever a packet can name independently of the rules being
// replaced -- a session, a policy group, a tenant -- and the application
// owning it decides what that is. Its policy is one immutable `Version`: any
// plain struct the application defines. A scope switches from its complete old
// version to its complete new one in a single pointer store, so a packet that
// binds the scope once sees one version in full, however many lookups it then
// makes through it:
//
//   packet operation:                  control side (a transaction):
//     const Version *v = scopes.Lookup(id);    Op::Upsert(name, EncodeKey(id),
//     if (v == nullptr) { /* no scope */ }                std::any(new_version))
//     ... every covered lookup goes through *v ...
//
// `Lookup` is the roadmap's ScopeCell::Read(): one acquire load per scope per
// packet operation, nothing per table operation. The pointer is valid until
// the calling worker's next quiescent state; do not cache it across task
// invocations.
//
// A Version follows three rules, which the application owns:
//  - Immutable: a change publishes a new Version. Never write through the
//    pointer a reader holds.
//  - Mutable state stays separate: token buckets, counters and other state
//    that changes under traffic are owned elsewhere (meters, WorkerSlots) and
//    the Version holds their ids, not copies. Naming them through `references`
//    lets the engine keep them alive for as long as a reader may hold the
//    Version that names them.
//  - Self-contained for the lookups it covers: a lookup that goes through the
//    Version must not give a different answer depending on rules outside it
//    (overlapping external rules cannot change which entry wins inside the
//    scope).
//
// Erasing a scope empties it after the transaction's removal cascade (as for
// every SlotResource): readers see the old Version until then and `nullptr`
// afterwards, so an operation must handle a missing scope.

struct ScopeIdTag;
using ScopeId = StrongId<ScopeIdTag, uint32_t>;
inline constexpr ScopeId kInvalidScopeId{};

// Scope id -> published Version. Ids are one-based (SlotTable): zero never names
// a scope.
template <typename Version>
using ScopeTable = SlotTable<ScopeId, Version>;

// A ScopeTable as a transactional resource: key = EncodeKey(ScopeId), value =
// the Version (std::any holding a Version). It is the only built-in resource
// that provides Consistency::kScopeSnapshot: an upsert is a single store of the
// whole Version, and a transaction asking for scope-snapshot visibility may
// contain nothing else. Constructor arguments are SlotResource's.
//
// A transaction that touches several scopes: each scope switches atomically;
// two scopes of one transaction switch one after the other, in publication
// order, and no packet binds both. A Version that names objects created for it
// -- a new action, a next hop -- is applied in two steps: create the
// referents in a referential transaction (nothing can name them yet), then
// switch the scope in a scope-snapshot one. Mixing both in one transaction is
// refused (Outcome::kUnsupported).
template <typename Version>
class ScopeResource final : public SlotResource<ScopeId, Version> {
  using Base = SlotResource<ScopeId, Version>;

 public:
  using Base::Base;

  Consistency ProvidedConsistency() const override {
    return Consistency::kScopeSnapshot;
  }
};

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_SCOPE_H_
