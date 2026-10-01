// SPDX-License-Identifier: BSD-3-Clause

#include "dataplane/transaction_engine.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include <glog/logging.h>

namespace bess {
namespace dataplane {

namespace internal {
void (*g_publish_window_hook)(bool entering) = nullptr;
}  // namespace internal

namespace {

std::string Describe(std::string_view resource, std::string_view key) {
  std::string out = std::string(resource) + "/";
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

TransactionEngine::Registration *TransactionEngine::Resolve(
    const Registration &from, std::string_view resource) {
  for (const auto &[name, reg] : from.deps) {
    if (name == resource) {
      return reg;
    }
  }
  return nullptr;
}

std::expected<void, std::string> TransactionEngine::Register(
    Resource *resource) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (resources_.contains(resource->name())) {
    return std::unexpected("resource '" + resource->name() +
                           "' is already registered");
  }
  auto reg = std::make_unique<Registration>();
  reg->resource = resource;
  for (const std::string &dep : resource->declared_references()) {
    if (dep == resource->name()) {
      return std::unexpected("resource '" + dep + "' cannot reference itself");
    }
    auto it = resources_.find(dep);
    if (it == resources_.end()) {
      // Its resource has not registered yet: bound when it does, so module
      // creation order (the planner's, by name) cannot decide the graph.
      reg->unresolved.push_back(dep);
      continue;
    }
    reg->deps.emplace_back(dep, it->second.get());
  }
  if (!resource->declared_references().empty() && resource->LiveCount() != 0) {
    return std::unexpected("resource '" + resource->name() +
                           "' is populated and may reference others: its "
                           "existing references are unknown to the engine");
  }
  resource->registration_ = reg.get();
  Registration *const registered = reg.get();
  resources_.emplace(resource->name(), std::move(reg));
  // Everything that declared this name binds to it now.
  for (auto &[name, other] : resources_) {
    if (other.get() != registered) {
      RebindDependenciesLocked(*other);
    }
  }
  RebuildIncomingLocked();
  ranks_dirty_ = true;
  // Ranks are derived here too, not only before Apply: a resource's rank is
  // then meaningful as soon as the graph settles, which is what the engine's
  // own tests and any introspection read.
  RecomputeRanksLocked();
  return {};
}

void TransactionEngine::RebindDependenciesLocked(Registration &reg) {
  for (size_t i = 0; i < reg.unresolved.size();) {
    auto it = resources_.find(std::string_view(reg.unresolved[i]));
    if (it == resources_.end()) {
      i++;
      continue;
    }
    reg.deps.emplace_back(reg.unresolved[i], it->second.get());
    reg.unresolved.erase(reg.unresolved.begin() +
                         static_cast<std::ptrdiff_t>(i));
  }
}

void TransactionEngine::RebuildIncomingLocked() {
  for (auto &[name, reg] : resources_) {
    reg->incoming.clear();
  }
  for (const auto &[name, reg] : resources_) {
    reg->resource->VisitReferences([&](const Reference &ref) {
      Registration *target = Resolve(*reg, ref.resource);
      if (target != nullptr) {
        ++target->incoming[ref.key];
      }
    });
  }
}

void TransactionEngine::RecomputeRanksLocked() {
  if (!ranks_dirty_) {
    return;
  }
  ranks_dirty_ = false;
  for (auto &[name, reg] : resources_) {
    reg->resource->rank_ = 0;
  }
  // Relaxation over the bound graph: in a DAG every rank settles within
  // one pass per resource, so a rank still moving after that is a cycle.
  bool changed = false;
  for (size_t pass = 0; pass <= resources_.size(); pass++) {
    changed = false;
    for (auto &[name, reg] : resources_) {
      int rank = 0;
      for (const auto &[dep_name, dep] : reg->deps) {
        rank = std::max(rank, dep->resource->rank_ + 1);
      }
      if (rank != reg->resource->rank_) {
        reg->resource->rank_ = rank;
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }
  if (changed) {
    reference_cycle_ = true;
    LOG(ERROR) << "registered resources declare a reference cycle: no "
                  "publication order exists, so transactions are refused "
                  "until the graph changes";
  } else {
    reference_cycle_ = false;
  }
}

std::expected<void, std::string> TransactionEngine::Unregister(
    const std::string &name) {
  return Unregister(std::span<const std::string>(&name, 1));
}

std::expected<void, std::string> TransactionEngine::Unregister(
    std::span<const std::string> names) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Registration *> group;
  for (const std::string &name : names) {
    auto res = resources_.find(std::string_view(name));
    if (res == resources_.end()) {
      return std::unexpected("resource '" + name + "' is not registered");
    }
    if (std::find(group.begin(), group.end(), res->second.get()) ==
        group.end()) {
      group.push_back(res->second.get());
    }
  }
  auto in_group = [&](const Registration *r) {
    return std::find(group.begin(), group.end(), r) != group.end();
  };
  for (Registration *reg : group) {
    const std::string &name = reg->resource->name();
    // Nothing outside the group may reference it. A resource that declares
    // the name but has not bound it (its resource was never registered)
    // counts: the declaration is what the ledger would use.
    bool referenced_in_group = false;
    for (const auto &[other_name, other] : resources_) {
      const bool declares =
          Resolve(*other, name) != nullptr ||
          std::find(other->unresolved.begin(), other->unresolved.end(),
                    name) != other->unresolved.end();
      if (!declares) {
        continue;
      }
      if (!in_group(other.get())) {
        return std::unexpected("resource '" + other_name +
                               "' may reference '" + name +
                               "' (unregister it first, or with it)");
      }
      referenced_in_group = true;
    }
    // Live keys hold references into their dependencies, so a resource may
    // go with keys in it only if everything it may reference goes with it:
    // no ledger outside the group counts its keys. A table that references
    // nothing (ExactMatch's rules, D-022), or a Router's routes leaving
    // with its next hops (D-023), qualifies.
    for (const auto &[dep_name, dep] : reg->deps) {
      if (!in_group(dep) && reg->resource->LiveCount() != 0) {
        return std::unexpected("resource '" + name + "' still has " +
                               std::to_string(reg->resource->LiveCount()) +
                               " live key(s) referencing '" + dep_name + "'");
      }
    }
    // Only a member can have referenced it (see above); anything else in
    // the ledger would be a bookkeeping error, reported rather than dropped.
    if (!referenced_in_group && !reg->incoming.empty()) {
      return std::unexpected("keys of '" + name + "' are still referenced");
    }
  }
  // Pending removal steps capture these resources' tables: unregistering
  // (after which the module may destroy them) must wait until they ran.
  ReclaimRetiredLocked();
  // Objects already handed to RCU are destroyed by code in the resource's
  // module (the deleter it instantiated); it must not go before the last of
  // them has run.
  domain_.ReclaimReady();
  for (Registration *reg : group) {
    const std::string &name = reg->resource->name();
    if (reg->pending_removals != 0) {
      return std::unexpected("removals of '" + name +
                             "' are still waiting for readers");
    }
    if (const size_t left = reg->outstanding.load(std::memory_order_acquire);
        left != 0) {
      return std::unexpected(std::to_string(left) +
                             " retired object(s) of '" + name +
                             "' are still waiting for readers");
    }
  }
  for (Registration *reg : group) {
    reg->resource->registration_ = nullptr;
    auto res = resources_.find(std::string_view(reg->resource->name()));
    resources_.erase(res);
  }
  return {};
}

std::expected<void, std::string> TransactionEngine::ReleaseForTeardown(
    std::span<const std::string> names) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Pending removal steps capture these resources' tables, and objects handed
  // to RCU are destroyed by code in the resource's module: neither may outlive
  // the release (as for Unregister).
  ReclaimRetiredLocked();
  domain_.ReclaimReady();

  std::vector<Registration *> group;
  for (const std::string &name : names) {
    auto res = resources_.find(std::string_view(name));
    if (res == resources_.end()) {
      continue;  // already released, or never registered
    }
    if (std::find(group.begin(), group.end(), res->second.get()) ==
        group.end()) {
      group.push_back(res->second.get());
    }
  }
  auto in_group = [&](const Registration *r) {
    return std::find(group.begin(), group.end(), r) != group.end();
  };
  for (Registration *reg : group) {
    const std::string &name = reg->resource->name();
    if (reg->pending_removals != 0) {
      return std::unexpected("removals of '" + name +
                             "' are still waiting for readers");
    }
    if (const size_t left = reg->outstanding.load(std::memory_order_acquire);
        left != 0) {
      return std::unexpected(std::to_string(left) +
                             " retired object(s) of '" + name +
                             "' are still waiting for readers");
    }
  }

  // Declarations naming a released resource stay declared but unbound.
  // Surviving values may temporarily name missing keys; rebuilding counts
  // after the release, and again when a replacement registers, keeps their
  // references in the ledger without retaining the old resource's lifetime.
  for (Registration *reg : group) {
    const std::string &name = reg->resource->name();
    if (!reg->incoming.empty()) {
      LOG(WARNING) << "resource '" << name << "' is released with "
                   << reg->incoming.size()
                   << " key(s) still referenced: those references are now "
                      "dangling (they resolve to nothing)";
    }
    for (auto &[other_name, other] : resources_) {
      if (in_group(other.get())) {
        continue;
      }
      const size_t removed = std::erase_if(
          other->deps, [&](const auto &dep) { return dep.first == name; });
      if (removed != 0 &&
          std::find(other->unresolved.begin(), other->unresolved.end(),
                    name) == other->unresolved.end()) {
        other->unresolved.push_back(name);
      }
    }
  }
  for (Registration *reg : group) {
    reg->resource->registration_ = nullptr;
    resources_.erase(resources_.find(std::string_view(reg->resource->name())));
  }
  RebuildIncomingLocked();
  ranks_dirty_ = true;
  return {};
}

Resource *TransactionEngine::FindResource(std::string_view name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = resources_.find(name);
  return it == resources_.end() ? nullptr : it->second->resource;
}

std::vector<std::string> TransactionEngine::ResourceNames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> names;
  names.reserve(resources_.size());
  for (const auto &[name, reg] : resources_) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

uint64_t TransactionEngine::generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}

size_t TransactionEngine::ReferenceCount(const std::string &resource,
                                         const ResourceKey &key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto res = resources_.find(resource);
  if (res == resources_.end()) {
    return 0;
  }
  auto it = res->second->incoming.find(std::string_view(key));
  return it == res->second->incoming.end() ? 0 : it->second;
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
        Registration &owner = RegistrationOf(step.owner);
        retirer.owner_ = step.owner;
        retirer.outstanding_ = &owner.outstanding;
        step.fn(retirer);
        deferred_objects_--;
        owner.pending_removals--;
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
  if (failed < n_ops) {
    result.ops[failed] = OpResult{OpStatus::kFailed, std::move(error)};
  }
  return result;
}

TransactionEngine::Result TransactionEngine::Apply(
    std::span<const Op> ops, std::optional<uint64_t> expected_generation,
    Consistency consistency) {
  std::lock_guard<std::mutex> lock(mutex_);
  ReclaimRetiredLocked();  // frees ids whose removal has completed
  // Ranks come from the bound dependency graph, which registrations may have
  // changed since the last Apply (a referent registering after its referrer).
  RecomputeRanksLocked();
  const size_t n = ops.size();

  if (reference_cycle_) {
    return Reject(n, 0,
                  "registered resources declare a reference cycle; the engine "
                  "cannot order their publication");
  }

  if (expected_generation && *expected_generation != generation_) {
    Result result;
    result.outcome = Outcome::kConflict;
    result.generation = generation_;
    result.ops.resize(n);
    return result;
  }

  // Working storage: cleared, capacity kept from earlier transactions.
  Scratch &w = scratch_;
  w.reg.assign(n, nullptr);
  w.keys.clear();
  w.reservations.clear();
  w.reservations.reserve(n);  // stable elements: views point into them
  w.staged.clear();
  w.removals_by_op.assign(n, 0);
  w.deltas.clear();
  w.touched.clear();

  // -- structure: known resources, one operation per key --------------------
  for (size_t i = 0; i < n; i++) {
    const Op &op = ops[i];
    auto it = resources_.find(std::string_view(op.resource));
    if (it == resources_.end()) {
      return Reject(n, i, "unknown resource '" + op.resource + "'");
    }
    w.reg[i] = it->second.get();
    if (consistency == Consistency::kScopeSnapshot &&
        w.reg[i]->resource->ProvidedConsistency() !=
            Consistency::kScopeSnapshot) {
      // Not a rejection of the operation's content: this request can never
      // succeed against this resource. Said so, not served as referential.
      Result result = Reject(
          n, i,
          "resource '" + op.resource +
              "' cannot take part in a scope-snapshot transaction: its "
              "operations become visible one by one (referential)");
      result.outcome = Outcome::kUnsupported;
      return result;
    }
    if (op.kind == OpKind::kUpsert && !op.value.has_value()) {
      return Reject(n, i, "upsert without a value");
    }
    w.keys.push_back(KeyRef{w.reg[i], op.key, static_cast<uint32_t>(i)});
  }
  auto key_less = [](const KeyRef &a, const KeyRef &b) {
    return a.reg != b.reg ? a.reg < b.reg
                          : a.key != b.key ? a.key < b.key : a.op < b.op;
  };
  std::sort(w.keys.begin(), w.keys.end(), key_less);
  {
    // The first operation, in request order, that repeats a key.
    uint32_t duplicate = kNone;
    for (size_t k = 1; k < w.keys.size(); k++) {
      if (w.keys[k].reg == w.keys[k - 1].reg &&
          w.keys[k].key == w.keys[k - 1].key) {
        duplicate = std::min(duplicate, w.keys[k].op);
      }
    }
    if (duplicate != kNone) {
      return Reject(n, duplicate,
                    "second operation on " +
                        Describe(ops[duplicate].resource, ops[duplicate].key) +
                        " in one transaction");
    }
  }
  // The operation on (reg, key) in this transaction, if any.
  auto op_on = [&](Registration *reg, std::string_view key) -> uint32_t {
    auto it = std::lower_bound(
        w.keys.begin(), w.keys.end(), KeyRef{reg, key, 0}, key_less);
    return it != w.keys.end() && it->reg == reg && it->key == key ? it->op
                                                                  : kNone;
  };

  // -- reserve: all fallible work, nothing visible ---------------------------
  // Room for the abort bookkeeping first: once a Reserve() has placed
  // something (a pending key), recording it for abort must not be able to
  // throw (external review: a failed push_back leaked the key).
  w.staged.reserve(n);
  w.touched.reserve(n);
  AbortGuard abort([&] {
    for (auto it = w.staged.rbegin(); it != w.staged.rend(); ++it) {
      it->work->Abort();
    }
    for (Registration *r : w.touched) {
      r->touched = false;
      r->resource->EndTransaction();
    }
    for (const Delta &d : w.deltas) {
      if (d.change > 0) {
        auto it = d.target->incoming.find(d.key);
        if (it != d.target->incoming.end() && it->second == 0) {
          d.target->incoming.erase(it);
        }
      }
    }
  });

  Resource::Footprint footprint;  // summed over the transaction
  for (size_t i = 0; i < n; i++) {
    const Op &op = ops[i];
    Registration *reg = w.reg[i];
    if (!reg->touched) {
      w.touched.push_back(reg);  // cannot throw: reserved above
      reg->touched = true;
    }
    auto reserved = reg->resource->Reserve(op);
    if (!reserved) {
      return Reject(n, i, std::move(reserved.error()));
    }
    // Neither push can throw (both reserved for n): the reservation is
    // armed for abort before anything else that can fail runs.
    w.reservations.push_back(std::move(*reserved));
    Resource::Reservation &r = w.reservations.back();
    // Aborted with the rest from here on: any rejection below must undo
    // this operation's reservation too (a pending key, say).
    w.staged.push_back(Staged{i, std::move(r.staged)});
    if (op.kind == OpKind::kErase) {
      // Checked below even if no reference to it changes in this transaction.
      w.deltas.push_back(Delta{reg, op.key, 0, kNone});
    }
    if (r.existed) {
      for (const Reference &ref : r.previous_references) {
        Registration *target = Resolve(*reg, ref.resource);
        // A teardown release can leave a live referrer naming an unbound
        // resource. Erasing that referrer is allowed; its old reference has
        // no registered target whose incoming count needs decrementing.
        if (target != nullptr) {
          w.deltas.push_back(Delta{target, ref.key, -1, kNone});
        }
      }
    }
    if (op.kind == OpKind::kUpsert) {
      for (const Reference &ref : r.references) {
        // Only declared references: the publication order is derived from
        // the declarations, so an undeclared one could publish before its
        // referent.
        Registration *target = Resolve(*reg, ref.resource);
        if (target == nullptr) {
          return Reject(n, i,
                        "undeclared reference: '" + reg->resource->name() +
                            "' may not reference '" + ref.resource +
                            "' (declare it when constructing the resource)");
        }
        // Holds by construction (declared references are registered first
        // and rank below); checked because publication order depends on it.
        DCHECK_LT(target->resource->rank(), reg->resource->rank());
        if (!target->resource->DefersErase()) {
          return Reject(n, i,
                        "resource '" + ref.resource +
                            "' cannot be referenced: its erases take effect "
                            "at once (Resource::DefersErase)");
        }
        w.deltas.push_back(
            Delta{target, ref.key, +1, static_cast<uint32_t>(i)});
      }
    }
    footprint.retires += r.footprint.retires;
    footprint.removals += r.footprint.removals;
    footprint.callbacks += r.footprint.callbacks;
    w.removals_by_op[i] = r.footprint.removals;
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
  // Merge the deltas per referent: sorted, then one entry per (target, key)
  // with the summed change and the first operation that added a reference.
  std::sort(w.deltas.begin(), w.deltas.end(),
            [](const Delta &a, const Delta &b) {
              return a.target != b.target ? a.target < b.target
                                          : a.key < b.key;
            });
  {
    size_t out = 0;
    for (size_t k = 0; k < w.deltas.size(); k++) {
      if (out != 0 && w.deltas[out - 1].target == w.deltas[k].target &&
          w.deltas[out - 1].key == w.deltas[k].key) {
        w.deltas[out - 1].change += w.deltas[k].change;
        w.deltas[out - 1].added_by =
            std::min(w.deltas[out - 1].added_by, w.deltas[k].added_by);
      } else {
        w.deltas[out++] = w.deltas[k];
      }
    }
    w.deltas.resize(out);
  }
  for (const Delta &d : w.deltas) {
    auto current = d.target->incoming.find(d.key);
    const int64_t after =
        (current == d.target->incoming.end()
             ? 0
             : static_cast<int64_t>(current->second)) +
        d.change;
    if (after <= 0) {
      continue;
    }
    const uint32_t op = op_on(d.target, d.key);
    const bool exists_after = op != kNone
                                  ? ops[op].kind == OpKind::kUpsert
                                  : d.target->resource->Contains(
                                        ResourceKey(d.key));
    if (exists_after) {
      continue;
    }
    if (op != kNone) {
      // The transaction erases a key that is still referenced afterwards.
      return Reject(n, op,
                    Describe(d.target->resource->name(), d.key) +
                        " is still referenced " + std::to_string(after) +
                        " time(s)");
    }
    if (d.added_by != kNone) {
      return Reject(n, d.added_by,
                    "references missing " +
                        Describe(d.target->resource->name(), d.key));
    }
  }

  // -- commit bookkeeping, allocated before anything is visible -------------
  w.order.resize(w.staged.size());
  for (size_t i = 0; i < w.order.size(); i++) {
    w.order[i] = i;
  }
  std::stable_sort(w.order.begin(), w.order.end(), [&](size_t a, size_t b) {
    const Op &x = ops[w.staged[a].op];
    const Op &y = ops[w.staged[b].op];
    const bool xu = x.kind == OpKind::kUpsert;
    const bool yu = y.kind == OpKind::kUpsert;
    if (xu != yu) {
      return xu;  // upserts before erases
    }
    const int rx = w.reg[w.staged[a].op]->resource->rank();
    const int ry = w.reg[w.staged[b].op]->resource->rank();
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
  // The removal cascade's skeleton, from the declared removals: one stage
  // per rank, highest first, sized exactly.
  std::vector<int> stage_ranks;
  Cascade cascade{0, {}, 0};
  if (footprint.removals != 0) {
    for (size_t i = 0; i < n; i++) {
      if (w.removals_by_op[i] != 0) {
        stage_ranks.push_back(w.reg[i]->resource->rank());
      }
    }
    std::sort(stage_ranks.begin(), stage_ranks.end(), std::greater<int>());
    stage_ranks.erase(std::unique(stage_ranks.begin(), stage_ranks.end()),
                      stage_ranks.end());
    cascade.stages.resize(stage_ranks.size());
    for (size_t s_i = 0; s_i < stage_ranks.size(); s_i++) {
      size_t steps = 0;
      for (size_t i = 0; i < n; i++) {
        if (w.reg[i]->resource->rank() == stage_ranks[s_i]) {
          steps += w.removals_by_op[i];
        }
      }
      cascade.stages[s_i].reserve(steps);
    }
    cascades_.reserve(cascades_.size() + 1);
  }

  // Ledger entries for new references, so publishing only updates counts.
  for (const Delta &d : w.deltas) {
    if (d.change > 0) {
      d.target->incoming.try_emplace(std::string(d.key), 0);
    }
  }

  // -- publish: from here on nothing may fail, and nothing allocates --------
  if (internal::g_publish_window_hook != nullptr) {
    internal::g_publish_window_hook(true);
  }
  retirer.enforce_ = true;
  for (size_t i : w.order) {
    Registration *owner = w.reg[w.staged[i].op];
    retirer.rank_ = owner->resource->rank();
    retirer.owner_ = owner->resource;
    retirer.owner_name_ = owner->resource->name().c_str();
    retirer.outstanding_ = &owner->outstanding;
    w.staged[i].work->Publish(retirer);
  }
  retirer.enforce_ = false;
  abort.Dismiss();
  for (const Delta &d : w.deltas) {
    auto it = d.target->incoming.find(d.key);
    if (it == d.target->incoming.end()) {
      continue;  // change <= 0 for a key with no references: nothing to record
    }
    const int64_t after = static_cast<int64_t>(it->second) + d.change;
    if (after <= 0) {
      d.target->incoming.erase(it);
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
    RegistrationOf(removal.owner).pending_removals++;
    deferred_objects_++;
  }
  retirer.removals_.clear();
  generation_++;

  // -- committed: resources' own cleanup and asynchronous reclamation --------
  for (Registration *r : w.touched) {
    r->touched = false;
    r->resource->EndTransaction();
  }
  w.touched.clear();
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
