// SPDX-License-Identifier: BSD-3-Clause

// M8 (Decision D-050): the two transaction consistency levels, proven against
// readers that are adversarial on purpose.
//
//   referential     operations take effect one by one, referents first; a
//                   reader may see part of a transaction applied.
//   scope-snapshot  each scope switches from its whole old version to its whole
//                   new one; a reader that binds the scope once sees one of
//                   them, never a mix.
//
// Two very different applications own what a "scope" is (the library does
// not): a per-session policy (meter, next hop and QoS class that move
// together) and a VFP-like policy group (two rule layers that must belong to
// one group version). Each is run against the same two kinds of tests:
//
//  - Schedule enumeration (a model test). The writer's publication is a
//    sequence of steps; a reader performs a sequence of loads. Every
//    interleaving of the two -- every non-decreasing assignment of loads to
//    "before the transaction", "after publication step j", "after the
//    transaction returned" -- is run, and the SET of observations is compared
//    with the contract: exactly the allowed states, each one reachable. A
//    reader that is allowed to see a mix must be shown to see it, or the test
//    proves nothing.
//  - Threads: real RcuDomain readers against a hammering writer.
//
// Plus failure injection (a scope-snapshot transaction that cannot be
// prepared leaves the old scope visible) and the typed refusal of a level a
// resource cannot provide.

#include "dataplane/scope.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dataplane/slot_resource.h"
#include "dataplane/slot_table.h"
#include "dataplane/strong_id.h"
#include "dataplane/transaction_engine.h"
#include "rcu/rcu_domain.h"

namespace bess::dataplane {
namespace {

using Outcome = TransactionEngine::Outcome;
using OpStatus = TransactionEngine::OpStatus;

// -- harness ---------------------------------------------------------------------

// Observes and sabotages a transaction from inside the engine.
struct Probe {
  size_t published = 0;                    // publication steps so far
  std::function<void(size_t)> on_step;     // called after each, with the count
  // Fails the fail_at-th Reserve() of the transaction (1-based, 0: never), by
  // an error return or by throwing (an allocation failure).
  int fail_at = 0;
  bool fail_by_throwing = false;
  int reserve_calls = 0;

  void Arm(int at, bool by_throwing) {
    fail_at = at;
    fail_by_throwing = by_throwing;
    reserve_calls = 0;
  }
  void Published() {
    ++published;
    if (on_step) {
      on_step(published);
    }
  }
};

class ObservedOp final : public StagedOp {
 public:
  ObservedOp(std::unique_ptr<StagedOp> inner, Probe &probe)
      : inner_(std::move(inner)), probe_(probe) {}
  void Publish(Retirer &retirer) noexcept override {
    inner_->Publish(retirer);
    probe_.Published();
  }
  void Abort() noexcept override { inner_->Abort(); }

 private:
  std::unique_ptr<StagedOp> inner_;
  Probe &probe_;
};

// Forwards a resource and reports each of its publication steps to the probe,
// the point at which a reader may run.
class Observed final : public Resource {
 public:
  Observed(Resource &inner, Probe &probe)
      : Resource(inner.name(), inner.declared_references()),
        inner_(inner),
        probe_(probe) {}

  bool Contains(const ResourceKey &key) const override {
    return inner_.Contains(key);
  }
  std::vector<Reference> ReferencesOf(const ResourceKey &key) const override {
    return inner_.ReferencesOf(key);
  }
  void VisitReferences(
      const std::function<void(const Reference &)> &visit) const override {
    inner_.VisitReferences(visit);
  }
  bool DefersErase() const override { return inner_.DefersErase(); }
  Consistency ProvidedConsistency() const override {
    return inner_.ProvidedConsistency();
  }
  size_t LiveCount() const override { return inner_.LiveCount(); }
  void EndTransaction() noexcept override { inner_.EndTransaction(); }

  std::expected<Reservation, std::string> Reserve(const Op &op) override {
    if (probe_.fail_at != 0 && ++probe_.reserve_calls == probe_.fail_at) {
      if (probe_.fail_by_throwing) {
        throw std::bad_alloc();
      }
      return std::unexpected("injected failure");
    }
    auto reserved = inner_.Reserve(op);
    if (reserved) {
      reserved->staged = std::make_unique<ObservedOp>(
          std::move(reserved->staged), probe_);
    }
    return reserved;
  }

 private:
  Resource &inner_;
  Probe &probe_;
};

// A reader that stays online and never reports a quiescent state: everything
// it has read stays valid, exactly as for a worker in the middle of a task.
class HeldReader {
 public:
  explicit HeldReader(rcu::RcuDomain &domain) : domain_(domain) {
    EXPECT_TRUE(domain_.Register(0).has_value());
    domain_.Online(0);
  }
  ~HeldReader() {
    domain_.Offline(0);
    domain_.Unregister(0);
  }

 private:
  rcu::RcuDomain &domain_;
};

// Runs every removal and retirement to completion (no reader online).
void Settle(TransactionEngine &engine, rcu::RcuDomain &domain) {
  while (engine.ReclaimRetired() != 0) {
  }
  domain.Drain();
}

// Calls `fn` with every non-decreasing assignment of `loads` reader loads to
// slots 0..last_slot: C(loads + last_slot, loads) interleavings.
void ForEachSchedule(size_t loads, size_t last_slot,
                     const std::function<void(const std::vector<size_t> &)> &fn) {
  std::vector<size_t> slot(loads, 0);
  std::function<void(size_t, size_t)> assign = [&](size_t i, size_t lowest) {
    if (i == loads) {
      fn(slot);
      return;
    }
    for (size_t s = lowest; s <= last_slot; s++) {
      slot[i] = s;
      assign(i + 1, s);
    }
  };
  assign(0, 0);
}

// Runs `apply` (a transaction of `steps` publication steps) while the reader
// performs load(0), load(1), ... at the slots of one schedule: slot 0 before
// the transaction, slot j in 1..steps right after its j-th publication step,
// slot steps+1 after it returned and reclamation ran (the reader still holds
// what it read: it has not reached a quiescent state).
void Interleave(Probe &probe, TransactionEngine &engine,
                const std::vector<size_t> &slot, size_t steps,
                const std::function<void()> &apply,
                const std::function<void(size_t)> &load) {
  auto run = [&](size_t at) {
    for (size_t i = 0; i < slot.size(); i++) {
      if (slot[i] == at) {
        load(i);
      }
    }
  };
  probe.published = 0;
  probe.on_step = run;
  run(0);
  apply();
  EXPECT_EQ(probe.published, steps);
  engine.ReclaimRetired();
  run(steps + 1);
  probe.on_step = nullptr;
}

// -- application 1: a per-session policy ---------------------------------------------

// Everything a session's packets are steered by. The fields move together or
// the session misbehaves (a new meter with the old next hop, say), which is
// what a scope is for. Each field derives from the epoch, so a mix is
// detectable.
struct SessionPolicy {
  uint32_t epoch = 0;
  uint32_t meter = 0;
  uint32_t next_hop = 0;
  uint32_t qos = 0;

  static SessionPolicy At(uint32_t epoch) {
    return {epoch, epoch * 10 + 1, epoch * 10 + 2, epoch * 10 + 3};
  }
  bool Coherent() const { return *this == At(epoch); }
  friend bool operator==(const SessionPolicy &, const SessionPolicy &) = default;
};
using Fields = std::array<uint32_t, 4>;
Fields FieldsOf(const SessionPolicy &p) {
  return {p.epoch, p.meter, p.next_hop, p.qos};
}
uint32_t Field(const SessionPolicy &p, size_t i) { return FieldsOf(p)[i]; }

struct SessionWorld {
  SessionWorld() { EXPECT_TRUE(engine.Register(&observed).has_value()); }

  static Op Set(uint32_t id, uint32_t epoch) {
    return Op::Upsert("sessions", EncodeKey(ScopeId(id)),
                      std::any(SessionPolicy::At(epoch)));
  }
  static Op Erase(uint32_t id) {
    return Op::Erase("sessions", EncodeKey(ScopeId(id)));
  }
  TransactionEngine::Result Snapshot(const std::vector<Op> &ops) {
    return engine.Apply(ops, std::nullopt, Consistency::kScopeSnapshot);
  }

  rcu::RcuDomain domain{8};
  ScopeTable<SessionPolicy> table{16};
  ScopeResource<SessionPolicy> resource{"sessions", table};
  Probe probe;
  Observed observed{resource, probe};
  TransactionEngine engine{domain};
};

// -- application 2: a VFP-like policy group --------------------------------------------

// Two layers: the first classifies a flow key, the second maps the class to an
// action. Both must come from one group version: a class of one version looked
// up in the layer of another finds nothing.
using Rules = std::vector<std::pair<uint32_t, uint32_t>>;
struct Layer {
  uint32_t version = 0;
  Rules rules;
};
std::optional<uint32_t> Find(const Layer &layer, uint32_t key) {
  for (const auto &[k, v] : layer.rules) {
    if (k == key) {
      return v;
    }
  }
  return std::nullopt;
}
Layer ClassifyLayer(uint32_t version) {
  Layer layer{version, {}};
  for (uint32_t key = 0; key < 4; key++) {
    layer.rules.push_back({key, version * 100 + key});  // the class
  }
  return layer;
}
Layer ActionLayer(uint32_t version) {
  Layer layer{version, {}};
  for (uint32_t key = 0; key < 4; key++) {
    layer.rules.push_back({version * 100 + key, version * 1000 + key});
  }
  return layer;
}
std::optional<uint32_t> Resolve(const Layer &classify, const Layer &action,
                                uint32_t key) {
  const auto cls = Find(classify, key);
  return cls ? Find(action, *cls) : std::nullopt;
}

struct GroupVersion {
  Layer classify;
  Layer action;
  static GroupVersion At(uint32_t version) {
    return {ClassifyLayer(version), ActionLayer(version)};
  }
};

struct GroupWorld {
  GroupWorld() { EXPECT_TRUE(engine.Register(&observed).has_value()); }
  static Op Set(uint32_t id, uint32_t version) {
    return Op::Upsert("groups", EncodeKey(ScopeId(id)),
                      std::any(GroupVersion::At(version)));
  }
  rcu::RcuDomain domain{8};
  ScopeTable<GroupVersion> table{8};
  ScopeResource<GroupVersion> resource{"groups", table};
  Probe probe;
  Observed observed{resource, probe};
  TransactionEngine engine{domain};
};

// The same group stored the referential way: each layer is its own object in
// an ordinary resource, replaced one at a time.
struct LayerTag;
using LayerId = StrongId<LayerTag, uint32_t>;
struct LayeredGroupWorld {
  LayeredGroupWorld() { EXPECT_TRUE(engine.Register(&observed).has_value()); }
  static Op SetClassify(uint32_t version) {
    return Op::Upsert("layers", EncodeKey(LayerId(1)),
                      std::any(ClassifyLayer(version)));
  }
  static Op SetAction(uint32_t version) {
    return Op::Upsert("layers", EncodeKey(LayerId(2)),
                      std::any(ActionLayer(version)));
  }
  rcu::RcuDomain domain{8};
  SlotTable<LayerId, Layer> table{4};
  SlotResource<LayerId, Layer> resource{"layers", table};
  Probe probe;
  Observed observed{resource, probe};
  TransactionEngine engine{domain};
};

// -- the referential chain: meters <- next hops --------------------------------------------

struct MeterTag;
struct NextHopTag;
using MeterId = StrongId<MeterTag, uint32_t>;
using NextHopId = StrongId<NextHopTag, uint32_t>;
struct Meter {
  uint32_t epoch = 0;
};
struct NextHop {
  uint32_t epoch = 0;
  uint32_t meter = 0;
};

struct ChainWorld {
  ChainWorld() {
    EXPECT_TRUE(engine.Register(&meters_observed).has_value());
    EXPECT_TRUE(engine.Register(&hops_observed).has_value());
  }
  static Op Met(uint32_t id, uint32_t epoch) {
    return Op::Upsert("meters", EncodeKey(MeterId(id)), std::any(Meter{epoch}));
  }
  static Op Hop(uint32_t id, uint32_t epoch, uint32_t meter) {
    return Op::Upsert("next_hops", EncodeKey(NextHopId(id)),
                      std::any(NextHop{epoch, meter}));
  }
  static Op EraseMet(uint32_t id) {
    return Op::Erase("meters", EncodeKey(MeterId(id)));
  }
  static Op EraseHop(uint32_t id) {
    return Op::Erase("next_hops", EncodeKey(NextHopId(id)));
  }

  rcu::RcuDomain domain{8};
  SlotTable<MeterId, Meter> meters{16};
  SlotTable<NextHopId, NextHop> hops{16};
  SlotResource<MeterId, Meter> meters_resource{"meters", meters};
  SlotResource<NextHopId, NextHop> hops_resource{
      "next_hops", hops,
      [](const NextHop &hop) {
        return std::vector<Reference>{
            {"meters", EncodeKey(MeterId(hop.meter))}};
      },
      std::vector<std::string>{"meters"}};
  Probe probe;
  Observed meters_observed{meters_resource, probe};
  Observed hops_observed{hops_resource, probe};
  TransactionEngine engine{domain};
};

// =========================================================================================
// 1. Schedule enumeration
// =========================================================================================

// A session reader binds the scope once (load 0) and then reads the policy's
// four fields (loads 1-4), anywhere relative to the switch. Every interleaving
// shows it one whole version: the old one or the new one.
TEST(ScopeSnapshotModelTest, SessionBoundOnceIsAlwaysOneWholeVersion) {
  std::set<Fields> seen;
  size_t schedules = 0;
  ForEachSchedule(5, /*last_slot=*/2, [&](const std::vector<size_t> &slot) {
    SessionWorld w;
    ASSERT_EQ(w.Snapshot({w.Set(1, 1)}).outcome, Outcome::kApplied);
    Fields observed{};
    {
      HeldReader reader(w.domain);
      const SessionPolicy *bound = nullptr;
      Interleave(
          w.probe, w.engine, slot, /*steps=*/1,
          [&] {
            EXPECT_EQ(w.Snapshot({w.Set(1, 2)}).outcome, Outcome::kApplied);
          },
          [&](size_t i) {
            if (i == 0) {
              bound = w.table.Lookup(ScopeId(1));
            } else if (bound != nullptr) {
              observed[i - 1] = Field(*bound, i - 1);
            }
          });
      EXPECT_NE(bound, nullptr);
    }
    Settle(w.engine, w.domain);
    seen.insert(observed);
    schedules++;
  });
  EXPECT_EQ(schedules, 21u);  // C(7, 5): the whole space was enumerated
  EXPECT_EQ(seen, (std::set<Fields>{FieldsOf(SessionPolicy::At(1)),
                                    FieldsOf(SessionPolicy::At(2))}));
}

// The harness can see a tear: a reader that breaks the contract by looking the
// scope up again for each field does observe a mix of old and new. (Without
// this, the test above could pass because nothing could ever go wrong.)
TEST(ScopeSnapshotModelTest, ReaderThatRebindsPerFieldSeesATear) {
  std::set<Fields> seen;
  ForEachSchedule(4, /*last_slot=*/2, [&](const std::vector<size_t> &slot) {
    SessionWorld w;
    ASSERT_EQ(w.Snapshot({w.Set(1, 1)}).outcome, Outcome::kApplied);
    Fields observed{};
    {
      HeldReader reader(w.domain);
      Interleave(
          w.probe, w.engine, slot, 1,
          [&] {
            EXPECT_EQ(w.Snapshot({w.Set(1, 2)}).outcome, Outcome::kApplied);
          },
          [&](size_t i) {
            const SessionPolicy *p = w.table.Lookup(ScopeId(1));
            ASSERT_NE(p, nullptr);
            observed[i] = Field(*p, i);
          });
    }
    Settle(w.engine, w.domain);
    seen.insert(observed);
  });
  size_t torn = 0;
  for (const Fields &f : seen) {
    const bool old_epoch = f[0] == 1;
    for (size_t i = 1; i < 4; i++) {
      torn += (f[i] / 10 == (old_epoch ? 1u : 2u)) ? 0 : 1;
    }
  }
  EXPECT_GT(torn, 0u);
}

// Two sessions switched by one scope-snapshot transaction: each is whole (old
// or new) whenever it is read; between the two there is no order. A reader
// that binds both can see one switched and the other not.
TEST(ScopeSnapshotModelTest, TwoScopesEachSwitchWholeWithNoOrderBetweenThem) {
  std::set<std::pair<uint32_t, uint32_t>> seen;  // (epoch of A, epoch of B)
  ForEachSchedule(2, /*last_slot=*/3, [&](const std::vector<size_t> &slot) {
    SessionWorld w;
    ASSERT_EQ(w.Snapshot({w.Set(1, 1), w.Set(2, 1)}).outcome,
              Outcome::kApplied);
    std::array<SessionPolicy, 2> bound{};
    {
      HeldReader reader(w.domain);
      Interleave(
          w.probe, w.engine, slot, /*steps=*/2,
          [&] {
            EXPECT_EQ(w.Snapshot({w.Set(1, 2), w.Set(2, 2)}).outcome,
                      Outcome::kApplied);
          },
          [&](size_t i) {
            const SessionPolicy *p =
                w.table.Lookup(ScopeId(static_cast<uint32_t>(i + 1)));
            ASSERT_NE(p, nullptr);
            bound[i] = *p;
          });
    }
    Settle(w.engine, w.domain);
    EXPECT_TRUE(bound[0].Coherent());
    EXPECT_TRUE(bound[1].Coherent());
    seen.insert({bound[0].epoch, bound[1].epoch});
  });
  EXPECT_TRUE(seen.contains({1, 1}));
  EXPECT_TRUE(seen.contains({2, 2}));
  // Not simultaneous, and documented as not: some interleaving shows one of the
  // two switched and the other not yet.
  EXPECT_TRUE(seen.contains({2, 1}) || seen.contains({1, 2}));
  for (const auto &[a, b] : seen) {
    EXPECT_TRUE(a == 1 || a == 2);
    EXPECT_TRUE(b == 1 || b == 2);
  }
}

// The policy group. A packet binds the group once and resolves key -> class ->
// action through its two layers: always from one version.
TEST(ScopeSnapshotModelTest, GroupBoundOnceResolvesThroughOneVersion) {
  std::set<std::optional<uint32_t>> seen;
  size_t schedules = 0;
  ForEachSchedule(3, /*last_slot=*/2, [&](const std::vector<size_t> &slot) {
    GroupWorld w;
    ASSERT_EQ(w.engine
                  .Apply(std::vector<Op>{w.Set(1, 1)}, std::nullopt,
                         Consistency::kScopeSnapshot)
                  .outcome,
              Outcome::kApplied);
    std::optional<uint32_t> cls, result;
    {
      HeldReader reader(w.domain);
      const GroupVersion *bound = nullptr;
      Interleave(
          w.probe, w.engine, slot, 1,
          [&] {
            EXPECT_EQ(w.engine
                          .Apply(std::vector<Op>{w.Set(1, 2)}, std::nullopt,
                                 Consistency::kScopeSnapshot)
                          .outcome,
                      Outcome::kApplied);
          },
          [&](size_t i) {
            if (i == 0) {
              bound = w.table.Lookup(ScopeId(1));
            } else if (bound != nullptr && i == 1) {
              cls = Find(bound->classify, 2);
            } else if (bound != nullptr && cls) {
              result = Find(bound->action, *cls);
            }
          });
    }
    Settle(w.engine, w.domain);
    seen.insert(result);
    schedules++;
  });
  EXPECT_EQ(schedules, 10u);
  EXPECT_EQ(seen, (std::set<std::optional<uint32_t>>{1002u, 2002u}));
}

// The same group stored the referential way (each layer replaced on its own):
// a packet can resolve through the old classification and the new actions, and
// find nothing. This is the documented intermediate state of a referential
// transaction, and the reason a group that needs one version asks for a scope.
TEST(ScopeSnapshotModelTest, GroupUpdatedReferentiallyCanBeObservedHalfApplied) {
  std::set<std::optional<uint32_t>> seen;
  ForEachSchedule(2, /*last_slot=*/3, [&](const std::vector<size_t> &slot) {
    LayeredGroupWorld w;
    ASSERT_EQ(
        w.engine.Apply(std::vector<Op>{w.SetClassify(1), w.SetAction(1)})
            .outcome,
        Outcome::kApplied);
    const Layer *classify = nullptr, *action = nullptr;
    std::optional<uint32_t> result;
    {
      HeldReader reader(w.domain);
      Interleave(
          w.probe, w.engine, slot, /*steps=*/2,
          [&] {
            EXPECT_EQ(w.engine
                          .Apply(std::vector<Op>{w.SetClassify(2),
                                                 w.SetAction(2)})
                          .outcome,
                      Outcome::kApplied);
          },
          [&](size_t i) {
            (i == 0 ? classify : action) =
                w.table.Lookup(LayerId(static_cast<uint32_t>(i + 1)));
          });
      ASSERT_NE(classify, nullptr);
      ASSERT_NE(action, nullptr);
      result = Resolve(*classify, *action, 2);
    }
    Settle(w.engine, w.domain);
    seen.insert(result);
  });
  EXPECT_EQ(seen, (std::set<std::optional<uint32_t>>{1002u, 2002u,
                                                     std::nullopt}));
}

// Referential contract, for a referrer (next hop) naming a referent (meter),
// both replaced in one transaction. A reader that finds the new next hop and
// follows its reference finds the new meter -- referents are published first --
// but a reader may find the new meter under the old next hop.
TEST(ReferentialModelTest, ReaderThatFollowsAReferenceFindsAtLeastTheNewReferent) {
  std::set<std::pair<uint32_t, uint32_t>> seen;  // (next hop epoch, meter epoch)
  ForEachSchedule(2, /*last_slot=*/3, [&](const std::vector<size_t> &slot) {
    ChainWorld w;
    ASSERT_EQ(w.engine
                  .Apply(std::vector<Op>{w.Met(1, 1), w.Hop(1, 1, 1)})
                  .outcome,
              Outcome::kApplied);
    const NextHop *hop = nullptr;
    const Meter *meter = nullptr;
    {
      HeldReader reader(w.domain);
      Interleave(
          w.probe, w.engine, slot, 2,
          [&] {
            // Referrer first in the request; the engine publishes referents
            // first regardless.
            EXPECT_EQ(w.engine
                          .Apply(std::vector<Op>{w.Hop(1, 2, 1), w.Met(1, 2)})
                          .outcome,
                      Outcome::kApplied);
          },
          [&](size_t i) {
            if (i == 0) {
              hop = w.hops.Lookup(NextHopId(1));
            } else if (hop != nullptr) {
              meter = w.meters.Lookup(MeterId(hop->meter));
            }
          });
      ASSERT_NE(hop, nullptr);
      ASSERT_NE(meter, nullptr);
      seen.insert({hop->epoch, meter->epoch});
    }
    Settle(w.engine, w.domain);
  });
  EXPECT_EQ(seen, (std::set<std::pair<uint32_t, uint32_t>>{
                      {1, 1}, {1, 2}, {2, 2}}));  // never (2, 1)
}

// The reverse reader (meter, then next hop) may see all four combinations: the
// mixed generations are the documented price of referential visibility.
TEST(ReferentialModelTest, ReaderOutsideTheReferenceOrderMaySeeMixedGenerations) {
  std::set<std::pair<uint32_t, uint32_t>> seen;  // (meter epoch, next hop epoch)
  ForEachSchedule(2, /*last_slot=*/3, [&](const std::vector<size_t> &slot) {
    ChainWorld w;
    ASSERT_EQ(w.engine
                  .Apply(std::vector<Op>{w.Met(1, 1), w.Hop(1, 1, 1)})
                  .outcome,
              Outcome::kApplied);
    const NextHop *hop = nullptr;
    const Meter *meter = nullptr;
    {
      HeldReader reader(w.domain);
      Interleave(
          w.probe, w.engine, slot, 2,
          [&] {
            EXPECT_EQ(w.engine
                          .Apply(std::vector<Op>{w.Met(1, 2), w.Hop(1, 2, 1)})
                          .outcome,
                      Outcome::kApplied);
          },
          [&](size_t i) {
            if (i == 0) {
              meter = w.meters.Lookup(MeterId(1));
            } else {
              hop = w.hops.Lookup(NextHopId(1));
            }
          });
      ASSERT_NE(hop, nullptr);
      ASSERT_NE(meter, nullptr);
      seen.insert({meter->epoch, hop->epoch});
    }
    Settle(w.engine, w.domain);
  });
  EXPECT_EQ(seen, (std::set<std::pair<uint32_t, uint32_t>>{
                      {1, 1}, {1, 2}, {2, 1}, {2, 2}}));
}

// A reader sees a new next hop only together with the meter it names, and
// nothing of the transaction before.
TEST(ReferentialModelTest, NewReferrerIsNeverVisibleWithoutItsReferent) {
  std::set<std::pair<bool, bool>> seen;  // (hop visible, meter visible)
  ForEachSchedule(2, /*last_slot=*/3, [&](const std::vector<size_t> &slot) {
    ChainWorld w;
    const NextHop *hop = nullptr;
    const Meter *meter = nullptr;
    {
      HeldReader reader(w.domain);
      Interleave(
          w.probe, w.engine, slot, 2,
          [&] {
            EXPECT_EQ(w.engine
                          .Apply(std::vector<Op>{w.Hop(3, 1, 7), w.Met(7, 1)})
                          .outcome,
                      Outcome::kApplied);
          },
          [&](size_t i) {
            if (i == 0) {
              hop = w.hops.Lookup(NextHopId(3));
            } else if (hop != nullptr) {
              meter = w.meters.Lookup(MeterId(hop->meter));
            }
          });
      seen.insert({hop != nullptr, meter != nullptr});
    }
    Settle(w.engine, w.domain);
  });
  EXPECT_EQ(seen, (std::set<std::pair<bool, bool>>{{false, false},
                                                   {true, true}}));
}

// A reader holding an old next hop keeps its meter for as long as it has not
// reached a quiescent state, even after the transaction erasing both returned;
// once it has, both go.
TEST(ReferentialModelTest, ErasedReferentOutlivesReadersHoldingItsReferrer) {
  ForEachSchedule(2, /*last_slot=*/3, [&](const std::vector<size_t> &slot) {
    ChainWorld w;
    ASSERT_EQ(w.engine
                  .Apply(std::vector<Op>{w.Met(7, 1), w.Hop(3, 1, 7)})
                  .outcome,
              Outcome::kApplied);
    const NextHop *hop = nullptr;
    const Meter *meter = nullptr;
    {
      HeldReader reader(w.domain);
      Interleave(
          w.probe, w.engine, slot, 2,
          [&] {
            EXPECT_EQ(w.engine
                          .Apply(std::vector<Op>{w.EraseMet(7), w.EraseHop(3)})
                          .outcome,
                      Outcome::kApplied);
          },
          [&](size_t i) {
            if (i == 0) {
              hop = w.hops.Lookup(NextHopId(3));
            } else if (hop != nullptr) {
              meter = w.meters.Lookup(MeterId(hop->meter));
            }
          });
      EXPECT_NE(hop, nullptr);
      EXPECT_NE(meter, nullptr);  // the reader that saw the hop finds its meter
      if (hop != nullptr) {
        EXPECT_EQ(hop->meter, 7u);
      }
    }
    Settle(w.engine, w.domain);
    EXPECT_EQ(w.hops.Lookup(NextHopId(3)), nullptr);
    EXPECT_EQ(w.meters.Lookup(MeterId(7)), nullptr);
  });
}

// =========================================================================================
// 2. Threads: real RCU readers against a hammering writer
// =========================================================================================

constexpr uint32_t kEpochs = 2500;

// The writer keeps going for at least this long as well, so that the readers
// (which take a while to start) certainly overlap it.
constexpr std::chrono::milliseconds kStressTime{300};

bool StressRunning(uint32_t epoch,
                   std::chrono::steady_clock::time_point deadline) {
  return epoch <= kEpochs || std::chrono::steady_clock::now() < deadline;
}

// Three sessions are switched together, over and over, while two readers
// (each a registered, online RCU reader reporting quiescence between "packet
// operations") bind a session and check the policy field by field, with
// yields in between to widen the window. Scope 3 is also erased and recreated
// now and then.
TEST(ScopeSnapshotStressTest, ConcurrentSessionReadersNeverSeeATornPolicy) {
  SessionWorld w;
  ASSERT_EQ(w.Snapshot({w.Set(1, 1), w.Set(2, 1), w.Set(3, 1)}).outcome,
            Outcome::kApplied);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> torn{0}, backwards{0}, reads{0}, absent{0};
  std::vector<std::thread> readers;
  for (rcu::ReaderId id = 1; id <= 2; id++) {
    ASSERT_TRUE(w.domain.Register(id).has_value());
    readers.emplace_back([&, id] {
      w.domain.Online(id);
      std::array<uint32_t, 4> last{};
      while (!stop.load(std::memory_order_relaxed)) {
        for (uint32_t scope = 1; scope <= 3; scope++) {
          const SessionPolicy *p = w.table.Lookup(ScopeId(scope));
          if (p == nullptr) {
            absent++;
            continue;
          }
          const uint32_t epoch = p->epoch;
          std::this_thread::yield();
          const uint32_t meter = p->meter;
          std::this_thread::yield();
          const uint32_t next_hop = p->next_hop;
          const uint32_t qos = p->qos;
          const SessionPolicy expected = SessionPolicy::At(epoch);
          if (meter != expected.meter || next_hop != expected.next_hop ||
              qos != expected.qos) {
            torn++;
          }
          if (epoch < last[scope]) {
            backwards++;
          }
          last[scope] = epoch;
          reads++;
        }
        w.domain.Quiescent(id);
      }
      w.domain.Offline(id);
    });
  }

  // Reclamation behind the readers (kBusy), or the id of an erased scope not
  // yet free (kRejected, "retiring"): let the readers move on and try again.
  auto retryable = [](const TransactionEngine::Result &r) {
    if (r.outcome == Outcome::kBusy) {
      return true;
    }
    for (const auto &op : r.ops) {
      if (op.error.find("retiring") != std::string::npos) {
        return true;
      }
    }
    return false;
  };
  auto apply = [&](const std::vector<Op> &ops) {
    for (;;) {
      const auto r = w.Snapshot(ops);
      if (r.outcome == Outcome::kApplied) {
        return true;
      }
      if (!retryable(r)) {
        return false;
      }
      w.engine.ReclaimRetired();
      std::this_thread::yield();
    }
  };
  bool writer_ok = true;
  const auto deadline = std::chrono::steady_clock::now() + kStressTime;
  uint32_t epoch = 2;
  for (; StressRunning(epoch, deadline) && writer_ok; epoch++) {
    if (epoch % 100 == 0) {
      // Retire scope 3, then bring it back under the new epoch.
      writer_ok = apply({w.Set(1, epoch), w.Set(2, epoch), w.Erase(3)});
      writer_ok = writer_ok && apply({w.Set(3, epoch)});
    } else {
      writer_ok = apply({w.Set(1, epoch), w.Set(2, epoch), w.Set(3, epoch)});
    }
    if (epoch % 8 == 0) {
      w.engine.ReclaimRetired();
    }
  }
  stop = true;
  for (auto &t : readers) {
    t.join();
  }
  EXPECT_TRUE(writer_ok);
  EXPECT_EQ(torn.load(), 0u);
  EXPECT_EQ(backwards.load(), 0u) << "a reader saw a scope go back in time";
  EXPECT_GT(reads.load(), 1000u);
  std::printf("[stress] sessions: %u epochs, %llu reads, %llu absent\n",
              epoch - 2, static_cast<unsigned long long>(reads.load()),
              static_cast<unsigned long long>(absent.load()));
  for (rcu::ReaderId id = 1; id <= 2; id++) {
    w.domain.Unregister(id);
  }
  Settle(w.engine, w.domain);
}

TEST(ScopeSnapshotStressTest, ConcurrentGroupReadersAlwaysResolveThroughOneVersion) {
  GroupWorld w;
  ASSERT_EQ(w.engine
                .Apply(std::vector<Op>{w.Set(1, 1)}, std::nullopt,
                       Consistency::kScopeSnapshot)
                .outcome,
            Outcome::kApplied);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> misses{0}, wrong{0}, resolved{0};
  std::vector<std::thread> readers;
  for (rcu::ReaderId id = 1; id <= 2; id++) {
    ASSERT_TRUE(w.domain.Register(id).has_value());
    readers.emplace_back([&, id] {
      w.domain.Online(id);
      while (!stop.load(std::memory_order_relaxed)) {
        const GroupVersion *group = w.table.Lookup(ScopeId(1));
        if (group != nullptr) {
          for (uint32_t key = 0; key < 4; key++) {
            const auto cls = Find(group->classify, key);
            std::this_thread::yield();
            const auto action =
                cls ? Find(group->action, *cls) : std::nullopt;
            if (!action) {
              misses++;
            } else if (*action != group->classify.version * 1000 + key) {
              wrong++;
            } else {
              resolved++;
            }
          }
        }
        w.domain.Quiescent(id);
      }
      w.domain.Offline(id);
    });
  }
  bool writer_ok = true;
  const auto deadline = std::chrono::steady_clock::now() + kStressTime;
  uint32_t version = 2;
  for (; StressRunning(version, deadline) && writer_ok; version++) {
    for (;;) {
      const auto r = w.engine.Apply(std::vector<Op>{w.Set(1, version)},
                                    std::nullopt, Consistency::kScopeSnapshot);
      if (r.outcome == Outcome::kApplied) {
        break;
      }
      if (r.outcome != Outcome::kBusy) {
        writer_ok = false;
        break;
      }
      w.engine.ReclaimRetired();
      std::this_thread::yield();
    }
    if (version % 8 == 0) {
      w.engine.ReclaimRetired();
    }
  }
  stop = true;
  for (auto &t : readers) {
    t.join();
  }
  EXPECT_TRUE(writer_ok);
  EXPECT_EQ(misses.load(), 0u);
  EXPECT_EQ(wrong.load(), 0u);
  EXPECT_GT(resolved.load(), 1000u);
  std::printf("[stress] group: %u versions, %llu resolutions\n", version - 2,
              static_cast<unsigned long long>(resolved.load()));
  for (rcu::ReaderId id = 1; id <= 2; id++) {
    w.domain.Unregister(id);
  }
  Settle(w.engine, w.domain);
}

// A referential transaction under real readers: the next hop is never seen
// without its meter, and never with an older one than the transaction that
// published it left.
TEST(ReferentialStressTest, ConcurrentReadersFollowReferencesToAtLeastTheSameEpoch) {
  ChainWorld w;
  ASSERT_EQ(w.engine.Apply(std::vector<Op>{w.Met(1, 1), w.Hop(1, 1, 1)}).outcome,
            Outcome::kApplied);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> dangling{0}, older{0}, reads{0};
  std::vector<std::thread> readers;
  for (rcu::ReaderId id = 1; id <= 2; id++) {
    ASSERT_TRUE(w.domain.Register(id).has_value());
    readers.emplace_back([&, id] {
      w.domain.Online(id);
      while (!stop.load(std::memory_order_relaxed)) {
        const NextHop *hop = w.hops.Lookup(NextHopId(1));
        if (hop != nullptr) {
          std::this_thread::yield();
          const Meter *meter = w.meters.Lookup(MeterId(hop->meter));
          if (meter == nullptr) {
            dangling++;
          } else if (meter->epoch < hop->epoch) {
            older++;
          }
          reads++;
        }
        w.domain.Quiescent(id);
      }
      w.domain.Offline(id);
    });
  }
  bool writer_ok = true;
  const auto deadline = std::chrono::steady_clock::now() + kStressTime;
  // Keep writing until the readers have done their reads too: on one CPU they
  // get few turns in kStressTime. The hard deadline only stops a hang.
  const auto hard_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  uint32_t epoch = 2;
  for (; (StressRunning(epoch, deadline) || reads.load() <= 1000u) &&
         std::chrono::steady_clock::now() < hard_deadline && writer_ok;
       epoch++) {
    if (epoch % 64 == 0) {
      std::this_thread::yield();
    }
    for (;;) {
      const auto r =
          w.engine.Apply(std::vector<Op>{w.Hop(1, epoch, 1), w.Met(1, epoch)});
      if (r.outcome == Outcome::kApplied) {
        break;
      }
      if (r.outcome != Outcome::kBusy) {
        writer_ok = false;
        break;
      }
      w.engine.ReclaimRetired();
      std::this_thread::yield();
    }
    if (epoch % 8 == 0) {
      w.engine.ReclaimRetired();
    }
  }
  stop = true;
  for (auto &t : readers) {
    t.join();
  }
  EXPECT_TRUE(writer_ok);
  EXPECT_EQ(dangling.load(), 0u);
  EXPECT_EQ(older.load(), 0u);
  EXPECT_GT(reads.load(), 1000u);
  std::printf("[stress] referential: %u epochs, %llu reads\n", epoch - 2,
              static_cast<unsigned long long>(reads.load()));
  for (rcu::ReaderId id = 1; id <= 2; id++) {
    w.domain.Unregister(id);
  }
  Settle(w.engine, w.domain);
}

// =========================================================================================
// 3. Failure injection: a scope-snapshot transaction that cannot be prepared
// =========================================================================================

// Four sessions at epoch 1, a switch of all four to epoch 2, and a failure of
// the n-th reservation -- by an error and by an exception. Whatever was
// reserved before it is undone: not one scope changed, no publication step
// ran, and the same switch applies afterwards.
TEST(ScopeSnapshotFailureTest, UnpreparableSwitchLeavesEveryOldScopeVisible) {
  for (const bool by_throwing : {false, true}) {
    for (int at = 1; at <= 4; at++) {
      SCOPED_TRACE(testing::Message() << "failure #" << at
                                      << (by_throwing ? " (throws)" : ""));
      SessionWorld w;
      const std::vector<Op> base = {w.Set(1, 1), w.Set(2, 1), w.Set(3, 1),
                                    w.Set(4, 1)};
      ASSERT_EQ(w.Snapshot(base).outcome, Outcome::kApplied);
      const uint64_t generation = w.engine.generation();
      std::array<const SessionPolicy *, 4> before{};
      for (uint32_t i = 0; i < 4; i++) {
        before[i] = w.table.Lookup(ScopeId(i + 1));
      }

      const std::vector<Op> to_two = {w.Set(1, 2), w.Set(2, 2), w.Set(3, 2),
                                      w.Set(4, 2)};
      w.probe.Arm(at, by_throwing);
      w.probe.published = 0;
      if (by_throwing) {
        EXPECT_THROW(w.Snapshot(to_two), std::bad_alloc);
      } else {
        const auto r = w.Snapshot(to_two);
        EXPECT_EQ(r.outcome, Outcome::kRejected);
        for (int i = 0; i < 4; i++) {
          EXPECT_EQ(r.ops[i].status, i + 1 == at ? OpStatus::kFailed
                                                 : OpStatus::kNotApplied);
        }
      }
      EXPECT_EQ(w.probe.published, 0u);  // nothing became visible, even briefly
      EXPECT_EQ(w.engine.generation(), generation);
      for (uint32_t i = 0; i < 4; i++) {
        // Not just equal contents: the very same version objects.
        EXPECT_EQ(w.table.Lookup(ScopeId(i + 1)), before[i]);
        EXPECT_EQ(w.table.Lookup(ScopeId(i + 1))->epoch, 1u);
      }

      w.probe.Arm(0, false);
      ASSERT_EQ(w.Snapshot(to_two).outcome, Outcome::kApplied);
      EXPECT_EQ(w.probe.published, 4u);
      for (uint32_t i = 0; i < 4; i++) {
        EXPECT_EQ(w.table.Lookup(ScopeId(i + 1))->epoch, 2u);
      }
      Settle(w.engine, w.domain);
    }
  }
}

// A reader that began before the failed switch reads the same, whole, old
// policy after it.
TEST(ScopeSnapshotFailureTest, ReaderHoldingTheOldVersionIsUndisturbedByAFailedSwitch) {
  SessionWorld w;
  ASSERT_EQ(w.Snapshot({w.Set(1, 1), w.Set(2, 1)}).outcome, Outcome::kApplied);
  HeldReader reader(w.domain);
  const SessionPolicy *held = w.table.Lookup(ScopeId(1));
  ASSERT_NE(held, nullptr);
  w.probe.Arm(2, false);
  EXPECT_EQ(w.Snapshot({w.Set(1, 2), w.Set(2, 2)}).outcome, Outcome::kRejected);
  EXPECT_EQ(w.table.Lookup(ScopeId(1)), held);
  EXPECT_EQ(*held, SessionPolicy::At(1));
}

// A scope version names the next hop its packets go to (a separate, shared
// object). The engine tracks that reference: a switch to a version naming a
// missing hop is refused with the old scope intact, a hop a live version names
// cannot be erased, and an erased hop stays readable for a reader still
// holding the old version that named it.
struct HopTag;
using HopId = StrongId<HopTag, uint32_t>;
struct Hop {
  uint32_t gate = 0;
};
struct RoutedSession {
  uint32_t epoch = 0;
  uint32_t hop = 0;
};

TEST(ScopeSnapshotFailureTest, ScopeVersionsKeepWhatTheyReferenceAlive) {
  rcu::RcuDomain domain(8);
  ScopeTable<RoutedSession> sessions(8);
  SlotTable<HopId, Hop> hops(8);
  ScopeResource<RoutedSession> sessions_res(
      "sessions", sessions,
      [](const RoutedSession &s) {
        return std::vector<Reference>{{"hops", EncodeKey(HopId(s.hop))}};
      },
      std::vector<std::string>{"hops"});
  SlotResource<HopId, Hop> hops_res("hops", hops);
  TransactionEngine engine(domain);
  ASSERT_TRUE(engine.Register(&hops_res).has_value());
  ASSERT_TRUE(engine.Register(&sessions_res).has_value());
  auto session = [](uint32_t epoch, uint32_t hop) {
    return Op::Upsert("sessions", EncodeKey(ScopeId(1)),
                      std::any(RoutedSession{epoch, hop}));
  };
  auto hop_op = [](uint32_t id) {
    return Op::Upsert("hops", EncodeKey(HopId(id)), std::any(Hop{id}));
  };
  auto snapshot = [&](const std::vector<Op> &ops) {
    return engine.Apply(ops, std::nullopt, Consistency::kScopeSnapshot);
  };

  // The new hop is created first, in a referential transaction: nothing can
  // name it yet. Then the scope switches to a version that does.
  ASSERT_EQ(engine.Apply(std::vector<Op>{hop_op(1)}).outcome, Outcome::kApplied);
  ASSERT_EQ(snapshot({session(1, 1)}).outcome, Outcome::kApplied);

  // A version naming a hop that does not exist: refused, old scope visible.
  const RoutedSession *old_version = sessions.Lookup(ScopeId(1));
  const auto missing = snapshot({session(2, 9)});
  EXPECT_EQ(missing.outcome, Outcome::kRejected);
  EXPECT_NE(missing.ops[0].error.find("missing"), std::string::npos)
      << missing.ops[0].error;
  EXPECT_EQ(sessions.Lookup(ScopeId(1)), old_version);
  EXPECT_EQ(old_version->epoch, 1u);

  // The hop the live version names cannot be erased.
  const auto still = engine.Apply(std::vector<Op>{Op::Erase("hops", EncodeKey(HopId(1)))});
  EXPECT_EQ(still.outcome, Outcome::kRejected);
  EXPECT_NE(still.ops[0].error.find("still referenced"), std::string::npos)
      << still.ops[0].error;

  // Move the scope to hop 2, then erase hop 1 while a reader still holds the
  // version that named it.
  ASSERT_EQ(engine.Apply(std::vector<Op>{hop_op(2)}).outcome, Outcome::kApplied);
  {
    HeldReader reader(domain);
    const RoutedSession *held = sessions.Lookup(ScopeId(1));
    ASSERT_EQ(snapshot({session(2, 2)}).outcome, Outcome::kApplied);
    ASSERT_EQ(
        engine.Apply(std::vector<Op>{Op::Erase("hops", EncodeKey(HopId(1)))})
            .outcome,
        Outcome::kApplied);
    engine.ReclaimRetired();
    ASSERT_EQ(held->hop, 1u);
    EXPECT_NE(hops.Lookup(HopId(1)), nullptr)
        << "the hop went while a reader could still follow it";
  }
  Settle(engine, domain);
  EXPECT_EQ(hops.Lookup(HopId(1)), nullptr);
  EXPECT_EQ(sessions.Lookup(ScopeId(1))->hop, 2u);
}

// =========================================================================================
// 4. The typed refusal of a level a resource cannot provide
// =========================================================================================

struct MixedWorld {
  MixedWorld() {
    EXPECT_TRUE(engine.Register(&meters_observed).has_value());
    EXPECT_TRUE(engine.Register(&sessions_observed).has_value());
  }
  Op Session(uint32_t epoch) const { return SessionWorld::Set(1, epoch); }
  static Op Met(uint32_t id, uint32_t epoch) {
    return ChainWorld::Met(id, epoch);
  }

  rcu::RcuDomain domain{8};
  SlotTable<MeterId, Meter> meters{16};
  SlotResource<MeterId, Meter> meters_resource{"meters", meters};
  ScopeTable<SessionPolicy> sessions{16};
  ScopeResource<SessionPolicy> sessions_resource{"sessions", sessions};
  Probe probe;
  Observed meters_observed{meters_resource, probe};
  Observed sessions_observed{sessions_resource, probe};
  TransactionEngine engine{domain};
};

TEST(ScopeSnapshotRefusalTest, ResourcesDeclareWhatTheyCanProvide) {
  SlotTable<MeterId, Meter> meters(4);
  SlotResource<MeterId, Meter> plain("meters", meters);
  ScopeTable<SessionPolicy> sessions(4);
  ScopeResource<SessionPolicy> scopes("sessions", sessions);
  EXPECT_EQ(plain.ProvidedConsistency(), Consistency::kReferential);
  EXPECT_EQ(scopes.ProvidedConsistency(), Consistency::kScopeSnapshot);
}

// A scope-snapshot request that names any resource without the capability is
// answered kUnsupported, before anything is reserved: the scope operations in
// the same request are not applied either, and the same operations apply as a
// referential transaction. Never "snapshot where possible".
TEST(ScopeSnapshotRefusalTest, RequestNamingAReferentialResourceIsRefusedNotDowngraded) {
  struct Case {
    const char *name;
    std::function<std::vector<Op>(const MixedWorld &)> ops;
    size_t culprit;  // index of the operation that cannot take part
  };
  const std::vector<Case> cases = {
      {"only a meter", [](const MixedWorld &) {
         return std::vector<Op>{MixedWorld::Met(1, 1)};
       }, 0},
      {"scope then meter", [](const MixedWorld &w) {
         return std::vector<Op>{w.Session(1), MixedWorld::Met(1, 1)};
       }, 1},
      {"meter then scope", [](const MixedWorld &w) {
         return std::vector<Op>{MixedWorld::Met(1, 1), w.Session(1)};
       }, 0},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.name);
    MixedWorld w;
    const std::vector<Op> ops = c.ops(w);
    const uint64_t generation = w.engine.generation();

    const auto refused =
        w.engine.Apply(ops, std::nullopt, Consistency::kScopeSnapshot);
    ASSERT_EQ(refused.outcome, Outcome::kUnsupported);
    EXPECT_EQ(refused.generation, generation);
    ASSERT_EQ(refused.ops.size(), ops.size());
    for (size_t i = 0; i < ops.size(); i++) {
      EXPECT_EQ(refused.ops[i].status, i == c.culprit ? OpStatus::kFailed
                                                      : OpStatus::kNotApplied);
    }
    // The reason names the resource and the level.
    EXPECT_NE(refused.ops[c.culprit].error.find("'meters'"), std::string::npos)
        << refused.ops[c.culprit].error;
    EXPECT_NE(refused.ops[c.culprit].error.find("scope-snapshot"),
              std::string::npos);
    // Nothing, of either resource, became visible.
    EXPECT_EQ(w.probe.published, 0u);
    EXPECT_EQ(w.engine.generation(), generation);
    EXPECT_EQ(w.meters.Lookup(MeterId(1)), nullptr);
    EXPECT_EQ(w.sessions.Lookup(ScopeId(1)), nullptr);

    // The same operations are a perfectly good referential transaction.
    const auto referential = w.engine.Apply(ops);
    EXPECT_EQ(referential.outcome, Outcome::kApplied);
    EXPECT_NE(w.meters.Lookup(MeterId(1)), nullptr);
    Settle(w.engine, w.domain);
  }
}

// A request that only names scope resources gets the level; a referential
// request on a scope resource is fine too (the strongest level a resource
// provides is not forced on every transaction).
TEST(ScopeSnapshotRefusalTest, ScopeOnlyRequestIsServedAtTheRequestedLevel) {
  MixedWorld w;
  EXPECT_EQ(w.engine
                .Apply(std::vector<Op>{w.Session(1)}, std::nullopt,
                       Consistency::kScopeSnapshot)
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(w.engine.Apply(std::vector<Op>{w.Session(2)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(w.sessions.Lookup(ScopeId(1))->epoch, 2u);
  Settle(w.engine, w.domain);
}

}  // namespace
}  // namespace bess::dataplane
