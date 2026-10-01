// SPDX-License-Identifier: BSD-3-Clause

// The session vertical slice (G1.2b, D-021's acceptance item): one
// transaction across ExactMatch (action mode), ActionTable, Meter and Router
// -- the rule that selects a session, the action it names, the meter that
// polices it and the route that forwards it -- with the engine's reference
// ledger, the removal cascade, and teardown in an order that does not respect
// the reference graph (the desired-state planner destroys modules by name).
//
// The packet path itself is covered end to end by
// bessctl/module_tests/session_pipeline.py against a live daemon; here each
// stage's decision is driven in process.

#include <gtest/gtest.h>

#include <array>
#include <any>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "runtime/runtime_state.h"
#include "dataplane/action_id.h"
#include "framework/resource_bindings.h"
#include "dataplane/transaction_engine.h"
#include "meter/meter.h"
#include "module.h"
#include "module_graph.h"
#include "modules/action_table.h"
#include "modules/exact_match.h"
#include "modules/meter.h"
#include "modules/router.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"

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

Router *CreateRouterModule(const std::string &name) {
  bess::pb::RouterArg arg;
  arg.set_max_routes(64);
  arg.set_max_tbl8s(16);
  arg.set_max_next_hops(64);
  return static_cast<Router *>(Create("Router", name, Pack(arg)));
}

Meter *CreateMeterModule(const std::string &name, uint64_t capacity) {
  bess::pb::MeterArg arg;
  arg.set_capacity(capacity);
  return static_cast<Meter *>(Create("Meter", name, Pack(arg)));
}

ActionTable *CreateActionTable(const std::string &name, const std::string &meters,
                               const std::string &next_hops,
                               uint64_t capacity = 64) {
  bess::pb::ActionTableArg arg;
  arg.set_capacity(capacity);
  arg.set_meters(meters);
  arg.set_next_hops(next_hops);
  return static_cast<ActionTable *>(Create("ActionTable", name, Pack(arg)));
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

bess::route::NextHop HopWithEgress(gate_idx_t egress) {
  bess::route::NextHop hop;
  hop.egress = egress;
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
                 std::any(uint64_t{1})),
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
    em->ClassifyActionsBatch(&batch, actions.data());
    EXPECT_EQ(actions[0], 1u);  // matched action id
    EXPECT_EQ(actions[1], 0u);  // miss
    EXPECT_EQ(actions[2], 0u);
  }
  const ActionTable::Action *action = at->LookupAction(ActionId(1));
  ASSERT_NE(nullptr, action);
  EXPECT_EQ(action->meter.value(), 1u);
  EXPECT_EQ(action->next_hop.value(), 1u);
  EXPECT_EQ(at->LookupAction(ActionId(2)), nullptr);

  {
    // The meter's decision on a batch whose ids the test writes: a 60-byte
    // packet fits the committed and peak buckets (100 bytes, no refill), the
    // next one does not.
    mt->set_attr_offset(0, 0);  // meter_id, at metadata offset 0
    bess::PacketBatch batch = packets.Batch(0, 2);
    for (int i = 0; i < batch.cnt(); i++) {
      _set_attr_with_offset<bess::utils::be32_t>(
          0, batch.packet(i), bess::utils::be32_t(1));
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
    EXPECT_EQ(hops[0]->egress, 7);
    EXPECT_EQ(hops[0]->neighbor, bess::route::NeighborState::kResolved);
  }
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
                             std::any(uint64_t{7}))});
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
  EXPECT_EQ(at->LookupAction(ActionId(1))->meter.value(), 1u);

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
                              std::any(uint64_t{1}))})
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
                              std::any(uint64_t{1}))})
                .outcome,
            Outcome::kApplied);
  ASSERT_NE(nullptr, rt->router()->Resolve(Ip(10, 9, 9, 9)));
  EXPECT_EQ(rt->router()->Resolve(Ip(10, 9, 9, 9))->egress, 9);
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
                              std::any(uint64_t{1}))})
                .outcome,
            Outcome::kApplied);

  ModuleGraph::DestroyAllModules();
  EXPECT_TRUE(Engine().ResourceNames().empty())
      << "a resource outlived its module";
}

// Module commands must not bypass the action-rule reference ledger. Restore
// also checks every new action before changing the existing rules.
TEST_F(SessionPipelineTest, ActionModeCommandsKeepReferenceCounts) {
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
  ASSERT_TRUE(em->GetInitialArg(bess::pb::EmptyArg()).data().UnpackTo(&initial));
  EXPECT_EQ(initial.action_resource(), "at/actions");

  bess::pb::ExactMatchCommandAddArg rule;
  rule.set_action_id(1);
  const std::string key = Key(Ip(10, 1, 2, 3), 0);
  rule.add_fields()->set_value_bin(key.substr(0, 4));
  rule.add_fields()->set_value_bin(key.substr(4));
  ASSERT_EQ(em->CommandAdd(rule).error().code(), 0);
  EXPECT_EQ(Engine().ReferenceCount("at/actions",
                                    bess::dataplane::EncodeKey(ActionId(1))),
            1u);
  EXPECT_EQ(Apply({Op::Erase("at/actions",
                            bess::dataplane::EncodeKey(ActionId(1)))})
                .outcome,
            Outcome::kRejected);

  bess::pb::ExactMatchConfig config;
  config.set_default_gate(DROP_GATE);
  *config.add_rules() = rule;
  config.mutable_rules(0)->set_action_id(99);
  EXPECT_NE(em->SetRuntimeConfig(config).error().code(), 0);
  EXPECT_EQ(Engine().ReferenceCount("at/actions",
                                    bess::dataplane::EncodeKey(ActionId(1))),
            1u);
  config.mutable_rules(0)->set_action_id(2);
  ASSERT_EQ(em->SetRuntimeConfig(config).error().code(), 0);
  EXPECT_EQ(Engine().ReferenceCount("at/actions",
                                    bess::dataplane::EncodeKey(ActionId(1))),
            0u);
  EXPECT_EQ(Engine().ReferenceCount("at/actions",
                                    bess::dataplane::EncodeKey(ActionId(2))),
            1u);
  ASSERT_EQ(em->CommandClear(bess::pb::EmptyArg()).error().code(), 0);
  EXPECT_EQ(Engine().ReferenceCount("at/actions",
                                    bess::dataplane::EncodeKey(ActionId(2))),
            0u);
  ASSERT_EQ(em->CommandAdd(rule).error().code(), 0);
  bess::pb::ExactMatchCommandDeleteArg remove;
  *remove.mutable_fields() = rule.fields();
  ASSERT_EQ(em->CommandDelete(remove).error().code(), 0);
  EXPECT_EQ(Engine().ReferenceCount("at/actions",
                                    bess::dataplane::EncodeKey(ActionId(1))),
            0u);
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
  bess::pb::ExactMatchCommandAddArg rule;
  rule.set_action_id(70000);
  const std::string key = Key(Ip(10, 1, 2, 3), 0);
  rule.add_fields()->set_value_bin(key.substr(0, 4));
  rule.add_fields()->set_value_bin(key.substr(4));
  ASSERT_EQ(em->CommandAdd(rule).error().code(), 0);
  Packets packets(1);
  packets.SetKey(0, Ip(10, 1, 2, 3), 0);
  bess::PacketBatch batch = packets.Batch(0, 1);
  std::array<uint32_t, 1> ids;
  em->ClassifyActionsBatch(&batch, ids.data());
  EXPECT_EQ(ids[0], 70000u);
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
  em_val.set_gate(1);
  em_val.set_action_id(1);
  const google::protobuf::Any packed_em_val = Pack(em_val);
  EXPECT_FALSE(em_codec
                   ->Value(packed_em_val.type_url(), packed_em_val.value())
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


}  // namespace
