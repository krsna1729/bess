// SPDX-License-Identifier: BSD-3-Clause

// Framework conformance tests (D-021, D-024, D-129): what every module that
// takes part in the transaction framework must promise, checked across
// modules rather than one module at a time.
//
//  - Init/resource parity: every table a module class declares is exactly the
//    set of resources the module registers, with the declared kind and codec
//    types, alive while it is initialized and gone after DeInit; a typed
//    write through the codec reads back, and a mismatched type is refused.
//  - The staging/publish contract: a failure while a resource stages its work
//    leaves nothing visible, aborts the reservation, leaks no pending key,
//    arena id or slot, and the same transaction applies on retry; and the
//    publish phase of every framework resource allocates nothing.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <any>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/empty.pb.h>

#include "classifier/concurrent_exact.h"
#include "classifier/concurrent_masked.h"
#include "classifier/exact_rule_resource.h"
#include "classifier/masked_rule_resource.h"
#include "dataplane/slot_resource.h"
#include "dataplane/slot_table.h"
#include "dataplane/strong_id.h"
#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/singleton_resource.h"
#include "module.h"
#include "module_graph.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "testing/allocation_faults.h"
#include "testing/module_harness.h"

namespace bess {
namespace {

namespace pb = bess::pb;
namespace dataplane = bess::dataplane;
namespace framework = bess::framework;

using dataplane::Op;
using dataplane::Resource;
using dataplane::ResourceKey;
using dataplane::TransactionEngine;
using Outcome = TransactionEngine::Outcome;
using OpStatus = TransactionEngine::OpStatus;
using testing::ModuleHarness;

// ---------------------------------------------------------------------------
// Init/resource parity
// ---------------------------------------------------------------------------

struct Sample {
  std::string table;
  std::shared_ptr<google::protobuf::Message> key;  // null: a singleton
  std::shared_ptr<google::protobuf::Message> value;
};

struct ModuleCase {
  std::string mclass;
  std::shared_ptr<google::protobuf::Message> arg;
  std::vector<Sample> samples;
};

template <typename M>
std::shared_ptr<M> Msg() {
  return std::make_shared<M>();
}

pb::FieldData BytesField(const std::string &bytes) {
  pb::FieldData field;
  field.set_value_bin(bytes);
  return field;
}

// One representative instance of each module class that registers framework
// resources, with one well-formed entry per declared table. The entries are
// listed referent before referrer (Router's next hop, then group, then the
// route naming them) so each write is valid where it is made.
std::vector<ModuleCase> ParityCases() {
  std::vector<ModuleCase> cases;

  {
    ModuleCase c;
    c.mclass = "ExactMatch";
    auto arg = Msg<pb::ExactMatchArg>();
    arg->add_fields()->set_offset(26);
    arg->mutable_fields(0)->set_num_bytes(4);
    c.arg = arg;

    auto rule_key = Msg<pb::ExactMatchRuleKey>();
    *rule_key->add_fields() = BytesField(std::string("\x01\x02\x03\x04", 4));
    auto rule_value = Msg<pb::ExactMatchRuleValue>();
    rule_value->set_gate(3);
    c.samples.push_back({"rules", rule_key, rule_value});

    auto settings = Msg<pb::ExactMatchSettings>();
    settings->set_default_gate(2);
    c.samples.push_back({"settings", nullptr, settings});
    cases.push_back(std::move(c));
  }

  {
    ModuleCase c;
    c.mclass = "WildcardMatch";
    auto arg = Msg<pb::WildcardMatchArg>();
    arg->add_fields()->set_offset(26);
    arg->mutable_fields(0)->set_num_bytes(4);
    c.arg = arg;

    auto rule_key = Msg<pb::WildcardMatchRuleKey>();
    *rule_key->add_values() = BytesField(std::string("\x0a\x00\x00\x01", 4));
    *rule_key->add_masks() = BytesField(std::string("\xff\xff\xff\xff", 4));
    auto rule_value = Msg<pb::WildcardMatchRuleValue>();
    rule_value->set_priority(1);
    rule_value->set_gate(4);
    c.samples.push_back({"rules", rule_key, rule_value});

    auto settings = Msg<pb::WildcardMatchSettings>();
    settings->set_default_gate(1);
    c.samples.push_back({"settings", nullptr, settings});
    cases.push_back(std::move(c));
  }

  {
    ModuleCase c;
    c.mclass = "Meter";
    auto arg = Msg<pb::MeterArg>();
    arg->set_capacity(64);
    c.arg = arg;

    auto key = Msg<pb::MeterIdKey>();
    key->set_id(1);
    auto value = Msg<pb::MeterPolicyValue>();
    value->mutable_sr_tcm()->set_committed_rate(1000);
    value->mutable_sr_tcm()->set_committed_burst(100);
    value->mutable_sr_tcm()->set_excess_burst(200);
    c.samples.push_back({"meters", key, value});
    cases.push_back(std::move(c));
  }

  {
    ModuleCase c;
    c.mclass = "Router";
    auto arg = Msg<pb::RouterArg>();
    // Destination mode: routes need a FIB, which the next-hop-id mode has
    // none of (D-183).
    arg->set_mode(pb::ROUTE_MODE_DESTINATION);
    arg->set_max_ipv4_routes(64);
    arg->set_max_next_hops(16);
    arg->set_max_groups(8);
    // An interface, so the neighbors table takes a binding.
    auto *interface = arg->add_interfaces();
    interface->set_egress_gate(1);
    interface->set_mac_addr("02:00:00:00:00:01");
    interface->add_addresses("192.0.2.1/24");
    c.arg = arg;

    auto hop_key = Msg<pb::RouterNextHopIdKey>();
    hop_key->set_id(1);
    auto hop = Msg<pb::RouterNextHopValue>();
    hop->set_egress_gate(2);  // an unset neighbor is the canonical read-back state
    c.samples.push_back({"next_hops", hop_key, hop});

    auto bound_key = Msg<pb::RouterNextHopIdKey>();
    bound_key->set_id(2);
    auto binding = Msg<pb::RouterNeighborBinding>();
    binding->set_interface_id(2);
    binding->set_ipv4("192.0.2.2");
    c.samples.push_back({"neighbors", bound_key, binding});

    auto group_key = Msg<pb::RouterGroupIdKey>();
    group_key->set_id(1);
    auto group = Msg<pb::RouterGroupValue>();
    group->add_member_ids(1);
    c.samples.push_back({"groups", group_key, group});

    auto route_key = Msg<pb::RouterRouteKey>();
    route_key->set_ipv4("10.0.0.0");
    route_key->set_prefix_length(8);
    auto route = Msg<pb::RouterRouteValue>();
    route->set_next_hop_id(1);
    c.samples.push_back({"routes", route_key, route});
    cases.push_back(std::move(c));
  }

  {
    ModuleCase c;
    c.mclass = "Firewall";
    c.arg = Msg<pb::FirewallArg>();

    auto key = Msg<pb::FirewallRuleKey>();
    key->set_id(1);
    auto value = Msg<pb::FirewallRuleValue>();
    value->set_priority(5);
    value->mutable_rule()->set_src_ip("10.0.0.0/8");
    value->mutable_rule()->set_drop(true);
    c.samples.push_back({"rules", key, value});
    cases.push_back(std::move(c));
  }

  return cases;
}

// A resource's name for `module`'s table `table`.
std::string ResourceName(const std::string &module, const std::string &table) {
  return module + "/" + table;
}

// The module's own resource names, from the engine's sorted list.
std::set<std::string> RegisteredNames(TransactionEngine &engine,
                                      const std::string &prefix) {
  std::set<std::string> names;
  for (const std::string &name : engine.ResourceNames()) {
    if (name.starts_with(prefix)) {
      names.insert(name);
    }
  }
  return names;
}

// Opens an allocation window when the engine starts publishing, so that what
// Publish() (and only Publish()) allocates is seen.
std::optional<fault_injection::AllocationFaults> g_publish_window;
std::atomic<uint64_t> g_publish_allocations{0};

void CountPublishAllocations(bool entering) {
  if (entering) {
    g_publish_window.emplace();
  } else if (g_publish_window) {
    g_publish_allocations += g_publish_window->allocations();
    g_publish_window.reset();
  }
}

TEST(ResourceParityTest, DeclaredTablesAreLiveTypedAndReleased) {
  ModuleHarness harness;
  TransactionEngine &engine = runtime::runtime().transactions();
  // A resolving Router needs the maintenance loop; stand in for its interval.
  runtime::runtime().ticks().set_interval(std::chrono::milliseconds(1));

  for (const ModuleCase &c : ParityCases()) {
    SCOPED_TRACE(c.mclass);
    Module *m = harness.Create(c.mclass, *c.arg, "parity_" + c.mclass);
    ASSERT_NE(m, nullptr);
    const std::string prefix = m->name() + "/";

    const ResourceTables &declared = m->module_builder()->resource_tables();
    ASSERT_FALSE(declared.empty()) << c.mclass << " declares no table";

    std::set<std::string> expected;
    for (const ResourceTable &table : declared) {
      expected.insert(ResourceName(m->name(), table.table));
    }
    EXPECT_EQ(RegisteredNames(engine, prefix), expected)
        << "a module registered a table its class does not declare, or omitted one";

    // While the module is initialized: every table is live and reachable
    // through a codec whose kinds and types are the declared ones.
    for (const ResourceTable &table : declared) {
      SCOPED_TRACE(table.table);
      const std::string name = ResourceName(m->name(), table.table);
      const Resource *resource = engine.FindResource(name);
      ASSERT_NE(resource, nullptr) << name << " is not registered";
      const framework::ResourceCodec *codec = harness.Codec(name);
      ASSERT_NE(codec, nullptr) << name << " has no codec";
      EXPECT_EQ(codec->kind() == framework::ResourceCodec::Kind::kSingleton,
                table.kind == ResourceTable::SINGLETON)
          << "kind";
      EXPECT_EQ(codec->key_type(), table.key_type) << "key type";
      EXPECT_EQ(codec->value_type(), table.value_type) << "value type";
      EXPECT_EQ(codec->requires_paused_workers(), table.requires_paused_workers)
          << "paused-worker requirement";
      // ValidatePipeline reports a module table as referential (D-180): one
      // that provides more must be declared so before the module exists.
      EXPECT_EQ(resource->ProvidedConsistency(), dataplane::Consistency::kReferential)
          << "consistency";

      // A message of the wrong type is refused by the type it is sent to.
      const google::protobuf::Empty empty;
      EXPECT_NE("", harness.UpsertResource(name, empty, empty))
          << "a mismatched key/value type was accepted";
    }

    // A well-formed entry of each declared type is accepted and reads back,
    // and publishing it or its erase allocates nothing (the engine reserved
    // what publication needs).
    g_publish_allocations = 0;
    dataplane::internal::g_publish_window_hook = CountPublishAllocations;
    for (const Sample &sample : c.samples) {
      SCOPED_TRACE(sample.table);
      const std::string name = ResourceName(m->name(), sample.table);
      if (sample.key == nullptr) {
        EXPECT_EQ("", harness.SetResource(name, *sample.value));
      } else {
        EXPECT_EQ("", harness.UpsertResource(name, *sample.key, *sample.value));
      }
      EXPECT_EQ("", harness.ReadBackDiffers(
                        name, {{sample.key.get(), sample.value.get()}}));
    }

    // Erase in reverse: referrers before the keys they name, so teardown
    // sees the resources as the controller would leave them.
    for (auto it = c.samples.rbegin(); it != c.samples.rend(); ++it) {
      if (it->key == nullptr) {
        continue;
      }
      EXPECT_EQ("", harness.EraseResource(ResourceName(m->name(), it->table),
                                          *it->key));
    }
    dataplane::internal::g_publish_window_hook = nullptr;
    EXPECT_EQ(g_publish_allocations.load(), 0u)
        << "publishing a sample, or its erase, allocated";
    while (engine.ReclaimRetired() != 0) {
    }
    for (const ResourceTable &table : declared) {
      if (table.kind == ResourceTable::MAP) {
        EXPECT_EQ("", harness.ReadBackDiffers(
                          ResourceName(m->name(), table.table), {}));
      }
    }

    // After DeInit (DestroyModule runs it) the module holds no resource.
    ModuleGraph::DestroyModule(m);
    for (const std::string &name : expected) {
      EXPECT_EQ(nullptr, engine.FindResource(name))
          << name << " outlived its module";
    }
    EXPECT_TRUE(RegisteredNames(engine, prefix).empty())
        << "a resource outlived its module";
  }
}

// D-180: every module class answers what its modules promise about their
// state, and a module created from an argument keeps that promise in the
// worker limit it sets: one worker for state only one worker owns, several
// for per-worker or shared state, and a flow-affinity need only where each
// worker owns its own state.
TEST(ModuleCapabilitiesTest, EveryClassDeclaresAndModulesAgree) {
  using State = ModuleCapabilities::State;
  using Affinity = ModuleCapabilities::FlowAffinity;
  for (const auto &[mclass, builder] : ModuleBuilder::all_module_builders()) {
    SCOPED_TRACE(mclass);
    auto declared = builder.Capabilities(google::protobuf::Any());
    ASSERT_TRUE(declared.has_value()) << declared.error();
    EXPECT_NE(declared->state, State::kUnspecified) << mclass << " declares no state";
    EXPECT_NE(declared->flow_affinity, Affinity::kUnspecified)
        << mclass << " declares no flow affinity";
    if (declared->flow_affinity != Affinity::kNone) {
      EXPECT_EQ(declared->state, State::kPerWorker)
          << "flow affinity matters only when each worker owns its state";
    }
  }

  ModuleHarness harness;
  auto check = [&](const std::string &mclass, const google::protobuf::Message &arg) {
    SCOPED_TRACE(mclass);
    google::protobuf::Any packed;
    ASSERT_TRUE(packed.PackFrom(arg));
    auto declared = ModuleBuilder::all_module_builders().at(mclass).Capabilities(packed);
    ASSERT_TRUE(declared.has_value()) << declared.error();
    Module *m = harness.Create(mclass, arg, "caps_" + mclass);
    ASSERT_NE(m, nullptr);
    switch (declared->state) {
      case State::kSingleWorker:
        EXPECT_EQ(1, m->max_allowed_workers()) << "state one worker owns";
        break;
      case State::kPerWorker:
      case State::kShared:
        EXPECT_LT(1, m->max_allowed_workers()) << "state several workers own or share";
        break;
      default:
        break;
    }
    ModuleGraph::DestroyModule(m);
  };
  for (const ModuleCase &c : ParityCases()) {
    check(c.mclass, *c.arg);
  }
  for (auto mode : {pb::ConnTrackArg::OWNED, pb::ConnTrackArg::PER_WORKER,
                    pb::ConnTrackArg::SHARED}) {
    pb::ConnTrackArg arg;
    arg.set_mode(mode);
    check("ConnTrack", arg);
  }
}

// ---------------------------------------------------------------------------
// Staging/publish contract
// ---------------------------------------------------------------------------

struct SlotTag;
using SlotId = dataplane::StrongId<SlotTag, uint32_t>;

struct SlotObject {
  static int live;

  explicit SlotObject(std::string p = {}) : payload(std::move(p)) { live++; }
  SlotObject(const SlotObject &other) : payload(other.payload) { live++; }
  SlotObject &operator=(const SlotObject &) = delete;
  ~SlotObject() { live--; }

  std::string payload;
};
int SlotObject::live = 0;

struct SettingsState {
  uint64_t gate = 0;
};

struct StagedSettings {
  uint64_t gate;
};

class PublishConformanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto exact = classifier::ConcurrentExactTable::Create(
        8, classifier::ConcurrentExactTable::CapacityFor(8), domain_);
    ASSERT_TRUE(exact.has_value()) << exact.error();
    exact_table_ = std::move(*exact);
    exact_ = std::make_unique<classifier::ExactRuleResource>("rules", *exact_table_);
    auto registered = engine_.Register(exact_.get());
    ASSERT_TRUE(registered.has_value()) << registered.error();

    auto masked = classifier::ConcurrentMaskedTable::Create(8, 8, domain_);
    ASSERT_TRUE(masked.has_value()) << masked.error();
    masked_table_ = std::move(*masked);
    masked_ = std::make_unique<classifier::MaskedRuleResource>(
        "masked",
        classifier::MaskedRuleResource::Hooks{.table = [this] { return masked_table_; }});
    registered = engine_.Register(masked_.get());
    ASSERT_TRUE(registered.has_value()) << registered.error();
    // The table allocates its pending-record id once, lazily, and keeps it.
    // Do that before a test snapshots the arena, so a staging failure's id
    // accounting is about the rule it prepared, not first-use table state.
    auto warm = masked_->Reserve(MaskedUpsert(99, 0, 1));
    ASSERT_TRUE(warm.has_value()) << warm.error();
    warm->staged->Abort();

    slot_ = std::make_unique<dataplane::SlotResource<SlotId, SlotObject>>(
        "slots", slots_);
    registered = engine_.Register(slot_.get());
    ASSERT_TRUE(registered.has_value()) << registered.error();

    auto settings = singleton_.Register(
        engine_.registry(), bindings_, "settings",
        [](const pb::ExactMatchSettings &value)
            -> std::expected<StagedSettings, std::string> {
          if (value.default_gate() > 8) {
            return std::unexpected("injected staging failure");
          }
          return StagedSettings{value.default_gate()};
        },
        [this](StagedSettings &staged) { settings_.gate = staged.gate; },
        [this] {
          pb::ExactMatchSettings value;
          value.set_default_gate(settings_.gate);
          return value;
        },
        nullptr);
    ASSERT_TRUE(settings.has_value()) << settings.error();
  }

  void TearDown() override {
    dataplane::internal::g_publish_window_hook = nullptr;
    singleton_.Release();
    const std::string names[] = {"rules", "masked", "slots"};
    auto released = engine_.ReleaseForTeardown(std::span<const std::string>(names));
    ASSERT_TRUE(released.has_value()) << released.error();
    for (int i = 0; i < 100000 && engine_.ReclaimRetired() != 0; i++) {
    }
    domain_.Drain();
  }

  // -- exact match -----------------------------------------------------------

  static std::string ExactKey(uint64_t key) {
    return dataplane::EncodeKey(key);
  }

  Op ExactUpsert(uint64_t key, uint64_t value) {
    return Op::Upsert(
        "rules", ExactKey(key),
        std::any(classifier::RuleResult{static_cast<gate_idx_t>(value), {}}));
  }

  Op ExactErase(uint64_t key) {
    return Op::Erase("rules", ExactKey(key));
  }

  // -- masked rules ----------------------------------------------------------

  // One all-ones mask and one all-zero value: canonical under the mask.
  static std::string MaskedKey(uint64_t value) {
    std::string key(8, '\xff');
    std::string bytes = dataplane::EncodeKey(value);
    key.append(bytes);
    return key;
  }

  Op MaskedUpsert(uint64_t value, int64_t priority, uint16_t gate) {
    return Op::Upsert("masked", MaskedKey(value),
                      std::any(classifier::MaskedRuleResource::Value{priority, gate, {}}));
  }

  Op MaskedErase(uint64_t value) {
    return Op::Erase("masked", MaskedKey(value));
  }

  size_t MaskedEntries() {
    size_t entries = 0;
    masked_table_->ForEach(
        [&](classifier::ConstBytes, classifier::ConstBytes,
            const classifier::ConcurrentMaskedTable::Rule &) { entries++; });
    return entries;
  }

  // -- slots -----------------------------------------------------------------

  static std::string SlotKey(uint32_t id) {
    return dataplane::EncodeKey(SlotId(id));
  }

  Op SlotUpsert(uint32_t id, std::string payload) {
    return Op::Upsert("slots", SlotKey(id), std::any(SlotObject(std::move(payload))));
  }

  Op SlotErase(uint32_t id) {
    return Op::Erase("slots", SlotKey(id));
  }

  // -- singleton -------------------------------------------------------------

  std::any SingletonValue(uint64_t gate) {
    const Resource *resource = engine_.FindResource("settings");
    EXPECT_NE(resource, nullptr);
    const framework::ResourceCodec *codec =
        resource == nullptr ? nullptr : bindings_.Find(*resource);
    EXPECT_NE(codec, nullptr);
    if (codec == nullptr) {
      return {};
    }
    pb::ExactMatchSettings message;
    message.set_default_gate(gate);
    google::protobuf::Any packed;
    EXPECT_TRUE(packed.PackFrom(message));
    auto staged = codec->Value(packed.type_url(), packed.value());
    EXPECT_TRUE(staged.has_value()) << staged.error();
    return staged.has_value() ? std::move(*staged) : std::any();
  }

  uint64_t ReadSingletonGate() {
    const Resource *resource = engine_.FindResource("settings");
    EXPECT_NE(resource, nullptr);
    const framework::ResourceCodec *codec =
        resource == nullptr ? nullptr : bindings_.Find(*resource);
    EXPECT_NE(codec, nullptr);
    if (codec == nullptr) {
      return 0;
    }
    uint64_t gate = 0;
    codec->Entries([&](const google::protobuf::Message &,
                       const google::protobuf::Message &value) {
      gate = dynamic_cast<const pb::ExactMatchSettings &>(value).default_gate();
    });
    return gate;
  }

  rcu::RcuDomain &domain_ = runtime::runtime().rcu();
  TransactionEngine engine_{domain_};
  framework::ResourceBindings bindings_;
  SettingsState settings_;
  framework::Singleton<pb::ExactMatchSettings, StagedSettings> singleton_;
  std::unique_ptr<classifier::ConcurrentExactTable> exact_table_;
  std::unique_ptr<classifier::ExactRuleResource> exact_;
  std::shared_ptr<classifier::ConcurrentMaskedTable> masked_table_;
  std::unique_ptr<classifier::MaskedRuleResource> masked_;
  dataplane::SlotTable<SlotId, SlotObject> slots_{8};
  std::unique_ptr<dataplane::SlotResource<SlotId, SlotObject>> slot_;
};

// A failure at any allocation while ExactRuleResource prepares an upsert
// leaves no pending key behind, and the same transaction applies on retry.
TEST_F(PublishConformanceTest, ExactRuleStagingFailureAtEveryAllocationLeavesNoTrace) {
  constexpr uint64_t kKey = 1;
  const Op op = ExactUpsert(kKey, 7);
  size_t threw = 0;
  const size_t points = fault_injection::ForEachFailurePoint([&](size_t k) {
    exact_table_->ReclaimAll();
    const size_t slots = exact_table_->slots_in_use();
    const size_t size = exact_table_->size();

    std::optional<dataplane::Resource::Reservation> held;
    bool threw_here = false;
    bool injected = false;
    {
      fault_injection::AllocationFaults faults(k);
      try {
        auto reserved = exact_->Reserve(op);
        if (reserved) {
          held.emplace(std::move(*reserved));
        }
      } catch (const std::bad_alloc &) {
        threw_here = true;
      }
      injected = faults.injected();
    }
    // The staging step may have placed the pending key; abort it.
    if (held) {
      held->staged->Abort();
    }
    exact_table_->ReclaimAll();

    threw += threw_here ? 1 : 0;
    EXPECT_EQ(exact_table_->size(), size) << "an aborted key was left placed";
    EXPECT_EQ(exact_table_->slots_in_use(), slots) << "a key slot leaked";
    EXPECT_FALSE(exact_->Contains(ExactKey(kKey)));
    if (!injected) {
      EXPECT_FALSE(threw_here);
      return;
    }

    ASSERT_EQ(engine_.Apply(std::vector<Op>{op}).outcome, Outcome::kApplied)
        << "the transaction does not apply after a failed staging";
    EXPECT_TRUE(exact_->Contains(ExactKey(kKey)));
    ASSERT_EQ(engine_.Apply(std::vector<Op>{ExactErase(kKey)}).outcome,
              Outcome::kApplied);
    exact_table_->ReclaimAll();
    EXPECT_EQ(exact_table_->size(), 0u);
  });
  EXPECT_GT(points, 0u);
  EXPECT_GT(threw, 0u);
}

// A failure at any allocation while MaskedRuleResource prepares a rule
// returns the arena id, leaves no pending entry, and the rule applies on
// retry.
TEST_F(PublishConformanceTest, MaskedRuleStagingFailureAtEveryAllocationLeavesNoTrace) {
  constexpr uint64_t kValue = 1;
  const Op op = MaskedUpsert(kValue, 2, 3);
  const size_t baseline_ids = masked_table_->ids_in_use();
  const size_t points = fault_injection::ForEachFailurePoint([&](size_t k) {
    const size_t size = masked_table_->size();

    std::optional<dataplane::Resource::Reservation> held;
    bool injected = false;
    bool threw = false;
    {
      fault_injection::AllocationFaults faults(k);
      try {
        auto reserved = masked_->Reserve(op);
        if (reserved) {
          held.emplace(std::move(*reserved));
        }
      } catch (const std::bad_alloc &) {
        threw = true;
      }
      injected = faults.injected();
    }
    if (held) {
      held->staged->Abort();
    }

    EXPECT_EQ(masked_table_->size(), size) << "an aborted rule became visible";
    EXPECT_EQ(masked_table_->ids_in_use(), baseline_ids)
        << "a prepared rule leaked its arena id";
    EXPECT_EQ(MaskedEntries(), 0u) << "a pending entry was left in the table";
    if (!injected) {
      EXPECT_FALSE(threw);
      return;
    }

    ASSERT_EQ(engine_.Apply(std::vector<Op>{op}).outcome, Outcome::kApplied)
        << "the rule does not apply after a failed staging";
    EXPECT_EQ(masked_table_->size(), 1u);
    const std::string key = MaskedKey(kValue);
    auto rule = masked_table_->FindRule(
        classifier::ConstBytes(reinterpret_cast<const std::byte *>(key.data()), 8),
        classifier::ConstBytes(reinterpret_cast<const std::byte *>(key.data()) + 8, 8));
    ASSERT_TRUE(rule.has_value());
    EXPECT_EQ(rule->priority, 2);
    EXPECT_EQ(rule->gate, 3);

    ASSERT_EQ(engine_.Apply(std::vector<Op>{MaskedErase(kValue)}).outcome,
              Outcome::kApplied);
    for (int i = 0; i < 1000 && engine_.ReclaimRetired() != 0; i++) {
    }
    EXPECT_EQ(masked_table_->size(), 0u);
    EXPECT_EQ(masked_table_->ids_in_use(), baseline_ids);
  });
  EXPECT_GT(points, 1u);
}

// A failure at any allocation while SlotResource stages an upsert frees the
// staged object, keeps the id free, and the object applies on retry.
TEST_F(PublishConformanceTest, SlotStagingFailureAtEveryAllocationLeavesNoTrace) {
  constexpr uint32_t kId = 1;
  const Op op = SlotUpsert(kId, "payload");
  const int baseline_live = SlotObject::live;  // the op holds the staged copy
  const size_t points = fault_injection::ForEachFailurePoint([&](size_t k) {
    const size_t size = slots_.size();

    std::optional<dataplane::Resource::Reservation> held;
    bool injected = false;
    bool threw = false;
    {
      fault_injection::AllocationFaults faults(k);
      try {
        auto reserved = slot_->Reserve(op);
        if (reserved) {
          held.emplace(std::move(*reserved));
        }
      } catch (const std::bad_alloc &) {
        threw = true;
      }
      injected = faults.injected();
    }
    held.reset();

    EXPECT_EQ(slots_.size(), size) << "an aborted object became visible";
    EXPECT_FALSE(slots_.Contains(SlotId(kId)));
    EXPECT_TRUE(slots_.CanPublish(SlotId(kId))) << "the id is not free";
    EXPECT_EQ(SlotObject::live, baseline_live) << "the staged object leaked";
    if (!injected) {
      EXPECT_FALSE(threw);
      return;
    }
    EXPECT_TRUE(threw) << "an allocation was refused without a failure";

    ASSERT_EQ(engine_.Apply(std::vector<Op>{op}).outcome, Outcome::kApplied)
        << "the object does not apply after a failed staging";
    EXPECT_TRUE(slots_.Contains(SlotId(kId)));
    ASSERT_EQ(engine_.Apply(std::vector<Op>{SlotErase(kId)}).outcome,
              Outcome::kApplied);
    for (int i = 0; i < 1000 && engine_.ReclaimRetired() != 0; i++) {
    }
    EXPECT_EQ(slots_.size(), size);
    EXPECT_EQ(SlotObject::live, baseline_live);
  });
  EXPECT_GT(points, 0u);
}

// A later operation's refusal aborts the reservation the earlier one had
// made: a pending exact key, a prepared masked rule, a staged slot object and
// a staged singleton all leave no trace, and a retry applies.
TEST_F(PublishConformanceTest, ALaterFailureAbortsEveryReservation) {
  // ExactRuleResource: the pending key placed by Reserve is erased.
  {
    exact_table_->ReclaimAll();
    const size_t slots = exact_table_->slots_in_use();
    const TransactionEngine::Result result = engine_.Apply(
        std::vector<Op>{ExactUpsert(1, 9), ExactErase(2)});
    ASSERT_EQ(result.outcome, Outcome::kRejected);
    EXPECT_EQ(result.ops[0].status, OpStatus::kNotApplied);
    EXPECT_EQ(exact_table_->size(), 0u) << "a pending key leaked";
    EXPECT_FALSE(exact_->Contains(ExactKey(1)));
    exact_table_->ReclaimAll();
    EXPECT_EQ(exact_table_->slots_in_use(), slots) << "a key slot leaked";
    ASSERT_EQ(engine_.Apply(std::vector<Op>{ExactUpsert(1, 9)}).outcome,
              Outcome::kApplied);
    EXPECT_TRUE(exact_->Contains(ExactKey(1)));
    ASSERT_EQ(engine_.Apply(std::vector<Op>{ExactErase(1)}).outcome,
              Outcome::kApplied);
    exact_table_->ReclaimAll();
  }

  // MaskedRuleResource: the prepared rule's arena id returns, and no pending
  // entry stays in its tuple.
  {
    const size_t ids = masked_table_->ids_in_use();
    const TransactionEngine::Result result =
        engine_.Apply(std::vector<Op>{MaskedUpsert(2, 1, 4), MaskedErase(3)});
    ASSERT_EQ(result.outcome, Outcome::kRejected);
    EXPECT_EQ(result.ops[0].status, OpStatus::kNotApplied);
    EXPECT_EQ(masked_table_->size(), 0u);
    EXPECT_EQ(masked_table_->ids_in_use(), ids) << "an arena id leaked";
    EXPECT_EQ(MaskedEntries(), 0u) << "a pending entry leaked";
    ASSERT_EQ(engine_.Apply(std::vector<Op>{MaskedUpsert(2, 1, 4)}).outcome,
              Outcome::kApplied);
    EXPECT_EQ(masked_table_->size(), 1u);
    ASSERT_EQ(engine_.Apply(std::vector<Op>{MaskedErase(2)}).outcome,
              Outcome::kApplied);
    for (int i = 0; i < 1000 && engine_.ReclaimRetired() != 0; i++) {
    }
    EXPECT_EQ(masked_table_->ids_in_use(), ids);
  }

  // SlotResource: the staged object is destroyed, the id stays free.
  {
    const TransactionEngine::Result result =
        engine_.Apply(std::vector<Op>{SlotUpsert(4, "x"), SlotErase(5)});
    ASSERT_EQ(result.outcome, Outcome::kRejected);
    EXPECT_EQ(result.ops[0].status, OpStatus::kNotApplied);
    EXPECT_EQ(slots_.size(), 0u);
    EXPECT_FALSE(slots_.Contains(SlotId(4)));
    EXPECT_TRUE(slots_.CanPublish(SlotId(4)));
    ASSERT_EQ(engine_.Apply(std::vector<Op>{SlotUpsert(4, "x")}).outcome,
              Outcome::kApplied);
    EXPECT_TRUE(slots_.Contains(SlotId(4)));
    ASSERT_EQ(engine_.Apply(std::vector<Op>{SlotErase(4)}).outcome,
              Outcome::kApplied);
    for (int i = 0; i < 1000 && engine_.ReclaimRetired() != 0; i++) {
    }
    EXPECT_EQ(SlotObject::live, 0);
  }

  // SingletonResource: the staged value is dropped, the module state is
  // untouched. The refusal is a later operation's reservation (erasing a key
  // the exact table does not hold), so the singleton's reservation has been
  // taken and must be rolled back. A duplicate key would be rejected before
  // any reservation and would not exercise this path.
  {
    const std::any staged = SingletonValue(3);
    const TransactionEngine::Result result =
        engine_.Apply(std::vector<Op>{Op::Upsert("settings", "", staged),
                                      ExactErase(0xDEADBEEF)});
    ASSERT_EQ(result.outcome, Outcome::kRejected);
    EXPECT_EQ(result.ops[0].status, OpStatus::kNotApplied);
    EXPECT_EQ(settings_.gate, 0u) << "an aborted singleton published";
    EXPECT_EQ(ReadSingletonGate(), 0u);
    ASSERT_EQ(engine_.Apply(std::vector<Op>{Op::Upsert("settings", "",
                                                       SingletonValue(3))})
                  .outcome,
              Outcome::kApplied);
    EXPECT_EQ(settings_.gate, 3u);
    EXPECT_EQ(ReadSingletonGate(), 3u);
  }
}

// A stage hook that refuses a value refuses before publication and leaves the
// module's state alone.
TEST_F(PublishConformanceTest, SingletonStagingFailureIsRefusedWithoutAChange) {
  const Resource *resource = engine_.FindResource("settings");
  ASSERT_NE(resource, nullptr);
  const framework::ResourceCodec *codec = bindings_.Find(*resource);
  ASSERT_NE(codec, nullptr);

  pb::ExactMatchSettings refused;
  refused.set_default_gate(99);
  google::protobuf::Any packed;
  ASSERT_TRUE(packed.PackFrom(refused));
  const auto staged = codec->Value(packed.type_url(), packed.value());
  ASSERT_FALSE(staged.has_value());
  EXPECT_EQ(settings_.gate, 0u);
  EXPECT_EQ(ReadSingletonGate(), 0u);

  ASSERT_EQ(engine_.Apply(std::vector<Op>{Op::Upsert("settings", "",
                                                     SingletonValue(5))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(settings_.gate, 5u);
}

// Publish is infallible: no framework resource allocates once the engine
// enters its publication phase.
TEST_F(PublishConformanceTest, PublicationDoesNotAllocate) {
  g_publish_allocations = 0;
  dataplane::internal::g_publish_window_hook = CountPublishAllocations;

  EXPECT_EQ(engine_.Apply(std::vector<Op>{ExactUpsert(10, 1)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(engine_.Apply(std::vector<Op>{ExactUpsert(10, 2)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(engine_.Apply(std::vector<Op>{ExactErase(10)}).outcome,
            Outcome::kApplied);

  EXPECT_EQ(engine_.Apply(std::vector<Op>{MaskedUpsert(20, 1, 2)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(engine_.Apply(std::vector<Op>{MaskedUpsert(20, 2, 3)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(engine_.Apply(std::vector<Op>{MaskedErase(20)}).outcome,
            Outcome::kApplied);

  EXPECT_EQ(engine_.Apply(std::vector<Op>{SlotUpsert(1, "a")}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(engine_.Apply(std::vector<Op>{SlotUpsert(1, "b")}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(engine_.Apply(std::vector<Op>{SlotErase(1)}).outcome,
            Outcome::kApplied);

  EXPECT_EQ(engine_.Apply(std::vector<Op>{Op::Upsert("settings", "",
                                                     SingletonValue(1))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(engine_.Apply(std::vector<Op>{Op::Upsert("settings", "",
                                                     SingletonValue(2))})
                .outcome,
            Outcome::kApplied);

  dataplane::internal::g_publish_window_hook = nullptr;
  EXPECT_EQ(g_publish_allocations.load(), 0u)
      << "the publication phase allocated";
}

}  // namespace
}  // namespace bess
