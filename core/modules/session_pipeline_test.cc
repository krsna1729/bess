// SPDX-License-Identifier: BSD-3-Clause

// The session vertical slice (G1.2b, D-021's acceptance item): one
// transaction across ExactMatch (action mode), ActionTable, Meter and Router
// -- the rule that selects a session, the action it names, the meter that
// polices it and the route that forwards it -- with the engine's reference
// ledger, the removal cascade, and teardown in an order that does not respect
// the reference graph (the desired-state planner destroys modules by name).
//
// Each stage's decision is driven in process; SessionPacketPathTest runs
// packets through the connected modules (the ModuleHarness), and
// tests/live/session_pipeline_test.cc writes transactions while a worker
// forwards traffic.

#include <gtest/gtest.h>

#include <array>
#include <any>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <random>
#include <vector>

#include "runtime/runtime_state.h"
#include "dataplane/action_id.h"
#include "framework/resource_bindings.h"
#include "dataplane/transaction_engine.h"
#include "meter/meter.h"
#include "module.h"
#include "module_graph.h"
#include "classifier/masked_rule_resource.h"
#include "modules/action_table.h"
#include "modules/wildcard_match.h"
#include "modules/exact_match.h"
#include "modules/meter.h"
#include "modules/router.h"
#include "modules/router_gate_map.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::dataplane::ActionId;
using bess::dataplane::Op;
using bess::dataplane::TransactionEngine;
using bess::meter::MeterId;
using bess::route::Ipv4Prefix;
using bess::route::NextHopId;
using Outcome = TransactionEngine::Outcome;

TransactionEngine &Engine() { return bess::runtime::runtime().transactions(); }

bess::framework::ResourceBindings &Bindings() {
  return bess::framework::ResourceBindings::ProcessDefault();
}

template <typename Arg>
google::protobuf::Any Pack(const Arg &arg) {
  google::protobuf::Any packed;
  EXPECT_TRUE(packed.PackFrom(arg));
  return packed;
}

Module *Create(const std::string &class_name, const std::string &name,
               const google::protobuf::Any &arg) {
  const auto &builders = ModuleBuilder::all_module_builders();
  const auto it = builders.find(class_name);
  EXPECT_NE(it, builders.end()) << class_name;
  pb_error_t perr;
  Module *m = ModuleGraph::CreateModule(it->second, name, arg, &perr);
  EXPECT_EQ(0, perr.code()) << perr.errmsg();
  return m;
}

// 4 bytes at offset 26 (IPv4 src), 2 bytes at offset 34.
ExactMatch *CreateExactMatch(const std::string &name,
                             const std::string &action_resource) {
  bess::pb::ExactMatchArg arg;
  auto *f = arg.add_fields();
  f->set_offset(26);
  f->set_num_bytes(4);
  f = arg.add_fields();
  f->set_offset(34);
  f->set_num_bytes(2);
  if (!action_resource.empty()) {
    arg.set_action_resource(action_resource);
  }
  return static_cast<ExactMatch *>(Create("ExactMatch", name, Pack(arg)));
}

Router *CreateRouterModule(const std::string &name,
                           const std::string &next_hop_attr = "") {
  bess::pb::RouterArg arg;
  arg.set_max_routes(64);
  arg.set_max_tbl8s(16);
  arg.set_max_next_hops(64);
  if (!next_hop_attr.empty()) {
    arg.set_next_hop_attr(next_hop_attr);
  }
  return static_cast<Router *>(Create("Router", name, Pack(arg)));
}

Meter *CreateMeterModule(const std::string &name, uint64_t capacity,
                         const std::string &id_attr = "",
                         uint32_t levels = 0) {
  bess::pb::MeterArg arg;
  arg.set_capacity(capacity);
  if (!id_attr.empty()) {
    arg.set_id_attr(id_attr);
  }
  if (levels != 0) {
    arg.set_levels(levels);
  }
  return static_cast<Meter *>(Create("Meter", name, Pack(arg)));
}

ActionTable *CreateActionTable(const std::string &name, const std::string &meters,
                               const std::string &next_hops, uint64_t capacity = 64,
                               const std::string &meter_attr = "",
                               const std::string &next_hop_attr = "",
                               const std::string &action_attr = "",
                               uint32_t levels = 0) {
  bess::pb::ActionTableArg arg;
  arg.set_capacity(capacity);
  arg.set_meters(meters);
  arg.set_next_hops(next_hops);
  if (!meter_attr.empty()) {
    arg.set_meter_attr(meter_attr);
  }
  if (!next_hop_attr.empty()) {
    arg.set_next_hop_attr(next_hop_attr);
  }
  if (!action_attr.empty()) {
    arg.set_action_attr(action_attr);
  }
  if (levels != 0) {
    arg.set_levels(levels);
  }
  return static_cast<ActionTable *>(Create("ActionTable", name, Pack(arg)));
}

// The index of `name` in the module's registered attributes, so a test names
// the attribute it means rather than a position Init happens to assign.
size_t AttrIndex(const Module *m, const std::string &name) {
  const auto &attrs = m->all_attrs();
  for (size_t i = 0; i < attrs.size(); i++) {
    if (attrs[i].name == name) {
      return i;
    }
  }
  ADD_FAILURE() << "'" << name << "' is not registered by " << m->name();
  return 0;
}

std::string Key(uint32_t a, uint16_t b) {
  std::string key(6, '\0');
  std::memcpy(key.data(), &a, sizeof(a));
  std::memcpy(key.data() + 4, &b, sizeof(b));
  return key;
}

Ipv4Prefix P(uint32_t addr, uint8_t len) {
  return Ipv4Prefix::Make(addr, len).value();
}

constexpr uint32_t Ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return a << 24 | b << 16 | c << 8 | d;
}

bess::route::NextHop HopWithEgress(uint32_t egress) {
  bess::route::NextHop hop;
  hop.egress = bess::dataplane::InterfaceId(egress);
  hop.neighbor = bess::route::NeighborState::kResolved;
  return hop;
}

bess::meter::TrTcmSpec SlowMeter(uint64_t committed_burst) {
  // One byte per second: nothing refills while a test runs.
  return bess::meter::TrTcmSpec{1, committed_burst, 1, committed_burst};
}

class SessionPipelineTest : public ::testing::Test {
 protected:
  void TearDown() override { ModuleGraph::DestroyAllModules(); }

  void Settle() {
    while (Engine().ReclaimRetired() != 0) {
    }
    bess::runtime::runtime().rcu().Drain();
  }

  TransactionEngine::Result Apply(std::vector<Op> ops) {
    return Engine().Apply(ops);
  }

  // Packets whose keys the test sets, batched 32 at a time.
  class Packets {
   public:
    explicit Packets(size_t n) : pool_(n + 64) {
      for (size_t i = 0; i < n; i++) {
        bess::PacketHandle pkt = pool_.Alloc(60);
        EXPECT_NE(pkt, nullptr);
        std::memset(bess::PacketRef(pkt).head_data<uint8_t *>(), 0, 60);
        handles_.push_back(pkt);
      }
    }
    ~Packets() {
      for (bess::PacketHandle pkt : handles_) {
        bess::PacketFree(pkt);
      }
    }

    void SetKey(size_t i, uint32_t a, uint16_t b) {
      uint8_t *p = bess::PacketRef(handles_[i]).head_data<uint8_t *>();
      std::memcpy(p + 26, &a, sizeof(a));
      std::memcpy(p + 34, &b, sizeof(b));
    }

    void SetLength(size_t i, uint16_t len) {
      bess::PacketRef(handles_[i]).set_data_len(len);
    }

    bess::PacketBatch Batch(size_t first, size_t n) const {
      bess::PacketBatch batch;
      batch.clear();
      for (size_t i = 0; i < n; i++) {
        batch.add(bess::PacketRef(handles_[first + i]));
      }
      return batch;
    }

   private:
    bess::PlainPacketPool pool_;
    std::vector<bess::PacketHandle> handles_;
  };
};

// The whole slice in one transaction, with the modules created referrer-first
// (the ExactMatch before the ActionTable it names, and so on): a declared
// reference binds when its resource registers, so module creation order does
// not decide the graph.
TEST_F(SessionPipelineTest, OneTransactionCreatesASession) {
  ExactMatch *em = CreateExactMatch("em", "at/actions");
  ASSERT_NE(nullptr, em);
  ActionTable *at = CreateActionTable("at", "mt/meters", "rt/next_hops");
  ASSERT_NE(nullptr, at);
  Meter *mt = CreateMeterModule("mt", 64);
  ASSERT_NE(nullptr, mt);
  Router *rt = CreateRouterModule("rt");
  ASSERT_NE(nullptr, rt);

  const uint32_t session_addr = Ip(10, 1, 2, 3);
  // One transaction: the meter, the next hop, the route to it, the action that
  // names both, and the rule that selects the action.
  auto result = Apply({
      Op::Upsert("mt/meters", bess::dataplane::EncodeKey(MeterId(1)),
                 std::any(bess::meter::MeterProfileSpec{SlowMeter(100)})),
      rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7)),
      rt->router()->SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(1)),
      Op::Upsert("at/actions", bess::dataplane::EncodeKey(ActionId(1)),
                 std::any(ActionTable::Action{MeterId(1), NextHopId(1)})),
      Op::Upsert("em/rules", Key(session_addr, 0),
                 std::any(bess::MakeGateResult(0, 1))),
  });
  ASSERT_EQ(result.outcome, Outcome::kApplied)
      << (result.ops.empty() ? "" : result.ops[0].error);

  // The ledger counts what each value names: the action names the meter, the
  // route and the action both name the next hop, and the rule names the
  // action.
  EXPECT_EQ(Engine().ReferenceCount("mt/meters",
                                    bess::dataplane::EncodeKey(MeterId(1))),
            1u);
  EXPECT_EQ(Engine().ReferenceCount(
                "rt/next_hops", bess::dataplane::EncodeKey(NextHopId(1))),
            2u);  // the route and the action
  EXPECT_EQ(Engine().ReferenceCount("at/actions",
                                    bess::dataplane::EncodeKey(ActionId(1))),
            1u);
  EXPECT_EQ(Engine().ReferenceCount(
                "rt/routes",
                bess::route::Router::RouteKey(P(Ip(10, 0, 0, 0), 8))),
            0u);  // routes are a root: nothing references a route

  // Every stage's decision, in process.
  Packets packets(3);
  packets.SetKey(0, session_addr, 0);   // the session's rule
  packets.SetKey(1, session_addr, 1);   // the same prefix, another field
  packets.SetKey(2, Ip(192, 0, 2, 1), 0);
  {
    bess::PacketBatch batch = packets.Batch(0, 3);
    std::array<uint32_t, 3> actions;
    std::array<gate_idx_t, 3> gates;
    em->ClassifyBatch(&batch, gates.data(), actions.data());
    EXPECT_EQ(actions[0], 1u);  // matched action id
    EXPECT_EQ(actions[1], 0u);  // miss
    EXPECT_EQ(actions[2], 0u);
  }
  const ActionTable::Action *action = at->LookupAction(ActionId(1));
  ASSERT_NE(nullptr, action);
  EXPECT_EQ(action->meters[0].value(), 1u);
  EXPECT_EQ(action->next_hop.value(), 1u);
  EXPECT_EQ(at->LookupAction(ActionId(2)), nullptr);

  {
    // The meter's decision on a batch whose ids the test writes: a 60-byte
    // packet fits the committed and peak buckets (100 bytes, no refill), the
    // next one does not.
    mt->set_attr_offset(0, 0);  // meter_id, at metadata offset 0
    bess::PacketBatch batch = packets.Batch(0, 2);
    for (bess::PacketRef pkt : batch.packets()) {
      _set_attr_with_offset<bess::utils::be32_t>(0, pkt,
                                                 bess::utils::be32_t(1));
    }
    std::array<gate_idx_t, 2> gates;
    mt->MeterBatch(&batch, gates.data());
    EXPECT_EQ(gates[0], Meter::kGreenGate);
    EXPECT_EQ(gates[1], Meter::kRedGate);
  }

  {
    // The router resolves next-hop ids; an id that names none is unresolved.
    const std::array<NextHopId, 2> ids = {NextHopId(1), NextHopId(9)};
    std::array<const bess::route::NextHop *, 2> hops = {nullptr, nullptr};
    const uint64_t resolved = rt->router()->LookupNextHops(ids, hops);
    EXPECT_EQ(resolved, 0b01u);
    ASSERT_NE(nullptr, hops[0]);
    EXPECT_EQ(hops[0]->egress, bess::dataplane::InterfaceId(7));
    EXPECT_EQ(hops[0]->neighbor, bess::route::NeighborState::kResolved);
  }
}

// A module registers the metadata name it was configured with: that name is
// what the graph resolves an offset for, so it is the whole contract between a
// writer and a reader. Two meters over one classifier result -- a flow maximum
// and a session aggregate -- work because each reads its own name, and neither
// module knows the other exists.
TEST_F(SessionPipelineTest, ConfiguredAttributeNamesAreTheOnesRegistered) {
  ActionTable *actions = CreateActionTable("flow_actions", "flow/meters",
                                           "rt/next_hops", 64, "flow_meter_id");
  Meter *flow = CreateMeterModule("flow", 64, "flow_meter_id");
  Router *rt = CreateRouterModule("rt");
  ASSERT_NE(nullptr, actions);
  ASSERT_NE(nullptr, flow);
  ASSERT_NE(nullptr, rt);

  const auto &meter_attrs = flow->all_attrs();
  ASSERT_EQ(1u, meter_attrs.size());
  EXPECT_EQ("flow_meter_id", meter_attrs[0].name);
  EXPECT_EQ(bess::metadata::Attribute::AccessMode::kRead, meter_attrs[0].mode);

  bool wrote_meter = false;
  bool read_action = false;
  for (const auto &attr : actions->all_attrs()) {
    if (attr.name == "flow_meter_id") {
      wrote_meter = true;
      EXPECT_EQ(bess::metadata::Attribute::AccessMode::kWrite, attr.mode);
    }
    if (attr.name == "action_id") {
      read_action = true;
      EXPECT_EQ(bess::metadata::Attribute::AccessMode::kRead, attr.mode);
    }
    EXPECT_NE("meter_id", attr.name) << "the default name was not registered";
  }
  EXPECT_TRUE(wrote_meter);
  EXPECT_TRUE(read_action);
}

// A transaction that would leave a dangling reference, or a value that names
// nothing, is rejected with nothing visible.
TEST_F(SessionPipelineTest, RejectionsLeaveNothingChanged) {
  ActionTable *at = CreateActionTable("at", "mt/meters", "rt/next_hops");
  ASSERT_NE(nullptr, at);
  Meter *mt = CreateMeterModule("mt", 64);
  ASSERT_NE(nullptr, mt);
  Router *rt = CreateRouterModule("rt");
  ASSERT_NE(nullptr, rt);
  ExactMatch *em = CreateExactMatch("em", "at/actions");
  ASSERT_NE(nullptr, em);

  // A rule naming an action that does not exist: refused.
  auto r = Apply({Op::Upsert("em/rules", Key(Ip(10, 1, 2, 3), 0),
                             std::any(bess::MakeGateResult(0, 7)))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("references missing"), std::string::npos)
      << r.ops[0].error;

  // An action naming a meter that does not exist: refused.
  r = Apply({Op::Upsert("at/actions", bess::dataplane::EncodeKey(ActionId(1)),
                        std::any(ActionTable::Action{MeterId(1),
                                                     NextHopId(1)}))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("references missing"), std::string::npos)
      << r.ops[0].error;

  // Establish the session, then try to remove what it names.
  ASSERT_EQ(Apply({Op::Upsert("mt/meters",
                              bess::dataplane::EncodeKey(MeterId(1)),
                              std::any(bess::meter::MeterProfileSpec{
                                  SlowMeter(1000)})),
                   rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7)),
                   Op::Upsert("at/actions",
                              bess::dataplane::EncodeKey(ActionId(1)),
                              std::any(ActionTable::Action{MeterId(1),
                                                           NextHopId(1)}))})
                .outcome,
            Outcome::kApplied);

  r = Apply({Op::Erase("mt/meters", bess::dataplane::EncodeKey(MeterId(1)))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("still referenced"), std::string::npos)
      << r.ops[0].error;
  r = Apply({rt->router()->RemoveNextHopOp(NextHopId(1))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("still referenced"), std::string::npos)
      << r.ops[0].error;

  // Nothing changed.
  EXPECT_EQ(Engine().ReferenceCount("mt/meters",
                                    bess::dataplane::EncodeKey(MeterId(1))),
            1u);
  EXPECT_NE(nullptr, mt);
  EXPECT_EQ(at->LookupAction(ActionId(1))->meters[0].value(), 1u);

  // A meter profile that rte_meter would refuse is refused here, and the
  // rejected transaction leaves the meter alone.
  r = Apply({Op::Upsert("mt/meters", bess::dataplane::EncodeKey(MeterId(1)),
                        std::any(bess::meter::MeterProfileSpec{
                            bess::meter::TrTcmSpec{1000, 100, 10, 100}}))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("invalid profile"), std::string::npos)
      << r.ops[0].error;
}

// Removing a session: the rule, the action, the route, the next hop and the
// meter go in one transaction. An erased action and meter stay readable -- and
// their ids unpublishable -- until the removal cascade has run, which a
// reader that has not reported quiescence holds off.
TEST_F(SessionPipelineTest, RemovalCascadeAndIdReuse) {
  ActionTable *at = CreateActionTable("at", "mt/meters", "rt/next_hops");
  Meter *mt = CreateMeterModule("mt", 64);
  Router *rt = CreateRouterModule("rt");
  ExactMatch *em = CreateExactMatch("em", "at/actions");
  ASSERT_NE(nullptr, em);
  ASSERT_NE(nullptr, at);
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);

  ASSERT_EQ(Apply({Op::Upsert("mt/meters",
                              bess::dataplane::EncodeKey(MeterId(1)),
                              std::any(bess::meter::MeterProfileSpec{
                                  SlowMeter(1000)})),
                   rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7)),
                   rt->router()->SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(1)),
                   Op::Upsert("at/actions",
                              bess::dataplane::EncodeKey(ActionId(1)),
                              std::any(ActionTable::Action{MeterId(1),
                                                           NextHopId(1)})),
                   Op::Upsert("em/rules", Key(Ip(10, 1, 2, 3), 0),
                              std::any(bess::MakeGateResult(0, 1)))})
                .outcome,
            Outcome::kApplied);

  // A reader that never reports quiescence: the removal cascade cannot run, so
  // what an erase leaves behind is observable.
  bess::rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  constexpr bess::rcu::ReaderId kReader = 25;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  domain.Online(kReader);

  ASSERT_EQ(Apply({Op::Erase("em/rules", Key(Ip(10, 1, 2, 3), 0)),
                   Op::Erase("at/actions",
                             bess::dataplane::EncodeKey(ActionId(1))),
                   rt->router()->RemoveRouteOp(P(Ip(10, 0, 0, 0), 8)),
                   rt->router()->RemoveNextHopOp(NextHopId(1)),
                   Op::Erase("mt/meters",
                             bess::dataplane::EncodeKey(MeterId(1))),
                   Op::Upsert("mt/meters",
                              bess::dataplane::EncodeKey(MeterId(2)),
                              std::any(bess::meter::MeterProfileSpec{
                                  SlowMeter(1000)}))}).outcome,
            Outcome::kApplied);

  // The control side sees none of them any more...
  EXPECT_EQ(Engine().ReferenceCount("mt/meters",
                                    bess::dataplane::EncodeKey(MeterId(1))),
            0u);
  EXPECT_EQ(rt->router()->next_hop_count(), 0u);
  EXPECT_EQ(rt->router()->route_count(), 0u);
  EXPECT_EQ(em->GetDesc().find("0 rules") != std::string::npos, true);
  // ...while a reader may still resolve what it looked up before: the action
  // object stays readable until the cascade reaches it.
  EXPECT_NE(at->LookupAction(ActionId(1)), nullptr);
  // An unrelated upsert in this same transaction must not publish a meter
  // generation that omits id 1 while a reader can still use the old action.
  mt->set_attr_offset(0, 0);
  Packets packets(1);
  bess::PacketBatch batch = packets.Batch(0, 1);
  _set_attr_with_offset<bess::utils::be32_t>(
      0, batch.packet(0), bess::utils::be32_t(1));
  std::array<gate_idx_t, 1> gates;
  mt->MeterBatch(&batch, gates.data());
  EXPECT_EQ(gates[0], Meter::kGreenGate);


  // Neither the meter's id nor the next hop's may be reused before then.
  auto r = Apply({Op::Upsert("mt/meters",
                             bess::dataplane::EncodeKey(MeterId(1)),
                             std::any(bess::meter::MeterProfileSpec{
                                 SlowMeter(1000)}))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("still retiring"), std::string::npos)
      << r.ops[0].error;
  r = Apply({rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(9))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);

  // Once the reader is done, the cascade runs and the ids come back.
  domain.Quiescent(kReader);
  domain.Offline(kReader);
  domain.Unregister(kReader);
  Settle();
  EXPECT_EQ(at->LookupAction(ActionId(1)), nullptr);
  ASSERT_EQ(Apply({Op::Upsert("mt/meters",
                              bess::dataplane::EncodeKey(MeterId(1)),
                              std::any(bess::meter::MeterProfileSpec{
                                  SlowMeter(1000)})),
                   rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(9)),
                   rt->router()->SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(1)),
                   Op::Upsert("at/actions",
                              bess::dataplane::EncodeKey(ActionId(1)),
                              std::any(ActionTable::Action{MeterId(1),
                                                           NextHopId(1)})),
                   Op::Upsert("em/rules", Key(Ip(10, 1, 2, 3), 0),
                              std::any(bess::MakeGateResult(0, 1)))})
                .outcome,
            Outcome::kApplied);
  ASSERT_NE(nullptr, rt->router()->Resolve(Ip(10, 9, 9, 9)));
  EXPECT_EQ(rt->router()->Resolve(Ip(10, 9, 9, 9))->egress, bess::dataplane::InterfaceId(9));
}

// Modules are destroyed in name order (the desired-state planner's), which is
// the reverse of the reference graph here: the referent goes first. The
// teardown release must make that harmless.
TEST_F(SessionPipelineTest, TeardownIgnoresDestructionOrder) {
  // Names chosen so that DestroyAllModules() destroys the router first, then
  // the meter, then the action table, then the ExactMatch that names it.
  Router *rt = CreateRouterModule("a_router");
  Meter *mt = CreateMeterModule("b_meter", 16);
  ActionTable *at = CreateActionTable("c_actions", "b_meter/meters",
                                      "a_router/next_hops");
  ExactMatch *em = CreateExactMatch("d_em", "c_actions/actions");
  ASSERT_NE(nullptr, em);
  ASSERT_NE(nullptr, at);
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);

  ASSERT_EQ(Apply({Op::Upsert("b_meter/meters",
                              bess::dataplane::EncodeKey(MeterId(1)),
                              std::any(bess::meter::MeterProfileSpec{
                                  SlowMeter(1000)})),
                   rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7)),
                   Op::Upsert("c_actions/actions",
                              bess::dataplane::EncodeKey(ActionId(1)),
                              std::any(ActionTable::Action{MeterId(1),
                                                           NextHopId(1)})),
                   Op::Upsert("d_em/rules", Key(Ip(10, 1, 2, 3), 0),
                              std::any(bess::MakeGateResult(0, 1)))})
                .outcome,
            Outcome::kApplied);

  ModuleGraph::DestroyAllModules();
  EXPECT_TRUE(Engine().ResourceNames().empty())
      << "a resource outlived its module";
}

// A rule change must not bypass the action-rule reference ledger. A client
// writes "em/rules"; the engine checks the rule's action and keeps the
// reference counts. (The legacy add/delete/clear/set_runtime_config commands
// are gone -- D-140.)
TEST_F(SessionPipelineTest, ActionModeRulesKeepReferenceCounts) {
  Meter *mt = CreateMeterModule("mt", 16);
  Router *rt = CreateRouterModule("rt");
  ActionTable *at = CreateActionTable("at", "mt/meters", "rt/next_hops");
  ExactMatch *em = CreateExactMatch("em", "at/actions");
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);
  ASSERT_NE(nullptr, at);
  ASSERT_NE(nullptr, em);
  ASSERT_EQ(Apply({Op::Upsert("mt/meters",
                              bess::dataplane::EncodeKey(MeterId(1)),
                              std::any(bess::meter::MeterProfileSpec{
                                  SlowMeter(1000)})),
                   rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7)),
                   Op::Upsert("at/actions",
                              bess::dataplane::EncodeKey(ActionId(1)),
                              std::any(ActionTable::Action{MeterId(1),
                                                           NextHopId(1)})),
                   Op::Upsert("at/actions",
                              bess::dataplane::EncodeKey(ActionId(2)),
                              std::any(ActionTable::Action{MeterId(1),
                                                           NextHopId(1)}))})
                .outcome,
            Outcome::kApplied);

  bess::pb::ExactMatchArg initial;
  ASSERT_TRUE(em->initial_arg().UnpackTo(&initial));
  EXPECT_EQ(initial.action_resource(), "at/actions");

  const std::string key = Key(Ip(10, 1, 2, 3), 0);
  ASSERT_EQ(Apply({Op::Upsert("em/rules", key, std::any(bess::MakeGateResult(0, 1)))}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(Engine().ReferenceCount("at/actions",
                                    bess::dataplane::EncodeKey(ActionId(1))),
            1u);
  EXPECT_EQ(Apply({Op::Erase("at/actions",
                            bess::dataplane::EncodeKey(ActionId(1)))})
                .outcome,
            Outcome::kRejected);

  // A rule naming an action that is not there is refused, and the rule it
  // would replace keeps its reference.
  EXPECT_EQ(Apply({Op::Upsert("em/rules", key, std::any(bess::MakeGateResult(0, 99)))}).outcome,
            Outcome::kRejected);
  EXPECT_EQ(Engine().ReferenceCount("at/actions",
                                    bess::dataplane::EncodeKey(ActionId(1))),
            1u);
  ASSERT_EQ(Apply({Op::Upsert("em/rules", key, std::any(bess::MakeGateResult(0, 2)))}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(Engine().ReferenceCount("at/actions",
                                    bess::dataplane::EncodeKey(ActionId(1))),
            0u);
  EXPECT_EQ(Engine().ReferenceCount("at/actions",
                                    bess::dataplane::EncodeKey(ActionId(2))),
            1u);
  // A referenced action cannot be erased: the rule still holds it.
  EXPECT_EQ(Apply({Op::Erase("at/actions",
                             bess::dataplane::EncodeKey(ActionId(2)))})
                .outcome,
            Outcome::kRejected);
  EXPECT_EQ(Engine().ReferenceCount("at/actions",
                                    bess::dataplane::EncodeKey(ActionId(2))),
            1u);
  // Erasing the rule releases it, and then the action goes.
  ASSERT_EQ(Apply({Op::Erase("em/rules", key)}).outcome, Outcome::kApplied);
  EXPECT_EQ(Engine().ReferenceCount("at/actions",
                                    bess::dataplane::EncodeKey(ActionId(2))),
            0u);
  ASSERT_EQ(Apply({Op::Erase("at/actions",
                             bess::dataplane::EncodeKey(ActionId(2)))})
                .outcome,
            Outcome::kApplied);
}

TEST_F(SessionPipelineTest, RouterCodecRejectsOutOfRangePrefix) {
  Router *rt = CreateRouterModule("rt");
  ASSERT_NE(nullptr, rt);
  const auto *codec = Bindings().Find(*rt->router()->routes_resource_object());
  bess::pb::RouterRouteKey key;
  key.set_ipv4("0.0.0.0");
  key.set_prefix_length(256);  // narrowing to uint8_t would silently make /0
  google::protobuf::Any packed = Pack(key);
  EXPECT_FALSE(codec->Key(packed.type_url(), packed.value()).has_value());
  key.set_prefix_length(0);
  packed = Pack(key);
  EXPECT_TRUE(codec->Key(packed.type_url(), packed.value()).has_value());
}

TEST_F(SessionPipelineTest, ResourceCodecUnpacksTypedWireMessages) {
  Router *rt = CreateRouterModule("rt");
  ASSERT_NE(nullptr, rt);
  const auto *codec = Bindings().Find(*rt->router()->routes_resource_object());
  bess::pb::RouterRouteKey key;
  key.set_ipv4("10.0.0.0");
  key.set_prefix_length(24);
  const google::protobuf::Any packed = Pack(key);
  EXPECT_TRUE(codec->Key(packed.type_url(), packed.value()).has_value());

  const google::protobuf::Any wrong_type =
      Pack(bess::pb::RouterNextHopIdKey{});
  EXPECT_FALSE(
      codec->Key(wrong_type.type_url(), wrong_type.value()).has_value());

  const std::string malformed(1, static_cast<char>(0xff));
  EXPECT_FALSE(codec->Key(packed.type_url(), malformed).has_value());
  bess::pb::RouterRouteValue value;
  value.set_next_hop_id(7);
  const google::protobuf::Any packed_value = Pack(value);
  EXPECT_TRUE(
      codec->Value(packed_value.type_url(), packed_value.value()).has_value());

  const google::protobuf::Any wrong_value_type =
      Pack(bess::pb::RouterNextHopValue{});
  EXPECT_FALSE(codec->Value(wrong_value_type.type_url(),
                            wrong_value_type.value())
                   .has_value());
  EXPECT_FALSE(
      codec->Value(packed_value.type_url(), malformed).has_value());
}

TEST_F(SessionPipelineTest, ActionIdsAreNotNarrowedToGateWidth) {
  ActionTable *at = CreateActionTable("at", "mt/meters", "rt/next_hops",
                                      70000);
  ExactMatch *em = CreateExactMatch("em", "at/actions");
  ASSERT_NE(nullptr, at);
  ASSERT_NE(nullptr, em);
  ASSERT_EQ(Apply({Op::Upsert("at/actions",
                              bess::dataplane::EncodeKey(ActionId(70000)),
                              std::any(ActionTable::Action{
                                  MeterId(0), NextHopId(0)}))})
                .outcome,
            Outcome::kApplied);
  const std::string key = Key(Ip(10, 1, 2, 3), 0);
  ASSERT_EQ(Apply({Op::Upsert("em/rules", key, std::any(bess::MakeGateResult(0, 70000)))}).outcome,
            Outcome::kApplied);
  Packets packets(1);
  packets.SetKey(0, Ip(10, 1, 2, 3), 0);
  bess::PacketBatch batch = packets.Batch(0, 1);
  std::array<uint32_t, 1> ids;
  std::array<gate_idx_t, 1> gates;
  em->ClassifyBatch(&batch, gates.data(), ids.data());
  EXPECT_EQ(ids[0], 70000u);
}

// D-141: one rule result carries a gate and an action id, and every
// combination is legal -- a gate alone, an action alone, both, or neither. A
// miss delivers the default gate with action id 0.
TEST_F(SessionPipelineTest, RuleResultCarriesGateAndActionTogether) {
  ActionTable *at = CreateActionTable("at", "mt/meters", "rt/next_hops");
  ASSERT_NE(nullptr, at);
  for (uint32_t id : {1u, 2u}) {
    ASSERT_EQ(Apply({Op::Upsert("at/actions",
                                bess::dataplane::EncodeKey(ActionId(id)),
                                std::any(ActionTable::Action{
                                    MeterId(0), NextHopId(0)}))})
                  .outcome,
              Outcome::kApplied);
  }
  ExactMatch *em = CreateExactMatch("em", "at/actions");
  ASSERT_NE(nullptr, em);
  ASSERT_EQ(Apply({Op::Upsert("em/rules", Key(Ip(10, 0, 0, 1), 0),
                              std::any(bess::MakeGateResult(4, 1))),
                   Op::Upsert("em/rules", Key(Ip(10, 0, 0, 2), 0),
                              std::any(bess::MakeGateResult(7, 0))),
                   Op::Upsert("em/rules", Key(Ip(10, 0, 0, 3), 0),
                              std::any(bess::MakeGateResult(0, 2)))})
                .outcome,
            Outcome::kApplied);

  Packets packets(4);
  packets.SetKey(0, Ip(10, 0, 0, 1), 0);
  packets.SetKey(1, Ip(10, 0, 0, 2), 0);
  packets.SetKey(2, Ip(10, 0, 0, 3), 0);
  packets.SetKey(3, Ip(192, 0, 2, 1), 0);
  bess::PacketBatch batch = packets.Batch(0, 4);
  std::array<gate_idx_t, 4> gates;
  std::array<uint32_t, 4> actions;
  em->ClassifyBatch(&batch, gates.data(), actions.data());

  EXPECT_EQ(gates[0], 4u) << "a rule with both takes its own gate";
  EXPECT_EQ(actions[0], 1u);
  EXPECT_EQ(gates[1], 7u) << "a rule with a gate and no action";
  EXPECT_EQ(actions[1], 0u);
  EXPECT_EQ(gates[2], 0u) << "a rule with an action and no gate of its own";
  EXPECT_EQ(actions[2], 2u);
  EXPECT_EQ(gates[3], DROP_GATE) << "a miss takes the default gate";
  EXPECT_EQ(actions[3], 0u) << "and carries no action id";
}

// D-141's wildcard half: a masked rule carries a gate and an action id, the
// match delivers both -- the action id in packet metadata -- and a miss
// delivers 0. The codec must accept the pair only where a resource can resolve
// it, exactly as ExactMatch does.
TEST_F(SessionPipelineTest, WildcardRuleCarriesGateAndActionTogether) {
  ActionTable *at = CreateActionTable("at", "mt/meters", "rt/next_hops");
  ASSERT_NE(nullptr, at);
  ASSERT_EQ(Apply({Op::Upsert("at/actions", bess::dataplane::EncodeKey(ActionId(3)),
                              std::any(ActionTable::Action{MeterId(0), NextHopId(0)}))})
                .outcome,
            Outcome::kApplied);

  bess::pb::WildcardMatchArg arg;
  auto *field = arg.add_fields();
  field->set_offset(26);
  field->set_num_bytes(4);
  arg.set_action_resource("at/actions");
  auto *wm = static_cast<WildcardMatch *>(Create("WildcardMatch", "wm", Pack(arg)));
  ASSERT_NE(nullptr, wm);

  // The resource key is the packed mask then the packed value; one 4-byte
  // field, so the field's bytes as the packet carries them, and the value
  // must be canonical (value & ~mask == 0), which both of these are.
  const auto field4 = [](uint32_t v) { return Key(v, 0).substr(0, 4); };
  ASSERT_EQ(Apply({Op::Upsert("wm/rules",
                              field4(Ip(255, 255, 255, 0)) + field4(Ip(10, 0, 0, 0)),
                              std::any(bess::classifier::MaskedRuleResource::Value{
                                  2, 4, 3})),
                   Op::Upsert("wm/rules",
                              field4(Ip(255, 255, 0, 0)) + field4(Ip(10, 0, 0, 0)),
                              std::any(bess::classifier::MaskedRuleResource::Value{
                                  1, 7, 0}))})
                .outcome,
            Outcome::kApplied);

  Packets packets(3);
  packets.SetKey(0, Ip(10, 0, 0, 5), 0);   // the /24 wins: gate 4, action 3
  packets.SetKey(1, Ip(10, 0, 9, 9), 0);   // only the /16: gate 7, no action
  packets.SetKey(2, Ip(192, 0, 2, 1), 0);  // neither: default gate, no action
  bess::PacketBatch batch = packets.Batch(0, 3);
  std::array<gate_idx_t, 3> gates;
  std::array<uint32_t, 3> actions;
  wm->ClassifyBatch(&batch, gates.data(), actions.data());
  EXPECT_EQ(gates[0], 4u);
  EXPECT_EQ(actions[0], 3u);
  EXPECT_EQ(gates[1], 7u);
  EXPECT_EQ(actions[1], 0u);
  EXPECT_EQ(gates[2], DROP_GATE);
  EXPECT_EQ(actions[2], 0u);

  // The codec: {gate, action_id} is accepted where an action resource can
  // resolve the id, and refused where there is none.
  bess::pb::WildcardMatchRuleValue v;
  v.set_gate(0);
  v.set_action_id(3);
  const google::protobuf::Any packed_value = Pack(v);
  const auto *with_resource = Bindings().Find(*Engine().FindResource("wm/rules"));
  ASSERT_NE(nullptr, with_resource);
  EXPECT_TRUE(with_resource->Value(packed_value.type_url(), packed_value.value())
                  .has_value());

  bess::pb::WildcardMatchArg plain;
  auto *plain_field = plain.add_fields();
  plain_field->set_offset(26);
  plain_field->set_num_bytes(4);
  auto *plain_wm = static_cast<WildcardMatch *>(
      Create("WildcardMatch", "wm_plain", Pack(plain)));
  ASSERT_NE(nullptr, plain_wm);
  const auto *no_resource =
      Bindings().Find(*Engine().FindResource("wm_plain/rules"));
  ASSERT_NE(nullptr, no_resource);
  EXPECT_FALSE(no_resource->Value(packed_value.type_url(), packed_value.value())
                   .has_value());
}

// D-141: a WildcardMatch whose rules reference an action resource must be
// destroyable while the ActionTable is still there. An action-mode rule leaves
// with live keys, which the engine refuses for a plain Unregister, so a
// pipeline's teardown would otherwise depend on module order (and abort the
// daemon).
TEST_F(SessionPipelineTest, WildcardTeardownWithAnActionResource) {
  ActionTable *at = CreateActionTable("at", "mt/meters", "rt/next_hops");
  ASSERT_NE(nullptr, at);
  ASSERT_EQ(Apply({Op::Upsert("at/actions", bess::dataplane::EncodeKey(ActionId(1)),
                              std::any(ActionTable::Action{MeterId(0),
                                                           NextHopId(0)}))})
                .outcome,
            Outcome::kApplied);

  bess::pb::WildcardMatchArg arg;
  auto *field = arg.add_fields();
  field->set_offset(26);
  field->set_num_bytes(4);
  arg.set_action_resource("at/actions");
  auto *wm =
      static_cast<WildcardMatch *>(Create("WildcardMatch", "wm", Pack(arg)));
  ASSERT_NE(nullptr, wm);
  ASSERT_EQ(Apply({Op::Upsert("wm/rules",
                              Key(Ip(255, 255, 255, 0), 0).substr(0, 4) +
                                  Key(Ip(10, 0, 0, 0), 0).substr(0, 4),
                              std::any(bess::classifier::MaskedRuleResource::Value{
                                  1, 0, 1}))})
                .outcome,
            Outcome::kApplied);

  // The rule is live and names an action; the table is untouched.
  ModuleGraph::DestroyModule(wm);
  EXPECT_NE(nullptr, Engine().FindResource("at/actions"))
      << "the action table outlives the classifier that referenced it";
  EXPECT_EQ(nullptr, Engine().FindResource("wm/rules"));
}

// The other order: the ActionTable goes first, while a live rule still names
// one of its actions (the order DestroyAllModules uses -- names sort), and
// the classifier follows with a dangling reference.
TEST_F(SessionPipelineTest, ActionTableTeardownBeforeTheClassifierThatNamesIt) {
  ActionTable *at = CreateActionTable("at", "mt/meters", "rt/next_hops");
  ASSERT_NE(nullptr, at);
  ASSERT_EQ(Apply({Op::Upsert("at/actions", bess::dataplane::EncodeKey(ActionId(1)),
                              std::any(ActionTable::Action{MeterId(0),
                                                           NextHopId(0)}))})
                .outcome,
            Outcome::kApplied);
  bess::pb::WildcardMatchArg arg;
  auto *field = arg.add_fields();
  field->set_offset(26);
  field->set_num_bytes(4);
  arg.set_action_resource("at/actions");
  auto *wm =
      static_cast<WildcardMatch *>(Create("WildcardMatch", "wm2", Pack(arg)));
  ASSERT_NE(nullptr, wm);
  ASSERT_EQ(Apply({Op::Upsert("wm2/rules",
                              Key(Ip(255, 255, 255, 0), 0).substr(0, 4) +
                                  Key(Ip(10, 0, 0, 0), 0).substr(0, 4),
                              std::any(bess::classifier::MaskedRuleResource::Value{
                                  1, 0, 1}))})
                .outcome,
            Outcome::kApplied);
  ModuleGraph::DestroyModule(at);
  ModuleGraph::DestroyModule(wm);
  EXPECT_EQ(nullptr, Engine().FindResource("wm2/rules"));
}

TEST_F(SessionPipelineTest, FusedKernelMatchesTheGenericPathInActionMode) {
  ActionTable *at = CreateActionTable("at", "mt/meters", "rt/next_hops");
  ASSERT_NE(nullptr, at);
  bess::pb::ExactMatchArg arg;
  auto *f = arg.add_fields();
  f->set_offset(26);
  f->set_num_bytes(4);
  arg.set_action_resource("at/actions");
  auto *em = static_cast<ExactMatch *>(Create("ExactMatch", "em", Pack(arg)));
  ASSERT_NE(nullptr, em);
  ASSERT_EQ(4u, em->fused_key_width());
  for (uint32_t id = 1; id <= 8; id++) {
    ASSERT_EQ(Apply({Op::Upsert("at/actions",
                                bess::dataplane::EncodeKey(ActionId(id)),
                                std::any(ActionTable::Action{
                                    MeterId(0), NextHopId(0)}))})
                  .outcome,
              Outcome::kApplied);
  }
  for (uint32_t k = 0; k < 16; k++) {
    // One 4-byte field: the key is that field's bytes.
    ASSERT_EQ(Apply({Op::Upsert("em/rules", Key(Ip(10, 0, 0, k), 0).substr(0, 4),
                                std::any(bess::MakeGateResult(0, 1 + k % 8)))})
                  .outcome,
              Outcome::kApplied);
  }

  // Packets 0-23 carry a rule's key, 24-31 none; every fourth is 29 bytes
  // (one short of the field's end), every fourth after it exactly 30.
  constexpr size_t kN = bess::PacketBatch::kMaxBurst;
  Packets packets(kN);
  for (size_t i = 0; i < kN; i++) {
    packets.SetKey(i, i < 24 ? Ip(10, 0, 0, static_cast<uint32_t>(i % 16))
                             : Ip(10, 0, 1, static_cast<uint32_t>(i)),
                   0);
    if (i % 4 == 0) {
      packets.SetLength(i, 29);
    } else if (i % 4 == 1) {
      packets.SetLength(i, 30);
    }
  }
  std::array<uint32_t, kN> fused;
  std::array<uint32_t, kN> generic;
  std::array<gate_idx_t, kN> fused_gates;
  std::array<gate_idx_t, kN> generic_gates;
  bess::PacketBatch batch = packets.Batch(0, kN);
  em->ClassifyBatch(&batch, fused_gates.data(), fused.data());
  Error err;
  ASSERT_TRUE(em->SetFusedKernelEnabled(false, &err)) << err.second;
  ASSERT_EQ(0u, em->fused_key_width());
  em->ClassifyBatch(&batch, generic_gates.data(), generic.data());
  EXPECT_EQ(fused, generic);
  EXPECT_EQ(fused_gates, generic_gates);
  for (size_t i = 0; i < kN; i++) {
    const uint32_t want =
        i < 24 && i % 4 != 0 ? 1 + static_cast<uint32_t>(i % 16) % 8 : 0;
    EXPECT_EQ(want, fused[i]) << "packet " << i;
  }
}

TEST_F(SessionPipelineTest, ModuleCapacitiesRejectNarrowing) {
  auto rejected = [](const std::string &type, const auto &arg) {
    pb_error_t error;
    Module *module = ModuleGraph::CreateModule(
        ModuleBuilder::all_module_builders().at(type), "oversized", Pack(arg),
        &error);
    EXPECT_EQ(module, nullptr);
    EXPECT_EQ(error.code(), EINVAL);
  };
  bess::pb::MeterArg meter;
  meter.set_capacity(~uint64_t{0});
  rejected("Meter", meter);
  bess::pb::ActionTableArg action;
  action.set_capacity(~uint64_t{0});
  action.set_meters("mt/meters");
  action.set_next_hops("rt/next_hops");
  rejected("ActionTable", action);
  bess::pb::RouterArg router;
  router.set_max_routes(uint64_t{1} << 32);
  rejected("Router", router);
}
TEST_F(SessionPipelineTest, CodecsRejectMismatchedFieldsAndUnknownEnums) {
  ExactMatch *em = CreateExactMatch("em", "at/actions");
  ASSERT_NE(nullptr, em);
  Router *rt = CreateRouterModule("rt");
  ASSERT_NE(nullptr, rt);

  const auto *em_res = Engine().FindResource("em/rules");
  ASSERT_NE(nullptr, em_res);
  const auto *em_codec = Bindings().Find(*em_res);
  bess::pb::ExactMatchRuleValue em_val;
  em_val.set_gate(MAX_GATES + 1);
  const google::protobuf::Any packed_em_val = Pack(em_val);
  EXPECT_FALSE(em_codec
                   ->Value(packed_em_val.type_url(), packed_em_val.value())
                   .has_value());

  // D-141: a rule may name a gate and an action id together; the shape the
  // codec used to reject is now the normal one.
  em_val.set_gate(4);
  em_val.set_action_id(1);
  const google::protobuf::Any packed_both = Pack(em_val);
  EXPECT_TRUE(em_codec
                  ->Value(packed_both.type_url(), packed_both.value())
                  .has_value());

  const auto *nh_codec = Bindings().Find(*rt->router()->next_hops_resource_object());
  ASSERT_NE(nullptr, nh_codec);
  bess::pb::RouterNextHopValue nh_val;
  nh_val.set_egress_gate(0);
  nh_val.set_neighbor(static_cast<bess::pb::RouterNeighborState>(99));
  const google::protobuf::Any packed_nh_val = Pack(nh_val);
  EXPECT_FALSE(nh_codec
                   ->Value(packed_nh_val.type_url(), packed_nh_val.value())
                   .has_value());
}

// -- The packet path end to end (was bessctl/module_tests/session_pipeline.py):
// em:0 -> at, at:0 -> mt, mt:0 (green) -> rt, packets run through the real
// module graph; yellow and red are left unconnected, so those packets are
// dropped -- the policy is the graph.

// Where a session's next hop sends the packet: a router output gate.
constexpr gate_idx_t kEgressGate = 7;

// The rule key of a packet from `sip`:`sport`, as the packet carries them.
std::string WireKey(const char *sip, uint16_t sport) {
  const auto ip = bess::testing::Ip4(sip);
  std::string key(ip.begin(), ip.end());
  key.push_back(static_cast<char>(sport >> 8));
  key.push_back(static_cast<char>(sport & 0xff));
  return key;
}

bess::testing::Bytes SessionPacket(const char *sip, uint16_t sport, std::mt19937 &rng) {
  return bess::testing::TcpPacket({.sip = sip, .dip = "10.9.9.9", .sport = sport, .dport = 80},
                                  rng);
}

// What the Router forwards a frame as: the TTL decremented and the checksum
// that covers it. The library's own function, not a copy -- a test that
// recomputed the checksum could pass while the packet path wrote a wrong one.
bess::testing::Bytes Forwarded(bess::testing::Bytes f) {
  auto *ip = reinterpret_cast<bess::utils::Ipv4 *>(f.data() + 14);
  EXPECT_EQ(bess::route::TtlResult::kForward, bess::route::DecrementTtl(*ip));
  return f;
}

class SessionPacketPathTest : public SessionPipelineTest {
 protected:
  // The four modules, created referrer-first (the ExactMatch and the
  // ActionTable name resources of modules that do not exist yet), connected.
  void SetUp() override {
    at_ = CreateActionTable("at", "mt/meters", "rt/next_hops");
    em_ = CreateExactMatch("em", "at/actions");
    mt_ = CreateMeterModule("mt", 64);
    rt_ = CreateRouterModule("rt");
    ASSERT_NE(nullptr, at_);
    ASSERT_NE(nullptr, em_);
    ASSERT_NE(nullptr, mt_);
    ASSERT_NE(nullptr, rt_);
    // No default hooks (Track counts per worker; the harness is not one).
    ASSERT_EQ(0, ModuleGraph::ConnectModules(em_, 0, at_, 0, true));
    ASSERT_EQ(0, ModuleGraph::ConnectModules(at_, 0, mt_, 0, true));
    ASSERT_EQ(0, ModuleGraph::ConnectModules(mt_, Meter::kGreenGate, rt_, 0, true));
  }

  // One session: the meter (`burst` bytes, no refill), the next hop to
  // kEgressGate, the route to it, the action naming both, and the rule.
  std::vector<Op> SessionOps(const char *sip, uint16_t sport, uint64_t burst) {
    return {
        Op::Upsert("mt/meters", bess::dataplane::EncodeKey(MeterId(1)),
                   std::any(bess::meter::MeterProfileSpec{SlowMeter(burst)})),
        rt_->router()->SetNextHopOp(
            NextHopId(1), HopWithEgress(bess::modules::router::InterfaceOf(kEgressGate).value())),
        rt_->router()->SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(1)),
        Op::Upsert("at/actions", bess::dataplane::EncodeKey(ActionId(1)),
                   std::any(ActionTable::Action{MeterId(1), NextHopId(1)})),
        Op::Upsert("em/rules", WireKey(sip, sport), std::any(bess::MakeGateResult(0, 1))),
    };
  }

  // What leaves the router's egress gate for `pkts` sent into the ExactMatch.
  std::vector<bess::testing::Bytes> Egress(const std::vector<bess::testing::Bytes> &pkts) {
    return h_.RunPipeline(em_, 0, pkts, rt_, {kEgressGate})[kEgressGate];
  }

  bess::testing::ModuleHarness h_;
  ExactMatch *em_ = nullptr;
  ActionTable *at_ = nullptr;
  Meter *mt_ = nullptr;
  Router *rt_ = nullptr;
  std::mt19937 rng_{7};
};

// One transaction creates the whole session; two 60-byte packets of it: the
// meter's 100-byte buckets take the first (green -> the router -> the next
// hop's egress, unchanged) and not the second (red -> dropped).
TEST_F(SessionPacketPathTest, SessionTransactionSteersAndPolices) {
  for (const char *name : {"em/rules", "at/actions", "mt/meters", "rt/next_hops", "rt/routes"}) {
    EXPECT_NE(nullptr, Engine().FindResource(name)) << name;
  }
  auto r = Apply(SessionOps("10.1.2.3", 1234, 100));
  ASSERT_EQ(Outcome::kApplied, r.outcome) << (r.ops.empty() ? "" : r.ops[0].error);

  const auto pkt1 = SessionPacket("10.1.2.3", 1234, rng_);
  const auto pkt2 = SessionPacket("10.1.2.3", 1234, rng_);
  ASSERT_EQ(60u, pkt1.size());
  EXPECT_EQ((std::vector<bess::testing::Bytes>{Forwarded(pkt1)}), Egress({pkt1, pkt2}));
}

// A packet whose rule does not match takes the ExactMatch default gate
// (drop); a packet whose action names no next hop is dropped at the router.
TEST_F(SessionPacketPathTest, OtherSessionsAndMissesAreDropped) {
  auto r = Apply(SessionOps("10.1.2.3", 1234, 100));
  ASSERT_EQ(Outcome::kApplied, r.outcome) << (r.ops.empty() ? "" : r.ops[0].error);
  r = Apply({
      Op::Upsert("at/actions", bess::dataplane::EncodeKey(ActionId(2)),
                 std::any(ActionTable::Action{MeterId(0), NextHopId(0)})),
      Op::Upsert("em/rules", WireKey("10.4.4.4", 4321), std::any(bess::MakeGateResult(0, 2))),
  });
  ASSERT_EQ(Outcome::kApplied, r.outcome) << (r.ops.empty() ? "" : r.ops[0].error);

  const auto miss = SessionPacket("192.0.2.1", 1234, rng_);
  const auto other = SessionPacket("10.4.4.4", 4321, rng_);
  EXPECT_TRUE(Egress({miss, other}).empty());
  // The session's own packet still goes through.
  const auto own = SessionPacket("10.1.2.3", 1234, rng_);
  EXPECT_EQ((std::vector<bess::testing::Bytes>{Forwarded(own)}), Egress({own}));
}

// Removing the session -- the rule, the action, the route, the next hop and
// the meter in one transaction -- stops its packets. (That the meter alone
// may not go while the action names it is RejectionsLeaveNothingChanged.)
TEST_F(SessionPacketPathTest, RemovedSessionIsDropped) {
  // 1000-byte buckets: the meter would pass both packets.
  auto r = Apply(SessionOps("10.1.2.3", 1234, 1000));
  ASSERT_EQ(Outcome::kApplied, r.outcome) << (r.ops.empty() ? "" : r.ops[0].error);
  const auto pkt = SessionPacket("10.1.2.3", 1234, rng_);
  ASSERT_EQ((std::vector<bess::testing::Bytes>{Forwarded(pkt)}), Egress({pkt}));

  r = Apply({
      Op::Erase("em/rules", WireKey("10.1.2.3", 1234)),
      Op::Erase("at/actions", bess::dataplane::EncodeKey(ActionId(1))),
      rt_->router()->RemoveRouteOp(P(Ip(10, 0, 0, 0), 8)),
      rt_->router()->RemoveNextHopOp(NextHopId(1)),
      Op::Erase("mt/meters", bess::dataplane::EncodeKey(MeterId(1))),
  });
  ASSERT_EQ(Outcome::kApplied, r.outcome) << (r.ops.empty() ? "" : r.ops[0].error);
  Settle();
  EXPECT_TRUE(Egress({pkt}).empty());
}

// -- D-129: the resources as a client writes and reads them --------------------

// One session as wire messages: the meter (`burst` bytes, no refill), the
// next hop to kEgressGate, the route to it, the action naming both, and the
// rule selecting the action.
struct WireSession {
  bess::pb::MeterIdKey meter_key;
  bess::pb::MeterPolicyValue meter;
  bess::pb::RouterNextHopIdKey hop_key;
  bess::pb::RouterNextHopValue hop;
  bess::pb::RouterRouteKey route_key;
  bess::pb::RouterRouteValue route;
  bess::pb::ActionIdKey action_key;
  bess::pb::ActionValue action;
  bess::pb::ExactMatchRuleKey rule_key;
  bess::pb::ExactMatchRuleValue rule;
};

WireSession MakeWireSession(const char *sip, uint16_t sport, uint64_t burst) {
  WireSession s;
  s.meter_key.set_id(1);
  auto *tr = s.meter.mutable_tr_tcm();
  tr->set_committed_rate(1);
  tr->set_committed_burst(burst);
  tr->set_peak_rate(1);
  tr->set_peak_burst(burst);
  s.hop_key.set_id(1);
  s.hop.set_egress_gate(kEgressGate);
  // Resolved: no neighbor, the canonical form (defaults omitted).
  s.hop.set_dst_mac(std::string("\x02\x00\x00\x00\x00\x01", 6));
  s.hop.set_src_mac(std::string("\x02\x00\x00\x00\x00\x02", 6));
  s.route_key.set_ipv4("10.0.0.0");
  s.route_key.set_prefix_length(8);
  s.route.set_next_hop_id(1);
  s.action_key.set_id(1);
  s.action.set_meter_id(1);
  s.action.set_next_hop_id(1);
  const std::string key = WireKey(sip, sport);
  s.rule_key.add_fields()->set_value_bin(key.substr(0, 4));
  s.rule_key.add_fields()->set_value_bin(key.substr(4));
  s.rule.set_action_id(1);
  return s;
}

// The session written as a client writes it steers and polices its packets
// as the internal operations do, and every resource reads back exactly what
// was upserted -- the action's references to the meter and the next hop, and
// the route's to the next hop, included.
TEST_F(SessionPacketPathTest, WireSessionSteersPolicesAndReadsBack) {
  const WireSession s = MakeWireSession("10.1.2.3", 1234, 100);
  // Referents first: each call is its own transaction.
  ASSERT_EQ("", h_.UpsertResource("mt/meters", s.meter_key, s.meter));
  ASSERT_EQ("", h_.UpsertResource("rt/next_hops", s.hop_key, s.hop));
  ASSERT_EQ("", h_.UpsertResource("rt/routes", s.route_key, s.route));
  ASSERT_EQ("", h_.UpsertResource("at/actions", s.action_key, s.action));
  ASSERT_EQ("", h_.UpsertResource("em/rules", s.rule_key, s.rule));

  const auto pkt1 = SessionPacket("10.1.2.3", 1234, rng_);
  const auto pkt2 = SessionPacket("10.1.2.3", 1234, rng_);
  // The meter's burst passes the first packet and drops the second; the frame
  // that leaves carries the next hop's addresses rather than the ones it
  // arrived with, and nothing after the Ethernet header is touched (D-153).
  bess::testing::Bytes want = pkt1;
  ASSERT_GE(want.size(), 12u);
  std::memcpy(want.data(), "\x02\x00\x00\x00\x00\x01", 6);
  std::memcpy(want.data() + 6, "\x02\x00\x00\x00\x00\x02", 6);
  EXPECT_EQ((std::vector<bess::testing::Bytes>{Forwarded(want)}), Egress({pkt1, pkt2}));

  EXPECT_EQ("", h_.ReadBackDiffers("mt/meters", {{&s.meter_key, &s.meter}}));
  EXPECT_EQ("", h_.ReadBackDiffers("rt/next_hops", {{&s.hop_key, &s.hop}}));
  EXPECT_EQ("", h_.ReadBackDiffers("rt/routes", {{&s.route_key, &s.route}}));
  EXPECT_EQ("", h_.ReadBackDiffers("at/actions", {{&s.action_key, &s.action}}));
}

// Every meter profile, neighbor state, the default route and actions with and
// without references read back as written; a reconfiguration reads back the
// new value, and an erased entry is gone from the read-back at once -- also a
// meter, whose removal stage has not run yet.
TEST_F(SessionPacketPathTest, ReadBackFollowsUpsertsAndErases) {
  bess::pb::MeterIdKey m1, m2, m3;
  m1.set_id(1);
  m2.set_id(2);
  m3.set_id(3);
  bess::pb::MeterPolicyValue sr, tr, rfc;
  sr.mutable_sr_tcm()->set_committed_rate(1000);
  sr.mutable_sr_tcm()->set_committed_burst(1500);
  tr.mutable_tr_tcm()->set_committed_rate(1000);
  tr.mutable_tr_tcm()->set_committed_burst(1500);
  tr.mutable_tr_tcm()->set_peak_rate(2000);
  tr.mutable_tr_tcm()->set_peak_burst(3000);
  rfc.mutable_tr_tcm_rfc4115()->set_committed_rate(1000);
  rfc.mutable_tr_tcm_rfc4115()->set_committed_burst(1500);
  rfc.mutable_tr_tcm_rfc4115()->set_excess_rate(500);
  rfc.mutable_tr_tcm_rfc4115()->set_excess_burst(700);
  ASSERT_EQ("", h_.UpsertResource("mt/meters", m1, sr));
  ASSERT_EQ("", h_.UpsertResource("mt/meters", m2, tr));
  ASSERT_EQ("", h_.UpsertResource("mt/meters", m3, rfc));

  bess::pb::RouterNextHopIdKey h1, h2, h3;
  h1.set_id(1);
  h2.set_id(2);
  h3.set_id(3);
  bess::pb::RouterNextHopValue resolved, incomplete, unreachable;
  resolved.set_egress_gate(3);
  // No neighbor: resolved, in the canonical form (defaults omitted).
  resolved.set_dst_mac(std::string("\x02\x00\x00\x00\x00\x01", 6));
  incomplete.set_neighbor(bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE);  // gate 0
  incomplete.set_src_mac(std::string("\x02\x00\x00\x00\x00\x02", 6));
  unreachable.set_egress_gate(DROP_GATE);
  unreachable.set_neighbor(bess::pb::ROUTER_NEIGHBOR_STATE_UNREACHABLE);
  ASSERT_EQ("", h_.UpsertResource("rt/next_hops", h1, resolved));
  ASSERT_EQ("", h_.UpsertResource("rt/next_hops", h2, incomplete));
  ASSERT_EQ("", h_.UpsertResource("rt/next_hops", h3, unreachable));

  bess::pb::RouterRouteKey r1, r2, r3;
  r1.set_ipv4("10.0.0.0");
  r1.set_prefix_length(8);
  r2.set_ipv4("0.0.0.0");  // the default route
  r3.set_ipv4("192.168.1.0");
  r3.set_prefix_length(24);
  bess::pb::RouterRouteValue via1, via2, via3;
  via1.set_next_hop_id(1);
  via2.set_next_hop_id(2);
  via3.set_next_hop_id(3);
  ASSERT_EQ("", h_.UpsertResource("rt/routes", r1, via1));
  ASSERT_EQ("", h_.UpsertResource("rt/routes", r2, via2));
  ASSERT_EQ("", h_.UpsertResource("rt/routes", r3, via3));

  bess::pb::ActionIdKey a1, a2, a3;
  a1.set_id(1);
  a2.set_id(2);
  a3.set_id(3);
  bess::pb::ActionValue both, hop_only, none;
  both.set_meter_id(1);
  both.set_next_hop_id(1);
  hop_only.set_next_hop_id(2);
  ASSERT_EQ("", h_.UpsertResource("at/actions", a1, both));
  ASSERT_EQ("", h_.UpsertResource("at/actions", a2, hop_only));
  ASSERT_EQ("", h_.UpsertResource("at/actions", a3, none));

  EXPECT_EQ("", h_.ReadBackDiffers("mt/meters", {{&m1, &sr}, {&m2, &tr}, {&m3, &rfc}}));
  EXPECT_EQ("", h_.ReadBackDiffers("rt/next_hops",
                                   {{&h1, &resolved}, {&h2, &incomplete}, {&h3, &unreachable}}));
  EXPECT_EQ("", h_.ReadBackDiffers("rt/routes", {{&r1, &via1}, {&r2, &via2}, {&r3, &via3}}));
  EXPECT_EQ("", h_.ReadBackDiffers("at/actions", {{&a1, &both}, {&a2, &hop_only}, {&a3, &none}}));

  // A reconfiguration reads back the new value.
  ASSERT_EQ("", h_.UpsertResource("mt/meters", m1, rfc));
  ASSERT_EQ("", h_.UpsertResource("at/actions", a3, hop_only));
  EXPECT_EQ("", h_.ReadBackDiffers("mt/meters", {{&m1, &rfc}, {&m2, &tr}, {&m3, &rfc}}));
  EXPECT_EQ("", h_.ReadBackDiffers("at/actions",
                                   {{&a1, &both}, {&a2, &hop_only}, {&a3, &hop_only}}));

  // Erases, before any removal stage runs.
  ASSERT_EQ("", h_.EraseResource("at/actions", a3));
  ASSERT_EQ("", h_.EraseResource("mt/meters", m3));
  ASSERT_EQ("", h_.EraseResource("rt/routes", r3));
  ASSERT_EQ("", h_.EraseResource("rt/next_hops", h3));
  EXPECT_EQ("", h_.ReadBackDiffers("mt/meters", {{&m1, &rfc}, {&m2, &tr}}));
  EXPECT_EQ("", h_.ReadBackDiffers("rt/next_hops", {{&h1, &resolved}, {&h2, &incomplete}}));
  EXPECT_EQ("", h_.ReadBackDiffers("rt/routes", {{&r1, &via1}, {&r2, &via2}}));
  EXPECT_EQ("", h_.ReadBackDiffers("at/actions", {{&a1, &both}, {&a2, &hop_only}}));
  Settle();
  EXPECT_EQ("", h_.ReadBackDiffers("mt/meters", {{&m1, &rfc}, {&m2, &tr}}));
  EXPECT_EQ("", h_.ReadBackDiffers("rt/next_hops", {{&h1, &resolved}, {&h2, &incomplete}}));
}

// A next hop reads back in one canonical form: defaults omitted -- no
// neighbor when resolved, a zero MAC absent. Spelling a default out (six
// zero bytes, RESOLVED) is the same next hop but not the same message; the
// read-back form is a fixed point -- applying it again changes nothing.
TEST_F(SessionPacketPathTest, NextHopReadsBackCanonically) {
  bess::pb::RouterNextHopIdKey key;
  key.set_id(4);
  bess::pb::RouterNextHopValue spelled;
  spelled.set_egress_gate(2);
  spelled.set_neighbor(bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
  spelled.set_dst_mac(std::string(6, '\0'));
  ASSERT_EQ("", h_.UpsertResource("rt/next_hops", key, spelled));

  const auto read = h_.ReadResource("rt/next_hops");
  ASSERT_EQ(1u, read.size());
  bess::pb::RouterNextHopValue canonical;
  ASSERT_TRUE(read[0].second.UnpackTo(&canonical));
  EXPECT_EQ(2u, canonical.egress_gate());
  EXPECT_EQ(bess::pb::ROUTER_NEIGHBOR_STATE_UNSPECIFIED, canonical.neighbor());
  EXPECT_TRUE(canonical.dst_mac().empty());
  EXPECT_TRUE(canonical.src_mac().empty());
  EXPECT_NE("", h_.ReadBackDiffers("rt/next_hops", {{&key, &spelled}}));
  EXPECT_EQ("", h_.ReadBackDiffers("rt/next_hops", {{&key, &canonical}}));

  ASSERT_EQ("", h_.UpsertResource("rt/next_hops", key, canonical));
  EXPECT_EQ("", h_.ReadBackDiffers("rt/next_hops", {{&key, &canonical}}));
}

// Bad values are refused through the codecs and the resources, and leave
// nothing behind.
TEST_F(SessionPacketPathTest, BadResourceValuesAreRefused) {
  bess::pb::MeterIdKey m1;
  m1.set_id(1);
  EXPECT_NE("", h_.UpsertResource("mt/meters", m1, bess::pb::MeterPolicyValue{}));
  bess::pb::MeterPolicyValue zero_rate;
  zero_rate.mutable_sr_tcm()->set_committed_burst(1500);
  EXPECT_NE("", h_.UpsertResource("mt/meters", m1, zero_rate));
  bess::pb::MeterIdKey m0;
  bess::pb::MeterPolicyValue sr;
  sr.mutable_sr_tcm()->set_committed_rate(1000);
  sr.mutable_sr_tcm()->set_committed_burst(1500);
  EXPECT_NE("", h_.UpsertResource("mt/meters", m0, sr));

  bess::pb::RouterNextHopIdKey h1;
  h1.set_id(1);
  bess::pb::RouterNextHopValue short_mac;
  short_mac.set_dst_mac(std::string(5, '\x02'));
  EXPECT_NE("", h_.UpsertResource("rt/next_hops", h1, short_mac));
  bess::pb::RouterNextHopValue bad_gate;
  bad_gate.set_egress_gate(DROP_GATE + 1);  // DROP_GATE itself is "no interface"
  EXPECT_NE("", h_.UpsertResource("rt/next_hops", h1, bad_gate));

  bess::pb::RouterRouteKey r1;
  r1.set_ipv4("10.0.0.0");
  r1.set_prefix_length(8);
  EXPECT_NE("", h_.UpsertResource("rt/routes", r1, bess::pb::RouterRouteValue{}));
  bess::pb::RouterRouteValue dangling_route;
  dangling_route.set_next_hop_id(9);
  EXPECT_NE("", h_.UpsertResource("rt/routes", r1, dangling_route));

  bess::pb::ActionIdKey a1;
  a1.set_id(1);
  bess::pb::ActionValue dangling_action;
  dangling_action.set_meter_id(9);
  EXPECT_NE("", h_.UpsertResource("at/actions", a1, dangling_action));

  for (const char *name : {"mt/meters", "rt/next_hops", "rt/routes", "at/actions"}) {
    EXPECT_TRUE(h_.ReadResource(name).empty()) << name;
  }
}

// Each class declares exactly the tables its modules bind (the control plane
// refuses creation otherwise), and none requires paused workers: meters
// publish generations, next hops, groups and actions are slot tables, routes
// are rte_lpm's in-place updates.
TEST_F(SessionPacketPathTest, DeclaredTablesMatchTheBoundCodecs) {
  const struct {
    const char *class_name;
    const char *table;
    const char *resource;
  } tables[] = {
      {"Meter", "meters", "mt/meters"},
      {"Router", "next_hops", "rt/next_hops"},
      {"Router", "groups", "rt/groups"},
      {"Router", "routes", "rt/routes"},
      {"ActionTable", "actions", "at/actions"},
  };
  for (const auto &t : tables) {
    const ModuleBuilder &builder = ModuleBuilder::all_module_builders().at(t.class_name);
    const ResourceTable *declared = builder.FindResourceTable(t.table);
    ASSERT_NE(nullptr, declared) << t.resource;
    const auto *resource = Engine().FindResource(t.resource);
    ASSERT_NE(nullptr, resource) << t.resource;
    const auto *codec = Bindings().Find(*resource);
    ASSERT_NE(nullptr, codec) << t.resource;
    EXPECT_EQ(ResourceTable::MAP, declared->kind) << t.resource;
    EXPECT_EQ(codec->key_type(), declared->key_type) << t.resource;
    EXPECT_EQ(codec->value_type(), declared->value_type) << t.resource;
    EXPECT_FALSE(declared->requires_paused_workers) << t.resource;
    EXPECT_FALSE(codec->requires_paused_workers()) << t.resource;
  }
  EXPECT_EQ(3u, ModuleBuilder::all_module_builders().at("Router").resource_tables().size());
}

// The configured names are the contract, not the registration: the graph
// resolves one offset for a writer's name and its reader's, and the packet
// path is the proof. SessionPacketPathTest runs the defaults.
TEST_F(SessionPipelineTest, ConfiguredNamesResolveToOneOffset) {
  ActionTable *at = CreateActionTable("at_cfg", "mt_cfg/meters",
                                      "rt_cfg/next_hops", 64, "flow_meter_id");
  ExactMatch *em = CreateExactMatch("em_cfg", "at_cfg/actions");
  Meter *mt = CreateMeterModule("mt_cfg", 64, "flow_meter_id");
  Router *rt = CreateRouterModule("rt_cfg");
  ASSERT_NE(nullptr, at);
  ASSERT_NE(nullptr, em);
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(em, 0, at, 0, true));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(at, 0, mt, 0, true));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(mt, Meter::kGreenGate, rt, 0, true));

  auto r = Apply({
      Op::Upsert("mt_cfg/meters", bess::dataplane::EncodeKey(MeterId(1)),
                 std::any(bess::meter::MeterProfileSpec{SlowMeter(100)})),
      rt->router()->SetNextHopOp(
          NextHopId(1),
          HopWithEgress(bess::modules::router::InterfaceOf(kEgressGate).value())),
      rt->router()->SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(1)),
      Op::Upsert("at_cfg/actions", bess::dataplane::EncodeKey(ActionId(1)),
                 std::any(ActionTable::Action{MeterId(1), NextHopId(1)})),
      Op::Upsert("em_cfg/rules", WireKey("10.1.2.3", 1234),
                 std::any(bess::MakeGateResult(0, 1))),
  });
  ASSERT_EQ(Outcome::kApplied, r.outcome)
      << (r.ops.empty() ? "" : r.ops[0].error);

  // The packet traverses every module: the rule's action id is read by the
  // table under "action_id" (its default), the table writes the meter id under
  // the configured name, the meter reads that name, and the router forwards.
  // Two packets against a 100-byte burst: the first is green, the second red.
  // Had the meter not read the id the table wrote, it would read 0
  // ("unmetered") and pass both; the single egress frame is the proof.
  std::random_device rd;
  std::mt19937 rng{rd()};
  const auto pkt1 = SessionPacket("10.1.2.3", 1234, rng);
  const auto pkt2 = SessionPacket("10.1.2.3", 1234, rng);
  bess::testing::ModuleHarness h;
  EXPECT_EQ((std::vector<bess::testing::Bytes>{Forwarded(pkt1)}),
            h.RunPipeline(em, 0, {pkt1, pkt2}, rt, {kEgressGate})[kEgressGate]);

  // The meter's read and the table's write resolved to one offset: the
  // names, not the strings that registered them, are the contract.
  EXPECT_EQ(mt->attr_offset(AttrIndex(mt, "flow_meter_id")),
            at->attr_offset(AttrIndex(at, "flow_meter_id")));
}

// A custom action name needs a writer and a reader configured to match, and
// each output needs its reader: SetMetadata writes "policy_id", the table
// reads it and writes "flow_meter_id" and "hop_id", and the meter and the
// router read those. Defaults are covered by SessionPacketPathTest.
TEST_F(SessionPipelineTest, ConfiguredActionAndNextHopNamesPairWithTheirReaders) {
  bess::pb::SetMetadataArg md_arg;
  auto *writes_policy = md_arg.add_attrs();
  writes_policy->set_name("policy_id");
  writes_policy->set_size(4);
  writes_policy->set_value_bin(std::string("\x00\x00\x00\x01", 4));
  Module *md = Create("SetMetadata", "md_cfg", Pack(md_arg));
  ActionTable *at = CreateActionTable("at_names", "mt_names/meters",
                                      "rt_names/next_hops", 64,
                                      "flow_meter_id", "hop_id", "policy_id");
  Meter *mt = CreateMeterModule("mt_names", 64, "flow_meter_id");
  Router *rt = CreateRouterModule("rt_names", "hop_id");
  ASSERT_NE(nullptr, md);
  ASSERT_NE(nullptr, at);
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(md, 0, at, 0, true));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(at, 0, mt, 0, true));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(mt, Meter::kGreenGate, rt, 0, true));

  auto r = Apply({
      Op::Upsert("mt_names/meters", bess::dataplane::EncodeKey(MeterId(1)),
                 std::any(bess::meter::MeterProfileSpec{SlowMeter(1000)})),
      rt->router()->SetNextHopOp(
          NextHopId(1),
          HopWithEgress(bess::modules::router::InterfaceOf(kEgressGate).value())),
      Op::Upsert("at_names/actions", bess::dataplane::EncodeKey(ActionId(1)),
                 std::any(ActionTable::Action{MeterId(1), NextHopId(1)})),
  });
  ASSERT_EQ(Outcome::kApplied, r.outcome)
      << (r.ops.empty() ? "" : r.ops[0].error);

  std::random_device rd;
  std::mt19937 rng{rd()};
  const auto pkt = SessionPacket("10.1.2.3", 1234, rng);
  bess::testing::ModuleHarness h;
  EXPECT_EQ((std::vector<bess::testing::Bytes>{Forwarded(pkt)}),
            h.RunPipeline(md, 0, {pkt}, rt, {kEgressGate})[kEgressGate]);

  // Each configured name paired with the writer's or the reader's.
  EXPECT_EQ(md->attr_offset(AttrIndex(md, "policy_id")),
            at->attr_offset(AttrIndex(at, "policy_id")));
  EXPECT_EQ(mt->attr_offset(AttrIndex(mt, "flow_meter_id")),
            at->attr_offset(AttrIndex(at, "flow_meter_id")));
  EXPECT_EQ(rt->attr_offset(AttrIndex(rt, "hop_id")),
            at->attr_offset(AttrIndex(at, "hop_id")));
}

// A multi-level action writes every slot of the one `meter_id` attribute the
// Meter reads, 4 bytes per level. The single-id form still names level 0 and
// fills the rest with 0 ("no meter"), so a Meter expecting more levels never
// reads a neighbour slot the action did not write.
TEST_F(SessionPipelineTest, AOneIdActionFillsEverySlotOfATwoLevelTable) {
  bess::testing::ModuleHarness h;
  ActionTable *at = CreateActionTable("at_legacy", "mt_legacy/meters",
                                      "rt_legacy/next_hops", 64, "", "", "", 2);
  Meter *mt = CreateMeterModule("mt_legacy", 64, "", 2);
  Router *rt = CreateRouterModule("rt_legacy");
  ASSERT_NE(nullptr, at);
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);
  ASSERT_EQ(8u, at->all_attrs()[AttrIndex(at, "meter_id")].size);
  ASSERT_EQ(8u, mt->all_attrs()[AttrIndex(mt, "meter_id")].size);

  bess::pb::MeterIdKey meter_key;
  meter_key.set_id(1);
  bess::pb::MeterPolicyValue meter;
  meter.mutable_tr_tcm()->set_committed_rate(1);
  meter.mutable_tr_tcm()->set_committed_burst(100);
  meter.mutable_tr_tcm()->set_peak_rate(1);
  meter.mutable_tr_tcm()->set_peak_burst(100);
  EXPECT_EQ("", h.UpsertResource("mt_legacy/meters", meter_key, meter));

  bess::pb::ActionIdKey action_key;
  action_key.set_id(1);
  bess::pb::ActionValue one_id;
  one_id.set_meter_id(1);
  EXPECT_EQ("", h.UpsertResource("at_legacy/actions", action_key, one_id));

  const ActionTable::Action *action = at->LookupAction(ActionId(1));
  ASSERT_NE(nullptr, action);
  EXPECT_EQ(2u, action->levels) << "the table's level count, not the form's";
  EXPECT_EQ(1u, action->meters[0].value());
  EXPECT_EQ(0u, action->meters[1].value()) << "the second slot is no meter";
}

// A two-level record is policed in order: green and yellow keep charging the
// later levels, red stops, and the shared session bucket is not billed for
// what the flow's first level dropped.
TEST_F(SessionPipelineTest, TwoLevelsPoliceInOrderAndARedChargesNoLaterLevel) {
  // The Meter reads two ids from the one 8-byte attribute; a writer names the
  // record per stream (1,2 then 3,2), so no ActionTable is needed to exercise
  // the ordered passes. 2 is the shared session bucket at both streams' level 2.
  bess::pb::SetMetadataArg flow_a_arg;
  auto *a = flow_a_arg.add_attrs();
  a->set_name("meter_id");
  a->set_size(8);
  a->set_value_bin(std::string("\x00\x00\x00\x01\x00\x00\x00\x02", 8));
  bess::pb::SetMetadataArg flow_b_arg;
  auto *b = flow_b_arg.add_attrs();
  b->set_name("meter_id");
  b->set_size(8);
  b->set_value_bin(std::string("\x00\x00\x00\x03\x00\x00\x00\x02", 8));
  Module *flow_a = Create("SetMetadata", "md_flow_a", Pack(flow_a_arg));
  Module *flow_b = Create("SetMetadata", "md_flow_b", Pack(flow_b_arg));
  Meter *mt = CreateMeterModule("mt_lv", 64, "", 2);
  ASSERT_NE(nullptr, flow_a);
  ASSERT_NE(nullptr, flow_b);
  ASSERT_NE(nullptr, mt);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(flow_a, 0, mt, 0, true));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(flow_b, 0, mt, 0, true));
  ASSERT_EQ(8u, mt->all_attrs()[AttrIndex(mt, "meter_id")].size);

  auto r = Apply({
      Op::Upsert("mt_lv/meters", bess::dataplane::EncodeKey(MeterId(1)),
                 std::any(bess::meter::MeterProfileSpec{SlowMeter(60)})),
      // The shared bucket holds exactly two packets: the first stream's green
      // packet and the second stream's, but not a charge from the red one.
      Op::Upsert("mt_lv/meters", bess::dataplane::EncodeKey(MeterId(2)),
                 std::any(bess::meter::MeterProfileSpec{SlowMeter(120)})),
      Op::Upsert("mt_lv/meters", bess::dataplane::EncodeKey(MeterId(3)),
                 std::any(bess::meter::MeterProfileSpec{SlowMeter(60)})),
  });
  ASSERT_EQ(Outcome::kApplied, r.outcome)
      << (r.ops.empty() ? "" : r.ops[0].error);

  std::random_device rd;
  std::mt19937 rng{rd()};
  const auto first = SessionPacket("10.1.2.3", 1234, rng);
  const auto second = SessionPacket("10.1.2.3", 1234, rng);
  bess::testing::ModuleHarness h;

  auto out = h.RunPipeline(flow_a, 0, {first, second}, mt, {0, 2});
  EXPECT_EQ(1u, out[0].size()) << "the first packet is green at both levels";
  ASSERT_EQ(1u, out[2].size()) << "the second is red at the first level";

  // The second stream passes its own first level. The shared level-2 bucket
  // still holds its burst, so this packet is green; it would be red had the
  // red packet above been charged to level 2 before being dropped.
  const auto other = SessionPacket("10.1.2.4", 4321, rng);
  out = h.RunPipeline(flow_b, 0, {other}, mt, {0, 2});
  EXPECT_EQ(1u, out[0].size()) << "the red packet charged no later level";
  EXPECT_TRUE(out[2].empty());
}

}  // namespace
