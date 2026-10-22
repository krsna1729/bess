// SPDX-License-Identifier: BSD-3-Clause

#include "route/router.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <cstring>
#include <vector>

#include "utils/logging.h"

#include "dataplane/slot_resource.h"
#include "dataplane/resource_registry.h"

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
  // The largest route value is the last group's: the IPv6 entry size follows.
  Config sized = config;
  if (sized.ipv6) {
    sized.ipv6->max_value = static_cast<uint32_t>(max_next_hops + max_groups);
  }
  auto default_domain = MakeDomain(name, sized, domain);
  if (!default_domain) {
    return std::unexpected(default_domain.error());
  }
  return std::unique_ptr<Router>(new Router(name, sized,
                                            std::move(*default_domain),
                                            max_next_hops, max_domains,
                                            max_groups, domain));
}

std::expected<std::unique_ptr<const Router::Domain>, RouteError>
Router::MakeDomain(const std::string &name, const Config &config,
                   rcu::RcuDomain &rcu) {
  std::unique_ptr<LpmRouteTable> table;
  if (config.ipv4) {
    auto made = LpmRouteTable::Create(name, *config.ipv4, rcu);
    if (!made) {
      return std::unexpected(made.error());
    }
    table = std::move(*made);
  }
  std::unique_ptr<Ipv6RouteTable> table6;
  if (config.ipv6) {
    auto made = Ipv6RouteTable::Create(name + "_6", *config.ipv6, rcu);
    if (!made) {
      return std::unexpected(made.error());
    }
    table6 = std::move(*made);
  }
  return std::make_unique<const Domain>(std::move(table), std::move(table6));
}

Router::Router(const std::string &name, const Config &config,
               std::unique_ptr<const Domain> default_domain,
               size_t max_next_hops, size_t max_domains, size_t max_groups,
               rcu::RcuDomain &rcu)
    : name_(name),
      rcu_(rcu),
      max_domains_(max_domains),
      max_next_hops_(static_cast<uint32_t>(max_next_hops)),
      domain_config_(config),
      domains_(max_domains),
      default_routes_(default_domain->table()),
      domain_ids_{kDefaultRouteDomainId},
      next_hops_(max_next_hops),
      references_(max_next_hops + 1, 0),
      // SlotTable needs a capacity of at least one; a router without groups
      // never publishes into it.
      groups_(std::max<size_t>(max_groups, 1)),
      group_references_(max_groups + 1, 0),
      member_references_(max_next_hops + 1, 0),
      next_hops_name_(name + "/next_hops"),
      neighbors_name_(name + "/neighbors"),
      routes_name_(name + "/routes"),
      groups_name_(name + "/groups"),
      bindings_(max_next_hops + 1) {
  // Before any reader can exist: nothing to retire.
  domains_.Publish(SlotOf(kDefaultRouteDomainId), std::move(default_domain));
}

// The SlotTables free what is still published (including retiring next hops
// and every domain's FIB) when they go: the owner destroys a Router only once
// no reader can use it.
Router::~Router() {
  if (engine_ != nullptr) {
    // Routes reference groups, bindings and next hops, groups reference
    // bindings and next hops: they leave together with their contents; with
    // readers gone, pending removals complete in the call.
    std::vector<std::string> names = {routes_name_, groups_name_, neighbors_name_,
                                      next_hops_name_};
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
// Neither backend can promise capacity (rules and tbl8 groups) for a set of
// prefixes in advance, so a new route is placed during Reserve() with a
// placeholder value: what its addresses resolve to today (the covering
// route, else the default, else id 0, a miss -- LpmRouteTable::
// CoveringValue). Readers see no change until Publish() stores the real
// next hop -- an in-place update of an existing rule, which cannot fail.
// A prefix that does not fit rejects the transaction with nothing visible;
// Abort() deletes the placeholder. Updates and deletes of existing rules
// need no tbl8 group (rte_fib: deps/dpdk-patches/0005, D-132), and the
// defer queue holds every freed group; RouteTableTest pins both. Placed
// prefixes are "pending" to Contains()/ReferencesOf() until the transaction
// ends.
//
// IPv6 routes (Ipv6RouteTable) are never placed: the first IPv6 operation of
// a transaction on a domain begins that table's Stage, which readies the
// table's unpublished spare instance, and it and every later IPv6 operation
// on the same table edit the spare in Reserve(), where all allocation, the
// max_routes bound and every tbl8 group are settled. The first of them to
// publish commits the stage -- one pointer exchange -- and hands the table
// the transaction's grace period (AfterGracePeriodStarts), which guards
// reuse of the instance it replaced; the rest find it done. That publishes
// the transaction's IPv6 upserts and erases on a table together, during the
// route rank's upserts: earlier than an erase would otherwise publish, which
// is safe because nothing references a route. An aborted transaction aborts
// each stage once, so readers never saw a change.
//
// A key of a family the router does not serve is refused at Reserve
// (kFamilyDisabled); such a family has no FIB to hold it.
class Router::RouteResource final : public dataplane::Resource {
 public:
  explicit RouteResource(Router &router)
      : Resource(router.routes_name_,
                 std::vector<std::string>{router.next_hops_name_, router.neighbors_name_,
                                          router.groups_name_}),
        router_(router) {}

  size_t LiveCount() const override { return router_.CountRoutes(); }

  bool Contains(const dataplane::ResourceKey &key) const override {
    if (const auto id = Decode6(key)) {
      return Find6(*id).has_value();
    }
    const auto id = Decode(key);
    return id && Find(*id).has_value();
  }

  std::vector<dataplane::Reference> ReferencesOf(
      const dataplane::ResourceKey &key) const override {
    if (const auto id = Decode6(key)) {
      const auto value = Find6(*id);
      return value ? References(*value) : std::vector<dataplane::Reference>{};
    }
    const auto id = Decode(key);
    const auto value = id ? Find(*id) : std::nullopt;
    return value ? References(*value) : std::vector<dataplane::Reference>{};
  }
  void VisitReferences(
      const std::function<void(const dataplane::Reference &)> &visit) const
      override {
    router_.ForEachDomain([&](RouteDomainId, const Domain &domain) {
      if (const LpmRouteTable *routes = domain.table()) {
        routes->ForEach([&](Ipv4Prefix, uint32_t value) {
          for (const auto &ref : References(value)) {
            visit(ref);
          }
        });
      }
      if (const Ipv6RouteTable *routes = domain.table6()) {
        routes->ForEach([&](Ipv6Prefix, uint32_t value) {
          for (const auto &ref : References(value)) {
            visit(ref);
          }
        });
      }
    });
  }


  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    if (!router_.has_ipv4() && !router_.has_ipv6()) {
      // A next-hop-only router (Config with no family): no FIB at all.
      return std::unexpected("this router has no FIB (no address family is "
                             "configured): it holds next hops, not routes");
    }
    if (const auto id6 = Decode6(op.key)) {
      if (!router_.has_ipv6()) {
        return std::unexpected(RouteErrorName(RouteError::kFamilyDisabled));
      }
      return Reserve6(op, *id6);
    }
    const auto id = Decode(op.key);
    if (!id) {
      return std::unexpected(
          "not a route key (Router::RouteKey) of a valid prefix");
    }
    if (!router_.has_ipv4()) {
      return std::unexpected(RouteErrorName(RouteError::kFamilyDisabled));
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
      // ValidGroupId is the whole test that groups exist: without max_groups
      // the resource is registered (a class registers every table it
      // declares) and holds nothing, so every id is invalid.
      if (!router_.ValidGroupId(*group)) {
        return std::unexpected(RouteErrorName(RouteError::kInvalidId));
      }
      value = router_.max_next_hops_ + group->value();
    } else {
      return std::unexpected("wrong value type (want NextHopId or NextHopGroupId)");
    }
    if (table == nullptr) {
      // A domain a controller writes a route into exists from that write on
      // (D-158): `max_domains` is a bound, not an allocation, so a router
      // configured for many network instances pays for the ones that hold
      // routes. Erases never create -- an erase in a missing domain is "not
      // found" above -- and this runs under the engine's lock, so a reader
      // (TableOf, one acquire load) sees the domain whole or not at all.
      std::lock_guard<std::mutex> lock(router_.mutex_);
      if (auto created =
              router_.CreateDomainLocked(id->domain, router_.domain_config_);
          !created) {
        return std::unexpected(RouteErrorName(created.error()));
      }
      table = router_.TableOf(id->domain);
      if (table == nullptr) {  // just created, within the bound: unreachable
        return std::unexpected(RouteErrorName(RouteError::kUnknownDomain));
      }
    }
    // Everything that can throw first; placing the prefix is the last step.
    auto upsert = std::make_unique<UpsertOp>(*table, prefix, value);
    Reservation reservation{nullptr, References(value, /*in_transaction=*/true)};
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
  void EndTransaction() noexcept override {
    pending_.clear();
    // A stage whose first edit was refused has no staged operation to abort
    // it; every other one was committed or aborted already.
    for (auto &[table, staged] : stages6_) {
      staged->Abort();
    }
    stages6_.clear();
  }

 private:
  struct Ipv6Id {
    RouteDomainId domain;
    Ipv6Prefix prefix;
  };

  static std::optional<Ipv6Id> Decode6(const dataplane::ResourceKey &key) {
    if (key.size() != 22 || static_cast<uint8_t>(key[0]) != 6) {
      return std::nullopt;
    }
    uint32_t domain = 0;
    std::memcpy(&domain, key.data() + 1, sizeof(domain));
    utils::Ipv6Address address;
    std::memcpy(address.bytes, key.data() + 5, sizeof(address.bytes));
    const auto prefix = Ipv6Prefix::Make(
        address, static_cast<uint8_t>(key[21]));
    if (!prefix || domain >= kMaxRouteDomains) {
      return std::nullopt;
    }
    return Ipv6Id{RouteDomainId(domain), *prefix};
  }

  // The committed route: the engine refuses a second operation on one key in
  // a transaction, so a staged edit never needs to be seen here.
  std::optional<uint32_t> Find6(const Ipv6Id &id) const {
    const Ipv6RouteTable *table = router_.Table6Of(id.domain);
    return table == nullptr ? std::nullopt : table->Find(id.prefix);
  }

  // One table's next version within a transaction (see the class comment).
  // Exactly one of Commit and Abort reaches the table.
  struct Stage6 {
    Ipv6RouteTable &table;
    std::unique_ptr<Ipv6RouteTable::Stage> stage;
    bool committed = false;
    bool aborted = false;

    void Abort() noexcept {
      if (!committed && !aborted) {
        aborted = true;
        table.Abort(*stage);
      }
    }
  };

  std::expected<Reservation, std::string> Reserve6(
      const dataplane::Op &op, const Ipv6Id &id) {
    const auto previous = Find6(id);
    const bool erase = op.kind == dataplane::OpKind::kErase;
    uint32_t value = 0;
    if (erase) {
      if (!previous) {
        return std::unexpected("not found");
      }
    } else if (const auto *hop = std::any_cast<NextHopId>(&op.value)) {
      if (!router_.ValidId(*hop)) {
        return std::unexpected(RouteErrorName(RouteError::kInvalidId));
      }
      value = hop->value();
    } else if (const auto *group = std::any_cast<NextHopGroupId>(&op.value)) {
      if (!router_.ValidGroupId(*group)) {
        return std::unexpected(RouteErrorName(RouteError::kInvalidId));
      }
      value = router_.max_next_hops_ + group->value();
    } else {
      return std::unexpected("wrong value type (want NextHopId or NextHopGroupId)");
    }
    Ipv6RouteTable *table = router_.Table6Of(id.domain);
    if (table == nullptr) {
      // As for IPv4 (D-158): the first route written into a domain creates it.
      // An erase never gets here (a missing domain holds nothing to erase).
      std::lock_guard<std::mutex> lock(router_.mutex_);
      if (auto created =
              router_.CreateDomainLocked(id.domain, router_.domain_config_);
          !created) {
        return std::unexpected(RouteErrorName(created.error()));
      }
      table = router_.Table6Of(id.domain);
      if (table == nullptr) {  // just created, within the bound: unreachable
        return std::unexpected(RouteErrorName(RouteError::kUnknownDomain));
      }
    }
    auto it = stages6_.find(table);
    const bool begins = it == stages6_.end();
    if (begins) {
      auto stage = table->BeginStage();
      if (!stage) {
        return std::unexpected(RouteErrorName(stage.error()));
      }
      it = stages6_
               .emplace(table, std::make_unique<Stage6>(
                                   Stage6{*table, std::move(*stage)}))
               .first;
    }
    Stage6 &staged = *it->second;
    auto commit = std::make_unique<Commit6Op>(staged);
    Reservation reservation{nullptr,
                            erase ? std::vector<dataplane::Reference>{}
                                  : References(value, /*in_transaction=*/true)};
    reservation.existed = previous.has_value();
    if (previous) {
      reservation.previous_references = References(*previous);
    }
    // The stage's commit hands the table the grace period once per table;
    // nothing is retired (the replaced instance stays the table's spare).
    reservation.footprint.retires = 0;
    reservation.footprint.callbacks = begins ? 1 : 0;
    // The edit is last: everything above that can fail has.
    const auto edited = erase ? staged.stage->Erase(id.prefix)
                              : staged.stage->Upsert(id.prefix, value);
    if (!edited) {
      return std::unexpected(RouteErrorName(edited.error()));
    }
    reservation.staged = std::move(commit);
    return reservation;
  }

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

  // What a route value references: its group, or its next hop in the
  // resource that holds it (HopReference). A stored value is read against the
  // committed bindings; a value being written against the transaction's.
  std::vector<dataplane::Reference> References(uint32_t value,
                                               bool in_transaction = false) const {
    if (value > router_.max_next_hops_) {
      return {{router_.groups_name_,
               dataplane::EncodeKey(NextHopGroupId(value - router_.max_next_hops_))}};
    }
    return {router_.HopReference(NextHopId(value), in_transaction)};
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
  // Every IPv6 operation on a table publishes its transaction's stage; the
  // first does it, the rest find it committed. Nothing allocates: the spare
  // was edited in Reserve(), Commit() is a pointer exchange, and the callback
  // that hands the table its grace period fits the Retirer's inline storage
  // and the footprint the stage's first operation declared.
  class Commit6Op final : public dataplane::StagedOp {
   public:
    explicit Commit6Op(Stage6 &stage) : stage_(stage) {}
    void Publish(dataplane::Retirer &retirer) noexcept override {
      if (stage_.committed) {
        return;
      }
      stage_.committed = true;
      stage_.table.Commit(*stage_.stage);
      Ipv6RouteTable *table = &stage_.table;
      retirer.AfterGracePeriodStarts(
          [table](rcu::GracePeriod token) { table->Committed(token); });
    }
    void Abort() noexcept override { stage_.Abort(); }

   private:
    Stage6 &stage_;
  };

  Router &router_;
  std::set<::bess::route::RouteKey> pending_;
  std::map<const Ipv6RouteTable *, std::unique_ptr<Stage6>> stages6_;
};


// "<router>/next_hops": the static next hops. A bound id ("<router>/neighbors")
// shares the slot table but is not this resource's: it is absent here, and a
// write or an erase of it is refused. The ids this transaction upserts are
// recorded, so the neighbors resource can refuse to bind them.
class Router::NextHopResource final
    : public dataplane::SlotResource<NextHopId, NextHop> {
 public:
  using Base = dataplane::SlotResource<NextHopId, NextHop>;

  explicit NextHopResource(Router &router)
      : Base(router.next_hops_name_, router.next_hops_), router_(router) {}

  size_t LiveCount() const override {
    return Base::LiveCount() - router_.bound_count_;
  }

  bool Contains(const dataplane::ResourceKey &key) const override {
    NextHopId id;
    return dataplane::DecodeKey(key, &id) && !router_.IsNeighborBound(id) &&
           Base::Contains(key);
  }

  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    NextHopId id;
    if (dataplane::DecodeKey(op.key, &id) &&
        (router_.IsNeighborBound(id) || router_.BoundInTransaction(id))) {
      return std::unexpected("next hop " + std::to_string(id.value()) +
                             " is bound to a neighbor (" + router_.neighbors_name_ +
                             ")");
    }
    auto reserved = Base::Reserve(op);
    if (reserved && op.kind == dataplane::OpKind::kUpsert) {
      router_.staged_static_.insert(id);
    }
    return reserved;
  }

  void EndTransaction() noexcept override { router_.staged_static_.clear(); }

 private:
  Router &router_;
};

// "<router>/neighbors" (D-180): next-hop id -> NeighborKey, the hops a
// resolver owns. Binding publishes the id's next hop in the shared slot table
// -- egress the neighbor's interface, incomplete -- and records the change for
// the owner (TakeNeighborBindingChanges); a NeighborManagedNextHop upsert on a
// bound id publishes the resolver's newer state; an erase unbinds, keeping
// the hop readable until the removal cascade, like a SlotResource erase. Its
// erase is deferred, so routes and groups may reference it, and the engine
// refuses an unbind while any does. Capacity is the policy's: bound ids,
// counted across the transaction.
class Router::NeighborResource final : public dataplane::Resource {
 public:
  explicit NeighborResource(Router &router)
      : Resource(router.neighbors_name_), router_(router) {}

  size_t LiveCount() const override { return router_.bound_count_; }

  std::optional<uint64_t> Capacity() const override {
    if (!router_.neighbor_policy_.validate) {
      return std::nullopt;
    }
    return router_.neighbor_policy_.capacity;
  }

  bool Contains(const dataplane::ResourceKey &key) const override {
    NextHopId id;
    return dataplane::DecodeKey(key, &id) && router_.IsNeighborBound(id);
  }

  bool DefersErase() const override { return true; }

  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    NextHopId id;
    if (!dataplane::DecodeKey(op.key, &id) || !router_.ValidId(id)) {
      return std::unexpected("invalid id");
    }
    const std::string named = "next hop " + std::to_string(id.value());
    const std::optional<NeighborKey> &current = router_.bindings_[id.value()];
    if (op.kind == dataplane::OpKind::kErase) {
      if (!current) {
        return std::unexpected("not found");
      }
      StageChange(id, std::nullopt);
      Reservation erase{std::make_unique<UnbindOp>(router_, id), {},
                        Footprint{.removals = 1}};
      erase.existed = true;
      return erase;
    }
    if (const auto *update = std::any_cast<NeighborManagedNextHop>(&op.value)) {
      if (!current || router_.staged_bindings_.contains(id)) {
        return std::unexpected(named + " is not bound to a neighbor");
      }
      NextHop hop = update->value;
      hop.egress = current->interface;
      hop.kind = NextHopKind::kForward;
      Reservation reservation{
          std::make_unique<PublishOp>(router_, id, std::make_unique<const NextHop>(hop),
                                      std::nullopt),
          {},
          Footprint{.retires = 1}};
      reservation.existed = true;
      return reservation;
    }
    const auto *neighbor = std::any_cast<NeighborKey>(&op.value);
    if (neighbor == nullptr) {
      return std::unexpected(
          "wrong value type (want NeighborKey or NeighborManagedNextHop)");
    }
    if (!router_.neighbor_policy_.validate) {
      return std::unexpected("neighbor resolution is not configured");
    }
    if (auto valid = router_.neighbor_policy_.validate(*neighbor); !valid) {
      return std::unexpected(valid.error());
    }
    if (current && *current == *neighbor) {
      Reservation same{std::make_unique<NoOp>(), {}, {}};
      same.existed = true;
      return same;
    }
    if (!current) {
      if (router_.next_hops_.Contains(id) || router_.staged_static_.contains(id)) {
        return std::unexpected(named + " is a static next hop (" +
                               router_.next_hops_name_ + ")");
      }
      if (!router_.next_hops_.CanPublish(id)) {
        return std::unexpected(named + " is still retiring");
      }
      // Bound after the transaction: the committed bindings, plus what it
      // has bound and minus what it has unbound so far, plus this one.
      size_t bound = router_.bound_count_ + 1;
      for (const auto &[staged, key] : router_.staged_bindings_) {
        if (key && !router_.bindings_[staged.value()]) {
          bound++;
        } else if (!key) {
          bound--;
        }
      }
      if (bound > router_.neighbor_policy_.capacity) {
        return std::unexpected("neighbor capacity (" +
                               std::to_string(router_.neighbor_policy_.capacity) +
                               " bindings) exhausted");
      }
    }
    NextHop hop;
    hop.egress = neighbor->interface;
    hop.neighbor = NeighborState::kIncomplete;
    auto object = std::make_unique<const NextHop>(hop);
    StageChange(id, *neighbor);
    Reservation reservation{
        std::make_unique<PublishOp>(router_, id, std::move(object), *neighbor),
        {},
        Footprint{.retires = current ? 1u : 0u}};
    reservation.existed = current.has_value();
    return reservation;
  }

  void EndTransaction() noexcept override { router_.staged_bindings_.clear(); }

 private:
  // Records the transaction's change to `id`'s binding, with room for it in
  // the change log, before anything can publish.
  void StageChange(NextHopId id, std::optional<NeighborKey> neighbor) {
    router_.staged_bindings_[id] = neighbor;
    router_.binding_changes_.reserve(router_.binding_changes_.size() +
                                     router_.staged_bindings_.size());
  }

  class NoOp final : public dataplane::StagedOp {
   public:
    void Publish(dataplane::Retirer &) noexcept override {}
  };

  // Publishes a bound id's next hop; with `binding`, also (re)binds the id.
  class PublishOp final : public dataplane::StagedOp {
   public:
    PublishOp(Router &router, NextHopId id, std::unique_ptr<const NextHop> hop,
              std::optional<NeighborKey> binding)
        : router_(router), id_(id), hop_(std::move(hop)), binding_(binding) {}
    void Publish(dataplane::Retirer &retirer) noexcept override {
      retirer.Retire(router_.next_hops_.Publish(id_, std::move(hop_)));
      if (binding_) {
        std::optional<NeighborKey> &slot = router_.bindings_[id_.value()];
        if (!slot) {
          router_.bound_count_++;
        }
        slot = *binding_;
        router_.binding_changes_.push_back({id_, *binding_});  // room reserved
      }
    }

   private:
    Router &router_;
    NextHopId id_;
    std::unique_ptr<const NextHop> hop_;
    std::optional<NeighborKey> binding_;
  };

  class UnbindOp final : public dataplane::StagedOp {
   public:
    UnbindOp(Router &router, NextHopId id) : router_(router), id_(id) {}
    void Publish(dataplane::Retirer &retirer) noexcept override {
      router_.next_hops_.Retire(id_);  // absent now, readable until the cascade
      retirer.RemoveLater([&table = router_.next_hops_, id = id_](dataplane::Retirer &later) {
        later.Retire(table.Unpublish(id));
      });
      router_.bindings_[id_.value()].reset();
      router_.bound_count_--;
      router_.binding_changes_.push_back({id_, std::nullopt});  // room reserved
    }

   private:
    Router &router_;
    NextHopId id_;
  };

  Router &router_;
};

namespace {

// A group's references: each distinct member once (a repeated member, a
// weight, is one reference), in the resource that holds it.
template <typename HopReferenceFn>
std::vector<dataplane::Reference> MemberReferences(const NextHopGroup &group,
                                                   HopReferenceFn &&reference) {
  std::vector<NextHopId> members(group.members.begin(),
                                 group.members.begin() + std::min<size_t>(group.size, NextHopGroup::kMaxMembers));
  std::sort(members.begin(), members.end());
  members.erase(std::unique(members.begin(), members.end()), members.end());
  std::vector<dataplane::Reference> refs;
  refs.reserve(members.size());
  for (const NextHopId m : members) {
    refs.push_back(reference(m));
  }
  return refs;
}

}  // namespace

// "<router>/groups": a SlotResource of immutable groups, each referencing its
// distinct members (a repeated member, a weight, is one reference) -- in
// "<router>/next_hops", or "<router>/neighbors" for a bound one -- so a group
// cannot name a next hop that does not exist and a member cannot be removed
// while a group names it. Reserve also checks the id and the member count,
// before the base reads the members, and reports a replaced group's
// references as they were committed.
class Router::GroupResource final
    : public dataplane::SlotResource<NextHopGroupId, NextHopGroup> {
 public:
  explicit GroupResource(Router &router)
      : SlotResource(router.groups_name_, router.groups_,
                     [&router](const NextHopGroup &group) {
                       return MemberReferences(group, [&router](NextHopId m) {
                         return router.HopReference(m, /*in_transaction=*/true);
                       });
                     },
                     {router.next_hops_name_, router.neighbors_name_}),
        router_(router) {}

  std::expected<Reservation, std::string> Reserve(const dataplane::Op &op) override {
    NextHopGroupId id;
    if (op.kind == dataplane::OpKind::kUpsert) {
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
    auto reserved = SlotResource::Reserve(op);
    if (reserved && reserved->existed && dataplane::DecodeKey(op.key, &id)) {
      if (const NextHopGroup *previous = router_.groups_.Current(id)) {
        reserved->previous_references =
            MemberReferences(*previous, [this](NextHopId m) {
              return router_.HopReference(m, /*in_transaction=*/false);
            });
      }
    }
    return reserved;
  }

 private:
  Router &router_;
};

dataplane::Reference Router::HopReference(NextHopId id, bool in_transaction) const {
  const bool bound = in_transaction ? BoundInTransaction(id) : IsNeighborBound(id);
  return {bound ? neighbors_name_ : next_hops_name_, dataplane::EncodeKey(id)};
}

bool Router::BoundInTransaction(NextHopId id) const {
  if (!ValidId(id)) {
    return false;
  }
  if (const auto staged = staged_bindings_.find(id); staged != staged_bindings_.end()) {
    return staged->second.has_value();
  }
  return bindings_[id.value()].has_value();
}

bool Router::IsNeighborBound(NextHopId id) const {
  return ValidId(id) && bindings_[id.value()].has_value();
}

void Router::ForEachNeighborBinding(
    const std::function<void(NextHopId, const NeighborKey &)> &fn) const {
  for (size_t i = 1; i < bindings_.size(); i++) {
    if (bindings_[i]) {
      fn(NextHopId(static_cast<uint32_t>(i)), *bindings_[i]);
    }
  }
}

std::vector<NeighborBindingChange> Router::TakeNeighborBindingChanges() {
  std::vector<NeighborBindingChange> out;
  out.swap(binding_changes_);
  return out;
}

std::expected<void, RouteError> Router::SetNeighborBindingPolicy(
    NeighborBindingPolicy policy) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  neighbor_policy_ = std::move(policy);
  return {};
}

std::expected<void, std::string> Router::Enroll(
    dataplane::ResourceRegistry &engine) {
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
  auto hops = std::make_unique<NextHopResource>(*this);
  // The neighbors resource is registered whether or not a binding policy is
  // set, and so is the groups resource whether or not max_groups is: the
  // control plane refuses a class that registers fewer tables than it
  // declares (CheckDeclaredResources), and the Router module declares all
  // four (D-156, D-180). Without its policy or bound it holds nothing.
  auto neighbors = std::make_unique<NeighborResource>(*this);
  auto groups = std::make_unique<GroupResource>(*this);
  auto routes = std::make_unique<RouteResource>(*this);
  if (auto r = engine.Register(hops.get()); !r) {
    return r;
  }
  if (auto r = engine.Register(neighbors.get()); !r) {
    CHECK(engine.Unregister(next_hops_name_));
    return r;
  }
  if (auto r = engine.Register(groups.get()); !r) {
    CHECK(engine.Unregister(neighbors_name_));
    CHECK(engine.Unregister(next_hops_name_));
    return r;
  }
  if (auto r = engine.Register(routes.get()); !r) {
    CHECK(engine.Unregister(groups_name_));
    CHECK(engine.Unregister(neighbors_name_));
    CHECK(engine.Unregister(next_hops_name_));
    return r;
  }
  engine_ = &engine;
  next_hops_res_ = std::move(hops);
  neighbors_res_ = std::move(neighbors);
  groups_res_ = std::move(groups);
  routes_res_ = std::move(routes);
  return {};
}

std::expected<void, std::string> Router::Release() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ == nullptr) {
    return {};  // not enrolled, or already released
  }
  std::vector<std::string> names = {routes_name_, groups_name_, neighbors_name_,
                                    next_hops_name_};
  auto released = engine_->ReleaseForTeardown(names);
  if (!released) {
    return released;
  }
  engine_ = nullptr;
  next_hops_res_.reset();
  neighbors_res_.reset();
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
  if (config.ipv4.has_value() != has_ipv4() ||
      config.ipv6.has_value() != has_ipv6()) {
    return std::unexpected(RouteError::kFamilyDisabled);
  }
  Config sized = config;
  if (sized.ipv6) {
    sized.ipv6->max_value = domain_config_.ipv6->max_value;
  }
  return CreateDomainLocked(domain, sized);
}

std::expected<void, RouteError> Router::CreateDomainLocked(
    RouteDomainId domain, const Config &config) {
  const DomainSlot slot = SlotOf(domain);
  if (!domains_.ValidId(slot)) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (domains_.Contains(slot)) {
    return std::unexpected(RouteError::kDomainExists);
  }
  auto object =
      MakeDomain(name_ + "_d" + std::to_string(domain.value()), config, rcu_);
  if (!object) {
    return std::unexpected(object.error());
  }
  ReserveOneMore(domain_ids_);
  domains_.Publish(slot, std::move(*object));
  domain_ids_.insert(
      std::lower_bound(domain_ids_.begin(), domain_ids_.end(), domain), domain);
  return {};
}

Router::FibCounts Router::fib_counts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  FibCounts counts;
  ForEachDomain([&](RouteDomainId, const Domain &d) {
    counts.ipv4 += d.table() != nullptr ? 1 : 0;
    counts.ipv6 += d.table6() != nullptr ? 1 : 0;
  });
  return counts;
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
  if (domains_.Lookup(slot)->size() != 0) {
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
    return std::unexpected(has_ipv4() ? RouteError::kUnknownDomain
                                      : RouteError::kFamilyDisabled);
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
    return std::unexpected(has_ipv4() ? RouteError::kUnknownDomain
                                      : RouteError::kFamilyDisabled);
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
  // The engine's lock is taken after this one is released, never under it: the
  // two are taken in both orders elsewhere (Enroll registers the resources, a
  // transaction's apply reaches RouteResource::Reserve), and a public method
  // is the wrong place for the second nesting. Enroll/Release are the only
  // writers of `engine_`, and the module lifecycle serialises them with every
  // caller of this method, so the copy is safe to call through.
  dataplane::ResourceRegistry *engine;
  std::string name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (engine_ == nullptr) {
      return ValidGroupId(id) ? group_references_[id.value()] : 0;
    }
    engine = engine_;
    name = groups_name_;
  }
  return engine->ReferenceCount(name, dataplane::EncodeKey(id));
}

// Enrolled, the engine counts references per (group, member) once, not per
// member position; the direct API counts positions.
size_t Router::GroupMemberships(NextHopId id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
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
    return std::unexpected(has_ipv4() ? RouteError::kUnknownDomain
                                      : RouteError::kFamilyDisabled);
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

std::expected<void, RouteError> Router::SetRoute(RouteDomainId domain,
                                                 Ipv6Prefix prefix,
                                                 NextHopId hop) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  Ipv6RouteTable *routes = Table6Of(domain);
  if (routes == nullptr) {
    return std::unexpected(has_ipv6() ? RouteError::kUnknownDomain
                                      : RouteError::kFamilyDisabled);
  }
  if (!ValidId(hop)) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (!next_hops_.Contains(hop)) {
    return std::unexpected(RouteError::kUnknownNextHop);
  }
  const auto previous = routes->Find(prefix);
  if (auto set = routes->Upsert(prefix, hop.value()); !set) {
    return set;
  }
  Reference(hop.value());
  if (previous) {
    Unreference(*previous);
  }
  return {};
}

std::expected<void, RouteError> Router::SetRoute(RouteDomainId domain,
                                                 Ipv6Prefix prefix,
                                                 NextHopGroupId group) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  Ipv6RouteTable *routes = Table6Of(domain);
  if (routes == nullptr) {
    return std::unexpected(has_ipv6() ? RouteError::kUnknownDomain
                                      : RouteError::kFamilyDisabled);
  }
  if (!ValidGroupId(group)) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (!groups_.Contains(group)) {
    return std::unexpected(RouteError::kUnknownNextHop);
  }
  const uint32_t value = max_next_hops_ + group.value();
  const auto previous = routes->Find(prefix);
  if (auto set = routes->Upsert(prefix, value); !set) {
    return set;
  }
  Reference(value);
  if (previous) {
    Unreference(*previous);
  }
  return {};
}

std::expected<void, RouteError> Router::RemoveRoute(RouteDomainId domain,
                                                    Ipv6Prefix prefix) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  Ipv6RouteTable *routes = Table6Of(domain);
  if (routes == nullptr) {
    return std::unexpected(has_ipv6() ? RouteError::kUnknownDomain
                                      : RouteError::kFamilyDisabled);
  }
  const auto previous = routes->Find(prefix);
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
    return std::unexpected(has_ipv4() ? RouteError::kUnknownDomain
                                      : RouteError::kFamilyDisabled);
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

// Callers hold mutex_, or the transaction engine's lock for an enrolled router
// (the one writer that can still create a domain holds both, D-158), or have
// exclusive access (Enroll's check).
size_t Router::CountRoutes() const {
  size_t count = 0;
  ForEachDomain([&](RouteDomainId, const Domain &d) { count += d.size(); });
  return count;
}

size_t Router::route_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return CountRoutes();
}

size_t Router::route_count(RouteDomainId domain) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Domain *d = domains_.Lookup(SlotOf(domain));
  return d != nullptr ? d->size() : 0;
}

size_t Router::next_hop_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return next_hops_.size();  // live, not counting retiring ones
}

void Router::ForEachNextHop(
    const std::function<void(NextHopId, const NextHop &)> &fn) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 1; i <= next_hops_.capacity(); i++) {
    const NextHopId id(static_cast<uint32_t>(i));
    if (const NextHop *hop = next_hops_.Current(id); hop != nullptr) {
      fn(id, *hop);
    }
  }
}

void Router::ForEachNextHopGroup(
    const std::function<void(NextHopGroupId, const NextHopGroup &)> &fn) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 1; i <= groups_.capacity(); i++) {
    const NextHopGroupId id(static_cast<uint32_t>(i));
    if (const NextHopGroup *group = groups_.Current(id); group != nullptr) {
      fn(id, *group);
    }
  }
}

void Router::ForEachRoute(
    const std::function<void(const ::bess::route::RouteKey &key, NextHopId hop,
                             NextHopGroupId group)> &fn) const {
  std::lock_guard<std::mutex> lock(mutex_);
  ForEachDomain([&](RouteDomainId domain, const Domain &d) {
    if (d.table() == nullptr) {
      return;
    }
    d.table()->ForEach([&](Ipv4Prefix prefix, uint32_t value) {
      const ::bess::route::RouteKey key{domain, prefix};
      if (value > max_next_hops_) {
        fn(key, NextHopId(0), NextHopGroupId(value - max_next_hops_));
      } else {
        fn(key, NextHopId(value), NextHopGroupId(0));
      }
    });
  });
}
void Router::ForEachIpv6Route(
    const std::function<void(RouteDomainId, Ipv6Prefix, NextHopId,
                             NextHopGroupId)> &fn) const {
  std::lock_guard<std::mutex> lock(mutex_);
  ForEachDomain([&](RouteDomainId domain, const Domain &d) {
    if (d.table6() == nullptr) {
      return;
    }
    d.table6()->ForEach([&](Ipv6Prefix prefix, uint32_t value) {
      if (value > max_next_hops_) {
        fn(domain, prefix, NextHopId(0),
           NextHopGroupId(value - max_next_hops_));
      } else {
        fn(domain, prefix, NextHopId(value), NextHopGroupId(0));
      }
    });
  });
}

size_t Router::RouteReferences(NextHopId id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    // Groups reference next hops too: count the routes' references only.
    const dataplane::Reference wanted = HopReference(id, /*in_transaction=*/false);
    size_t n = 0;
    routes_res_->VisitReferences([&](const dataplane::Reference &ref) { n += ref == wanted; });
    return n;
  }
  return ValidId(id) ? references_[id.value()] : 0;
}

}  // namespace bess::route
