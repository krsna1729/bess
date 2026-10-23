// SPDX-License-Identifier: BSD-3-Clause

// The session vertical slice (G1.2b, D-021's acceptance item): one
// transaction across ExactMatch, Meter and Router -- the rule that selects a
// session, the meter and next hop it names in its result record, and the route
// that forwards it -- with the engine's reference ledger, the removal cascade,
// and teardown in an order that does not respect the reference graph (the
// desired-state planner destroys modules by name).
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
#include "framework/resource_bindings.h"
#include "dataplane/transaction_engine.h"
#include "meter/meter.h"
#include "metadata.h"
#include "module.h"
#include "module_graph.h"
#include "classifier/masked_rule_resource.h"
#include "classifier/rule_result.h"
#include "utils/endian.h"
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

using bess::dataplane::Op;
using bess::dataplane::TransactionEngine;
using bess::meter::MeterId;
using bess::route::Ipv4Prefix;
using bess::route::NextHopId;
using bess::testing::Bytes;
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

// The 8-byte result record a session rule carries: the meter id then the
// next-hop id, both big-endian 32-bit ids (the metadata convention).
std::string SessionRecord(uint32_t meter, uint32_t next_hop) {
  const bess::utils::be32_t m(meter);
  const bess::utils::be32_t h(next_hop);
  return std::string(reinterpret_cast<const char *>(&m), sizeof(m)) +
         std::string(reinterpret_cast<const char *>(&h), sizeof(h));
}

std::any Rule(uint64_t gate, uint32_t meter, uint32_t next_hop) {
  const std::string bytes = SessionRecord(meter, next_hop);
  return std::any(bess::classifier::RuleResult{
      static_cast<gate_idx_t>(gate),
      std::vector<std::byte>(
          reinterpret_cast<const std::byte *>(bytes.data()),
          reinterpret_cast<const std::byte *>(bytes.data()) + bytes.size())});
}

std::any RuleWithBytes(std::string bytes) {
  std::vector<std::byte> record(
      reinterpret_cast<const std::byte *>(bytes.data()),
      reinterpret_cast<const std::byte *>(bytes.data()) + bytes.size());
  return std::any(bess::classifier::RuleResult{DROP_GATE, std::move(record)});
}

// A rule's result names the meter that polices the session and the next hop
// it forwards to; both are transactional references (D-172).
void AddSessionResultFields(bess::pb::ExactMatchArg *arg,
                            const std::string &meters,
                            const std::string &next_hops,
                            const std::string &meter_attr,
                            const std::string &next_hop_attr) {
  auto *meter = arg->add_result_fields();
  meter->set_attr_name(meter_attr);
  meter->set_value_offset(0);
  meter->set_num_bytes(4);
  meter->set_reference(meters);
  meter->set_reference_key(bess::pb::ResultField::BE32);
  auto *hop = arg->add_result_fields();
  hop->set_attr_name(next_hop_attr);
  hop->set_value_offset(4);
  hop->set_num_bytes(4);
  hop->set_reference(next_hops);
  hop->set_reference_key(bess::pb::ResultField::BE32);
}

// 4 bytes at offset 26 (IPv4 src), 2 bytes at offset 34. With both resource
// names set, a match also writes the rule's meter and next-hop ids.
ExactMatch *CreateExactMatch(const std::string &name,
                             const std::string &meters = "",
                             const std::string &next_hops = "",
                             const std::string &meter_attr = "meter_id",
                             const std::string &next_hop_attr = "next_hop_id") {
  bess::pb::ExactMatchArg arg;
  auto *f = arg.add_fields();
  f->set_offset(26);
  f->set_num_bytes(4);
  f = arg.add_fields();
  f->set_offset(34);
  f->set_num_bytes(2);
  if (!meters.empty() && !next_hops.empty()) {
    AddSessionResultFields(&arg, meters, next_hops, meter_attr, next_hop_attr);
  }
  return static_cast<ExactMatch *>(Create("ExactMatch", name, Pack(arg)));
}

// A next-hop-id Router: it forwards on the id the rule's result writes and
// has no FIB, so it holds no routes (D-183).
Router *CreateRouterModule(const std::string &name,
                           const std::string &next_hop_attr = "") {
  bess::pb::RouterArg arg;
  arg.set_max_next_hops(64);
  if (!next_hop_attr.empty()) {
    arg.set_next_hop_attr(next_hop_attr);
  }
  return static_cast<Router *>(Create("Router", name, Pack(arg)));
}

// A destination-mode Router with an IPv4 FIB, for the tests about routes.
Router *CreateRoutingRouterModule(const std::string &name) {
  bess::pb::RouterArg arg;
  arg.set_mode(bess::pb::ROUTE_MODE_DESTINATION);
  arg.set_max_ipv4_routes(64);
  arg.set_max_ipv4_tbl8s(16);
  arg.set_max_next_hops(64);
  return static_cast<Router *>(Create("Router", name, Pack(arg)));
}

Meter *CreateMeterModule(const std::string &name, uint64_t capacity,
                         const std::string &id_attr = "",
                         uint32_t levels = 0,
                         bess::pb::MeterArg::MeterCost cost =
                             bess::pb::MeterArg::METER_COST_BYTES) {
  bess::pb::MeterArg arg;
  arg.set_capacity(capacity);
  if (!id_attr.empty()) {
    arg.set_id_attr(id_attr);
  }
  if (levels != 0) {
    arg.set_levels(levels);
  }
  arg.set_cost(cost);
  return static_cast<Meter *>(Create("Meter", name, Pack(arg)));
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
// (the ExactMatch before the meter and next-hop resources it names): a
// declared reference binds when its resource registers, so module creation
// order does not decide the graph.
TEST_F(SessionPipelineTest, OneTransactionCreatesASession) {
  ExactMatch *em = CreateExactMatch("em", "mt/meters", "rt/next_hops");
  ASSERT_NE(nullptr, em);
  Meter *mt = CreateMeterModule("mt", 64);
  ASSERT_NE(nullptr, mt);
  Router *rt = CreateRoutingRouterModule("rt");
  ASSERT_NE(nullptr, rt);

  const uint32_t session_addr = Ip(10, 1, 2, 3);
  // One transaction: the meter, the next hop, the route to it, and the rule
  // whose result record names both.
  auto result = Apply({
      Op::Upsert("mt/meters", bess::dataplane::EncodeKey(MeterId(1)),
                 std::any(bess::meter::MeterProfileSpec{SlowMeter(100)})),
      rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7)),
      rt->router()->SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(1)),
      Op::Upsert("em/rules", Key(session_addr, 0),
                 Rule(0, 1, 1)),
  });
  ASSERT_EQ(result.outcome, Outcome::kApplied)
      << (result.ops.empty() ? "" : result.ops[0].error);

  // The ledger counts what each value names: the rule names the meter and the
  // route and the rule both name the next hop.
  EXPECT_EQ(Engine().ReferenceCount("mt/meters",
                                    bess::dataplane::EncodeKey(MeterId(1))),
            1u);
  EXPECT_EQ(Engine().ReferenceCount(
                "rt/next_hops", bess::dataplane::EncodeKey(NextHopId(1))),
            2u);  // the route and the rule
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
    std::array<uint32_t, 3> records;
    std::array<gate_idx_t, 3> gates;
    em->ClassifyBatch(&batch, gates.data(), records.data());
    EXPECT_EQ(gates[0], 0u);     // the rule's own gate
    EXPECT_NE(records[0], 0u);   // its result record
    EXPECT_EQ(records[1], 0u);   // miss
    EXPECT_EQ(records[2], 0u);
  }

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
// writer and a reader. The classifier's result field and the meter name the
// same attribute, and neither module knows the other exists.
TEST_F(SessionPipelineTest, ConfiguredAttributeNamesAreTheOnesRegistered) {
  ExactMatch *em = CreateExactMatch("em", "flow/meters", "rt/next_hops",
                                    "flow_meter_id");
  Meter *flow = CreateMeterModule("flow", 64, "flow_meter_id");
  Router *rt = CreateRouterModule("rt");
  ASSERT_NE(nullptr, em);
  ASSERT_NE(nullptr, flow);
  ASSERT_NE(nullptr, rt);

  const auto &meter_attrs = flow->all_attrs();
  ASSERT_EQ(1u, meter_attrs.size());
  EXPECT_EQ("flow_meter_id", meter_attrs[0].name);
  EXPECT_EQ(bess::metadata::Attribute::AccessMode::kRead, meter_attrs[0].mode);

  bool wrote_meter = false;
  bool wrote_hop = false;
  for (const auto &attr : em->all_attrs()) {
    if (attr.name == "flow_meter_id") {
      wrote_meter = true;
      EXPECT_EQ(bess::metadata::Attribute::AccessMode::kWrite, attr.mode);
    }
    if (attr.name == "next_hop_id") {
      wrote_hop = true;
      EXPECT_EQ(bess::metadata::Attribute::AccessMode::kWrite, attr.mode);
    }
    EXPECT_NE("meter_id", attr.name) << "the default name was not registered";
  }
  EXPECT_TRUE(wrote_meter);
  EXPECT_TRUE(wrote_hop);
}

// A transaction that would leave a dangling reference, or a value that names
// nothing, is rejected with nothing visible.
TEST_F(SessionPipelineTest, RejectionsLeaveNothingChanged) {
  Meter *mt = CreateMeterModule("mt", 64);
  ASSERT_NE(nullptr, mt);
  Router *rt = CreateRouterModule("rt");
  ASSERT_NE(nullptr, rt);
  ExactMatch *em = CreateExactMatch("em", "mt/meters", "rt/next_hops");
  ASSERT_NE(nullptr, em);

  // A result field that declares a reference is checked like any referrer:
  // a rule naming a meter and a next hop that do not exist is refused
  // (D-172).
  auto r = Apply({Op::Upsert("em/rules", Key(Ip(10, 1, 2, 3), 0),
                             Rule(0, 7, 7))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("references missing"), std::string::npos)
      << r.ops[0].error;

  // Establish the session, then try to remove what it names.
  ASSERT_EQ(Apply({Op::Upsert("mt/meters",
                              bess::dataplane::EncodeKey(MeterId(1)),
                              std::any(bess::meter::MeterProfileSpec{
                                  SlowMeter(1000)})),
                   rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7)),
                   Op::Upsert("em/rules", Key(Ip(10, 1, 2, 3), 0),
                              Rule(0, 1, 1))})
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
  EXPECT_EQ(Engine().ReferenceCount(
                "rt/next_hops", bess::dataplane::EncodeKey(NextHopId(1))),
            1u);

  // A meter profile that rte_meter would refuse is refused here, and the
  // rejected transaction leaves the meter alone.
  r = Apply({Op::Upsert("mt/meters", bess::dataplane::EncodeKey(MeterId(1)),
                        std::any(bess::meter::MeterProfileSpec{
                            bess::meter::TrTcmSpec{1000, 100, 10, 100}}))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("invalid profile"), std::string::npos)
      << r.ops[0].error;
}

// Removing a session: the rule, the route, the next hop and the meter go in
// one transaction. An erased meter stays readable -- and its id unpublishable
// -- until the removal cascade has run, which a reader that has not reported
// quiescence holds off.
TEST_F(SessionPipelineTest, RemovalCascadeAndIdReuse) {
  Meter *mt = CreateMeterModule("mt", 64);
  Router *rt = CreateRoutingRouterModule("rt");
  ExactMatch *em = CreateExactMatch("em", "mt/meters", "rt/next_hops");
  ASSERT_NE(nullptr, em);
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);

  ASSERT_EQ(Apply({Op::Upsert("mt/meters",
                              bess::dataplane::EncodeKey(MeterId(1)),
                              std::any(bess::meter::MeterProfileSpec{
                                  SlowMeter(1000)})),
                   rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7)),
                   rt->router()->SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(1)),
                   Op::Upsert("em/rules", Key(Ip(10, 1, 2, 3), 0),
                              Rule(0, 1, 1))})
                .outcome,
            Outcome::kApplied);

  // A reader that never reports quiescence: the removal cascade cannot run, so
  // what an erase leaves behind is observable.
  bess::rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  constexpr bess::rcu::ReaderId kReader = 25;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  domain.Online(kReader);

  ASSERT_EQ(Apply({Op::Erase("em/rules", Key(Ip(10, 1, 2, 3), 0)),
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
  // ...while a reader may still resolve what it looked up before: the meter
  // object stays readable until the cascade reaches it.
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
  ASSERT_EQ(Apply({Op::Upsert("mt/meters",
                              bess::dataplane::EncodeKey(MeterId(1)),
                              std::any(bess::meter::MeterProfileSpec{
                                  SlowMeter(1000)})),
                   rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(9)),
                   rt->router()->SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(1)),
                   Op::Upsert("em/rules", Key(Ip(10, 1, 2, 3), 0),
                              Rule(0, 1, 1))})
                .outcome,
            Outcome::kApplied);
  ASSERT_NE(nullptr, rt->router()->Resolve(Ip(10, 9, 9, 9)));
  EXPECT_EQ(rt->router()->Resolve(Ip(10, 9, 9, 9))->egress, bess::dataplane::InterfaceId(9));
}

// Modules are destroyed in name order (the desired-state planner's), which is
// the reverse of the reference graph here: the referents go first. The
// teardown release must make that harmless.
TEST_F(SessionPipelineTest, TeardownIgnoresDestructionOrder) {
  // Names chosen so that DestroyAllModules() destroys the router first, then
  // the meter, then the ExactMatch that names both.
  Router *rt = CreateRouterModule("a_router");
  Meter *mt = CreateMeterModule("b_meter", 16);
  ExactMatch *em = CreateExactMatch("c_em", "b_meter/meters",
                                    "a_router/next_hops");
  ASSERT_NE(nullptr, em);
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);

  ASSERT_EQ(Apply({Op::Upsert("b_meter/meters",
                              bess::dataplane::EncodeKey(MeterId(1)),
                              std::any(bess::meter::MeterProfileSpec{
                                  SlowMeter(1000)})),
                   rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7)),
                   Op::Upsert("c_em/rules", Key(Ip(10, 1, 2, 3), 0),
                              Rule(0, 1, 1))})
                .outcome,
            Outcome::kApplied);
  ModuleGraph::DestroyAllModules();
  EXPECT_TRUE(Engine().ResourceNames().empty())
      << "a resource outlived its module";
}

// A wildcard rule's result can name another resource, so its teardown must
// release the referrers rather than unregistering a resource with live keys.
// Names destroy the router (the referent) before the wildcard table.
TEST_F(SessionPipelineTest, WildcardResultReferencesReleaseAtTeardown) {
  Router *rt = CreateRouterModule("a_router");
  bess::pb::WildcardMatchArg wm_arg;
  auto *wf = wm_arg.add_fields();
  wf->set_offset(26);
  wf->set_num_bytes(4);
  auto *field = wm_arg.add_result_fields();
  field->set_attr_name("next_hop_id");
  field->set_value_offset(0);
  field->set_num_bytes(4);
  field->set_reference("a_router/next_hops");
  field->set_reference_key(bess::pb::ResultField::BE32);
  auto *wm = static_cast<WildcardMatch *>(
      Create("WildcardMatch", "b_wm", Pack(wm_arg)));
  ASSERT_NE(nullptr, rt);
  ASSERT_NE(nullptr, wm);

  const auto ip_bytes = [](const char *ip) {
    const auto bytes = bess::testing::Ip4(ip);
    return std::string(bytes.begin(), bytes.end());
  };
  const bess::utils::be32_t hop_id(1);
  const std::vector<std::byte> record(
      reinterpret_cast<const std::byte *>(&hop_id),
      reinterpret_cast<const std::byte *>(&hop_id) + sizeof(hop_id));
  ASSERT_EQ(Apply({rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7)),
                   Op::Upsert("b_wm/rules",
                              ip_bytes("255.255.255.0") +
                                  ip_bytes("10.1.0.0"),
                              std::any(bess::classifier::MaskedRuleResource::
                                           Value{1, 0, record}))})
                .outcome,
            Outcome::kApplied);
  const auto hop_key = bess::dataplane::EncodeKey(NextHopId(1));
  EXPECT_EQ(Engine().ReferenceCount("a_router/next_hops", hop_key), 1u);

  ModuleGraph::DestroyAllModules();
  EXPECT_TRUE(Engine().ResourceNames().empty())
      << "a resource outlived its module";
}

TEST_F(SessionPipelineTest, RouterCodecRejectsOutOfRangePrefix) {
  Router *rt = CreateRoutingRouterModule("rt");
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
  Router *rt = CreateRoutingRouterModule("rt");
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

// A rule's value is a gate and a result record; every combination is legal --
// a gate alone, a record alone, both, or neither. A miss delivers the default
// gate and no record.
TEST_F(SessionPipelineTest, RuleResultCarriesGateAndRecordTogether) {
  Meter *mt = CreateMeterModule("mt", 64);
  Router *rt = CreateRouterModule("rt");
  ExactMatch *em = CreateExactMatch("em", "mt/meters", "rt/next_hops");
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);
  ASSERT_NE(nullptr, em);
  ASSERT_EQ(Apply({Op::Upsert("mt/meters",
                              bess::dataplane::EncodeKey(MeterId(1)),
                              std::any(bess::meter::MeterProfileSpec{
                                  SlowMeter(1000)})),
                   rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7))})
                .outcome,
            Outcome::kApplied);
  auto ruled = Apply({Op::Upsert("em/rules", Key(Ip(10, 0, 0, 1), 0),
                              Rule(4, 1, 1)),
                   Op::Upsert("em/rules", Key(Ip(10, 0, 0, 2), 0),
                              std::any(bess::classifier::RuleResult{7, {}})),
                   Op::Upsert("em/rules", Key(Ip(10, 0, 0, 3), 0),
                              Rule(0, 1, 1))});
  ASSERT_EQ(ruled.outcome, Outcome::kApplied)
      << "ops=" << ruled.ops.size() << " err0="
      << (ruled.ops.size() > 0 ? ruled.ops[0].error : "none")
      << " err1=" << (ruled.ops.size() > 1 ? ruled.ops[1].error : "none")
      << " err2=" << (ruled.ops.size() > 2 ? ruled.ops[2].error : "none");

  Packets packets(4);
  packets.SetKey(0, Ip(10, 0, 0, 1), 0);
  packets.SetKey(1, Ip(10, 0, 0, 2), 0);
  packets.SetKey(2, Ip(10, 0, 0, 3), 0);
  packets.SetKey(3, Ip(192, 0, 2, 1), 0);
  bess::PacketBatch batch = packets.Batch(0, 4);
  std::array<gate_idx_t, 4> gates;
  std::array<uint32_t, 4> records;
  em->ClassifyBatch(&batch, gates.data(), records.data());

  EXPECT_EQ(gates[0], 4u) << "a rule with both takes its own gate";
  EXPECT_NE(records[0], 0u) << "and its record";
  EXPECT_EQ(gates[1], 7u) << "a rule with a gate and no result";
  EXPECT_NE(records[1], 0u) << "the empty result is a zero-padded record";
  EXPECT_EQ(gates[2], 0u) << "a rule with a record and no gate of its own";
  EXPECT_NE(records[2], 0u) << "and its record";
  EXPECT_EQ(gates[3], DROP_GATE) << "a miss takes the default gate";
  EXPECT_EQ(records[3], 0u) << "and has no record";
}

// Replacing a rule's result moves the reference ledger: the old referent is
// released, the new one is acquired, and only the new one is protected.
TEST_F(SessionPipelineTest, ReferenceLedgerFollowsRuleReplacement) {
  Meter *mt = CreateMeterModule("mt", 64);
  Router *rt = CreateRouterModule("rt");
  ExactMatch *em = CreateExactMatch("em", "mt/meters", "rt/next_hops");
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);
  ASSERT_NE(nullptr, em);
  ASSERT_EQ(Apply({Op::Upsert("mt/meters",
                              bess::dataplane::EncodeKey(MeterId(1)),
                              std::any(bess::meter::MeterProfileSpec{
                                  SlowMeter(1000)})),
                   Op::Upsert("mt/meters",
                              bess::dataplane::EncodeKey(MeterId(2)),
                              std::any(bess::meter::MeterProfileSpec{
                                  SlowMeter(1000)})),
                   rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7)),
                   rt->router()->SetNextHopOp(NextHopId(2), HopWithEgress(8))})
                .outcome,
            Outcome::kApplied);
  const std::string key = Key(Ip(10, 0, 0, 1), 0);
  const auto meter1 = bess::dataplane::EncodeKey(MeterId(1));
  const auto meter2 = bess::dataplane::EncodeKey(MeterId(2));
  const auto hop1 = bess::dataplane::EncodeKey(NextHopId(1));
  const auto hop2 = bess::dataplane::EncodeKey(NextHopId(2));

  ASSERT_EQ(Apply({Op::Upsert("em/rules", key, Rule(0, 1, 1))}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(Engine().ReferenceCount("mt/meters", meter1), 1u);
  EXPECT_EQ(Engine().ReferenceCount("rt/next_hops", hop1), 1u);
  ASSERT_EQ(Apply({Op::Upsert("em/rules", key, Rule(0, 2, 2))}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(Engine().ReferenceCount("mt/meters", meter1), 0u);
  EXPECT_EQ(Engine().ReferenceCount("rt/next_hops", hop1), 0u);
  EXPECT_EQ(Engine().ReferenceCount("mt/meters", meter2), 1u);
  EXPECT_EQ(Engine().ReferenceCount("rt/next_hops", hop2), 1u);
  EXPECT_EQ(Apply({Op::Erase("mt/meters", meter1)}).outcome, Outcome::kApplied);
  auto refused = Apply({Op::Erase("mt/meters", meter2)});
  ASSERT_EQ(refused.outcome, Outcome::kRejected);
  EXPECT_NE(refused.ops[0].error.find("still referenced"), std::string::npos);
  ASSERT_EQ(Apply({Op::Erase("em/rules", key)}).outcome, Outcome::kApplied);
  EXPECT_EQ(Apply({Op::Erase("mt/meters", meter2)}).outcome, Outcome::kApplied);
}

// A two-level meter's 8-byte result is two BE32 ids; a field that carries
// several ids names each of them (reference_count), so both levels are in the
// ledger and neither can be erased while the rule remains.
TEST_F(SessionPipelineTest, AMultiLevelMeterFieldReferencesEveryLevel) {
  bess::pb::ExactMatchArg arg;
  auto *f = arg.add_fields();
  f->set_offset(26);
  f->set_num_bytes(4);
  auto *meter = arg.add_result_fields();
  meter->set_attr_name("meter_id");
  meter->set_value_offset(0);
  meter->set_num_bytes(8);
  meter->set_reference("mt/meters");
  meter->set_reference_key(bess::pb::ResultField::BE32);
  meter->set_reference_count(2);
  auto *hop = arg.add_result_fields();
  hop->set_attr_name("next_hop_id");
  hop->set_value_offset(8);
  hop->set_num_bytes(4);
  hop->set_reference("rt/next_hops");
  hop->set_reference_key(bess::pb::ResultField::BE32);
  auto *em = static_cast<ExactMatch *>(Create("ExactMatch", "em", Pack(arg)));
  Meter *mt = CreateMeterModule("mt", 64, "", 2);
  Router *rt = CreateRouterModule("rt");
  ASSERT_NE(nullptr, em);
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);

  // The meter's two levels, the next hop, and the rule naming all three.
  std::string record = SessionRecord(1, 2);  // both meter levels
  const bess::utils::be32_t hop_bytes(1);
  record.append(reinterpret_cast<const char *>(&hop_bytes), sizeof(hop_bytes));
  auto applied = Apply({Op::Upsert("mt/meters",
                                   bess::dataplane::EncodeKey(MeterId(1)),
                                   std::any(bess::meter::MeterProfileSpec{
                                       SlowMeter(1000)})),
                        Op::Upsert("mt/meters",
                                   bess::dataplane::EncodeKey(MeterId(2)),
                                   std::any(bess::meter::MeterProfileSpec{
                                       SlowMeter(1000)})),
                        rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7)),
                        Op::Upsert("em/rules",
                                   Key(Ip(10, 0, 0, 1), 0).substr(0, 4),
                                   RuleWithBytes(record))});
  std::string errs;
  for (const auto &op : applied.ops) {
    errs += op.error + "; ";
  }
  ASSERT_EQ(applied.outcome, Outcome::kApplied) << errs;
  const auto meter1 = bess::dataplane::EncodeKey(MeterId(1));
  const auto meter2 = bess::dataplane::EncodeKey(MeterId(2));
  const auto hop1 = bess::dataplane::EncodeKey(NextHopId(1));
  EXPECT_EQ(Engine().ReferenceCount("mt/meters", meter1), 1u);
  EXPECT_EQ(Engine().ReferenceCount("mt/meters", meter2), 1u);
  EXPECT_EQ(Engine().ReferenceCount("rt/next_hops", hop1), 1u);

  for (const auto &op : {Op::Erase("mt/meters", meter1),
                         Op::Erase("mt/meters", meter2)}) {
    auto refused = Apply({op});
    ASSERT_EQ(refused.outcome, Outcome::kRejected);
    EXPECT_NE(refused.ops[0].error.find("still referenced"), std::string::npos);
  }
  auto refused = Apply({rt->router()->RemoveNextHopOp(NextHopId(1))});
  ASSERT_EQ(refused.outcome, Outcome::kRejected);
  EXPECT_NE(refused.ops[0].error.find("still referenced"), std::string::npos);

  // Both levels are released with the rule.
  ASSERT_EQ(Apply({Op::Erase("em/rules",
                             Key(Ip(10, 0, 0, 1), 0).substr(0, 4))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(Apply({Op::Erase("mt/meters", meter1)}).outcome, Outcome::kApplied);
  EXPECT_EQ(Apply({Op::Erase("mt/meters", meter2)}).outcome, Outcome::kApplied);
  EXPECT_EQ(Apply({rt->router()->RemoveNextHopOp(NextHopId(1))}).outcome,
            Outcome::kApplied);
}

// RAW references use the field bytes as the target key, so a resource keyed by
// a host-order id is named by EncodeKey(id) rather than a BE32 wire id.
TEST_F(SessionPipelineTest, RawReferenceNamesTheKeyVerbatim) {
  Router *rt = CreateRouterModule("rt_raw");
  ASSERT_NE(nullptr, rt);
  bess::pb::ExactMatchArg arg;
  auto *f = arg.add_fields();
  f->set_offset(26);
  f->set_num_bytes(4);
  auto *field = arg.add_result_fields();
  field->set_offset(40);
  field->set_value_offset(0);
  field->set_num_bytes(4);
  field->set_reference("rt_raw/next_hops");  // reference_key defaults RAW
  auto *em = static_cast<ExactMatch *>(Create("ExactMatch", "em_raw", Pack(arg)));
  ASSERT_NE(nullptr, em);

  const std::string hop = bess::dataplane::EncodeKey(NextHopId(1));
  const uint32_t addr = Ip(10, 0, 0, 1);
  const std::string key(reinterpret_cast<const char *>(&addr), sizeof(addr));
  auto r = Apply({rt->router()->SetNextHopOp(NextHopId(1), HopWithEgress(7)),
                  Op::Upsert("em_raw/rules", key, RuleWithBytes(hop))});
  std::string errs;
  for (const auto &op : r.ops) {
    errs += op.error + "; ";
  }
  ASSERT_EQ(r.outcome, Outcome::kApplied) << errs;
  EXPECT_EQ(Engine().ReferenceCount("rt_raw/next_hops", hop), 1u);
  EXPECT_EQ(Apply({rt->router()->RemoveNextHopOp(NextHopId(1))}).outcome,
            Outcome::kRejected);
}

// A BE32 field carries reference_count consecutive 4-byte ids; that count is
// what decides how many entries the rule's result names.
TEST_F(SessionPipelineTest, ResultReferenceCountIsValidated) {
  const auto make = [](uint32_t num_bytes, uint32_t reference_count,
                       bess::pb::ResultField::ReferenceKey key) {
    bess::pb::ExactMatchArg arg;
    auto *f = arg.add_fields();
    f->set_offset(26);
    f->set_num_bytes(4);
    auto *r = arg.add_result_fields();
    r->set_offset(40);
    r->set_value_offset(0);
    r->set_num_bytes(num_bytes);
    r->set_reference(key == bess::pb::ResultField::BE32 ? "mt/meters" : "");
    r->set_reference_key(key);
    r->set_reference_count(reference_count);
    return Pack(arg);
  };
  const auto create = [](const google::protobuf::Any &arg, pb_error_t *error) {
    return ModuleGraph::CreateModule(
        ModuleBuilder::all_module_builders().at("ExactMatch"), "em", arg,
        error);
  };

  pb_error_t error;
  EXPECT_NE(nullptr, create(make(8, 2, bess::pb::ResultField::BE32), &error));
  ModuleGraph::DestroyAllModules();

  EXPECT_EQ(nullptr, create(make(8, 1, bess::pb::ResultField::BE32), &error));
  EXPECT_EQ(EINVAL, error.code());
  EXPECT_EQ(nullptr, create(make(8, 0, bess::pb::ResultField::BE32), &error));
  EXPECT_EQ(EINVAL, error.code());
  EXPECT_EQ(nullptr, create(make(4, 2, bess::pb::ResultField::RAW), &error));
  EXPECT_EQ(EINVAL, error.code());
}

TEST_F(SessionPipelineTest, FusedKernelMatchesTheGenericPathWithResultFields) {
  // One 4-byte field; the result record is placed at packet offset 40, so a
  // hit has a record without naming any module resource.
  bess::pb::ExactMatchArg arg;
  auto *f = arg.add_fields();
  f->set_offset(26);
  f->set_num_bytes(4);
  auto *result = arg.add_result_fields();
  result->set_offset(40);
  result->set_value_offset(0);
  result->set_num_bytes(4);
  auto *em = static_cast<ExactMatch *>(Create("ExactMatch", "em", Pack(arg)));
  ASSERT_NE(nullptr, em);
  ASSERT_EQ(4u, em->fused_key_width());
  for (uint32_t k = 0; k < 16; k++) {
    // One 4-byte field: the key is that field's bytes.
    ASSERT_EQ(Apply({Op::Upsert("em/rules", Key(Ip(10, 0, 0, k), 0).substr(0, 4),
                                std::any(bess::classifier::RuleResult{
                                    0, std::vector<std::byte>(
                                           4, std::byte{static_cast<uint8_t>(k)})}))})
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
    const bool matched = i < 24 && i % 4 != 0;
    EXPECT_EQ(matched, fused[i] != 0) << "packet " << i;
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
  bess::pb::RouterArg router;
  router.set_mode(bess::pb::ROUTE_MODE_DESTINATION);
  router.set_max_ipv4_routes(uint64_t{1} << 32);
  rejected("Router", router);
}
TEST_F(SessionPipelineTest, CodecsRejectMismatchedFieldsAndUnknownEnums) {
  // Gate-only: no result fields, so the schema's record is zero bytes.
  ExactMatch *em = CreateExactMatch("em");
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

  // A gate and an empty result are accepted; a result wider than the schema's
  // zero-byte record is refused.
  em_val.set_gate(4);
  const google::protobuf::Any packed_gate = Pack(em_val);
  EXPECT_TRUE(em_codec
                  ->Value(packed_gate.type_url(), packed_gate.value())
                  .has_value());
  em_val.set_result(std::string(4, '\0'));
  const google::protobuf::Any packed_result = Pack(em_val);
  EXPECT_FALSE(em_codec
                   ->Value(packed_result.type_url(), packed_result.value())
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
// em:0 -> mt, mt:0 (green) -> rt, packets run through the real module graph;
// yellow and red are left unconnected, so those packets are dropped -- the
// policy is the graph.

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
  // The three modules, created referrer-first (the ExactMatch names resources
  // of modules that do not exist yet), connected.
  void SetUp() override {
    em_ = CreateExactMatch("em", "mt/meters", "rt/next_hops");
    mt_ = CreateMeterModule("mt", 64);
    rt_ = CreateRouterModule("rt");
    ASSERT_NE(nullptr, em_);
    ASSERT_NE(nullptr, mt_);
    ASSERT_NE(nullptr, rt_);
    // No default hooks (Track counts per worker; the harness is not one).
    ASSERT_EQ(0, ModuleGraph::ConnectModules(em_, 0, mt_, 0, true));
    ASSERT_EQ(0, ModuleGraph::ConnectModules(mt_, Meter::kGreenGate, rt_, 0, true));
  }

  // One session: the meter (`burst` bytes, no refill), the next hop to
  // kEgressGate, and the rule naming both. The router forwards on the id the
  // rule writes; it holds no routes (D-183).
  std::vector<Op> SessionOps(const char *sip, uint16_t sport, uint64_t burst) {
    return {
        Op::Upsert("mt/meters", bess::dataplane::EncodeKey(MeterId(1)),
                   std::any(bess::meter::MeterProfileSpec{SlowMeter(burst)})),
        rt_->router()->SetNextHopOp(
            NextHopId(1), HopWithEgress(bess::modules::router::InterfaceOf(kEgressGate).value())),
        Op::Upsert("em/rules", WireKey(sip, sport), Rule(0, 1, 1)),
    };
  }

  // What leaves the router's egress gate for `pkts` sent into the ExactMatch.
  std::vector<bess::testing::Bytes> Egress(const std::vector<bess::testing::Bytes> &pkts) {
    return h_.RunPipeline(em_, 0, pkts, rt_, {kEgressGate})[kEgressGate];
  }

  bess::testing::ModuleHarness h_;
  ExactMatch *em_ = nullptr;
  Meter *mt_ = nullptr;
  Router *rt_ = nullptr;
  std::mt19937 rng_{7};
};

// One transaction creates the whole session; two 60-byte packets of it: the
// meter's 100-byte buckets take the first (green -> the router -> the next
// hop's egress, unchanged) and not the second (red -> dropped).
TEST_F(SessionPacketPathTest, SessionTransactionSteersAndPolices) {
  for (const char *name : {"em/rules", "mt/meters", "rt/next_hops", "rt/routes"}) {
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
// (drop); a packet whose record names no next hop is dropped at the router.
TEST_F(SessionPacketPathTest, OtherSessionsAndMissesAreDropped) {
  auto r = Apply(SessionOps("10.1.2.3", 1234, 100));
  ASSERT_EQ(Outcome::kApplied, r.outcome) << (r.ops.empty() ? "" : r.ops[0].error);
  r = Apply({
      Op::Upsert("em/rules", WireKey("10.4.4.4", 4321), Rule(0, 0, 0)),
  });
  ASSERT_EQ(Outcome::kApplied, r.outcome) << (r.ops.empty() ? "" : r.ops[0].error);

  const auto miss = SessionPacket("192.0.2.1", 1234, rng_);
  const auto other = SessionPacket("10.4.4.4", 4321, rng_);
  EXPECT_TRUE(Egress({miss, other}).empty());
  // The session's own packet still goes through.
  const auto own = SessionPacket("10.1.2.3", 1234, rng_);
  EXPECT_EQ((std::vector<bess::testing::Bytes>{Forwarded(own)}), Egress({own}));
}

// Removing the session -- the rule, the next hop and the meter in one
// transaction -- stops its packets. (That the meter alone may not go while the
// rule names it is RejectionsLeaveNothingChanged.)
TEST_F(SessionPacketPathTest, RemovedSessionIsDropped) {
  // 1000-byte buckets: the meter would pass both packets.
  auto r = Apply(SessionOps("10.1.2.3", 1234, 1000));
  ASSERT_EQ(Outcome::kApplied, r.outcome) << (r.ops.empty() ? "" : r.ops[0].error);
  const auto pkt = SessionPacket("10.1.2.3", 1234, rng_);
  ASSERT_EQ((std::vector<bess::testing::Bytes>{Forwarded(pkt)}), Egress({pkt}));

  r = Apply({
      Op::Erase("em/rules", WireKey("10.1.2.3", 1234)),
      rt_->router()->RemoveNextHopOp(NextHopId(1)),
      Op::Erase("mt/meters", bess::dataplane::EncodeKey(MeterId(1))),
  });
  ASSERT_EQ(Outcome::kApplied, r.outcome) << (r.ops.empty() ? "" : r.ops[0].error);
  Settle();
  EXPECT_TRUE(Egress({pkt}).empty());
}

// A controller composition, not a module dependency: WildcardMatch writes a
// class id into metadata and a second ExactMatch matches it. The two
// classifiers share no resource and neither knows the other exists; the class
// field is the whole contract between them.
TEST_F(SessionPipelineTest, AClassFieldComposesTwoClassifiers) {
  const auto class_key = [](uint32_t id) {
    const bess::utils::be32_t bytes(id);
    return std::string(reinterpret_cast<const char *>(&bytes), sizeof(bytes));
  };
  const auto class_bytes = [&](uint32_t id) {
    const std::string bytes = class_key(id);
    return std::vector<std::byte>(
        reinterpret_cast<const std::byte *>(bytes.data()),
        reinterpret_cast<const std::byte *>(bytes.data()) + bytes.size());
  };
  const auto field = [](const char *ip) {
    const auto bytes = bess::testing::Ip4(ip);
    return std::string(bytes.begin(), bytes.end());
  };

  bess::pb::WildcardMatchArg wm_arg;
  auto *wf = wm_arg.add_fields();
  wf->set_offset(26);
  wf->set_num_bytes(4);
  auto *class_field = wm_arg.add_result_fields();
  class_field->set_attr_name("class_id");
  class_field->set_value_offset(0);
  class_field->set_num_bytes(4);
  auto *wm = static_cast<WildcardMatch *>(
      Create("WildcardMatch", "wm", Pack(wm_arg)));
  ASSERT_NE(nullptr, wm);

  // The second classifier matches the class id the first one wrote.
  bess::pb::ExactMatchArg em_arg;
  auto *ef = em_arg.add_fields();
  ef->set_attr_name("class_id");
  ef->set_num_bytes(4);
  AddSessionResultFields(&em_arg, "mt/meters", "rt/next_hops", "meter_id",
                         "next_hop_id");
  auto *em = static_cast<ExactMatch *>(Create("ExactMatch", "em", Pack(em_arg)));
  Meter *mt = CreateMeterModule("mt", 64);
  Router *rt = CreateRouterModule("rt");
  ASSERT_NE(nullptr, em);
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(wm, 0, em, 0, true));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(em, 0, mt, 0, true));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(mt, Meter::kGreenGate, rt, 0, true));

  auto r = Apply({
      Op::Upsert("mt/meters", bess::dataplane::EncodeKey(MeterId(1)),
                 std::any(bess::meter::MeterProfileSpec{SlowMeter(1000)})),
      Op::Upsert("mt/meters", bess::dataplane::EncodeKey(MeterId(2)),
                 std::any(bess::meter::MeterProfileSpec{SlowMeter(1000)})),
      rt->router()->SetNextHopOp(
          NextHopId(1),
          HopWithEgress(bess::modules::router::InterfaceOf(7).value())),
      rt->router()->SetNextHopOp(
          NextHopId(2),
          HopWithEgress(bess::modules::router::InterfaceOf(8).value())),
      // Class 1 for 10.1.0.0/16, class 2 for 10.2.0.0/16: the key is the
      // packed mask then the packed value.
      Op::Upsert("wm/rules",
                 field("255.255.0.0") + field("10.1.0.0"),
                 std::any(bess::classifier::MaskedRuleResource::Value{
                     1, 0, class_bytes(1)})),
      Op::Upsert("wm/rules",
                 field("255.255.0.0") + field("10.2.0.0"),
                 std::any(bess::classifier::MaskedRuleResource::Value{
                     2, 0, class_bytes(2)})),
      Op::Upsert("em/rules", class_key(1), Rule(0, 1, 1)),
      Op::Upsert("em/rules", class_key(2), Rule(0, 2, 2)),
  });
  ASSERT_EQ(Outcome::kApplied, r.outcome)
      << (r.ops.empty() ? "" : r.ops[0].error);

  std::mt19937 rng{std::random_device{}()};
  const auto first = SessionPacket("10.1.2.3", 1234, rng);
  const auto second = SessionPacket("10.2.3.4", 1234, rng);
  bess::testing::ModuleHarness h;
  auto out = h.RunPipeline(wm, 0, {first, second}, rt, {7, 8});
  ASSERT_EQ(1u, out[7].size()) << "class 1 selected next hop 1";
  ASSERT_EQ(1u, out[8].size()) << "class 2 selected next hop 2";
  EXPECT_EQ(Forwarded(first), out[7][0]);
  EXPECT_EQ(Forwarded(second), out[8][0]);
}

// -- D-129: the resources as a client writes and reads them --------------------

// One session as wire messages: the meter (`burst` bytes, no refill), the
// next hop to kEgressGate, and the rule whose result record names both.
struct WireSession {
  bess::pb::MeterIdKey meter_key;
  bess::pb::MeterPolicyValue meter;
  bess::pb::RouterNextHopIdKey hop_key;
  bess::pb::RouterNextHopValue hop;
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
  const std::string key = WireKey(sip, sport);
  s.rule_key.add_fields()->set_value_bin(key.substr(0, 4));
  s.rule_key.add_fields()->set_value_bin(key.substr(4));
  s.rule.set_result(SessionRecord(1, 1));
  return s;
}

// The session written as a client writes it steers and polices its packets
// as the internal operations do, and every resource reads back exactly what
// was upserted -- the rule's references to the meter and the next hop
// included.
TEST_F(SessionPacketPathTest, WireSessionSteersPolicesAndReadsBack) {
  const WireSession s = MakeWireSession("10.1.2.3", 1234, 100);
  // Referents first: each call is its own transaction.
  ASSERT_EQ("", h_.UpsertResource("mt/meters", s.meter_key, s.meter));
  ASSERT_EQ("", h_.UpsertResource("rt/next_hops", s.hop_key, s.hop));
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
}

// Every meter profile, neighbor state and the default route read back as
// written; a reconfiguration reads back the new value, and an erased entry is
// gone from the read-back at once -- also a meter, whose removal stage has not
// run yet. Routes live in a destination-mode router's FIB.
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
  Router *dst = CreateRoutingRouterModule("rt_dst");
  ASSERT_NE(nullptr, dst);
  ASSERT_EQ("", h_.UpsertResource("rt_dst/next_hops", h1, resolved));
  ASSERT_EQ("", h_.UpsertResource("rt_dst/next_hops", h2, incomplete));
  ASSERT_EQ("", h_.UpsertResource("rt_dst/next_hops", h3, unreachable));

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
  ASSERT_EQ("", h_.UpsertResource("rt_dst/routes", r1, via1));
  ASSERT_EQ("", h_.UpsertResource("rt_dst/routes", r2, via2));
  ASSERT_EQ("", h_.UpsertResource("rt_dst/routes", r3, via3));

  EXPECT_EQ("", h_.ReadBackDiffers("mt/meters", {{&m1, &sr}, {&m2, &tr}, {&m3, &rfc}}));
  EXPECT_EQ("", h_.ReadBackDiffers("rt/next_hops",
                                   {{&h1, &resolved}, {&h2, &incomplete}, {&h3, &unreachable}}));
  EXPECT_EQ("", h_.ReadBackDiffers("rt_dst/routes", {{&r1, &via1}, {&r2, &via2}, {&r3, &via3}}));

  // A reconfiguration reads back the new value.
  ASSERT_EQ("", h_.UpsertResource("mt/meters", m1, rfc));
  EXPECT_EQ("", h_.ReadBackDiffers("mt/meters", {{&m1, &rfc}, {&m2, &tr}, {&m3, &rfc}}));

  // Erases, before any removal stage runs.
  ASSERT_EQ("", h_.EraseResource("mt/meters", m3));
  ASSERT_EQ("", h_.EraseResource("rt_dst/routes", r3));
  ASSERT_EQ("", h_.EraseResource("rt/next_hops", h3));
  EXPECT_EQ("", h_.ReadBackDiffers("mt/meters", {{&m1, &rfc}, {&m2, &tr}}));
  EXPECT_EQ("", h_.ReadBackDiffers("rt/next_hops", {{&h1, &resolved}, {&h2, &incomplete}}));
  EXPECT_EQ("", h_.ReadBackDiffers("rt_dst/routes", {{&r1, &via1}, {&r2, &via2}}));
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

  // A next-hop-id router has no FIB: every route write is refused (D-183).
  bess::pb::RouterRouteKey r1;
  r1.set_ipv4("10.0.0.0");
  r1.set_prefix_length(8);
  EXPECT_NE("", h_.UpsertResource("rt/routes", r1, bess::pb::RouterRouteValue{}));
  bess::pb::RouterRouteValue route;
  route.set_next_hop_id(1);
  EXPECT_NE("", h_.UpsertResource("rt/routes", r1, route));

  for (const char *name : {"mt/meters", "rt/next_hops", "rt/routes"}) {
    EXPECT_TRUE(h_.ReadResource(name).empty()) << name;
  }
}

// Each class declares exactly the tables its modules bind (the control plane
// refuses creation otherwise), and none requires paused workers: meters
// publish generations, rules, next hops and groups are slot tables, routes are
// rte_lpm's in-place updates.
TEST_F(SessionPacketPathTest, DeclaredTablesMatchTheBoundCodecs) {
  const struct {
    const char *class_name;
    const char *table;
    const char *resource;
  } tables[] = {
      {"ExactMatch", "rules", "em/rules"},
      {"Meter", "meters", "mt/meters"},
      {"Router", "next_hops", "rt/next_hops"},
      {"Router", "groups", "rt/groups"},
      {"Router", "routes", "rt/routes"},
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
  EXPECT_EQ(4u, ModuleBuilder::all_module_builders().at("Router").resource_tables().size())
      << "next_hops, neighbors, groups and routes";
}

// The configured names are the contract, not the registration: the graph
// resolves one offset for a writer's name and its reader's, and the packet
// path is the proof. SessionPacketPathTest runs the defaults.
TEST_F(SessionPipelineTest, ConfiguredNamesResolveToOneOffset) {
  ExactMatch *em = CreateExactMatch("em_cfg", "mt_cfg/meters",
                                    "rt_cfg/next_hops", "flow_meter_id");
  Meter *mt = CreateMeterModule("mt_cfg", 64, "flow_meter_id");
  Router *rt = CreateRouterModule("rt_cfg");
  ASSERT_NE(nullptr, em);
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(em, 0, mt, 0, true));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(mt, Meter::kGreenGate, rt, 0, true));

  auto r = Apply({
      Op::Upsert("mt_cfg/meters", bess::dataplane::EncodeKey(MeterId(1)),
                 std::any(bess::meter::MeterProfileSpec{SlowMeter(100)})),
      rt->router()->SetNextHopOp(
          NextHopId(1),
          HopWithEgress(bess::modules::router::InterfaceOf(kEgressGate).value())),
      Op::Upsert("em_cfg/rules", WireKey("10.1.2.3", 1234),
                 Rule(0, 1, 1)),
  });
  ASSERT_EQ(Outcome::kApplied, r.outcome)
      << [&] {
           std::string all;
           for (const auto &o : r.ops) { all += o.error; all += " | "; }
           return all;
         }();

  // The packet traverses every module: the rule's result writes the meter id
  // under the configured name, the meter reads that name, and the router
  // forwards. Two packets against a 100-byte burst: the first is green, the
  // second red. Had the meter not read the id the rule wrote, it would read 0
  // ("unmetered") and pass both; the single egress frame is the proof.
  std::random_device rd;
  std::mt19937 rng{rd()};
  const auto pkt1 = SessionPacket("10.1.2.3", 1234, rng);
  const auto pkt2 = SessionPacket("10.1.2.3", 1234, rng);
  bess::testing::ModuleHarness h;
  EXPECT_EQ((std::vector<bess::testing::Bytes>{Forwarded(pkt1)}),
            h.RunPipeline(em, 0, {pkt1, pkt2}, rt, {kEgressGate})[kEgressGate]);

  // The meter's read and the rule's write resolved to one offset: the names,
  // not the strings that registered them, are the contract.
  EXPECT_EQ(mt->attr_offset(AttrIndex(mt, "flow_meter_id")),
            em->attr_offset(AttrIndex(em, "flow_meter_id")));
}

// A custom meter and next-hop name need a writer and readers configured to
// match: the rule writes "flow_meter_id" and "hop_id", and the meter and the
// router read those. Defaults are covered by SessionPacketPathTest.
TEST_F(SessionPipelineTest, ConfiguredNextHopNamePairsWithItsReader) {
  ExactMatch *em = CreateExactMatch("em_names", "mt_names/meters",
                                    "rt_names/next_hops", "flow_meter_id",
                                    "hop_id");
  Meter *mt = CreateMeterModule("mt_names", 64, "flow_meter_id");
  Router *rt = CreateRouterModule("rt_names", "hop_id");
  ASSERT_NE(nullptr, em);
  ASSERT_NE(nullptr, mt);
  ASSERT_NE(nullptr, rt);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(em, 0, mt, 0, true));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(mt, Meter::kGreenGate, rt, 0, true));

  auto r = Apply({
      Op::Upsert("mt_names/meters", bess::dataplane::EncodeKey(MeterId(1)),
                 std::any(bess::meter::MeterProfileSpec{SlowMeter(1000)})),
      rt->router()->SetNextHopOp(
          NextHopId(1),
          HopWithEgress(bess::modules::router::InterfaceOf(kEgressGate).value())),
      Op::Upsert("em_names/rules", WireKey("10.1.2.3", 1234),
                 Rule(0, 1, 1)),
  });
  ASSERT_EQ(Outcome::kApplied, r.outcome)
      << (r.ops.empty() ? "" : r.ops[0].error);

  std::random_device rd;
  std::mt19937 rng{rd()};
  const auto pkt = SessionPacket("10.1.2.3", 1234, rng);
  bess::testing::ModuleHarness h;
  EXPECT_EQ((std::vector<bess::testing::Bytes>{Forwarded(pkt)}),
            h.RunPipeline(em, 0, {pkt}, rt, {kEgressGate})[kEgressGate]);

  // Each configured name paired with the writer's or the reader's.
  EXPECT_EQ(mt->attr_offset(AttrIndex(mt, "flow_meter_id")),
            em->attr_offset(AttrIndex(em, "flow_meter_id")));
  EXPECT_EQ(rt->attr_offset(AttrIndex(rt, "hop_id")),
            em->attr_offset(AttrIndex(em, "hop_id")));
}

// Yellow survives: a level that marks a packet yellow must not be reset to
// green by the next level (colour-aware checks never improve a packet). The
// second packet empties level 0's committed bucket but fits its peak bucket.
TEST_F(SessionPipelineTest, AYellowLevelIsNotResetByALaterLevel) {
  bess::testing::ModuleHarness h;
  bess::pb::SetMetadataArg md_arg;
  auto *attr = md_arg.add_attrs();
  attr->set_name("meter_id");
  attr->set_size(8);
  // Level 0: the yellow-producing flow meter; level 1: the generous session
  // meter, which must take the yellow input as yellow, not re-colour it green.
  attr->set_value_bin(std::string("\0\0\0\x02\0\0\0\x01", 8));
  Module *md = Create("SetMetadata", "md_yellow", Pack(md_arg));
  Meter *mt = CreateMeterModule("mt_yellow", 64, "", 2);
  ASSERT_NE(nullptr, md);
  ASSERT_NE(nullptr, mt);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(md, 0, mt, 0, true));

  std::random_device rd;
  std::mt19937 rng{rd()};
  const auto first = SessionPacket("10.1.2.3", 1234, rng);
  const auto second = SessionPacket("10.1.2.3", 1234, rng);
  const uint64_t len = first.size();
  auto r = Apply({
      Op::Upsert("mt_yellow/meters", bess::dataplane::EncodeKey(MeterId(1)),
                 std::any(bess::meter::MeterProfileSpec{SlowMeter(1000)})),
      // Committed burst is exactly one packet; the peak bucket holds many, so
      // the second packet in the same batch is yellow (RFC 2698).
      Op::Upsert("mt_yellow/meters", bess::dataplane::EncodeKey(MeterId(2)),
                 std::any(bess::meter::MeterProfileSpec{
                     bess::meter::TrTcmSpec{1, len, 1'000'000, 1000}})),
  });
  ASSERT_EQ(Outcome::kApplied, r.outcome)
      << (r.ops.empty() ? "" : r.ops[0].error);

  auto out = h.RunPipeline(md, 0, {first, second}, mt, {0, 1, 2});
  EXPECT_EQ(1u, out[0].size()) << "the first packet is green at both levels";
  ASSERT_EQ(1u, out[1].size())
      << "the second is yellow at level 0 and stays yellow through level 1";
  EXPECT_EQ(std::vector<bess::testing::Bytes>{second}, out[1]);
  EXPECT_TRUE(out[2].empty());
}

// A zero id is "no meter at this level" and passes; an unknown nonzero id is
// dropped rather than forwarded unmetered, at any level.
TEST_F(SessionPipelineTest, ZeroIdsPassAtEveryLevel) {
  bess::testing::ModuleHarness h;
  Meter *mt = CreateMeterModule("mt_zero", 64, "", 2);
  ASSERT_NE(nullptr, mt);
  bess::pb::SetMetadataArg md_arg;
  auto *attr = md_arg.add_attrs();
  attr->set_name("meter_id");
  attr->set_size(8);
  attr->set_value_bin(std::string("\0\0\0\0\0\0\0\0", 8));
  Module *md = Create("SetMetadata", "md_zero", Pack(md_arg));
  ASSERT_NE(nullptr, md);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(md, 0, mt, 0, true));

  std::random_device rd;
  std::mt19937 rng{rd()};
  auto out = h.RunPipeline(md, 0, {SessionPacket("10.1.2.3", 1234, rng)}, mt,
                           {0, 1, 2});
  EXPECT_EQ(1u, out[0].size()) << "id 0 is no meter at either level";
  EXPECT_TRUE(out[1].empty());
  EXPECT_TRUE(out[2].empty());
}

TEST_F(SessionPipelineTest, AnUnknownIdAboveLevelZeroDropsThePacket) {
  bess::testing::ModuleHarness h;
  Meter *mt = CreateMeterModule("mt_unknown", 64, "", 2);
  ASSERT_NE(nullptr, mt);
  auto r = Apply({
      Op::Upsert("mt_unknown/meters", bess::dataplane::EncodeKey(MeterId(1)),
                 std::any(bess::meter::MeterProfileSpec{SlowMeter(1000)})),
  });
  ASSERT_EQ(Outcome::kApplied, r.outcome)
      << (r.ops.empty() ? "" : r.ops[0].error);

  bess::pb::SetMetadataArg md_arg;
  auto *attr = md_arg.add_attrs();
  attr->set_name("meter_id");
  attr->set_size(8);
  attr->set_value_bin(std::string("\0\0\0\x01\0\0\0\x63", 8));  // 1, 99
  Module *md = Create("SetMetadata", "md_unknown", Pack(md_arg));
  ASSERT_NE(nullptr, md);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(md, 0, mt, 0, true));

  std::random_device rd;
  std::mt19937 rng{rd()};
  auto out = h.RunPipeline(md, 0, {SessionPacket("10.1.2.3", 1234, rng)}, mt,
                           {0, 1, 2});
  EXPECT_TRUE(out[0].empty());
  EXPECT_TRUE(out[1].empty());
  EXPECT_TRUE(out[2].empty()) << "an unknown id at level 1 was forwarded";
}

// The level count is bounded by the size of the one `meter_id` attribute.
TEST_F(SessionPipelineTest, MeterLevelCountIsBounded) {
  bess::testing::ModuleHarness h;
  bess::pb::MeterArg meter_too_many;
  meter_too_many.set_levels(
      static_cast<uint32_t>(bess::metadata::kMetadataAttrMaxSize /
                            sizeof(bess::utils::be32_t)) +
      1);
  EXPECT_NE("", h.CreateError("Meter", meter_too_many));
}

// A two-level record is policed in order: green and yellow keep charging the
// later levels, red stops, and the shared session bucket is not billed for
// what the flow's first level dropped.
TEST_F(SessionPipelineTest, TwoLevelsPoliceInOrderAndARedChargesNoLaterLevel) {
  // The Meter reads two ids from the one 8-byte attribute; a writer names the
  // pair per stream (1,2 then 3,2). 2 is the shared session bucket at both
  // streams' level 2.
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

// Every basis charges exactly the bytes it names. A threshold pair proves the
// number: a committed burst equal to the charge is green, one byte less is
// yellow. L3 parses the version itself and accepts both families; L4 counts
// the transport segment (header + data), so the same basis charges a UDP
// segment 12 bytes and a TCP segment 24 bytes -- it does not parse the
// transport protocol, only the IP header's length.
TEST_F(SessionPipelineTest, TheAccountingBasisChoosesTheChargedBytes) {
  bess::testing::ModuleHarness h;
  bess::testing::PacketBuilder ub;
  const Bytes udp = ub.Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                        .IPv4("10.1.2.3", "10.9.9.9")
                        .Udp(1234, 80)
                        .Payload("abcd")
                        .Build();
  bess::testing::PacketBuilder tb;
  const Bytes tcp = tb.Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                        .IPv4("10.1.2.3", "10.9.9.9")
                        .Tcp(1234, 80)
                        .Payload("abcd")
                        .Build();
  Bytes v6(66, 0);  // 14 Ethernet + 40 IPv6 + 12 bytes of payload
  v6[12] = 0x86;
  v6[13] = 0xdd;
  v6[14] = 0x60;  // version 6
  v6[18] = 0;
  v6[19] = 12;  // payload length

  int next = 0;
  // The gate the one packet leaves on under `basis` and a committed burst of
  // `cbs` bytes.
  const auto gate_for = [&](bess::pb::MeterByteBasis basis, uint32_t off,
                            const Bytes &pkt, uint32_t cbs) -> int {
    const std::string name = "mt_basis_" + std::to_string(next++);
    bess::pb::MeterArg marg;
    marg.set_capacity(16);
    marg.set_byte_basis(basis);
    marg.set_byte_offset(off);
    Meter *mt = static_cast<Meter *>(Create("Meter", name, Pack(marg)));
    EXPECT_NE(nullptr, mt);
    auto r = Apply({
        Op::Upsert(name + "/meters", bess::dataplane::EncodeKey(MeterId(1)),
                   std::any(bess::meter::MeterProfileSpec{
                       bess::meter::TrTcmSpec{1, cbs, 1'000'000, 1000}})),
    });
    EXPECT_EQ(Outcome::kApplied, r.outcome)
        << (r.ops.empty() ? "" : r.ops[0].error);

    bess::pb::SetMetadataArg md;
    auto *a = md.add_attrs();
    a->set_name("meter_id");
    a->set_size(4);
    a->set_value_bin(std::string("\0\0\0\x01", 4));
    Module *writer = Create("SetMetadata", name + "_md", Pack(md));
    EXPECT_NE(nullptr, writer);
    EXPECT_EQ(0, ModuleGraph::ConnectModules(writer, 0, mt, 0, true));
    auto out = h.RunPipeline(writer, 0, {pkt}, mt, {0, 1, 2});
    for (gate_idx_t g = 0; g < 3; g++) {
      if (!out[g].empty()) {
        return g;
      }
    }
    return 3;  // dropped
  };
  const auto expect_cost = [&](bess::pb::MeterByteBasis basis, uint32_t off,
                               const Bytes &pkt, uint32_t bytes) {
    EXPECT_EQ(0, gate_for(basis, off, pkt, bytes)) << bytes;
    EXPECT_EQ(1, gate_for(basis, off, pkt, bytes - 1)) << bytes;
  };

  expect_cost(bess::pb::METER_BYTE_BASIS_L2, 0, udp, udp.size());
  expect_cost(bess::pb::METER_BYTE_BASIS_L3, 14, udp, 32);   // 20 + 8 + 4
  expect_cost(bess::pb::METER_BYTE_BASIS_L4, 14, udp, 12);     // 8 + 4
  expect_cost(bess::pb::METER_BYTE_BASIS_L4, 14, tcp, 24);     // 20 + 4
  expect_cost(bess::pb::METER_BYTE_BASIS_L3, 14, v6, 52);    // 40 + 12
  expect_cost(bess::pb::METER_BYTE_BASIS_L4, 14, v6, 12);
  expect_cost(bess::pb::METER_BYTE_BASIS_CUSTOM, 14, udp, 32);

  // An empty IPv6 packet (Next Header 59) is measurable: 40 bytes at L3 and
  // an empty transport segment at L4.
  Bytes empty_v6(54, 0);
  empty_v6[12] = 0x86;
  empty_v6[13] = 0xdd;
  empty_v6[14] = 0x60;
  empty_v6[20] = 59;
  expect_cost(bess::pb::METER_BYTE_BASIS_L3, 14, empty_v6, 40);
  EXPECT_EQ(0, gate_for(bess::pb::METER_BYTE_BASIS_L4, 14, empty_v6, 1))
      << "an empty transport segment charges 0 bytes";

  // Two empty packets at a one-byte committed burst: a zero charge leaves both
  // green; a charge of one byte would make the second yellow.
  {
    const std::string name = "mt_basis_empty_pair";
    bess::pb::MeterArg marg;
    marg.set_capacity(16);
    marg.set_byte_basis(bess::pb::METER_BYTE_BASIS_L4);
    marg.set_byte_offset(14);
    Meter *mt = static_cast<Meter *>(Create("Meter", name, Pack(marg)));
    ASSERT_NE(nullptr, mt);
    auto r = Apply({
        Op::Upsert(name + "/meters", bess::dataplane::EncodeKey(MeterId(1)),
                   std::any(bess::meter::MeterProfileSpec{
                       bess::meter::TrTcmSpec{1, 1, 1'000'000, 1000}})),
    });
    ASSERT_EQ(Outcome::kApplied, r.outcome)
        << (r.ops.empty() ? "" : r.ops[0].error);
    bess::pb::SetMetadataArg md;
    auto *a = md.add_attrs();
    a->set_name("meter_id");
    a->set_size(4);
    a->set_value_bin(std::string("\0\0\0\x01", 4));
    Module *writer = Create("SetMetadata", name + "_md", Pack(md));
    ASSERT_NE(nullptr, writer);
    ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, mt, 0, true));
    auto out = h.RunPipeline(writer, 0, {empty_v6, empty_v6}, mt, {0, 1, 2});
    EXPECT_EQ(2u, out[0].size()) << "both empty segments charged 0 bytes";
    EXPECT_TRUE(out[1].empty());
    EXPECT_TRUE(out[2].empty());
  }
}

// A packet the basis cannot measure is dropped when it names a meter: an
// unknown IP version, and an offset past the frame.
TEST_F(SessionPipelineTest, AnUnmeasurablePacketIsDropped) {
  bess::testing::ModuleHarness h;
  const auto run = [&](const std::string &name, bess::pb::MeterByteBasis basis,
                       uint32_t off, const Bytes &pkt) {
    bess::pb::MeterArg marg;
    marg.set_capacity(16);
    marg.set_byte_basis(basis);
    marg.set_byte_offset(off);
    Meter *mt = static_cast<Meter *>(Create("Meter", name, Pack(marg)));
    EXPECT_NE(nullptr, mt);
    auto r = Apply({
        Op::Upsert(name + "/meters", bess::dataplane::EncodeKey(MeterId(1)),
                   std::any(bess::meter::MeterProfileSpec{SlowMeter(1000)})),
    });
    EXPECT_EQ(Outcome::kApplied, r.outcome)
        << (r.ops.empty() ? "" : r.ops[0].error);
    bess::pb::SetMetadataArg md;
    auto *a = md.add_attrs();
    a->set_name("meter_id");
    a->set_size(4);
    a->set_value_bin(std::string("\0\0\0\x01", 4));
    Module *writer = Create("SetMetadata", name + "_md", Pack(md));
    EXPECT_NE(nullptr, writer);
    EXPECT_EQ(0, ModuleGraph::ConnectModules(writer, 0, mt, 0, true));
    return h.RunPipeline(writer, 0, {pkt}, mt, {0, 1, 2});
  };

  auto not_ip = run("mt_bad_ip", bess::pb::METER_BYTE_BASIS_L3, 0, Bytes(64, 0));
  EXPECT_TRUE(not_ip[0].empty());
  EXPECT_TRUE(not_ip[1].empty());
  EXPECT_TRUE(not_ip[2].empty()) << "a non-IP frame was forwarded unmetered";

  Bytes jumbogram(54, 0);  // length 0 with a Hop-by-Hop next header
  jumbogram[14] = 0x60;
  jumbogram[20] = 0;
  auto jumbo = run("mt_bad_jumbo", bess::pb::METER_BYTE_BASIS_L3, 14, jumbogram);
  EXPECT_TRUE(jumbo[0].empty());
  EXPECT_TRUE(jumbo[1].empty());
  EXPECT_TRUE(jumbo[2].empty()) << "a jumbogram was metered at 40 bytes";

  auto short_frame = run("mt_bad_from", bess::pb::METER_BYTE_BASIS_CUSTOM, 64,
                         Bytes(8, 0));
  EXPECT_TRUE(short_frame[0].empty());
  EXPECT_TRUE(short_frame[1].empty());
  EXPECT_TRUE(short_frame[2].empty()) << "a frame before the offset was metered";
}

// The basis and its offset are validated when the module is created.
TEST_F(SessionPipelineTest, TheAccountingBasisIsValidated) {
  bess::testing::ModuleHarness h;
  bess::pb::MeterArg l2_offset;
  l2_offset.set_byte_basis(bess::pb::METER_BYTE_BASIS_L2);
  l2_offset.set_byte_offset(14);
  EXPECT_NE("", h.CreateError("Meter", l2_offset));

  bess::pb::MeterArg huge_offset;
  huge_offset.set_byte_basis(bess::pb::METER_BYTE_BASIS_CUSTOM);
  huge_offset.set_byte_offset(65536);
  EXPECT_NE("", h.CreateError("Meter", huge_offset));

  bess::pb::MeterArg unknown;
  unknown.set_byte_basis(static_cast<bess::pb::MeterByteBasis>(7));
  EXPECT_NE("", h.CreateError("Meter", unknown));
}

// A packet-rate meter charges one unit per packet, whatever its length: a
// two-packet burst admits two 60-byte frames. A byte meter with the same
// profile would refuse the first (60 bytes against a burst of 2).
TEST_F(SessionPipelineTest, APacketRateMeterChargesOneUnitPerPacket) {
  bess::pb::SetMetadataArg md_arg;
  auto *writes = md_arg.add_attrs();
  writes->set_name("meter_id");
  writes->set_size(4);
  writes->set_value_bin(std::string("\x00\x00\x00\x01", 4));
  Module *md = Create("SetMetadata", "md_cost", Pack(md_arg));
  Meter *mt = CreateMeterModule("mt_cost", 64, "", 0,
                                bess::pb::MeterArg::METER_COST_PACKETS);
  ASSERT_NE(nullptr, md);
  ASSERT_NE(nullptr, mt);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(md, 0, mt, 0, true));

  auto r = Apply({Op::Upsert("mt_cost/meters",
                             bess::dataplane::EncodeKey(MeterId(1)),
                             std::any(bess::meter::MeterProfileSpec{SlowMeter(2)}))});
  ASSERT_EQ(Outcome::kApplied, r.outcome)
      << (r.ops.empty() ? "" : r.ops[0].error);

  std::random_device rd;
  std::mt19937 rng{rd()};
  const auto first = SessionPacket("10.1.2.3", 1234, rng);
  const auto second = SessionPacket("10.1.2.3", 1234, rng);
  const auto third = SessionPacket("10.1.2.3", 1234, rng);
  bess::testing::ModuleHarness h;

  auto out = h.RunPipeline(md, 0, {first, second}, mt, {0, 2});
  EXPECT_EQ(2u, out[0].size()) << "two packets fit a two-packet burst";
  EXPECT_TRUE(out[2].empty());

  out = h.RunPipeline(md, 0, {third}, mt, {0, 2});
  EXPECT_TRUE(out[0].empty());
  ASSERT_EQ(1u, out[2].size()) << "the third is over the burst";
}

// A packet-rate meter charges one unit for any frame: the byte basis that
// would fail a non-IP frame closed in byte mode is not consulted.
TEST_F(SessionPipelineTest, APacketRateMeterIgnoresTheByteBasis) {
  bess::pb::SetMetadataArg md_arg;
  auto *writes = md_arg.add_attrs();
  writes->set_name("meter_id");
  writes->set_size(4);
  writes->set_value_bin(std::string("\x00\x00\x00\x01", 4));
  Module *md = Create("SetMetadata", "md_cost_basis", Pack(md_arg));
  bess::pb::MeterArg marg;
  marg.set_capacity(64);
  marg.set_cost(bess::pb::MeterArg::METER_COST_PACKETS);
  marg.set_byte_basis(bess::pb::METER_BYTE_BASIS_L3);
  Meter *mt = static_cast<Meter *>(Create("Meter", "mt_cost_basis", Pack(marg)));
  ASSERT_NE(nullptr, md);
  ASSERT_NE(nullptr, mt);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(md, 0, mt, 0, true));

  auto r = Apply(
      {Op::Upsert("mt_cost_basis/meters", bess::dataplane::EncodeKey(MeterId(1)),
                  std::any(bess::meter::MeterProfileSpec{SlowMeter(1)}))});
  ASSERT_EQ(Outcome::kApplied, r.outcome)
      << (r.ops.empty() ? "" : r.ops[0].error);

  // A frame with no IP header: unmeasurable under the L3 basis, and still one
  // packet to a packet-rate meter.
  bess::testing::ModuleHarness h;
  auto out = h.RunPipeline(md, 0, {Bytes(64, 0)}, mt, {0, 2});
  EXPECT_EQ(1u, out[0].size()) << "the non-IP frame is one packet";
  EXPECT_TRUE(out[2].empty());
}

}  // namespace
