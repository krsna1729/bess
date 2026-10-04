// SPDX-License-Identifier: BSD-3-Clause

#include "route/router.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "utils/logging.h"

#include "dataplane/slot_resource.h"
#include "dataplane/transaction_engine.h"

namespace bess::route {

namespace {

// Room for one more element, grown geometrically: the push_back that follows
// a commit cannot then fail.
template <typename T>
void ReserveOneMore(std::vector<T> &v) {
  if (v.size() == v.capacity()) {
    v.reserve(v.empty() ? 4 : 2 * v.capacity());
  }
}

}  // namespace

std::expected<std::unique_ptr<Router>, RouteError> Router::Create(
    std::string name, const Config &config, size_t max_next_hops,
    rcu::RcuDomain &domain, size_t max_domains, size_t max_groups) {
  if (max_next_hops == 0 || max_next_hops > LpmRouteTable::kMaxValue ||
      max_groups > LpmRouteTable::kMaxValue - max_next_hops) {
    return std::unexpected(RouteError::kValueOutOfRange);
  }
  if (max_domains == 0 || max_domains > kMaxRouteDomains) {
    return std::unexpected(RouteError::kValueOutOfRange);
  }
  auto routes = LpmRouteTable::Create(name, config, domain);
  if (!routes) {
    return std::unexpected(routes.error());
  }
  return std::unique_ptr<Router>(new Router(
      std::move(name), std::move(*routes), max_next_hops, max_domains,
      max_groups, domain));
}

Router::Router(std::string name, std::unique_ptr<LpmRouteTable> default_routes,
               size_t max_next_hops, size_t max_domains, size_t max_groups,
               rcu::RcuDomain &rcu)
    : name_(name),
      rcu_(rcu),
      max_domains_(max_domains),
      max_next_hops_(static_cast<uint32_t>(max_next_hops)),
      domains_(max_domains),
      default_routes_(default_routes.get()),
      domain_ids_{kDefaultRouteDomainId},
      next_hops_(max_next_hops),
      references_(max_next_hops + 1, 0),
      // SlotTable needs a capacity of at least one; a router without groups
      // never publishes into it.
      groups_(std::max<size_t>(max_groups, 1)),
      group_references_(max_groups + 1, 0),
      member_references_(max_next_hops + 1, 0),
      next_hops_name_(name + "/next_hops"),
      routes_name_(name + "/routes"),
      groups_name_(name + "/groups") {
  // Before any reader can exist: nothing to retire.
  domains_.Publish(SlotOf(kDefaultRouteDomainId),
                   std::make_unique<const Domain>(std::move(default_routes)));
}

// The SlotTables free what is still published (including retiring next hops
// and every domain's FIB) when they go: the owner destroys a Router only once
// no reader can use it.
Router::~Router() {
  if (engine_ != nullptr) {
    // Routes reference groups and next hops, groups reference next hops: they
    // leave together with their contents; with readers gone, pending removals
    // complete in the call.
    std::vector<std::string> names = {routes_name_};
    if (groups_res_ != nullptr) {
      names.push_back(groups_name_);
    }
    names.push_back(next_hops_name_);
    auto unregistered = engine_->Unregister(names);
    CHECK(unregistered) << unregistered.error();
  }
}

// "<router>/routes" (D-023): key Router::RouteKey(domain, prefix), value
// NextHopId or NextHopGroupId; each route references what it names in
// "<router>/next_hops" or "<router>/groups" (the FIB value max_next_hops + id
// is a group, D-065). A
// root: erasing a route takes effect at once (rte_lpm deletes in place), so
// nothing may reference a route. One resource covers every domain: the domain
// is part of the key, so the same prefix in two domains is two routes, and a
// single transaction can change several domains' routes. The set of domains
// is frozen while enrolled, so the FIB pointers this resource holds stay
// valid.
//
// rte_lpm cannot promise capacity (rules and tbl8 groups) for a set of
// prefixes in advance, so a new route is placed during Reserve() with a
// placeholder value: what its addresses resolve to today (the covering
// route, else the default, else id 0, a miss -- LpmRouteTable::
// CoveringValue). Readers see no change until Publish() stores the real
// next hop -- an in-place update of an existing rule, which cannot fail.
// A prefix that does not fit rejects the transaction with nothing visible;
// Abort() deletes the placeholder (deletes cannot fail: the tbl8 defer
// queue holds every group, pinned by RouteTest). Placed prefixes are
// "pending" to Contains()/ReferencesOf() until the transaction ends.
class Router::RouteResource final : public dataplane::Resource {
 public:
  explicit RouteResource(Router &router, bool groups)
      : Resource(router.routes_name_,
                 groups ? std::vector<std::string>{router.next_hops_name_, router.groups_name_}
                        : std::vector<std::string>{router.next_hops_name_}),
        router_(router),
        groups_(groups) {}

  size_t LiveCount() const override { return router_.CountRoutes(); }

  bool Contains(const dataplane::ResourceKey &key) const override {
    const auto id = Decode(key);
    return id && Find(*id).has_value();
  }

  std::vector<dataplane::Reference> ReferencesOf(
      const dataplane::ResourceKey &key) const override {
    const auto id = Decode(key);
    const auto value = id ? Find(*id) : std::nullopt;
    return value ? References(*value) : std::vector<dataplane::Reference>{};
  }
  void VisitReferences(
      const std::function<void(const dataplane::Reference &)> &visit) const
      override {
    router_.ForEachDomain([&](RouteDomainId, LpmRouteTable &routes) {
      routes.ForEach([&](Ipv4Prefix, uint32_t value) {
        for (const auto &ref : References(value)) {
          visit(ref);
        }
      });
    });
  }


  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    const auto id = Decode(op.key);
    if (!id) {
      return std::unexpected(
          "not a route key (Router::RouteKey) of a valid prefix");
    }
    LpmRouteTable *table = router_.TableOf(id->domain);
    const Ipv4Prefix prefix = id->prefix;
    const std::optional<uint32_t> previous = Find(*id);
    if (op.kind == dataplane::OpKind::kErase) {
      if (!previous) {
        return std::unexpected("not found");
      }
      Reservation erase{std::make_unique<EraseOp>(*table, prefix), {}};
      erase.existed = true;
      erase.previous_references = References(*previous);
      return erase;
    }
    uint32_t value = 0;  // the FIB value: a next-hop id, or max_next_hops + group id
    if (const NextHopId *hop = std::any_cast<NextHopId>(&op.value)) {
      if (!router_.ValidId(*hop)) {
        return std::unexpected(RouteErrorName(RouteError::kInvalidId));
      }
      value = hop->value();
    } else if (const NextHopGroupId *group = std::any_cast<NextHopGroupId>(&op.value)) {
      if (!groups_ || !router_.ValidGroupId(*group)) {
        return std::unexpected(RouteErrorName(RouteError::kInvalidId));
      }
      value = router_.max_next_hops_ + group->value();
    } else {
      return std::unexpected("wrong value type (want NextHopId or NextHopGroupId)");
    }
    if (table == nullptr) {
      return std::unexpected(RouteErrorName(RouteError::kUnknownDomain));
    }
    // Everything that can throw first; placing the prefix is the last step.
    auto upsert = std::make_unique<UpsertOp>(*table, prefix, value);
    Reservation reservation{nullptr, References(value)};
    reservation.existed = previous.has_value();
    if (previous) {
      reservation.previous_references = References(*previous);
    } else if (prefix.length() != 0) {
      pending_.insert(*id);  // may throw: before anything is placed
      const uint32_t placeholder = table->CoveringValue(prefix).value_or(0);
      if (auto placed = table->Upsert(prefix, placeholder); !placed) {
        pending_.erase(*id);
        return std::unexpected(RouteErrorName(placed.error()));
      }
      upsert->MarkPlaced();  // Abort() now deletes it
    }
    reservation.staged = std::move(upsert);
    return reservation;
  }

  void EndTransaction() noexcept override { pending_.clear(); }

 private:
  static std::optional<::bess::route::RouteKey> Decode(
      const dataplane::ResourceKey &key) {
    uint64_t raw = 0;
    if (!dataplane::DecodeKey(key, &raw)) {
      return std::nullopt;
    }
    const auto prefix = Ipv4Prefix::Make(static_cast<uint32_t>(raw >> 8),
                                         static_cast<uint8_t>(raw & 0xff));
    if (!prefix) {
      return std::nullopt;
    }
    return ::bess::route::RouteKey{
        RouteDomainId(static_cast<uint32_t>(raw >> 40)), *prefix};
  }

  // The route's FIB value.
  std::optional<uint32_t> Find(const ::bess::route::RouteKey &id) const {
    if (pending_.contains(id)) {
      return std::nullopt;
    }
    const LpmRouteTable *table = router_.TableOf(id.domain);
    if (table == nullptr) {
      return std::nullopt;
    }
    return table->Find(id.prefix);
  }

  std::vector<dataplane::Reference> References(uint32_t value) const {
    if (value > router_.max_next_hops_) {
      return {{router_.groups_name_,
               dataplane::EncodeKey(NextHopGroupId(value - router_.max_next_hops_))}};
    }
    return {{router_.next_hops_name_, dataplane::EncodeKey(NextHopId(value))}};
  }

  class UpsertOp final : public dataplane::StagedOp {
   public:
    UpsertOp(LpmRouteTable &table, Ipv4Prefix prefix, uint32_t value)
        : table_(table), prefix_(prefix), value_(value) {}
    void MarkPlaced() noexcept { placed_ = true; }
    void Publish(dataplane::Retirer &) noexcept override {
      // The prefix is present (placed in Reserve(), or already a route): an
      // in-place value store, which cannot fail.
      auto set = table_.Upsert(prefix_, value_);
      CHECK(set) << RouteErrorName(set.error());
    }
    void Abort() noexcept override {
      if (placed_) {
        auto erased = table_.Erase(prefix_);
        CHECK(erased) << RouteErrorName(erased.error());
      }
    }

   private:
    LpmRouteTable &table_;
    Ipv4Prefix prefix_;
    uint32_t value_;
    bool placed_ = false;
  };

  class EraseOp final : public dataplane::StagedOp {
   public:
    EraseOp(LpmRouteTable &table, Ipv4Prefix prefix)
        : table_(table), prefix_(prefix) {}
    void Publish(dataplane::Retirer &) noexcept override {
      auto erased = table_.Erase(prefix_);
      CHECK(erased) << RouteErrorName(erased.error());
    }

   private:
    LpmRouteTable &table_;
    Ipv4Prefix prefix_;
  };

  Router &router_;
  const bool groups_;  // the router registered "<name>/groups"
  std::set<::bess::route::RouteKey> pending_;  // placed in this transaction
};

// "<router>/groups": a SlotResource of immutable groups, each referencing its
// distinct members (a repeated member, a weight, is one reference), so a group
// cannot name a next hop that does not exist and a member cannot be removed
// while a group names it. Reserve also checks the id and the member count,
// before the base reads the members.
class Router::GroupResource final
    : public dataplane::SlotResource<NextHopGroupId, NextHopGroup> {
 public:
  explicit GroupResource(Router &router)
      : SlotResource(router.groups_name_, router.groups_,
                     [hops = router.next_hops_name_](const NextHopGroup &group) {
                       std::vector<NextHopId> members(group.members.begin(),
                                                      group.members.begin() + group.size);
                       std::sort(members.begin(), members.end());
                       members.erase(std::unique(members.begin(), members.end()), members.end());
                       std::vector<dataplane::Reference> refs;
                       refs.reserve(members.size());
                       for (const NextHopId m : members) {
                         refs.push_back({hops, dataplane::EncodeKey(m)});
                       }
                       return refs;
                     },
                     {router.next_hops_name_}),
        router_(router) {}

  std::expected<Reservation, std::string> Reserve(const dataplane::Op &op) override {
    if (op.kind == dataplane::OpKind::kUpsert) {
      NextHopGroupId id;
      if (!dataplane::DecodeKey(op.key, &id) || !router_.ValidGroupId(id)) {
        return std::unexpected(RouteErrorName(RouteError::kInvalidId));
      }
      const NextHopGroup *group = std::any_cast<NextHopGroup>(&op.value);
      if (group == nullptr) {
        return std::unexpected("wrong value type (want NextHopGroup)");
      }
      if (group->size == 0 || group->size > NextHopGroup::kMaxMembers) {
        return std::unexpected("a group has 1 to " + std::to_string(NextHopGroup::kMaxMembers) +
                               " members");
      }
    }
    return SlotResource::Reserve(op);
  }

 private:
  Router &router_;
};

std::expected<void, std::string> Router::Enroll(
    dataplane::TransactionEngine &engine) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected("already enrolled");
  }
  if (CompleteRetirementsLocked() != 0) {
    return std::unexpected("removed next hops are still retiring");
  }
  if (CountRoutes() != 0) {
    return std::unexpected("enroll before adding routes");
  }
  if (groups_.size() != 0 || !retiring_groups_.empty()) {
    return std::unexpected("enroll before adding next-hop groups");
  }
  const bool with_groups = group_references_.size() > 1;  // max_groups > 0
  auto hops = std::make_unique<dataplane::SlotResource<NextHopId, NextHop>>(
      next_hops_name_, next_hops_);
  std::unique_ptr<GroupResource> groups =
      with_groups ? std::make_unique<GroupResource>(*this) : nullptr;
  auto routes = std::make_unique<RouteResource>(*this, with_groups);
  if (auto r = engine.Register(hops.get()); !r) {
    return r;
  }
  if (groups != nullptr) {
    if (auto r = engine.Register(groups.get()); !r) {
      CHECK(engine.Unregister(next_hops_name_));
      return r;
    }
  }
  if (auto r = engine.Register(routes.get()); !r) {
    if (groups != nullptr) {
      CHECK(engine.Unregister(groups_name_));
    }
    CHECK(engine.Unregister(next_hops_name_));
    return r;
  }
  engine_ = &engine;
  next_hops_res_ = std::move(hops);
  groups_res_ = std::move(groups);
  routes_res_ = std::move(routes);
  return {};
}

std::expected<void, std::string> Router::Release() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ == nullptr) {
    return {};  // not enrolled, or already released
  }
  std::vector<std::string> names = {routes_name_};
  if (groups_res_ != nullptr) {
    names.push_back(groups_name_);
  }
  names.push_back(next_hops_name_);
  auto released = engine_->ReleaseForTeardown(names);
  if (!released) {
    return released;
  }
  engine_ = nullptr;
  next_hops_res_.reset();
  groups_res_.reset();
  routes_res_.reset();
  return {};
}

size_t Router::CompleteRetirementsLocked() {
  // Room first, for everything that may complete: an object unpublished
  // below must reach the retire queue, never be dropped by a failed push.
  std::vector<std::unique_ptr<const NextHopGroup>> emptied_groups;
  std::vector<std::unique_ptr<const NextHop>> emptied;
  emptied_groups.reserve(retiring_groups_.size());
  emptied.reserve(retiring_.size());
  rcu_.ReserveRetirements(retiring_groups_.size() + retiring_.size());
  // Groups first: a retired group's members stay referenced (so the next
  // hops stay published) until no reader can still select from it.
  std::erase_if(retiring_groups_, [&](const RetiringGroup &r) {
    if (!rcu_.IsComplete(r.token)) {
      return false;
    }
    auto group = groups_.Unpublish(r.id);
    for (uint32_t i = 0; i < group->size; i++) {
      member_references_[group->members[i].value()]--;
    }
    emptied_groups.push_back(std::move(group));
    return true;
  });
  if (!emptied_groups.empty()) {
    const rcu::GracePeriod token = rcu_.StartGracePeriod();
    for (auto &group : emptied_groups) {
      rcu_.Retire(token, std::move(group));
    }
  }

  std::erase_if(retiring_, [&](const Retiring &r) {
    if (!rcu_.IsComplete(r.token)) {
      return false;
    }
    // No reader can still obtain the id from a route; one may have just
    // looked the object up, so it is freed after one more grace period.
    emptied.push_back(next_hops_.Unpublish(r.id));
    return true;
  });
  if (!emptied.empty()) {
    const rcu::GracePeriod token = rcu_.StartGracePeriod();
    for (auto &hop : emptied) {
      rcu_.Retire(token, std::move(hop));
    }
    rcu_.ReclaimReady();
  }
  return retiring_.size() + retiring_groups_.size();
}

size_t Router::ReclaimRetired() {
  std::lock_guard<std::mutex> lock(mutex_);
  return CompleteRetirementsLocked();
}

std::expected<void, RouteError> Router::SetNextHop(NextHopId id,
                                                   const NextHop &hop) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  if (!ValidId(id)) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (!next_hops_.CanPublish(id)) {
    return std::unexpected(RouteError::kNextHopRetiring);
  }
  // One pointer store; a reader holding the previous object keeps it until
  // its next quiescent state. Room in the retire queue first: once the new
  // object is visible nothing may fail.
  auto hop_object = std::make_unique<const NextHop>(hop);
  rcu_.ReserveRetirements(1);
  auto replaced = next_hops_.Publish(id, std::move(hop_object));
  if (replaced) {
    rcu_.Retire(rcu_.StartGracePeriod(), std::move(replaced));
    rcu_.ReclaimReady();
  }
  return {};
}

std::expected<void, RouteError> Router::RemoveNextHop(NextHopId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  if (!ValidId(id)) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (!next_hops_.Contains(id)) {
    return std::unexpected(RouteError::kUnknownNextHop);
  }
  if (references_[id.value()] != 0 || member_references_[id.value()] != 0) {
    return std::unexpected(RouteError::kNextHopInUse);
  }
  // No route names `id` any more, but a reader may have looked one up just
  // before the last such route went and not yet loaded the next hop. Keep it
  // published until every reader has passed a quiescent state; a later
  // control call empties the slot. No waiting here (a stalled worker must not
  // stall the command path, and a transaction must not hold a blocking wait).
  ReserveOneMore(retiring_);
  next_hops_.Retire(id);
  retiring_.push_back({id, rcu_.StartGracePeriod()});
  return {};
}

std::expected<void, RouteError> Router::CreateDomain(RouteDomainId domain,
                                                     const Config &config) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  const DomainSlot slot = SlotOf(domain);
  if (!domains_.ValidId(slot)) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (domains_.Contains(slot)) {
    return std::unexpected(RouteError::kDomainExists);
  }
  auto table = LpmRouteTable::Create(
      name_ + "_d" + std::to_string(domain.value()), config, rcu_);
  if (!table) {
    return std::unexpected(table.error());
  }
  // Fully built before the one store that makes it visible; nothing was
  // published at this slot, so there is nothing to retire.
  auto object = std::make_unique<const Domain>(std::move(*table));
  ReserveOneMore(domain_ids_);
  domains_.Publish(slot, std::move(object));
  domain_ids_.insert(
      std::lower_bound(domain_ids_.begin(), domain_ids_.end(), domain), domain);
  return {};
}

std::expected<void, RouteError> Router::RemoveDomain(RouteDomainId domain) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  if (domain == kDefaultRouteDomainId) {
    return std::unexpected(RouteError::kDomainInUse);
  }
  const DomainSlot slot = SlotOf(domain);
  if (!domains_.Contains(slot)) {
    return std::unexpected(RouteError::kUnknownDomain);
  }
  if (TableOf(domain)->size() != 0) {
    return std::unexpected(RouteError::kDomainInUse);
  }
  // A reader that already loaded the Domain keeps it (and its empty FIB)
  // until its next quiescent state; a later lookup misses. No domain id is
  // handed out by a referrer, so the slot can be emptied at once.
  rcu_.ReserveRetirements(1);
  domains_.Retire(slot);
  // Unpublished first, then the grace period that covers it (as RcuPtr does).
  auto removed = domains_.Unpublish(slot);
  rcu_.Retire(rcu_.StartGracePeriod(), std::move(removed));
  domain_ids_.erase(
      std::lower_bound(domain_ids_.begin(), domain_ids_.end(), domain));
  rcu_.ReclaimReady();
  return {};
}

size_t Router::domain_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return domain_ids_.size();
}

std::expected<void, RouteError> Router::SetRoute(RouteDomainId domain,
                                                 Ipv4Prefix prefix,
                                                 NextHopId hop) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  LpmRouteTable *routes = TableOf(domain);
  if (routes == nullptr) {
    return std::unexpected(RouteError::kUnknownDomain);
  }
  if (!ValidId(hop)) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (!next_hops_.Contains(hop)) {
    return std::unexpected(RouteError::kUnknownNextHop);
  }
  const std::optional<uint32_t> previous = routes->Find(prefix);
  if (auto set = routes->Upsert(prefix, static_cast<uint32_t>(hop.value()));
      !set) {
    return set;
  }
  Reference(hop.value());
  if (previous) {
    Unreference(*previous);
  }
  return {};
}

std::expected<void, RouteError> Router::SetRoute(RouteDomainId domain,
                                                 Ipv4Prefix prefix,
                                                 NextHopGroupId group) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  LpmRouteTable *routes = TableOf(domain);
  if (routes == nullptr) {
    return std::unexpected(RouteError::kUnknownDomain);
  }
  if (!groups_.ValidId(group) || group.value() >= group_references_.size()) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (!groups_.Contains(group)) {
    return std::unexpected(RouteError::kUnknownNextHop);
  }
  // The group was published (release store) before this route can be.
  const uint32_t value = max_next_hops_ + group.value();
  const std::optional<uint32_t> previous = routes->Find(prefix);
  if (auto set = routes->Upsert(prefix, value); !set) {
    return set;
  }
  Reference(value);
  if (previous) {
    Unreference(*previous);
  }
  return {};
}

void Router::Reference(uint32_t value) noexcept {
  if (value > max_next_hops_) {
    group_references_[value - max_next_hops_]++;
  } else {
    references_[value]++;
  }
}

void Router::Unreference(uint32_t value) noexcept {
  if (value > max_next_hops_) {
    group_references_[value - max_next_hops_]--;
  } else {
    references_[value]--;
  }
}

std::expected<void, RouteError> Router::SetNextHopGroup(
    NextHopGroupId id, std::span<const NextHopId> members) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  if (!groups_.ValidId(id) || id.value() >= group_references_.size() ||
      members.empty() || members.size() > NextHopGroup::kMaxMembers) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (!groups_.CanPublish(id)) {
    return std::unexpected(RouteError::kNextHopRetiring);
  }
  auto group = std::make_unique<NextHopGroup>();
  group->size = static_cast<uint32_t>(members.size());
  for (size_t i = 0; i < members.size(); i++) {
    if (!ValidId(members[i])) {
      return std::unexpected(RouteError::kInvalidId);
    }
    if (!next_hops_.Contains(members[i])) {
      return std::unexpected(RouteError::kUnknownNextHop);
    }
    group->members[i] = members[i];
  }
  rcu_.ReserveRetirements(1);  // for the group this one may replace
  for (const NextHopId m : members) {
    member_references_[m.value()]++;
  }
  // One pointer store; the members were published before.
  auto replaced = groups_.Publish(id, std::unique_ptr<const NextHopGroup>(std::move(group)));
  if (replaced) {
    // The old members are released now. A reader may still select from the
    // old object until its quiescent state, but a member removed after this
    // retires with a grace period that starts later, so it stays published
    // until such readers are gone (the argument that lets RemoveRoute release
    // its next hop at once).
    for (uint32_t i = 0; i < replaced->size; i++) {
      member_references_[replaced->members[i].value()]--;
    }
    rcu_.Retire(rcu_.StartGracePeriod(), std::move(replaced));
    rcu_.ReclaimReady();
  }
  return {};
}

std::expected<void, RouteError> Router::RemoveNextHopGroup(NextHopGroupId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  if (!groups_.ValidId(id) || id.value() >= group_references_.size()) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (!groups_.Contains(id)) {
    return std::unexpected(RouteError::kUnknownNextHop);
  }
  if (group_references_[id.value()] != 0) {
    return std::unexpected(RouteError::kNextHopInUse);
  }
  // As RemoveNextHop: readable until a grace period passes.
  ReserveOneMore(retiring_groups_);
  groups_.Retire(id);
  retiring_groups_.push_back({id, rcu_.StartGracePeriod()});
  return {};
}

size_t Router::next_hop_group_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return groups_.size();
}

size_t Router::GroupReferences(NextHopGroupId id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr && groups_res_ != nullptr) {
    return engine_->ReferenceCount(groups_name_, dataplane::EncodeKey(id));
  }
  return ValidGroupId(id) ? group_references_[id.value()] : 0;
}

// Enrolled, the engine counts references per (group, member) once, not per
// member position; the direct API counts positions.
size_t Router::GroupMemberships(NextHopId id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr && groups_res_ != nullptr) {
    size_t n = 0;
    groups_res_->VisitReferences([&](const dataplane::Reference &ref) {
      n += ref.key == dataplane::EncodeKey(id) ? 1 : 0;
    });
    return n;
  }
  return ValidId(id) ? member_references_[id.value()] : 0;
}

std::expected<void, RouteError> Router::RemoveRoute(RouteDomainId domain,
                                                    Ipv4Prefix prefix) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  LpmRouteTable *routes = TableOf(domain);
  if (routes == nullptr) {
    return std::unexpected(RouteError::kUnknownDomain);
  }
  const std::optional<uint32_t> previous = routes->Find(prefix);
  if (!previous) {
    return std::unexpected(RouteError::kNotFound);
  }
  if (auto erased = routes->Erase(prefix); !erased) {
    return erased;
  }
  Unreference(*previous);
  return {};
}

std::expected<void, RouteError> Router::ReplaceRouteSetAtomic(
    RouteDomainId domain, const RouteSet &routes) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  LpmRouteTable *table = TableOf(domain);
  if (table == nullptr) {
    return std::unexpected(RouteError::kUnknownDomain);
  }

  // 1. Everything checkable without building: each route's next hop exists.
  std::map<Ipv4Prefix, NextHopId> wanted;  // a repeated prefix: last entry wins
  for (const RouteEntry &entry : routes) {
    if (!ValidId(entry.hop)) {
      return std::unexpected(RouteError::kInvalidId);
    }
    if (!next_hops_.Contains(entry.hop)) {
      return std::unexpected(RouteError::kUnknownNextHop);
    }
    wanted[entry.prefix] = entry.hop;
  }
  std::vector<LpmRouteTable::Rule> rules;
  rules.reserve(wanted.size());
  for (const auto &[prefix, hop] : wanted) {
    rules.push_back({prefix, static_cast<uint32_t>(hop.value())});
  }
  // What the old set referenced, taken before it is replaced.
  std::vector<uint32_t> released;
  table->ForEach([&](Ipv4Prefix, uint32_t hop) { released.push_back(hop); });

  // 2. Build the replacement FIB off to the side and publish it once. Rule and
  // tbl8 capacity can only be settled by building (rte_lpm cannot predict it),
  // so this is where a shortage surfaces -- with the old set still published.
  if (auto replaced = table->ReplaceAll(rules); !replaced) {
    return replaced;
  }

  // 3. Published; nothing below can fail. The counts now match the new set.
  for (const uint32_t value : released) {
    Unreference(value);  // the old set may have named groups
  }
  for (const auto &entry : wanted) {
    references_[entry.second.value()]++;
  }
  return {};
}

// Callers hold mutex_, or the router is enrolled (domains frozen), or have
// exclusive access (Enroll's check).
size_t Router::CountRoutes() const {
  size_t count = 0;
  ForEachDomain([&](RouteDomainId, LpmRouteTable &routes) {
    count += routes.size();
  });
  return count;
}

size_t Router::route_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return CountRoutes();
}

size_t Router::route_count(RouteDomainId domain) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const LpmRouteTable *routes = TableOf(domain);
  return routes != nullptr ? routes->size() : 0;
}

size_t Router::next_hop_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return next_hops_.size();  // live, not counting retiring ones
}

size_t Router::RouteReferences(NextHopId id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    if (groups_res_ == nullptr) {
      return engine_->ReferenceCount(next_hops_name_, dataplane::EncodeKey(id));
    }
    // Groups reference next hops too: count the routes' references only.
    const dataplane::Reference wanted{next_hops_name_, dataplane::EncodeKey(id)};
    size_t n = 0;
    routes_res_->VisitReferences([&](const dataplane::Reference &ref) { n += ref == wanted; });
    return n;
  }
  return ValidId(id) ? references_[id.value()] : 0;
}

}  // namespace bess::route
