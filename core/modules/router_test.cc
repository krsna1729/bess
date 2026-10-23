// SPDX-License-Identifier: BSD-3-Clause

// Router's packet path, in process (D-153): the next-hop id in metadata picks
// the egress, and a resolved hop that names both of its addresses has them
// written into the frame's Ethernet header before the frame leaves. A hop that
// names none, or only one, is forwarded with the header it arrived with; a
// frame whose header cannot be written is dropped and counted, because
// forwarding it with the addresses it arrived with would put it on the wire as
// someone else's.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <chrono>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "gate.h"
#include "module.h"
#include "module_graph.h"
#include "modules/router.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"
#include "runtime/runtime_state.h"
#include "utils/ether.h"

namespace {

using bess::testing::Bytes;
using bess::testing::FoldChecksum;
using bess::testing::Ip4;
using bess::testing::Mac;
using bess::testing::ModuleHarness;
using bess::testing::OnesSum;
using bess::testing::PacketBuilder;
using bess::utils::Ethernet;

// The frame the tests send: the addresses it arrives with (an upstream
// neighbor's toward this port, say), an IPv4 payload.
Bytes Frame() {
  return PacketBuilder()
      .Ether("02:00:00:00:00:0a", "02:00:00:00:00:0b")
      .IPv4("192.0.2.1", "198.51.100.1")
      .Udp(1000, 2000)
      .Payload("payload")
      .Build();
}

std::array<uint8_t, 6> DstOf(const Bytes &frame) {
  std::array<uint8_t, 6> mac{};
  std::copy_n(frame.begin(), 6, mac.begin());
  return mac;
}

std::array<uint8_t, 6> SrcOf(const Bytes &frame) {
  std::array<uint8_t, 6> mac{};
  std::copy_n(frame.begin() + 6, 6, mac.begin());
  return mac;
}

// A packet's bytes, across its segments.
Bytes BytesOf(bess::PacketHandle packet) {
  Bytes bytes;
  bytes.reserve(bess::PacketRef(packet).total_len());
  for (bess::PacketHandle h = packet; h != nullptr; h = h->next) {
    const bess::PacketRef seg(h);
    const auto *p = seg.head_data<const uint8_t *>();
    bytes.insert(bytes.end(), p, p + seg.head_len());
  }
  return bytes;
}

// A helper module: for every packet, keep the original and forward a clone.
// The clone shares its payload, so a module that writes into the frame must
// copy it first (K4 EnsureWritable) -- the state a decap or a span leaves
// behind. The originals stay with this module, for the test to read back.
class Sharer final : public Module {
 public:
  static const gate_idx_t kNumIGates = 1;
  static const gate_idx_t kNumOGates = 1;

  Sharer() { max_allowed_workers_ = Worker::kMaxWorkers; }

  // No Init: the base's Init(EmptyArg) returns success, and declaring one here
  // would shadow it (clang-tidy's derived-method-shadowing check).

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    bess::PacketHandle originals[bess::PacketBatch::kMaxBurst];
    const int cnt = batch->cnt();
    for (int i = 0; i < cnt; i++) {
      originals[i] = batch->packet(i).handle();
      kept_.push_back(originals[i]);
    }
    batch->clear();
    for (int i = 0; i < cnt; i++) {
      bess::PacketHandle clone = bess::PacketClone(originals[i]);
      if (clone != nullptr) {
        batch->add(bess::PacketRef(clone));
      }
    }
    RunNextModule(ctx, batch);
  }

  std::vector<bess::PacketHandle> kept_;
};

ADD_MODULE(Sharer, "router_test_sharer",
           "test: forwards a clone of each packet and keeps the original")

// The "aa:bb:cc:dd:ee:ff" form RouterInterface.mac_addr takes.
std::string MacString(const std::array<uint8_t, 6> &mac) {
  return Ethernet::Address(mac.data()).ToString();
}

// A destination-mode router argument with both address families enabled and
// small FIBs (D-180): there, RouterArg enables a family only when its route
// capacity is set.
bess::pb::RouterArg DualStackArg() {
  bess::pb::RouterArg arg;
  arg.set_mode(bess::pb::ROUTE_MODE_DESTINATION);
  arg.set_max_ipv4_routes(64);
  arg.set_max_ipv4_tbl8s(16);
  arg.set_max_ipv6_routes(64);
  arg.set_max_ipv6_tbl8s(64);
  return arg;
}

// A next-hop-id router argument: empty, since that mode has no FIB and so no
// family setting (D-180).
bess::pb::RouterArg NextHopArg() { return bess::pb::RouterArg(); }

class RouterTest : public ::testing::Test {
 protected:
  // The tests stand in for the maintenance loop: one tick is 1 ms, so the
  // resolver's millisecond timings equal ticks, and the tick count restarts
  // at 0 for each test.
  void SetUp() override {
    bess::runtime::runtime().ticks().set_interval(std::chrono::milliseconds(1));
    (void)bess::runtime::runtime().ticks().Run(std::chrono::nanoseconds(0), 0);
  }

  // A router named "rt" with one next hop, written as a client writes it (the
  // codec path, so the MAC fields are the RPC's; a null `dst_mac`/`src_mac`
  // leaves the field out, which reads back as the zero address, D-129).
  Router *MakeRouter(uint64_t egress_gate, const char *dst_mac, const char *src_mac,
                     bess::pb::RouterNeighborState neighbor) {
    Router *rt = dynamic_cast<Router *>(h_.Create("Router", NextHopArg(), "rt"));
    if (rt == nullptr) {
      return nullptr;
    }
    bess::pb::RouterNextHopIdKey key;
    key.set_id(1);
    bess::pb::RouterNextHopValue value;
    value.set_egress_gate(egress_gate);
    value.set_neighbor(neighbor);
    if (dst_mac != nullptr) {
      const std::array<uint8_t, 6> mac = Mac(dst_mac);
      value.set_dst_mac(reinterpret_cast<const char *>(mac.data()), mac.size());
    }
    if (src_mac != nullptr) {
      const std::array<uint8_t, 6> mac = Mac(src_mac);
      value.set_src_mac(reinterpret_cast<const char *>(mac.data()), mac.size());
    }
    EXPECT_EQ("", h_.UpsertResource("rt/next_hops", key, value));
    return rt;
  }

  // The metadata writer a graph uses for this attribute: a SetMetadata
  // setting next_hop_id = `id` (big-endian, as the Router reads it), which in
  // a graph is what a classifier result writes.
  // A fresh name per call: the harness keeps every module it creates until the
  // test ends (ModuleHarness::~ModuleHarness destroys them), so a second
  // writer in one test would fail creation with a name already taken.
  Module *MakeWriter(uint32_t id) {
    bess::pb::SetMetadataArg arg;
    bess::pb::SetMetadataArg::Attribute *attr = arg.add_attrs();
    attr->set_name("next_hop_id");
    attr->set_size(4);
    const uint8_t bytes[4] = {static_cast<uint8_t>(id >> 24), static_cast<uint8_t>(id >> 16),
                              static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
    attr->set_value_bin(reinterpret_cast<const char *>(bytes), sizeof(bytes));
    return h_.Create("SetMetadata", arg, "writer" + std::to_string(next_writer_++));
  }

  // One frame through a writer that sets next_hop_id = 1, into `rt`: the
  // graph a deployment has (a classifier result upstream of the Router).
  std::map<gate_idx_t, std::vector<Bytes>> Run(Router *rt, const Bytes &frame,
                                              std::vector<gate_idx_t> ogates = {0, 1}) {
    Module *writer = MakeWriter(1);
    EXPECT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
    return h_.RunPipeline(writer, 0, {frame}, rt, std::move(ogates));
  }

  // A router that routes by destination (D-155): the mode is configuration,
  // not a guess about the graph's wiring. `groups` sets max_groups (D-156).
  Router *MakeDestinationRouter(uint64_t domains = 1, uint64_t groups = 0,
                                const std::string &domain_attr = "") {
    bess::pb::RouterArg arg = DualStackArg();
    arg.set_mode(bess::pb::ROUTE_MODE_DESTINATION);
    arg.set_max_domains(domains);
    if (!domain_attr.empty()) {
      arg.set_domain_attr(domain_attr);
    }
    if (groups != 0) {
      arg.set_max_groups(groups);
    }
    return dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  }

  // A destination-mode router that punts what it cannot forward (D-162): a miss
  // to `miss_gate`, an unresolved neighbor to `unresolved_gate`.
  Router *MakePuntingRouter(uint64_t miss_gate, uint64_t unresolved_gate) {
    bess::pb::RouterArg arg = DualStackArg();
    arg.set_mode(bess::pb::ROUTE_MODE_DESTINATION);
    arg.set_punt_miss_gate(miss_gate);
    arg.set_punt_unresolved_gate(unresolved_gate);
    return dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  }

  // A next hop: egress gate `gate`, addresses named or not (D-129).
  void MakeHop(Router *rt, uint32_t id, uint64_t gate, const char *dst_mac, const char *src_mac,
               bess::pb::RouterNeighborState neighbor = bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED) {
    bess::pb::RouterNextHopIdKey key;
    key.set_id(id);
    bess::pb::RouterNextHopValue value;
    value.set_egress_gate(gate);
    value.set_neighbor(neighbor);
    if (dst_mac != nullptr) {
      const std::array<uint8_t, 6> mac = Mac(dst_mac);
      value.set_dst_mac(reinterpret_cast<const char *>(mac.data()), mac.size());
    }
    if (src_mac != nullptr) {
      const std::array<uint8_t, 6> mac = Mac(src_mac);
      value.set_src_mac(reinterpret_cast<const char *>(mac.data()), mac.size());
    }
    EXPECT_EQ("", h_.UpsertResource("rt/next_hops", key, value));
  }

  // A local next hop (D-178): delivery to this node, no egress, no addresses.
  void MakeLocalHop(Router *rt, uint32_t id) {
    bess::pb::RouterNextHopIdKey key;
    key.set_id(id);
    bess::pb::RouterNextHopValue value;
    value.set_kind(bess::pb::ROUTER_NEXT_HOP_KIND_LOCAL);
    EXPECT_EQ("", h_.UpsertResource("rt/next_hops", key, value));
  }

  // One route in one domain, naming `hop`.
  void MakeRoute(Router *rt, const char *prefix, uint32_t length, uint32_t domain, uint32_t hop) {
    bess::pb::RouterRouteKey key;
    key.set_ipv4(prefix);
    key.set_prefix_length(length);
    key.set_domain(domain);
    bess::pb::RouterRouteValue value;
    value.set_next_hop_id(hop);
    EXPECT_EQ("", h_.UpsertResource("rt/routes", key, value));
  }
  void MakeRoute6(Router *rt, const char *prefix, uint32_t length,
                  uint32_t domain, uint32_t hop) {
    bess::pb::RouterRouteKey key;
    key.set_ipv6(prefix);
    key.set_prefix_length(length);
    key.set_domain(domain);
    bess::pb::RouterRouteValue value;
    value.set_next_hop_id(hop);
    EXPECT_EQ("", h_.UpsertResource("rt/routes", key, value));
  }

  // One next-hop group (D-156), written as a client writes it: the members in
  // selection order, a repeated member a weight.
  void MakeGroup(Router *rt, uint32_t id, const std::vector<uint32_t> &members) {
    bess::pb::RouterGroupIdKey key;
    key.set_id(id);
    bess::pb::RouterGroupValue value;
    for (const uint32_t member : members) {
      value.add_member_ids(member);
    }
    EXPECT_EQ("", h_.UpsertResource("rt/groups", key, value));
  }

  // One route naming a group instead of a hop (D-156).
  void MakeGroupRoute(Router *rt, const char *prefix, uint32_t length, uint32_t domain,
                      uint32_t group) {
    bess::pb::RouterRouteKey key;
    key.set_ipv4(prefix);
    key.set_prefix_length(length);
    key.set_domain(domain);
    bess::pb::RouterRouteValue value;
    value.set_group_id(group);
    EXPECT_EQ("", h_.UpsertResource("rt/routes", key, value));
  }

  // What a VIF writes: the route domain a packet arrived in. A fresh name per
  // call, as MakeWriter's.
  Module *MakeDomainWriter(uint32_t domain,
                           const std::string &attr_name = "route_domain") {
    bess::pb::SetMetadataArg arg;
    bess::pb::SetMetadataArg::Attribute *attr = arg.add_attrs();
    attr->set_name(attr_name);
    attr->set_size(4);
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(domain >> 24), static_cast<uint8_t>(domain >> 16),
        static_cast<uint8_t>(domain >> 8), static_cast<uint8_t>(domain)};
    attr->set_value_bin(reinterpret_cast<const char *>(bytes), sizeof(bytes));
    return h_.Create("SetMetadata", arg, "vif" + std::to_string(next_writer_++));
  }
  Module *MakeNeighborInterfaceWriter(uint32_t id) {
    bess::pb::SetMetadataArg arg;
    bess::pb::SetMetadataArg::Attribute *attr = arg.add_attrs();
    attr->set_name("vif_id");
    attr->set_size(4);
    const uint8_t bytes[4] = {static_cast<uint8_t>(id >> 24),
                              static_cast<uint8_t>(id >> 16),
                              static_cast<uint8_t>(id >> 8),
                              static_cast<uint8_t>(id)};
    attr->set_value_bin(reinterpret_cast<const char *>(bytes), sizeof(bytes));
    return h_.Create("SetMetadata", arg,
                     "neighbor_vif" + std::to_string(next_writer_++));
  }

  // A frame in the domain it carries, through the VIF that writes it.
  std::map<gate_idx_t, std::vector<Bytes>> RunToDestination(
      Router *rt, const Bytes &frame, uint32_t domain = 0,
      std::vector<gate_idx_t> ogates = {0, 1},
      const std::string &domain_attr = "route_domain") {
    Module *vif = MakeDomainWriter(domain, domain_attr);
    EXPECT_EQ(0, ModuleGraph::ConnectModules(vif, 0, rt, 0, true));
    return h_.RunPipeline(vif, 0, {frame}, rt, std::move(ogates));
  }

  // Binds next hop `id` of Router `module` to `address` (IPv4 or IPv6 text)
  // on `interface`, through "<module>/neighbors"; "" on success.
  std::string BindNeighbor(const std::string &module, uint32_t id, uint32_t interface,
                           const std::string &address) {
    bess::pb::RouterNextHopIdKey key;
    key.set_id(id);
    bess::pb::RouterNeighborBinding value;
    value.set_interface_id(interface);
    if (address.find(':') != std::string::npos) {
      value.set_ipv6(address);
    } else {
      value.set_ipv4(address);
    }
    return h_.UpsertResource(module + "/neighbors", key, value);
  }
  std::string UnbindNeighbor(const std::string &module, uint32_t id) {
    bess::pb::RouterNextHopIdKey key;
    key.set_id(id);
    return h_.EraseResource(module + "/neighbors", key);
  }

  // A resolving Router named `name` from `arg`, with next hops 1..`bindings`
  // bound to 192.0.2.2 on interface 2 (ResolverArg's).
  Router *CreateResolver(const bess::pb::RouterArg &arg, uint32_t bindings = 1,
                         const std::string &name = "rt") {
    auto *rt = dynamic_cast<Router *>(h_.Create("Router", arg, name));
    for (uint32_t id = 1; rt != nullptr && id <= bindings; id++) {
      EXPECT_EQ("", BindNeighbor(name, id, 2, "192.0.2.2"));
    }
    return rt;
  }

  // A resolving Router from `arg` (ResolverArg's interfaces) with two
  // resolved neighbors on interface 2: id 1 -> A (192.0.2.2 at
  // 02:00:00:00:00:aa) and id 2 -> B (192.0.2.3 at 02:00:00:00:00:bb), their
  // solicitations drained. Ticks 1 and 2 are used.
  Router *CreateTwoResolvedNeighbors(const bess::pb::RouterArg &arg) {
    auto *rt = CreateResolver(arg);
    if (rt == nullptr) {
      return nullptr;
    }
    EXPECT_EQ("", BindNeighbor("rt", 2, 2, "192.0.2.3"));
    Module *writer = MakeNeighborInterfaceWriter(2);
    EXPECT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
    (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
    EXPECT_EQ(2u, h_.RunTask(rt, {0, 1})[1].size());
    const auto reply = [](const char *mac, const char *ip) {
      return PacketBuilder()
          .Ether("02:00:00:00:00:01", mac)
          .Arp(2, mac, ip, "02:00:00:00:00:01", "192.0.2.1")
          .PadTo(60)
          .Build();
    };
    (void)h_.RunPipeline(writer, 0,
                         {reply("02:00:00:00:00:aa", "192.0.2.2"),
                          reply("02:00:00:00:00:bb", "192.0.2.3")},
                         rt, {0});
    (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 2);
    for (uint32_t id : {1u, 2u}) {
      const auto *hop = rt->router()->LookupNextHop(bess::route::NextHopId(id));
      EXPECT_TRUE(hop != nullptr && hop->neighbor == bess::route::NeighborState::kResolved)
          << "id " << id;
    }
    return rt;
  }

  // Next hop `id` is resolved on `mac`.
  void ExpectResolved(Router *rt, uint32_t id, const char *mac) {
    const auto *hop = rt->router()->LookupNextHop(bess::route::NextHopId(id));
    ASSERT_NE(nullptr, hop) << "id " << id;
    EXPECT_EQ(bess::route::NeighborState::kResolved, hop->neighbor) << "id " << id;
    EXPECT_EQ(mac, hop->dst_mac.ToString()) << "id " << id;
  }

  // Names for the metadata writers, one per call (see MakeWriter).
  int next_writer_ = 0;
  ModuleHarness h_;
};

TEST_F(RouterTest, ArpReplyPublishesResolverNextHop) {
  constexpr char kLocalMac[] = "02:00:00:00:00:01";
  constexpr char kPeerMac[] = "02:00:00:00:00:02";
  bess::pb::RouterArg arg = NextHopArg();
  auto *interface = arg.add_interfaces();
  interface->set_id(2);
  interface->set_egress_gate(1);
  const auto local_mac = Mac(kLocalMac);
  interface->set_mac_addr(MacString(local_mac));
  interface->add_addresses("192.0.2.1/24");
  arg.set_neighbor_first_probe_ms(0);  // on the tick that takes the binding up
  arg.set_neighbor_probe_interval_ms(1);
  arg.set_neighbor_max_probe_interval_ms(1);
  arg.set_neighbor_max_probes(2);
  arg.set_neighbor_reachable_lifetime_ms(4);
  arg.set_neighbor_unreachable_retry_ms(2);
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  ASSERT_EQ("", BindNeighbor("rt", 1, 2, "192.0.2.2"));
  const auto *initial =
      rt->router()->LookupNextHop(bess::route::NextHopId(1));
  ASSERT_NE(nullptr, initial);
  EXPECT_EQ(bess::route::NeighborState::kIncomplete, initial->neighbor);
  // A bound id is the binding's: an ordinary next-hop write or erase of it is
  // refused, and the hop stays the resolver's.
  bess::pb::RouterNextHopIdKey hop_key;
  hop_key.set_id(1);
  bess::pb::RouterNextHopValue static_hop;
  static_hop.set_egress_gate(1);
  static_hop.set_neighbor(bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
  EXPECT_NE("", h_.UpsertResource("rt/next_hops", hop_key, static_hop));
  EXPECT_NE("", h_.EraseResource("rt/next_hops", hop_key));
  const auto *owned = rt->router()->LookupNextHop(bess::route::NextHopId(1));
  ASSERT_NE(nullptr, owned);
  EXPECT_EQ(bess::route::NeighborState::kIncomplete, owned->neighbor);
  EXPECT_TRUE(owned->dst_mac.IsZero());

  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  const auto first_probe = h_.RunTask(rt, {0, 1});
  ASSERT_EQ(1u, first_probe.at(1).size());
  const auto request = bess::route::ParseArp(first_probe.at(1)[0]);
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(bess::utils::Arp::kRequest, request->opcode);

  const Bytes reply =
      PacketBuilder()
          .Ether(kLocalMac, kPeerMac)
          .Arp(2, kPeerMac, "192.0.2.2", kLocalMac, "192.0.2.1")
          .PadTo(60)
          .Build();
  (void)h_.RunPipeline(writer, 0, {reply}, rt, {0});
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 2);
  const auto *resolved =
      rt->router()->LookupNextHop(bess::route::NextHopId(1));
  ASSERT_NE(nullptr, resolved);
  EXPECT_EQ(bess::route::NeighborState::kResolved, resolved->neighbor);
  EXPECT_EQ(Ethernet::Address(Mac(kPeerMac).data()), resolved->dst_mac);
  EXPECT_EQ(Ethernet::Address(local_mac.data()), resolved->src_mac);

  // The reachable lifetime (4) runs out at tick 6: the neighbor is solicited
  // again but keeps forwarding on its MAC while it is reconfirmed.
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 6);
  const auto *aged =
      rt->router()->LookupNextHop(bess::route::NextHopId(1));
  ASSERT_NE(nullptr, aged);
  EXPECT_EQ(bess::route::NeighborState::kResolved, aged->neighbor);
  EXPECT_EQ(Ethernet::Address(Mac(kPeerMac).data()), aged->dst_mac);
  EXPECT_EQ(1u, h_.RunTask(rt, {0, 1})[1].size());
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 7);
  EXPECT_EQ(1u, h_.RunTask(rt, {0, 1})[1].size());
  EXPECT_EQ(bess::route::NeighborState::kResolved,
            rt->router()->LookupNextHop(bess::route::NextHopId(1))->neighbor);
  // Both probes (max_probes 2) unanswered: it fails closed.
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 8);
  const auto *unreachable =
      rt->router()->LookupNextHop(bess::route::NextHopId(1));
  ASSERT_NE(nullptr, unreachable);
  EXPECT_EQ(bess::route::NeighborState::kUnreachable, unreachable->neighbor);
  EXPECT_TRUE(unreachable->dst_mac.IsZero());
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 10);
  const auto retry_probe = h_.RunTask(rt, {0, 1});
  ASSERT_EQ(1u, retry_probe.at(1).size());
  const auto *retry =
      rt->router()->LookupNextHop(bess::route::NextHopId(1));
  ASSERT_NE(nullptr, retry);
  EXPECT_EQ(bess::route::NeighborState::kIncomplete, retry->neighbor);
}
// One resolver task run that solicits on two interfaces leaves through two
// gates. The task module must flush its own per-gate batches: left in the
// shared Context, the next module would read them against its own gates.
TEST_F(RouterTest, SolicitationsOnTwoInterfacesLeaveThroughBothGates) {
  bess::pb::RouterArg arg = NextHopArg();
  for (uint32_t id = 1; id <= 2; id++) {
    auto *interface = arg.add_interfaces();
    interface->set_id(id);
    interface->set_egress_gate(id - 1);
    const auto mac = Mac(id == 1 ? "02:00:00:00:00:01" : "02:00:00:00:00:02");
    interface->set_mac_addr(MacString(mac));
    interface->add_addresses(id == 1 ? "192.0.2.1/24" : "198.51.100.1/24");
  }
  arg.set_neighbor_first_probe_ms(0);
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  ASSERT_EQ("", BindNeighbor("rt", 1, 1, "192.0.2.2"));
  ASSERT_EQ("", BindNeighbor("rt", 2, 2, "198.51.100.2"));
  Module *writer = MakeNeighborInterfaceWriter(1);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  const auto probes = h_.RunTask(rt, {0, 1});
  ASSERT_EQ(1u, probes.count(0));
  ASSERT_EQ(1u, probes.count(1));
  EXPECT_EQ(1u, probes.at(0).size());
  EXPECT_EQ(1u, probes.at(1).size());
}
// max_neighbors bounds the bindings, counted across a transaction: one more
// is refused until one goes.
TEST_F(RouterTest, NeighborBindingCapacityIsBounded) {
  bess::pb::RouterArg arg = NextHopArg();
  arg.set_max_neighbors(1);
  auto *interface = arg.add_interfaces();
  interface->set_id(2);
  interface->set_egress_gate(1);
  interface->set_mac_addr("02:00:00:00:00:01");
  interface->add_addresses("192.0.2.1/24");
  ASSERT_NE(nullptr, h_.Create("Router", arg, "rt"));
  EXPECT_EQ("", BindNeighbor("rt", 1, 2, "192.0.2.2"));
  EXPECT_NE("", BindNeighbor("rt", 2, 2, "192.0.2.3")) << "a second binding";
  EXPECT_EQ("", BindNeighbor("rt", 1, 2, "192.0.2.3")) << "a rebind takes no room";
  EXPECT_EQ("", UnbindNeighbor("rt", 1));
  EXPECT_EQ("", BindNeighbor("rt", 2, 2, "192.0.2.3")) << "room again";
  arg.set_max_neighbors(arg.max_next_hops() == 0 ? 4097 : arg.max_next_hops() + 1);
  EXPECT_NE("", h_.CreateError("Router", arg)) << "more bindings than next hops";
}
TEST_F(RouterTest, NdAdvertisementPublishesResolverNextHop) {
  constexpr char kLocalMac[] = "02:00:00:00:00:01";
  constexpr char kPeerMac[] = "02:00:00:00:00:02";
  const auto local_mac = Mac(kLocalMac);
  const auto peer_mac = Mac(kPeerMac);
  const auto local_ip = bess::utils::ParseIpv6Address("fe80::1").value();
  const auto peer_ip = bess::utils::ParseIpv6Address("fe80::2").value();
  bess::pb::RouterArg arg = NextHopArg();
  auto *interface = arg.add_interfaces();
  interface->set_id(2);
  interface->set_egress_gate(1);
  interface->set_mac_addr(MacString(local_mac));
  interface->add_addresses("fe80::1/64");
  arg.set_neighbor_first_probe_ms(0);
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  ASSERT_EQ("", BindNeighbor("rt", 1, 2, "fe80::2"));

  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  const auto first_probe = h_.RunTask(rt, {0, 1});
  ASSERT_EQ(1u, first_probe.at(1).size());
  const auto solicitation =
      bess::route::ParseNeighborDiscovery(first_probe.at(1)[0]);
  ASSERT_TRUE(solicitation.has_value());
  EXPECT_EQ(bess::route::NdType::kNeighborSolicitation,
            solicitation->type);

  std::array<uint8_t, bess::route::kNdFrameSize> frame{};
  ASSERT_EQ(frame.size(),
            bess::route::BuildNeighborAdvertisement(
                Ethernet::Address(peer_mac.data()), peer_ip, peer_ip,
                Ethernet::Address(local_mac.data()), local_ip, true, true,
                frame));
  (void)h_.RunPipeline(
      writer, 0, {Bytes(frame.begin(), frame.end())}, rt, {0});
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 2);

  const auto *resolved =
      rt->router()->LookupNextHop(bess::route::NextHopId(1));
  ASSERT_NE(nullptr, resolved);
  EXPECT_EQ(bess::route::NeighborState::kResolved, resolved->neighbor);
  EXPECT_EQ(Ethernet::Address(peer_mac.data()), resolved->dst_mac);
  EXPECT_EQ(Ethernet::Address(local_mac.data()), resolved->src_mac);
}

// A hop that names both addresses: the frame leaves with them, on the gate the
// hop's egress names, and every other byte of the frame is untouched.
// Defined with the destination-mode helpers below; the hop-mode arms use the
// same builder and reader so both modes are judged on the same bytes. The
// default TTL stays on the definition, which follows these arms.
Bytes FrameTo(const char *dst, uint8_t ttl);
uint8_t TtlOf(const Bytes &frame);

TEST_F(RouterTest, ANamedHopIsWrittenIntoTheFrame) {
  Router *rt = MakeRouter(1, "02:00:00:00:00:bb", "02:00:00:00:00:aa",
                          bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
  ASSERT_NE(nullptr, rt);
  const Bytes frame = Frame();
  auto out = Run(rt, frame);

  ASSERT_EQ(1u, out[1].size()) << "the hop's gate (egress_gate 1)";
  EXPECT_TRUE(out[0].empty());
  const Bytes &got = out[1][0];
  ASSERT_EQ(frame.size(), got.size());
  EXPECT_EQ(Mac("02:00:00:00:00:bb"), DstOf(got));
  EXPECT_EQ(Mac("02:00:00:00:00:aa"), SrcOf(got));
  EXPECT_TRUE(std::equal(frame.begin() + 12, frame.begin() + 14,
                         got.begin() + 12))
      << "the ethertype is the frame's own";
  EXPECT_EQ(63, TtlOf(got)) << "a router decrements what it forwards";
  EXPECT_EQ(0u, rt->header_write_failures());
}

// A hop that names no addresses: the frame keeps the addresses it arrived
// with, and only the TTL changes -- the hop count is the router's, whichever
// addresses the hop names. Every graph in the tree is this shape today, so
// this is the compatibility case, not an edge one.
TEST_F(RouterTest, AHopThatNamesNoAddressesKeepsItsAddresses) {
  Router *rt = MakeRouter(1, nullptr, nullptr, bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
  ASSERT_NE(nullptr, rt);
  const Bytes frame = Frame();
  auto out = Run(rt, frame);

  ASSERT_EQ(1u, out[1].size());
  const Bytes &got = out[1][0];
  ASSERT_EQ(frame.size(), got.size());
  EXPECT_EQ(DstOf(frame), DstOf(got));
  EXPECT_EQ(SrcOf(frame), SrcOf(got));
  EXPECT_EQ(63, TtlOf(got));
  EXPECT_EQ(0u, rt->header_write_failures());
}

// Half a pair is not a rewrite: writing the destination alone would put a zero
// source address on the wire. The TTL still moves, as it does for any IPv4
// frame a router forwards.
TEST_F(RouterTest, AHopThatNamesOnlyOneAddressKeepsItsAddresses) {
  Router *rt = MakeRouter(1, "02:00:00:00:00:bb", nullptr,
                          bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
  ASSERT_NE(nullptr, rt);
  const Bytes frame = Frame();
  auto out = Run(rt, frame);

  ASSERT_EQ(1u, out[1].size());
  const Bytes &got = out[1][0];
  ASSERT_EQ(frame.size(), got.size());
  EXPECT_EQ(DstOf(frame), DstOf(got)) << "half a pair writes nothing";
  EXPECT_EQ(SrcOf(frame), SrcOf(got));
  EXPECT_EQ(63, TtlOf(got));
  EXPECT_EQ(0u, rt->header_write_failures()) << "not a failure: the hop named nothing to write";
}

// A frame whose payload is shared (a clone, as a decap or a span leaves one):
// the header is written after the payload is copied, and the other holder of
// the payload does not see the write.
TEST_F(RouterTest, ASharedFrameIsCopiedBeforeTheHeaderIsWritten) {
  Router *rt = MakeRouter(1, "02:00:00:00:00:bb", "02:00:00:00:00:aa",
                          bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
  ASSERT_NE(nullptr, rt);
  Sharer *sharer = dynamic_cast<Sharer *>(h_.Create("Sharer", bess::pb::EmptyArg(), "sharer"));
  ASSERT_NE(nullptr, sharer);
  Module *writer = MakeWriter(1);
  // The clone's metadata is not copied with it (PacketClone's contract), so
  // the writer that names the next hop comes after the clone: the Router then
  // meets a frame whose payload is shared.
  ASSERT_EQ(0, ModuleGraph::ConnectModules(sharer, 0, writer, 0, true));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));

  const Bytes frame = Frame();
  auto out = h_.RunPipeline(sharer, 0, {frame}, rt, {1});

  ASSERT_EQ(1u, out[1].size());
  EXPECT_EQ(Mac("02:00:00:00:00:bb"), DstOf(out[1][0])) << "the clone was rewritten";
  EXPECT_EQ(Mac("02:00:00:00:00:aa"), SrcOf(out[1][0]));
  ASSERT_EQ(1u, sharer->kept_.size());
  const Bytes &original = BytesOf(sharer->kept_[0]);
  EXPECT_EQ(frame, original) << "the payload's other holder is untouched";
  EXPECT_EQ(0u, rt->header_write_failures());
  for (bess::PacketHandle kept : sharer->kept_) {
    bess::PacketFree(kept);
  }
}

// A frame whose header cannot be written -- here one shorter than the Ethernet
// header, the same `head_len < sizeof(Ethernet)` check a header in a later
// segment fails -- is dropped and counted, never forwarded with the addresses
// it arrived with.
TEST_F(RouterTest, AFrameWhoseHeaderCannotBeWrittenIsDroppedAndCounted) {
  Router *rt = MakeRouter(1, "02:00:00:00:00:bb", "02:00:00:00:00:aa",
                          bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
  ASSERT_NE(nullptr, rt);
  const Bytes runt = PacketBuilder().Payload("hi").Build();
  ASSERT_LT(runt.size(), sizeof(Ethernet));
  auto out = Run(rt, runt);

  EXPECT_TRUE(out[0].empty());
  EXPECT_TRUE(out[1].empty()) << "dropped, not forwarded";
  EXPECT_EQ(1u, rt->header_write_failures());
  EXPECT_NE(std::string::npos, rt->GetDesc().find("1 header write failures"))
      << rt->GetDesc();
}

// The hop mode decrements the TTL on the hop's own write (one implementation
// with the destination mode): a frame with a TTL of 1 is dropped and counted
// rather than forwarded with a zero hop count.
TEST_F(RouterTest, AHopModeExpiringTtlIsDroppedAndCounted) {
  Router *rt = MakeRouter(1, "02:00:00:00:00:bb", "02:00:00:00:00:aa",
                          bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
  ASSERT_NE(nullptr, rt);
  auto out = Run(rt, FrameTo("10.0.0.2", 1));

  EXPECT_TRUE(out[0].empty());
  EXPECT_TRUE(out[1].empty()) << "expired, not forwarded";
  EXPECT_EQ(1u, rt->ttl_expired_frames());
  EXPECT_EQ(0u, rt->header_write_failures());
}

// TTL 2 leaves as 1: the boundary DecrementTtl documents.
TEST_F(RouterTest, AHopModeTtlTwoLeavesAsOne) {
  Router *rt = MakeRouter(1, "02:00:00:00:00:bb", "02:00:00:00:00:aa",
                          bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
  ASSERT_NE(nullptr, rt);
  auto out = Run(rt, FrameTo("10.0.0.2", 2));

  ASSERT_EQ(1u, out[1].size());
  EXPECT_EQ(1, TtlOf(out[1][0]));
  EXPECT_EQ(0u, rt->ttl_expired_frames());
}

// A non-initial fragment still carries the IPv4 header, so it leaves with the
// hop count written: the mode routes by id, not by a hash that needs the ports
// in the fragment this packet does not have.
TEST_F(RouterTest, AHopModeFragmentStillDecrementsTheTtl) {
  Router *rt = MakeRouter(1, "02:00:00:00:00:bb", "02:00:00:00:00:aa",
                          bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
  ASSERT_NE(nullptr, rt);
  Bytes frame = FrameTo("10.0.0.2", 2);
  frame[21] = 0x20;  // the fragment field's low byte: an offset, not zero
  auto out = Run(rt, frame);

  ASSERT_EQ(1u, out[1].size());
  EXPECT_EQ(1, TtlOf(out[1][0]));
  EXPECT_EQ(Mac("02:00:00:00:00:bb"), DstOf(out[1][0]));
  EXPECT_EQ(0u, rt->ttl_expired_frames());
  EXPECT_EQ(0u, rt->header_write_failures());
}

// An unresolved neighbor is dropped whatever it names: dropping is the choice
// that never forwards on a guess.
TEST_F(RouterTest, AnUnresolvedHopIsDropped) {
  Router *rt = MakeRouter(1, "02:00:00:00:00:bb", "02:00:00:00:00:aa",
                          bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE);
  ASSERT_NE(nullptr, rt);
  auto out = Run(rt, Frame());

  EXPECT_TRUE(out[0].empty());
  EXPECT_TRUE(out[1].empty());
  EXPECT_EQ(0u, rt->header_write_failures()) << "a drop for another reason, not a failed write";
}

// -- mode 2: routing by destination (D-155) ----------------------------------

// A frame addressed to `dst`, arriving with `ttl` and the addresses a port
// would use.
Bytes FrameTo(const char *dst, uint8_t ttl = 64) {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:0a", "02:00:00:00:00:0b")
      .IPv4("192.0.2.1", dst)
      .Udp(1000, 2000)
      .Payload("payload");
  b.Last<PacketBuilder::IPv4Hdr>().ttl = ttl;
  return b.Build();
}
Bytes Frame6To(const char *dst, uint8_t hop_limit = 64) {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:0a", "02:00:00:00:00:0b")
      .IPv6("2001:db8::1", dst)
      .Udp(1000, 2000)
      .Payload("payload");
  b.Last<PacketBuilder::IPv6Hdr>().hlim = hop_limit;
  return b.Build();
}

uint8_t HopLimitOf(const Bytes &frame) { return frame[14 + 7]; }

// The hop mode decrements the IPv6 hop limit as it does the IPv4 TTL (and as
// the destination mode does): 2 leaves as 1 with the hop's addresses, and 1
// is dropped and counted as expired.
TEST_F(RouterTest, AHopModeDecrementsTheIpv6HopLimit) {
  Router *rt = MakeRouter(1, "02:00:00:00:00:bb", "02:00:00:00:00:aa",
                          bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
  ASSERT_NE(nullptr, rt);
  Module *writer = MakeWriter(1);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  const Bytes frame = Frame6To("2001:db8:1::9", 2);
  auto out = h_.RunPipeline(writer, 0, {frame}, rt, {0, 1});
  ASSERT_EQ(1u, out[1].size());
  EXPECT_EQ(1, HopLimitOf(out[1][0]));
  EXPECT_EQ(Mac("02:00:00:00:00:bb"), DstOf(out[1][0]));
  EXPECT_TRUE(std::equal(frame.begin() + 22, frame.end(), out[1][0].begin() + 22));
  EXPECT_EQ(0u, rt->ttl_expired_frames());

  auto expired =
      h_.RunPipeline(writer, 0, {Frame6To("2001:db8:1::9", 1)}, rt, {0, 1});
  EXPECT_TRUE(expired[1].empty()) << "expired, not forwarded";
  EXPECT_EQ(1u, rt->ttl_expired_frames());
  EXPECT_EQ(0u, rt->header_write_failures());
}

// The frame's IPv4 TTL: Ethernet is 14 bytes, and TTL is byte 8 of the header.
uint8_t TtlOf(const Bytes &frame) { return frame[14 + 8]; }

// The header's own checksum, which covers the TTL.
uint16_t ChecksumOf(const Bytes &frame) {
  return static_cast<uint16_t>(frame[14 + 10] << 8 | frame[14 + 11]);
}

// A route in the FIB is enough: the module looks the packet's destination up
// itself, writes the hop's addresses, and decrements the TTL as a router must.
TEST_F(RouterTest, DestinationRoutingForwardsOnTheFibsGateAndDecrementsTtl) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  EXPECT_TRUE(rt->routes_by_destination());
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "10.0.0.0", 8, 0, 1);

  const Bytes frame = FrameTo("10.1.2.3");
  auto out = RunToDestination(rt, frame);

  ASSERT_EQ(1u, out[1].size()) << "the hop's egress gate";
  EXPECT_TRUE(out[0].empty());
  const Bytes &got = out[1][0];
  ASSERT_EQ(frame.size(), got.size());
  EXPECT_EQ(Mac("02:00:00:00:00:bb"), DstOf(got));
  EXPECT_EQ(Mac("02:00:00:00:00:aa"), SrcOf(got));
  EXPECT_EQ(63, TtlOf(got)) << "a router decrements what it forwards";
  EXPECT_NE(ChecksumOf(frame), ChecksumOf(got)) << "the checksum covers the TTL";
  // FoldChecksum returns the complement of the folded sum, so a valid header
  // folds to 0: the checksum covers the TTL it left with.
  EXPECT_EQ(0, FoldChecksum(OnesSum(std::span<const uint8_t>(got.data() + 14, 20))))
      << "the header it left with is a valid one";
  EXPECT_TRUE(std::equal(frame.begin() + 12, frame.begin() + 22, got.begin() + 12))
      << "the ethertype and the header up to the TTL are the frame's own";
  EXPECT_TRUE(std::equal(frame.begin() + 26, frame.end(), got.begin() + 26))
      << "and so is everything after the checksum";
}

// The selector attributes' names are configuration: a router reads the
// next-hop id and the route domain from the names a graph gives it, so a
// classifier that writes its own names needs no adapter module.
TEST_F(RouterTest, TheSelectorAttributesAreConfiguration) {
  bess::pb::RouterArg arg = NextHopArg();
  arg.set_max_next_hops(64);
  arg.set_next_hop_attr("nh_id");
  // Each mode declares the attribute it reads, under its configured name, and
  // not the other mode's (D-180): a declared read is a requirement on every
  // path into the module.
  const auto reads = [](const Router *rt) {
    std::vector<std::string> names;
    for (const auto &attr : rt->all_attrs()) {
      EXPECT_EQ(bess::metadata::Attribute::AccessMode::kRead, attr.mode);
      names.push_back(attr.name);
    }
    return names;
  };
  auto *by_id = dynamic_cast<Router *>(h_.Create("Router", arg, "rt_named"));
  ASSERT_NE(nullptr, by_id);
  EXPECT_EQ(std::vector<std::string>{"nh_id"}, reads(by_id));

  arg.MergeFrom(DualStackArg());
  arg.set_domain_attr("vrf");
  auto *by_destination =
      dynamic_cast<Router *>(h_.Create("Router", arg, "rt_dest"));
  ASSERT_NE(nullptr, by_destination);
  EXPECT_EQ(std::vector<std::string>{"vrf"}, reads(by_destination));
}

// A burst is resolved in one lookup for its run, and every frame keeps its own
// outcome: two forwards and a miss in one batch, with the counters the scalar
// path would have written.
TEST_F(RouterTest, ABatchOfFramesIsResolvedAndEachKeepsItsOutcome) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "10.0.0.0", 8, 0, 1);

  auto out = h_.RunPipeline(
      rt, 0, {FrameTo("10.1.2.3"), FrameTo("192.0.2.1"), FrameTo("10.9.9.9")}, rt, {0, 1});
  ASSERT_EQ(2u, out[1].size()) << "the two frames with a route";
  EXPECT_TRUE(out[0].empty()) << "the miss is dropped, so it reaches no gate";
  EXPECT_EQ(63, TtlOf(out[1][0])) << "the first frame was forwarded";
  EXPECT_EQ(63, TtlOf(out[1][1])) << "and so was the third";
  EXPECT_EQ(1u, rt->no_route_frames()) << "counted once, for the miss";
  EXPECT_EQ(0u, rt->ttl_expired_frames());
}

// The mode decides, not the wiring: a module configured to route by
// destination ignores a `next_hop_id` another module writes.
TEST_F(RouterTest, TheMetadataIdIsIgnoredInDestinationMode) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeHop(rt, 2, 0, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "10.0.0.0", 8, 0, 1);

  Module *writer = MakeWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  auto out = h_.RunPipeline(writer, 0, {FrameTo("10.1.2.3")}, rt, {0, 1});

  ASSERT_EQ(1u, out[1].size()) << "the FIB's hop, not the id metadata named";
  EXPECT_TRUE(out[0].empty());
}

// The default mode keeps failing closed: with no writer the module drops every
// packet; it has no FIB to route by destination with (D-155, D-180).
TEST_F(RouterTest, TheIdModeStillFailsClosedWithoutAWriter) {
  Router *rt = dynamic_cast<Router *>(h_.Create("Router", NextHopArg(), "rt"));
  ASSERT_NE(nullptr, rt);
  EXPECT_FALSE(rt->routes_by_destination());
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");

  auto out = h_.RunPipeline(rt, 0, {FrameTo("10.1.2.3")}, rt, {0, 1});

  EXPECT_TRUE(out[0].empty());
  EXPECT_TRUE(out[1].empty()) << "dropped, not routed by destination";
}

TEST_F(RouterTest, AnUnknownModeIsRefused) {
  bess::pb::RouterArg arg = DualStackArg();
  arg.set_mode(static_cast<bess::pb::RouteMode>(7));
  EXPECT_NE("", h_.CreateError("Router", arg));
}

// The route domain is declared optional-with-default (D-177): a graph with no
// VIF/writer routes in domain 0, and the resolved attribute is the declared
// default, not an unresolved offset. This is the module-level half of the
// contract; metadata_test.cc covers the framework's report.
TEST_F(RouterTest, TheDefaultRouteDomainAppliesWithoutAWriter) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "10.0.0.0", 8, 0, 1);

  // No SetMetadata (VIF) is connected: the router is the input.
  auto out = h_.RunPipeline(rt, 0, {FrameTo("10.1.2.3")}, rt, {0, 1});

  ASSERT_EQ(1u, out[1].size()) << "the domain-0 route names gate 1";
  EXPECT_TRUE(out[0].empty());
  EXPECT_EQ(0u, rt->no_route_frames());

  size_t i = 0;
  bool found = false;
  for (const auto &attr : rt->all_attrs()) {
    if (attr.name == "route_domain") {
      found = true;
      EXPECT_EQ(bess::metadata::kMetadataOffsetDefault, rt->attr_offset(i));
      EXPECT_EQ(bess::metadata::Attribute::Contract::kDefault,
                attr.contract);
    }
    i++;
  }
  EXPECT_TRUE(found) << "the default domain attribute was not registered";
}

// A route domain the graph writes replaces the default: the same router reads
// the packet's domain, not the constant, after the next resume.
TEST_F(RouterTest, AWriterReplacesTheDefaultRouteDomain) {
  Router *rt = MakeDestinationRouter(/*domains=*/2);
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "10.0.0.0", 8, 1, 1);

  // Domain 1 has the route; domain 0 does not.
  auto one = RunToDestination(rt, FrameTo("10.1.2.3"), /*domain=*/1);
  ASSERT_EQ(1u, one[1].size());
  auto zero = RunToDestination(rt, FrameTo("10.1.2.3"), /*domain=*/0);
  EXPECT_TRUE(zero[0].empty());
  EXPECT_TRUE(zero[1].empty());
}

// A destination the FIB does not cover, and a hop whose neighbor is not
// resolved: both drop, and both say why.
TEST_F(RouterTest, AMissAndAnUnresolvedNeighborAreDroppedAndCounted) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa",
          bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE);
  MakeRoute(rt, "10.0.0.0", 8, 0, 1);

  auto unresolved = RunToDestination(rt, FrameTo("10.1.2.3"));
  EXPECT_TRUE(unresolved[0].empty());
  EXPECT_TRUE(unresolved[1].empty());
  EXPECT_EQ(1u, rt->unresolved_frames());
  EXPECT_EQ(0u, rt->no_route_frames());

  auto miss = RunToDestination(rt, FrameTo("192.168.1.1"));
  EXPECT_TRUE(miss[0].empty());
  EXPECT_TRUE(miss[1].empty());
  EXPECT_EQ(1u, rt->no_route_frames());
  EXPECT_NE(std::string::npos, rt->GetDesc().find("no route: 1")) << rt->GetDesc();
}

// D-160: a hop whose egress gate is DROP_GATE names no interface, and a route
// to it is the domain's blackhole -- `0.0.0.0/0` to such a hop is a default
// that drops. The frame is dropped and counted apart from a miss, because the
// route exists and chose that hop.
TEST_F(RouterTest, ARouteToAHopThatForwardsNowhereIsABlackhole) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, DROP_GATE, nullptr, nullptr);
  MakeRoute(rt, "0.0.0.0", 0, 0, 1);

  auto out = RunToDestination(rt, FrameTo("10.1.2.3"));
  EXPECT_TRUE(out[0].empty());
  EXPECT_TRUE(out[1].empty());
  EXPECT_EQ(1u, rt->blackholed_frames());
  EXPECT_EQ(0u, rt->no_route_frames()) << "the route exists: this is not a miss";
  EXPECT_EQ(0u, rt->unresolved_frames()) << "a hop that forwards nowhere resolves nothing";
  EXPECT_NE(std::string::npos, rt->GetDesc().find("blackholed: 1")) << rt->GetDesc();
}

// The same blackhole hop, with a neighbor state nothing ever resolved: a hop a
// controller wrote before any resolution. The route still ends the frame -- the
// neighbor state of a hop that forwards nowhere decides nothing (router.h) --
// so it must not be counted as an unresolved neighbor, and with a punt gate
// armed (D-162) it must not take it either: a route meant to end the packet
// must not hand it to a slow path. A local /32 is a different hop kind now
// (D-178); this is the intentional-drop shape.
TEST_F(RouterTest, ABlackholeHopIsNotAnUnresolvedNeighbor) {
  Router *rt = MakePuntingRouter(/*miss_gate=*/2, /*unresolved_gate=*/3);
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, DROP_GATE, nullptr, nullptr,
          bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE);
  MakeRoute(rt, "0.0.0.0", 0, 0, 1);

  const std::vector<gate_idx_t> ogates = {0, 1, 2, 3};
  auto out = RunToDestination(rt, FrameTo("10.1.2.3"), 0, ogates);
  // The capture map holds an entry only for a gate that received a frame, so
  // the loop names the gates: iterating the map would assert nothing in the
  // case that matters -- a frame punted to gate 2 or 3 would add an entry and
  // the counters would still read blackholed, which is the defect this test
  // exists for.
  for (const gate_idx_t gate : ogates) {
    EXPECT_EQ(0u, out.count(gate)) << "gate " << gate << " took a frame";
  }
  EXPECT_TRUE(out.empty()) << "the router emitted a frame";
  EXPECT_EQ(1u, rt->blackholed_frames());
  EXPECT_EQ(0u, rt->unresolved_frames()) << "a hop that forwards nowhere resolves nothing";
  EXPECT_EQ(0u, rt->no_route_frames());
}

// D-162: a punt gate sends what the destination path cannot forward to a slow
// path instead of dropping it -- a miss and an unresolved neighbor to their own
// gates -- and the counter still records what happened.
TEST_F(RouterTest, AMissAndAnUnresolvedNeighborPuntToTheirGates) {
  Router *rt = MakePuntingRouter(/*miss_gate=*/2, /*unresolved_gate=*/3);
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa",
          bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE);
  MakeHop(rt, 2, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "10.0.0.0", 8, 0, 1);      // hop 1: unresolved
  MakeRoute(rt, "192.168.0.0", 16, 0, 2);  // hop 2: resolved

  const std::vector<gate_idx_t> ogates = {0, 1, 2, 3};
  auto unresolved = RunToDestination(rt, FrameTo("10.1.2.3"), 0, ogates);
  EXPECT_TRUE(unresolved[0].empty());
  EXPECT_TRUE(unresolved[1].empty());
  ASSERT_EQ(1u, unresolved[3].size()) << "the unresolved punt gate";
  EXPECT_EQ(1u, rt->unresolved_frames());

  auto miss = RunToDestination(rt, FrameTo("172.16.1.1"), 0, ogates);
  ASSERT_EQ(1u, miss[2].size()) << "the miss punt gate";
  EXPECT_TRUE(miss[3].empty()) << "not the unresolved one";
  EXPECT_EQ(1u, rt->no_route_frames());
  EXPECT_NE(std::string::npos, rt->GetDesc().find("a miss punts to gate 2"))
      << rt->GetDesc();

  // What forwards is untouched by the policy.
  auto routed = RunToDestination(rt, FrameTo("192.168.1.1"), 0, ogates);
  ASSERT_EQ(1u, routed[1].size()) << "the hop's own egress gate";
  EXPECT_TRUE(routed[2].empty());
  EXPECT_TRUE(routed[3].empty());
}

// D-178: a local route is delivery, not forwarding. The node's own address
// leaves on the local gate, the frame is untouched (no TTL decrement, no L2
// rewrite), and the counter is its own outcome -- not a miss, an unresolved
// neighbor or a blackhole. Another host on the same subnet is still the
// connected route, and a destination outside it is still a miss.
TEST_F(RouterTest, ALocalAddressIsDeliveredToItsOwnGate) {
  bess::pb::RouterArg arg = DualStackArg();
  arg.set_mode(bess::pb::ROUTE_MODE_DESTINATION);
  arg.set_punt_miss_gate(2);
  arg.set_punt_unresolved_gate(3);
  arg.set_punt_local_gate(4);
  Router *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, nullptr, nullptr, bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE);
  MakeLocalHop(rt, 2);
  MakeRoute(rt, "192.0.2.0", 24, 0, 1);
  MakeRoute(rt, "192.0.2.7", 32, 0, 2);

  const std::vector<gate_idx_t> ogates = {0, 1, 2, 3, 4};
  auto local = RunToDestination(rt, FrameTo("192.0.2.7"), 0, ogates);
  ASSERT_EQ(1u, local[4].size()) << "the local gate";
  EXPECT_TRUE(local[1].empty()) << "not forwarded";
  EXPECT_TRUE(local[2].empty());
  EXPECT_TRUE(local[3].empty());
  EXPECT_EQ(1u, rt->local_frames());
  EXPECT_EQ(0u, rt->no_route_frames());
  EXPECT_EQ(0u, rt->unresolved_frames());
  EXPECT_EQ(0u, rt->blackholed_frames());
  EXPECT_EQ(FrameTo("192.0.2.7"), local[4][0])
      << "delivery writes nothing: no TTL, no addresses";

  // The connected route still decides another host on the subnet.
  auto host = RunToDestination(rt, FrameTo("192.0.2.8"), 0, ogates);
  EXPECT_TRUE(host[4].empty());
  ASSERT_EQ(1u, host[3].size()) << "the unresolved punt gate";
  EXPECT_EQ(1u, rt->unresolved_frames());

  auto miss = RunToDestination(rt, FrameTo("203.0.113.1"), 0, ogates);
  ASSERT_EQ(1u, miss[2].size()) << "the miss punt gate";
  EXPECT_TRUE(miss[4].empty());
  EXPECT_EQ(1u, rt->no_route_frames());

  EXPECT_NE(std::string::npos, rt->GetDesc().find("local: 1")) << rt->GetDesc();
  EXPECT_NE(std::string::npos, rt->GetDesc().find("a local address punts to gate 4"))
      << rt->GetDesc();
}

// The id mode has no punt gates (D-162): a next-hop id naming a local hop is
// not forwarded, and it is counted as local rather than as a miss.
TEST_F(RouterTest, ALocalHopInTheIdModeIsNotForwarded) {
  Router *rt = dynamic_cast<Router *>(h_.Create("Router", NextHopArg(), "rt"));
  ASSERT_NE(nullptr, rt);
  MakeLocalHop(rt, 1);

  auto out = Run(rt, FrameTo("192.0.2.7"), {0, 1});
  EXPECT_TRUE(out[0].empty());
  EXPECT_TRUE(out[1].empty()) << "a local hop is never forwarded";
  EXPECT_EQ(1u, rt->local_frames());
}

// A local hop's canonical form is kind LOCAL alone; it is refused when it
// also names an egress gate or an adjacency (D-129, D-178).
TEST_F(RouterTest, ALocalHopIsCanonicalAndRejectsAnEgressOrAddresses) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  MakeLocalHop(rt, 7);
  bess::pb::RouterNextHopIdKey key;
  key.set_id(7);
  bess::pb::RouterNextHopValue canonical;
  canonical.set_kind(bess::pb::ROUTER_NEXT_HOP_KIND_LOCAL);
  EXPECT_EQ("", h_.ReadBackDiffers("rt/next_hops", {{&key, &canonical}}));

  bess::pb::RouterNextHopIdKey other_key;
  other_key.set_id(8);
  bess::pb::RouterNextHopValue with_gate;
  with_gate.set_kind(bess::pb::ROUTER_NEXT_HOP_KIND_LOCAL);
  with_gate.set_egress_gate(1);
  EXPECT_NE("", h_.UpsertResource("rt/next_hops", other_key, with_gate));

  bess::pb::RouterNextHopValue with_mac;
  with_mac.set_kind(bess::pb::ROUTER_NEXT_HOP_KIND_LOCAL);
  const std::array<uint8_t, 6> mac = Mac("02:00:00:00:00:aa");
  with_mac.set_dst_mac(reinterpret_cast<const char *>(mac.data()), mac.size());
  EXPECT_NE("", h_.UpsertResource("rt/next_hops", other_key, with_mac));
}

// A punt gate that is not a gate this graph can have is refused at Init, like
// an unknown mode (D-162).
TEST_F(RouterTest, AnInvalidPuntGateIsRefused) {
  bess::pb::RouterArg arg = DualStackArg();
  arg.set_mode(bess::pb::ROUTE_MODE_DESTINATION);
  arg.set_punt_miss_gate(70000);  // beyond MAX_GATES and not DROP_GATE
  EXPECT_NE("", h_.CreateError("Router", arg));
}

// TTL 0 and 1 expire (nothing is forwarded); 2 leaves as 1 -- the boundary
// DecrementTtl documents, and the frame is dropped without being written.
TEST_F(RouterTest, AnExpiringTtlIsDroppedAndCounted) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "10.0.0.0", 8, 0, 1);

  for (uint8_t ttl : {uint8_t{0}, uint8_t{1}}) {
    auto out = RunToDestination(rt, FrameTo("10.1.2.3", ttl));
    EXPECT_TRUE(out[0].empty());
    EXPECT_TRUE(out[1].empty()) << "ttl " << static_cast<int>(ttl);
  }
  EXPECT_EQ(2u, rt->ttl_expired_frames());

  auto out = RunToDestination(rt, FrameTo("10.1.2.3", 2));
  ASSERT_EQ(1u, out[1].size());
  EXPECT_EQ(1, TtlOf(out[1][0]));
}

// The domain a packet carries picks its FIB, and a domain this router does not
// hold misses rather than aliasing another domain's routes.
TEST_F(RouterTest, TheDomainAttributeSelectsTheFib) {
  Router *rt = MakeDestinationRouter(/*domains=*/3);
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 0, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeHop(rt, 2, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "10.0.0.0", 8, 1, 1);
  MakeRoute(rt, "10.0.0.0", 8, 2, 2);
  MakeRoute(rt, "10.0.0.0", 8, 0, 1);

  auto one = RunToDestination(rt, FrameTo("10.1.2.3"), /*domain=*/1);
  ASSERT_EQ(1u, one[0].size()) << "domain 1's route names gate 0";
  EXPECT_TRUE(one[1].empty());

  auto two = RunToDestination(rt, FrameTo("10.1.2.3"), /*domain=*/2);
  ASSERT_EQ(1u, two[1].size()) << "domain 2's route names gate 1";
  EXPECT_TRUE(two[0].empty());

  // 65536 is not a domain of this router: it must not alias domain 0 (the
  // domain id is 24 bits wide, so narrowing it would).
  auto unknown = RunToDestination(rt, FrameTo("10.1.2.3"), /*domain=*/65536);
  EXPECT_TRUE(unknown[0].empty());
  EXPECT_TRUE(unknown[1].empty());
}

TEST_F(RouterTest, AConfiguredDomainAttributeNameSelectsTheFib) {
  Router *rt = MakeDestinationRouter(/*domains=*/3, /*groups=*/0,
                                     "vrf");
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "10.0.0.0", 8, 2, 1);

  auto routed = RunToDestination(rt, FrameTo("10.1.2.3"), /*domain=*/2,
                                 {0, 1}, "vrf");
  ASSERT_EQ(1u, routed[1].size());
  EXPECT_TRUE(routed[0].empty());
}

// What destination routing cannot route: a frame that is neither IPv4 nor
// IPv6, one whose headers do not parse, a non-initial fragment (its L4 header,
// and so the flow hash a group route needs, is in an earlier fragment), and an
// IPv6 frame with no IPv6 route.
TEST_F(RouterTest, FramesWithoutARoutableIpHeaderAreDroppedAndCounted) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "10.0.0.0", 8, 0, 1);

  const Bytes arp = PacketBuilder()
                        .Ether("02:00:00:00:00:0a", "02:00:00:00:00:0b")
                        .Arp(1, "02:00:00:00:00:0a", "192.0.2.1", "00:00:00:00:00:00", "10.1.2.3")
                        .Build();
  const Bytes ipv6 = PacketBuilder()
                         .Ether("02:00:00:00:00:0a", "02:00:00:00:00:0b")
                         .IPv6("2001:db8::1", "2001:db8::2")
                         .Udp(1000, 2000)
                         .Payload("payload")
                         .Build();
  // A frame that says IPv4 and carries no header: malformed, not "not IP".
  const Bytes no_header = PacketBuilder()
                              .Ether("02:00:00:00:00:0a", "02:00:00:00:00:0b", 0x0800)
                              .Build();
  const Bytes fragment = PacketBuilder()
                             .Ether("02:00:00:00:00:0a", "02:00:00:00:00:0b")
                             .Add(PacketBuilder::IPv4Hdr{.src = Ip4("192.0.2.1"),
                                                         .dst = Ip4("10.1.2.3"),
                                                         .frag = 0x0008})
                             .Payload("payload")
                             .Build();

  for (const Bytes *frame : {&arp, &no_header, &fragment}) {
    auto out = RunToDestination(rt, *frame);
    EXPECT_TRUE(out[0].empty());
    EXPECT_TRUE(out[1].empty());
  }
  auto v6_out = RunToDestination(rt, ipv6);
  EXPECT_TRUE(v6_out[0].empty());
  EXPECT_TRUE(v6_out[1].empty());
  EXPECT_EQ(1u, rt->not_ip_frames()) << "the ARP frame";
  EXPECT_EQ(1u, rt->no_route_frames()) << "IPv6 is recognized, but has no IPv6 route";
  EXPECT_EQ(1u, rt->malformed_frames()) << "the frame that says IPv4 and has none";
  EXPECT_EQ(1u, rt->non_initial_fragments());
  EXPECT_NE(std::string::npos, rt->GetDesc().find("non-initial fragments: 1")) << rt->GetDesc();
}

// A shared payload is copied before the TTL is written: the other holder of
// the payload keeps the TTL it arrived with.
TEST_F(RouterTest, ASharedFrameIsCopiedBeforeTheTtlIsWritten) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "10.0.0.0", 8, 0, 1);

  Sharer *sharer = dynamic_cast<Sharer *>(h_.Create("Sharer", bess::pb::EmptyArg(), "sharer"));
  ASSERT_NE(nullptr, sharer);
  Module *vif = MakeDomainWriter(0);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(sharer, 0, vif, 0, true));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(vif, 0, rt, 0, true));

  const Bytes frame = FrameTo("10.1.2.3");
  auto out = h_.RunPipeline(sharer, 0, {frame}, rt, {1});

  ASSERT_EQ(1u, out[1].size());
  EXPECT_EQ(63, TtlOf(out[1][0])) << "the clone was decremented";
  ASSERT_EQ(1u, sharer->kept_.size());
  const Bytes &original = BytesOf(sharer->kept_[0]);
  EXPECT_EQ(64, TtlOf(original)) << "the payload's other holder is untouched";
  EXPECT_EQ(frame, original) << "byte for byte, including its checksum";
  EXPECT_EQ(0u, rt->header_write_failures());
  for (bess::PacketHandle kept : sharer->kept_) {
    bess::PacketFree(kept);
  }
}

// A frame to `dst` whose flow -- the addresses and ports a group route hashes
// to pick a member (route::FlowHash) -- differs by its ports.
Bytes FlowFrame(const char *dst, uint16_t src_port, uint16_t dst_port) {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:0a", "02:00:00:00:00:0b")
      .IPv4("192.0.2.1", dst)
      .Udp(src_port, dst_port)
      .Payload("payload");
  return b.Build();
}

// -- mode 2: next-hop groups (D-156) ------------------------------------------

// A route naming a group spreads flows over its members, and a flow keeps
// the member it landed on: the same packet hashes the same way every time.
TEST_F(RouterTest, AGroupRoutePicksOneMemberPerFlow) {
  Router *rt = MakeDestinationRouter(1, 1);
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeHop(rt, 2, 2, "02:00:00:00:00:cc", "02:00:00:00:00:aa");
  MakeGroup(rt, 1, {1, 2});
  MakeGroupRoute(rt, "198.51.100.0", 24, 0, 1);

  // The gate the one frame left on: 1 or 2, -1 for anything else.
  const auto gate_of = [&](const Bytes &frame) -> int {
    auto out = RunToDestination(rt, frame, 0, {0, 1, 2});
    for (gate_idx_t gate : {1, 2}) {
      if (!out[gate].empty()) {
        EXPECT_EQ(1u, out[gate].size());
        EXPECT_EQ(gate == 1 ? Mac("02:00:00:00:00:bb") : Mac("02:00:00:00:00:cc"),
                  DstOf(out[gate][0]))
            << "the picked member's own addresses were written";
        EXPECT_EQ(63, TtlOf(out[gate][0])) << "a router decrements what it forwards";
        return gate;
      }
    }
    return -1;
  };

  std::map<uint16_t, int> picked;
  std::map<int, int> spread;
  for (uint16_t port = 1; port <= 64; port++) {
    const int gate = gate_of(FlowFrame("198.51.100.9", 4000, port));
    ASSERT_GE(gate, 1) << "flow " << port << " was forwarded";
    picked[port] = gate;
    spread[gate]++;
  }
  EXPECT_EQ(2u, spread.size())
      << "64 flows spread over both members: " << spread[1] << "/" << spread[2];
  for (const auto &[port, gate] : picked) {
    EXPECT_EQ(gate, gate_of(FlowFrame("198.51.100.9", 4000, port)))
        << "flow " << port << " kept its member";
  }
}

// Which fields a group hashes is the graph's choice (D-163). `FLOW_HASH_L3`
// reads the address pair alone, so every flow to one destination takes the
// same member -- what a graph wants when a subscriber's traffic must not be
// spread -- while the default spreads the flows.
TEST_F(RouterTest, L3OnlyHashingPinsEveryFlowToOneDestinationToOneMember) {
  bess::pb::RouterArg arg = DualStackArg();
  arg.set_mode(bess::pb::ROUTE_MODE_DESTINATION);
  arg.set_max_groups(1);
  arg.set_flow_hash(bess::pb::FLOW_HASH_L3);
  Router *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeHop(rt, 2, 2, "02:00:00:00:00:cc", "02:00:00:00:00:aa");
  MakeGroup(rt, 1, {1, 2});
  MakeGroupRoute(rt, "198.51.100.0", 24, 0, 1);

  const auto gate_of = [&](const Bytes &frame) -> int {
    auto out = RunToDestination(rt, frame, 0, {0, 1, 2});
    for (gate_idx_t gate : {1, 2}) {
      if (!out[gate].empty()) {
        return gate;
      }
    }
    return -1;
  };

  // 64 flows, all to one destination: with the address pair alone hashed, the
  // ports and the protocol cannot move any of them off the first member.
  const int first = gate_of(FlowFrame("198.51.100.9", 4000, 1));
  ASSERT_GE(first, 1) << "the flow was forwarded";
  for (uint16_t port = 2; port <= 64; port++) {
    EXPECT_EQ(first, gate_of(FlowFrame("198.51.100.9", 4000, port)))
        << "flow " << port << " was spread: only the addresses hash (D-163)";
  }
}

// A repeated member is a weight: it takes that many selection slots, so
// {1, 1, 2} sends about two thirds of the flows to hop 1 -- and the rest
// still reach hop 2, a weight is not a takeover.
TEST_F(RouterTest, ARepeatedMemberIsAWeight) {
  Router *rt = MakeDestinationRouter(1, 1);
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeHop(rt, 2, 2, "02:00:00:00:00:cc", "02:00:00:00:00:aa");
  MakeGroup(rt, 1, {1, 1, 2});
  MakeGroupRoute(rt, "198.51.100.0", 24, 0, 1);

  size_t ones = 0;
  size_t twos = 0;
  for (uint16_t port = 1; port <= 192; port++) {
    auto out = RunToDestination(rt, FlowFrame("198.51.100.9", 4000, port), 0, {1, 2});
    ones += out[1].size();
    twos += out[2].size();
  }
  EXPECT_EQ(192u, ones + twos) << "every flow was forwarded to one member";
  EXPECT_GT(ones, twos) << "the member twice in the list takes two of three slots: " << ones
                        << " to hop 1, " << twos << " to hop 2";
  EXPECT_GT(twos, 0) << "the other member still gets the slots it holds";
}

// A route names a next hop or a group (the two id fields), and whatever it names must
// exist by the end of the transaction: a route to a group that was never
// written, to one past max_groups, and one with no target at all are refused.
TEST_F(RouterTest, ARouteToAnUnknownGroupIsRefused) {
  Router *rt = MakeDestinationRouter(1, 4);
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, nullptr, nullptr);

  bess::pb::RouterRouteKey key;
  key.set_ipv4("198.51.100.0");
  key.set_prefix_length(24);
  bess::pb::RouterRouteValue value;
  value.set_group_id(1);
  EXPECT_NE("", h_.UpsertResource("rt/routes", key, value))
      << "group 1 does not exist: a route references what it names";
  value.set_group_id(5);
  EXPECT_NE("", h_.UpsertResource("rt/routes", key, value))
      << "group id 5 is past max_groups";
  bess::pb::RouterRouteValue no_target;
  EXPECT_NE("", h_.UpsertResource("rt/routes", key, no_target))
      << "a route names a next hop or a group";
  bess::pb::RouterRouteValue both;
  both.set_next_hop_id(1);
  both.set_group_id(1);
  const std::string both_error = h_.UpsertResource("rt/routes", key, both);
  EXPECT_NE(std::string::npos, both_error.find("not both"))
      << "the two ids are separate namespaces: one target, not both: " << both_error;
  MakeGroup(rt, 1, {1});
  value.set_group_id(1);
  EXPECT_EQ("", h_.UpsertResource("rt/routes", key, value)) << "now the group exists";
}

// The groups resource reads back in the form a client sends (D-129), members
// in their stored order with a weight as a repeat; a route naming a group
// reads back naming the group.
TEST_F(RouterTest, GroupsAndGroupRoutesReadBack) {
  Router *rt = MakeDestinationRouter(1, 1);
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, nullptr, nullptr);
  MakeHop(rt, 2, 2, nullptr, nullptr);
  MakeGroup(rt, 1, {1, 2, 1});
  MakeGroupRoute(rt, "198.51.100.0", 24, 0, 1);

  auto groups = h_.ReadResource("rt/groups");
  ASSERT_EQ(1u, groups.size());
  bess::pb::RouterGroupIdKey group_key;
  bess::pb::RouterGroupValue group_value;
  ASSERT_TRUE(groups[0].first.UnpackTo(&group_key));
  ASSERT_TRUE(groups[0].second.UnpackTo(&group_value));
  EXPECT_EQ(1u, group_key.id());
  EXPECT_EQ((std::vector<uint32_t>{1, 2, 1}),
            (std::vector<uint32_t>(group_value.member_ids().begin(),
                                   group_value.member_ids().end())))
      << "the order is the group's meaning, a repeat is its weight";

  auto routes = h_.ReadResource("rt/routes");
  ASSERT_EQ(1u, routes.size());
  bess::pb::RouterRouteValue route_value;
  ASSERT_TRUE(routes[0].second.UnpackTo(&route_value));
  EXPECT_EQ(0u, route_value.next_hop_id()) << "the route names the group, not a hop";
  EXPECT_EQ(1u, route_value.group_id());
  EXPECT_NE(std::string::npos, rt->GetDesc().find("1 groups")) << rt->GetDesc();
}

// References are the engine's (D-023): a group a route names cannot leave,
// and neither can a member the group names; removing them in the right order
// works.
TEST_F(RouterTest, AGroupInUseCannotLeave) {
  Router *rt = MakeDestinationRouter(1, 1);
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, nullptr, nullptr);
  MakeHop(rt, 2, 2, nullptr, nullptr);
  MakeGroup(rt, 1, {1, 2});
  MakeGroupRoute(rt, "198.51.100.0", 24, 0, 1);

  bess::pb::RouterRouteKey route_key;
  route_key.set_ipv4("198.51.100.0");
  route_key.set_prefix_length(24);
  bess::pb::RouterGroupIdKey group_key;
  group_key.set_id(1);
  bess::pb::RouterNextHopIdKey hop_key;
  hop_key.set_id(2);

  EXPECT_NE("", h_.EraseResource("rt/groups", group_key)) << "a route names the group";
  EXPECT_NE("", h_.EraseResource("rt/next_hops", hop_key)) << "the group names the hop";
  EXPECT_EQ("", h_.EraseResource("rt/routes", route_key));
  EXPECT_EQ("", h_.EraseResource("rt/groups", group_key));
  EXPECT_EQ("", h_.EraseResource("rt/next_hops", hop_key));
  EXPECT_EQ(0u, rt->router()->next_hop_group_count());
}

// Without max_groups the module still registers the groups resource -- a
// class registers every table it declares (D-156) -- and it holds nothing and
// refuses every id: a group write and a route naming a group are refused the
// same way an unknown one is.
TEST_F(RouterTest, WithoutMaxGroupsTheGroupsResourceIsEmpty) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, nullptr, nullptr);

  EXPECT_TRUE(h_.ReadResource("rt/groups").empty()) << "registered, and empty";
  bess::pb::RouterGroupIdKey group_key;
  group_key.set_id(1);
  bess::pb::RouterGroupValue group_value;
  group_value.add_member_ids(1);
  EXPECT_NE("", h_.UpsertResource("rt/groups", group_key, group_value));
  bess::pb::RouterRouteKey route_key;
  route_key.set_ipv4("198.51.100.0");
  route_key.set_prefix_length(24);
  bess::pb::RouterRouteValue route_value;
  route_value.set_group_id(1);
  EXPECT_NE("", h_.UpsertResource("rt/routes", route_key, route_value));
  EXPECT_NE(std::string::npos, rt->GetDesc().find("0 groups")) << rt->GetDesc();
}

TEST_F(RouterTest, DestinationModeForwardsIpv4AndIpv6Routes) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "198.51.100.0", 24, 0, 1);
  MakeRoute6(rt, "2001:db8:1::", 64, 0, 1);

  const Bytes v4 = FrameTo("198.51.100.9");
  auto out4 = RunToDestination(rt, v4);
  ASSERT_EQ(1u, out4[1].size());
  EXPECT_EQ(63, TtlOf(out4[1][0]));
  EXPECT_EQ(0, FoldChecksum(OnesSum(
                   std::span<const uint8_t>(out4[1][0].data() + 14, 20))));
  EXPECT_TRUE(std::equal(v4.begin() + 34, v4.end(),
                         out4[1][0].begin() + 34));

  const Bytes v6 = Frame6To("2001:db8:1::9");
  auto out6 = RunToDestination(rt, v6);
  ASSERT_EQ(1u, out6[1].size());
  EXPECT_EQ(63, HopLimitOf(out6[1][0]));
  EXPECT_EQ(Mac("02:00:00:00:00:bb"), DstOf(out6[1][0]));
  EXPECT_TRUE(std::equal(v6.begin() + 22, v6.end(),
                         out6[1][0].begin() + 22));

  const auto routes = h_.ReadResource("rt/routes");
  EXPECT_EQ(2u, routes.size());
}

TEST_F(RouterTest, Ipv6NoRouteAndMalformedFramesFailClosed) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, nullptr, nullptr);
  MakeRoute6(rt, "2001:db8:1::", 64, 0, 1);

  auto miss = RunToDestination(rt, Frame6To("2001:db8:2::1"));
  EXPECT_TRUE(miss[0].empty());
  EXPECT_TRUE(miss[1].empty());
  EXPECT_EQ(1u, rt->no_route_frames());

  Bytes malformed = Frame6To("2001:db8:1::9");
  malformed.resize(20);
  auto bad = RunToDestination(rt, malformed);
  EXPECT_TRUE(bad[0].empty());
  EXPECT_TRUE(bad[1].empty());
  EXPECT_EQ(1u, rt->malformed_frames());
}

// A local IPv6 route (D-178) delivers to the local gate and counts as local,
// as an IPv4 one does; it is neither forwarded nor a blackhole.
TEST_F(RouterTest, AnIpv6LocalRouteIsDeliveredToTheLocalGate) {
  bess::pb::RouterArg arg = DualStackArg();
  arg.set_mode(bess::pb::ROUTE_MODE_DESTINATION);
  arg.set_punt_local_gate(2);
  Router *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeLocalHop(rt, 7);
  MakeRoute6(rt, "2001:db8::", 32, 0, 1);
  MakeRoute6(rt, "2001:db8::5", 128, 0, 7);

  const Bytes frame = Frame6To("2001:db8::5");
  auto out = RunToDestination(rt, frame, 0, {0, 1, 2});
  ASSERT_EQ(1u, out[2].size());
  EXPECT_TRUE(out[1].empty());
  EXPECT_EQ(frame, out[2][0]) << "delivered, not forwarded: nothing written";
  EXPECT_EQ(1u, rt->local_frames());
  EXPECT_EQ(0u, rt->blackholed_frames());

  auto forwarded = RunToDestination(rt, Frame6To("2001:db8::6"), 0, {0, 1, 2});
  ASSERT_EQ(1u, forwarded[1].size());
  EXPECT_EQ(63, HopLimitOf(forwarded[1][0]));
}

// Link-scoped IPv6 never leaves through a route (RFC 4291 2.5.6): a
// link-local or multicast destination, or a link-local source, takes the
// local punt gate -- even with a default route that would forward it.
TEST_F(RouterTest, LinkScopedIpv6IsPuntedNotForwarded) {
  bess::pb::RouterArg arg = DualStackArg();
  arg.set_mode(bess::pb::ROUTE_MODE_DESTINATION);
  arg.set_punt_local_gate(2);
  Router *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute6(rt, "::", 0, 0, 1);

  const Bytes from_link_local = PacketBuilder()
                                    .Ether("02:00:00:00:00:0a", "02:00:00:00:00:0b")
                                    .IPv6("fe80::9", "2001:db8::1")
                                    .Udp(1000, 2000)
                                    .Payload("payload")
                                    .Build();
  for (const Bytes &frame :
       {Frame6To("fe80::1"), Frame6To("ff02::1"), from_link_local}) {
    auto out = RunToDestination(rt, frame, 0, {0, 1, 2});
    EXPECT_TRUE(out[1].empty());
    ASSERT_EQ(1u, out[2].size());
    EXPECT_EQ(frame, out[2][0]);
  }
  EXPECT_EQ(3u, rt->link_scope_frames());
  EXPECT_EQ(0u, rt->no_route_frames());
  auto routed = RunToDestination(rt, Frame6To("2001:db8::1"), 0, {0, 1, 2});
  EXPECT_EQ(1u, routed[1].size()) << "a global destination still routes";
}

TEST_F(RouterTest, LinkScopedIpv6WithoutALocalGateIsDroppedAndCounted) {
  Router *rt = MakeDestinationRouter();
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute6(rt, "::", 0, 0, 1);
  auto out = RunToDestination(rt, Frame6To("ff02::2"));
  EXPECT_TRUE(out[0].empty());
  EXPECT_TRUE(out[1].empty());
  EXPECT_EQ(1u, rt->link_scope_frames());
  EXPECT_NE(std::string::npos, rt->GetDesc().find("link scope: 1")) << rt->GetDesc();
}

// -- address families (D-180) -------------------------------------------------

// A destination-mode router with only the families whose route capacity is
// set: IPv4 when `ipv4`, IPv6 when `ipv6`.
bess::pb::RouterArg FamilyArg(bool ipv4, bool ipv6) {
  bess::pb::RouterArg arg;
  arg.set_mode(bess::pb::ROUTE_MODE_DESTINATION);
  if (ipv4) {
    arg.set_max_ipv4_routes(16);
  }
  if (ipv6) {
    arg.set_max_ipv6_routes(16);
    arg.set_max_ipv6_tbl8s(32);
  }
  return arg;
}

bess::pb::RouterRouteKey RouteKey(const char *ipv4, const char *ipv6, uint32_t length) {
  bess::pb::RouterRouteKey key;
  if (ipv4 != nullptr) {
    key.set_ipv4(ipv4);
  } else {
    key.set_ipv6(ipv6);
  }
  key.set_prefix_length(length);
  return key;
}

// IPv4 only: IPv4 is routed; an IPv6 frame is dropped and counted as family
// disabled, an IPv6 route is refused, and no IPv6 FIB exists in any domain.
TEST_F(RouterTest, AnIpv4OnlyRouterDropsAndCountsIpv6) {
  bess::pb::RouterArg arg = FamilyArg(true, false);
  arg.set_max_domains(2);
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  EXPECT_TRUE(rt->router()->has_ipv4());
  EXPECT_FALSE(rt->router()->has_ipv6());
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "198.51.100.0", 24, 0, 1);
  MakeRoute(rt, "198.51.100.0", 24, 1, 1);
  bess::pb::RouterRouteValue value;
  value.set_next_hop_id(1);
  EXPECT_NE("", h_.UpsertResource("rt/routes", RouteKey(nullptr, "2001:db8::", 32), value));

  EXPECT_EQ(1u, RunToDestination(rt, FrameTo("198.51.100.9"))[1].size());
  auto out6 = RunToDestination(rt, Frame6To("2001:db8:1::9"));
  EXPECT_TRUE(out6[0].empty());
  EXPECT_TRUE(out6[1].empty());
  EXPECT_EQ(1u, rt->family_disabled_frames());
  EXPECT_EQ(0u, rt->not_ip_frames());
  EXPECT_EQ(0u, rt->router()->fib_counts().ipv6);
  EXPECT_EQ(2u, rt->router()->fib_counts().ipv4);
  EXPECT_NE(std::string::npos, rt->GetDesc().find("family disabled: 1")) << rt->GetDesc();
}

// IPv6 only: the mirror image, and no IPv4 table (rte_lpm's 64 MiB tbl24) is
// built for any domain.
TEST_F(RouterTest, AnIpv6OnlyRouterDropsAndCountsIpv4) {
  bess::pb::RouterArg arg = FamilyArg(false, true);
  arg.set_max_domains(2);
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  EXPECT_FALSE(rt->router()->has_ipv4());
  EXPECT_TRUE(rt->router()->has_ipv6());
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute6(rt, "2001:db8:1::", 64, 0, 1);
  MakeRoute6(rt, "2001:db8:1::", 64, 1, 1);
  bess::pb::RouterRouteValue value;
  value.set_next_hop_id(1);
  EXPECT_NE("", h_.UpsertResource("rt/routes", RouteKey("198.51.100.0", nullptr, 24), value));

  auto out6 = RunToDestination(rt, Frame6To("2001:db8:1::9"));
  ASSERT_EQ(1u, out6[1].size());
  EXPECT_EQ(63, HopLimitOf(out6[1][0]));
  auto out4 = RunToDestination(rt, FrameTo("198.51.100.9"));
  EXPECT_TRUE(out4[0].empty());
  EXPECT_TRUE(out4[1].empty());
  EXPECT_EQ(1u, rt->family_disabled_frames());
  EXPECT_EQ(0u, rt->router()->fib_counts().ipv4);
  EXPECT_EQ(2u, rt->router()->fib_counts().ipv6);
}

// Dual stack: both families route through one domain's two FIBs, and a burst
// that mixes them keeps each frame's outcome.
TEST_F(RouterTest, ADualStackRouterRoutesAMixedBurst) {
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", FamilyArg(true, true), "rt"));
  ASSERT_NE(nullptr, rt);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "198.51.100.0", 24, 0, 1);
  MakeRoute6(rt, "2001:db8:1::", 64, 0, 1);
  Module *vif = MakeDomainWriter(0);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(vif, 0, rt, 0, true));
  auto out = h_.RunPipeline(vif, 0,
                            {FrameTo("198.51.100.9"), Frame6To("2001:db8:1::9"),
                             FrameTo("203.0.113.9"), Frame6To("2001:db8:2::9")},
                            rt, {0, 1});
  ASSERT_EQ(2u, out[1].size());
  EXPECT_EQ(63, TtlOf(out[1][0]));
  EXPECT_EQ(63, HopLimitOf(out[1][1]));
  EXPECT_EQ(2u, rt->no_route_frames());
  EXPECT_EQ(0u, rt->family_disabled_frames());
  EXPECT_EQ(1u, rt->router()->fib_counts().ipv4);
  EXPECT_EQ(1u, rt->router()->fib_counts().ipv6);
}

// The next-hop-id mode has no FIB and no family setting (D-180): an empty
// RouterArg is valid, allocates no FIB in any family, forwards IPv4 and IPv6
// on the hop the metadata names with each packet's own hop count decremented,
// and refuses a route write with an error that names the mode.
TEST_F(RouterTest, AnEmptyArgNextHopRouterForwardsBothFamiliesWithoutAFib) {
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", NextHopArg(), "rt"));
  ASSERT_NE(nullptr, rt);
  EXPECT_FALSE(rt->routes_by_destination());
  EXPECT_FALSE(rt->router()->has_ipv4());
  EXPECT_FALSE(rt->router()->has_ipv6());
  EXPECT_EQ(0u, rt->router()->fib_counts().ipv4);
  EXPECT_EQ(0u, rt->router()->fib_counts().ipv6);
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  auto out4 = Run(rt, FrameTo("198.51.100.9"));
  ASSERT_EQ(1u, out4[1].size());
  EXPECT_EQ(63, TtlOf(out4[1][0]));
  auto out6 = Run(rt, Frame6To("2001:db8:1::9"));
  ASSERT_EQ(1u, out6[1].size());
  EXPECT_EQ(63, HopLimitOf(out6[1][0]));
  EXPECT_EQ(0u, rt->family_disabled_frames());

  bess::pb::RouterRouteValue value;
  value.set_next_hop_id(1);
  const std::string v4 =
      h_.UpsertResource("rt/routes", RouteKey("198.51.100.0", nullptr, 24), value);
  EXPECT_NE(std::string::npos, v4.find("ROUTE_MODE_NEXT_HOP_ID")) << v4;
  const std::string v6 =
      h_.UpsertResource("rt/routes", RouteKey(nullptr, "2001:db8::", 32), value);
  EXPECT_NE(std::string::npos, v6.find("ROUTE_MODE_NEXT_HOP_ID")) << v6;
  EXPECT_EQ(0u, rt->router()->fib_counts().ipv4 + rt->router()->fib_counts().ipv6);
  EXPECT_NE(std::string::npos, rt->GetDesc().find("no FIB")) << rt->GetDesc();
}

// Destination-only fields configure a FIB, its groups, its domains and what
// its lookups punt; a next-hop-id router has none of those, so it refuses
// each rather than accept a setting that can never take effect.
TEST_F(RouterTest, TheNextHopModeRefusesFibCapacities) {
  const auto refused = [this](const bess::pb::RouterArg &arg, const char *what) {
    const std::string error = h_.CreateError("Router", arg);
    EXPECT_NE(std::string::npos, error.find("has no FIB")) << what << ": " << error;
  };
  bess::pb::RouterArg arg;
  arg.set_max_ipv4_routes(16);
  refused(arg, "max_ipv4_routes");
  arg = NextHopArg();
  arg.set_max_ipv4_tbl8s(16);
  refused(arg, "max_ipv4_tbl8s");
  arg = NextHopArg();
  arg.set_max_ipv6_routes(16);
  refused(arg, "max_ipv6_routes");
  arg = NextHopArg();
  arg.set_max_ipv6_tbl8s(16);
  refused(arg, "max_ipv6_tbl8s");
  arg = NextHopArg();
  arg.set_backend(bess::pb::ROUTE_BACKEND_FIB);
  refused(arg, "backend");
  arg = NextHopArg();
  arg.set_max_groups(1);
  refused(arg, "max_groups");
  arg = NextHopArg();
  arg.set_max_domains(2);
  refused(arg, "max_domains");
  arg = NextHopArg();
  arg.set_flow_hash(bess::pb::FLOW_HASH_L3);
  refused(arg, "flow_hash");
  arg = NextHopArg();
  arg.set_domain_attr("route_domain");
  refused(arg, "domain_attr, even its default name");
  arg = NextHopArg();
  arg.set_punt_miss_gate(2);
  refused(arg, "punt_miss_gate");
  arg = NextHopArg();
  arg.set_punt_unresolved_gate(DROP_GATE);
  refused(arg, "punt_unresolved_gate, even an explicit drop");

  // What the mode does read is still accepted: one domain spelled out, the
  // default hash, and the gates that serve its own traffic.
  arg = NextHopArg();
  arg.set_max_domains(1);
  arg.set_flow_hash(bess::pb::FLOW_HASH_L3_L4);
  arg.set_punt_local_gate(2);
  arg.set_punt_neighbor_gate(3);
  EXPECT_EQ("", h_.CreateError("Router", arg));
}

// The 24-bit route-value bound on max_next_hops + max_groups is the FIB's: a
// destination router is held to it, a next-hop-id router only to the 32-bit
// id, and a group write to a next-hop-id router is refused with the mode.
TEST_F(RouterTest, TheRouteValueBoundIsTheFibs) {
  bess::pb::RouterArg arg = DualStackArg();
  arg.set_max_next_hops(bess::route::LpmRouteTable::kMaxValue + 1);
  EXPECT_NE(std::string::npos,
            h_.CreateError("Router", arg).find("exceeds backend limits"));
  arg = DualStackArg();
  arg.set_max_next_hops(bess::route::LpmRouteTable::kMaxValue);
  arg.set_max_groups(1);
  EXPECT_NE(std::string::npos,
            h_.CreateError("Router", arg).find("exceeds backend limits"));

  arg = NextHopArg();
  arg.set_max_next_hops(uint64_t{bess::route::Router::kMaxNextHopsWithoutFib} + 1);
  EXPECT_NE(std::string::npos,
            h_.CreateError("Router", arg).find("exceeds backend limits"));
  // The library holds a router with a FIB to the same bound (it refuses
  // before allocating anything).
  EXPECT_FALSE(bess::route::Router::Create("fibbed", {.ipv4 = bess::route::LpmRouteTable::Config{}},
                                           bess::route::LpmRouteTable::kMaxValue + 1,
                                           bess::runtime::runtime().rcu()));

  Router *rt = dynamic_cast<Router *>(h_.Create("Router", NextHopArg(), "rt"));
  ASSERT_NE(nullptr, rt);
  bess::pb::RouterGroupIdKey group_key;
  group_key.set_id(1);
  bess::pb::RouterGroupValue group_value;
  group_value.add_member_ids(1);
  const std::string error = h_.UpsertResource("rt/groups", group_key, group_value);
  EXPECT_NE(std::string::npos, error.find("ROUTE_MODE_NEXT_HOP_ID")) << error;
}

// Sizing is per family in destination mode: a family is on when its route
// capacity is set, zero is not a way to say "off", a tbl8 budget alone enables
// nothing, and a destination router with neither family is refused.
TEST_F(RouterTest, FamilySizingIsValidated) {
  EXPECT_NE("", h_.CreateError("Router", FamilyArg(false, false))) << "no family";
  bess::pb::RouterArg arg = FamilyArg(false, false);
  arg.set_max_ipv4_routes(0);
  EXPECT_NE("", h_.CreateError("Router", arg)) << "explicit zero";
  arg = FamilyArg(true, false);
  arg.set_max_ipv4_tbl8s(0);
  EXPECT_NE("", h_.CreateError("Router", arg)) << "zero groups";
  arg = FamilyArg(true, false);
  arg.set_max_ipv6_tbl8s(64);
  EXPECT_NE("", h_.CreateError("Router", arg)) << "IPv6 groups without IPv6 routes";
  arg = FamilyArg(false, true);
  arg.set_backend(bess::pb::ROUTE_BACKEND_FIB);
  EXPECT_NE("", h_.CreateError("Router", arg)) << "an IPv4 backend without IPv4";
  arg = FamilyArg(false, true);
  arg.set_max_ipv6_routes(uint64_t{1} << 32);
  EXPECT_NE("", h_.CreateError("Router", arg)) << "beyond 32 bits";
}

// A next-hop-id resolver router with IPv4 and IPv6 addresses on interface 2
// (gate 1), with `punt_local_gate`; its first probe goes out on the tick that
// takes a binding up (CreateResolver binds them).
bess::pb::RouterArg ResolverArg(uint64_t punt_local_gate) {
  bess::pb::RouterArg arg = NextHopArg();
  auto *interface = arg.add_interfaces();
  interface->set_id(2);
  interface->set_egress_gate(1);
  const auto local_mac = Mac("02:00:00:00:00:01");
  interface->set_mac_addr(MacString(local_mac));
  interface->add_addresses("192.0.2.1/24");
  interface->add_addresses("fe80::1/64");
  arg.set_punt_local_gate(punt_local_gate);
  arg.set_neighbor_first_probe_ms(0);
  return arg;
}

// An ARP request and a Neighbor Solicitation (from the neighbor to us).
std::vector<Bytes> NeighborRequests() {
  const Bytes arp_request =
      PacketBuilder()
          .Ether("ff:ff:ff:ff:ff:ff", "02:00:00:00:00:02")
          .Arp(1, "02:00:00:00:00:02", "192.0.2.2", "00:00:00:00:00:00",
               "192.0.2.1")
          .PadTo(60)
          .Build();
  std::array<uint8_t, bess::route::kNdFrameSize> ns{};
  const size_t ns_len = bess::route::BuildNeighborSolicitation(
      Ethernet::Address(Mac("02:00:00:00:00:02").data()),
      bess::utils::ParseIpv6Address("fe80::2").value(),
      bess::utils::ParseIpv6Address("fe80::1").value(), ns);
  EXPECT_EQ(ns.size(), ns_len);
  return {arp_request, Bytes(ns.begin(), ns.end())};
}

// Requests are not the resolver's input (it learns only from replies): they
// go to the local punt gate, untouched, and are counted.
TEST_F(RouterTest, NeighborRequestsTakeTheLocalGate) {
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", ResolverArg(3), "rt"));
  ASSERT_NE(nullptr, rt);
  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  const std::vector<Bytes> requests = NeighborRequests();
  auto out = h_.RunPipeline(writer, 0, requests, rt, {0, 1, 3});
  ASSERT_EQ(2u, out[3].size());
  EXPECT_EQ(requests[0], out[3][0]);
  EXPECT_EQ(requests[1], out[3][1]);
  EXPECT_TRUE(out[1].empty());
  EXPECT_EQ(2u, rt->neighbor_requests());
  EXPECT_EQ(0u, rt->neighbor_input_drops());
}

TEST_F(RouterTest, WithoutALocalGateNeighborRequestsAreDroppedAndCounted) {
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", ResolverArg(0), "rt"));
  ASSERT_NE(nullptr, rt);
  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  auto out = h_.RunPipeline(writer, 0, NeighborRequests(), rt, {0, 1});
  EXPECT_TRUE(out[0].empty());
  EXPECT_TRUE(out[1].empty());
  EXPECT_EQ(2u, rt->neighbor_requests());
  EXPECT_NE(std::string::npos, rt->GetDesc().find("requests: 2")) << rt->GetDesc();
}

// punt_neighbor_gate gives requests a gate of their own (a responder), apart
// from local delivery; DROP_GATE drops them even with a local gate set.
TEST_F(RouterTest, NeighborRequestsTakeTheNeighborGateWhenOneIsSet) {
  bess::pb::RouterArg arg = ResolverArg(3);
  arg.set_punt_neighbor_gate(4);
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  const std::vector<Bytes> requests = NeighborRequests();
  auto out = h_.RunPipeline(writer, 0, requests, rt, {0, 1, 3, 4});
  ASSERT_EQ(2u, out[4].size());
  EXPECT_EQ(requests[0], out[4][0]);
  EXPECT_EQ(requests[1], out[4][1]);
  EXPECT_TRUE(out[3].empty());
  EXPECT_NE(std::string::npos, rt->GetDesc().find("neighbor requests punt to gate 4"))
      << rt->GetDesc();

  arg.set_punt_neighbor_gate(DROP_GATE);
  auto *dropping = dynamic_cast<Router *>(h_.Create("Router", arg, "rt_drop"));
  ASSERT_NE(nullptr, dropping);
  Module *writer2 = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer2, 0, dropping, 0, true));
  out = h_.RunPipeline(writer2, 0, requests, dropping, {0, 1, 3, 4});
  EXPECT_TRUE(out[3].empty());
  EXPECT_TRUE(out[4].empty());
  EXPECT_EQ(2u, dropping->neighbor_requests());

  arg.set_punt_neighbor_gate(MAX_GATES + 1);
  EXPECT_NE("", h_.CreateError("Router", arg)) << "not a gate";
}

// In the next-hop-id mode, interface addresses and bindings of either family
// are accepted, and the neighbor protocols read follow the interfaces'
// configured address families: an IPv6-only interface set reads no ARP.
TEST_F(RouterTest, TheNextHopModeReadsTheInterfacesFamilies) {
  bess::pb::RouterArg arg = ResolverArg(3);
  auto *dual = dynamic_cast<Router *>(h_.Create("Router", arg, "dual"));
  ASSERT_NE(nullptr, dual);
  EXPECT_EQ("", BindNeighbor("dual", 1, 2, "192.0.2.2"));
  EXPECT_EQ("", BindNeighbor("dual", 2, 2, "fe80::2"));

  arg.mutable_interfaces(0)->clear_addresses();
  arg.mutable_interfaces(0)->add_addresses("fe80::1/64");
  auto *v6 = dynamic_cast<Router *>(h_.Create("Router", arg, "v6"));
  ASSERT_NE(nullptr, v6);
  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, v6, 0, true));
  const std::vector<Bytes> requests = NeighborRequests();
  auto out = h_.RunPipeline(writer, 0, requests, v6, {0, 1, 3});
  ASSERT_EQ(1u, out[3].size()) << "only the Neighbor Solicitation is a request here";
  EXPECT_EQ(requests[1], out[3][0]);
  EXPECT_EQ(1u, v6->neighbor_requests());
}

// A reply on an interface the resolver does not have is dropped and counted
// when it arrives, never queued for the resolver.
TEST_F(RouterTest, AReplyOnAnUnknownInterfaceIsDroppedAtInput) {
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", ResolverArg(0), "rt"));
  ASSERT_NE(nullptr, rt);
  Module *writer = MakeNeighborInterfaceWriter(5);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  const Bytes reply = PacketBuilder()
                          .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                          .Arp(2, "02:00:00:00:00:02", "192.0.2.2",
                               "02:00:00:00:00:01", "192.0.2.1")
                          .PadTo(60)
                          .Build();
  (void)h_.RunPipeline(writer, 0, {reply}, rt, {0, 1});
  EXPECT_EQ(1u, rt->neighbor_input_drops());
  EXPECT_EQ(0u, rt->neighbor_requests());
}

// An explicit 0 reachable lifetime is the library's "never age" (field
// presence, not zero, selects the default): a resolved neighbor is not
// solicited again however long it goes unconfirmed.
TEST_F(RouterTest, AnExplicitZeroReachableLifetimeDisablesAging) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.set_neighbor_reachable_lifetime_ms(0);
  auto *rt = CreateResolver(arg);
  ASSERT_NE(nullptr, rt);
  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  ASSERT_EQ(1u, h_.RunTask(rt, {0, 1})[1].size());
  const Bytes reply = PacketBuilder()
                          .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                          .Arp(2, "02:00:00:00:00:02", "192.0.2.2",
                               "02:00:00:00:00:01", "192.0.2.1")
                          .PadTo(60)
                          .Build();
  (void)h_.RunPipeline(writer, 0, {reply}, rt, {0});
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 2);
  ASSERT_EQ(bess::route::NeighborState::kResolved,
            rt->router()->LookupNextHop(bess::route::NextHopId(1))->neighbor);
  // Past the 30000-tick default: no solicitation, still resolved.
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1),
                                              40000);
  EXPECT_TRUE(h_.RunTask(rt, {0, 1})[1].empty());
  EXPECT_EQ(bess::route::NeighborState::kResolved,
            rt->router()->LookupNextHop(bess::route::NextHopId(1))->neighbor);
}

// One neighbor change can be many next-hop updates (every hop bound to it).
// A tick publishes a bounded number of them and leaves the rest pending for
// the next tick, so a large change never becomes one transaction the engine
// refuses on every tick; nothing is lost.
TEST_F(RouterTest, NeighborPublicationIsBoundedPerTickAndConverges) {
  constexpr uint32_t kHops = 70;  // more than one tick publishes (64)
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.set_neighbor_probe_interval_ms(1);
  arg.set_neighbor_max_probes(1);
  arg.set_neighbor_unreachable_retry_ms(0);  // stays unreachable once failed
  auto *rt = CreateResolver(arg, kHops);
  ASSERT_NE(nullptr, rt);
  const auto unreachable = [rt] {
    uint32_t n = 0;
    for (uint32_t id = 1; id <= kHops; id++) {
      const auto *hop = rt->router()->LookupNextHop(bess::route::NextHopId(id));
      n += hop != nullptr &&
           hop->neighbor == bess::route::NeighborState::kUnreachable;
    }
    return n;
  };
  // Tick 1 solicits; tick 2 fails the one neighbor and so all its hops.
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(100), 1);
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(100), 2);
  EXPECT_EQ(kHops - 64, rt->pending_neighbor_updates());
  EXPECT_EQ(64u, unreachable());
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(100), 3);
  EXPECT_EQ(0u, rt->pending_neighbor_updates());
  EXPECT_EQ(kHops, unreachable());
}

// A destination-mode Router does not declare the next-hop attribute, so a
// path into it with no next-hop writer (its ARP/ND input, say) leaves that
// attribute's scope valid for the next-hop readers sharing the graph.
TEST_F(RouterTest, ADestinationRouterDoesNotDeclareTheNextHopAttribute) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.MergeFrom(DualStackArg());
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  std::vector<std::string> names;
  for (const auto &attr : rt->all_attrs()) {
    names.push_back(attr.name);
  }
  EXPECT_EQ((std::vector<std::string>{"route_domain", "vif_id"}),
            names);
}

// Neighbor resolution runs on the maintenance loop's ticks: with the loop off
// nothing would ever be solicited, so Init refuses instead of leaving every
// bound hop unresolved.
TEST_F(RouterTest, NeighborResolutionWithoutTheMaintenanceLoopIsRefused) {
  bess::runtime::runtime().ticks().set_interval(std::chrono::microseconds(0));
  EXPECT_NE("", h_.CreateError("Router", ResolverArg(0)));
}

// Timings are milliseconds, turned into ticks from the loop's interval and
// counted from the tick that takes a binding up, not from daemon start.
TEST_F(RouterTest, NeighborTimingsAreMillisecondsFromTheBinding) {
  bess::runtime::runtime().ticks().set_interval(std::chrono::milliseconds(10));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::nanoseconds(0), 100);
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.set_neighbor_first_probe_ms(25);  // 2.5 ticks, rounded up to 3
  auto *rt = CreateResolver(arg);
  ASSERT_NE(nullptr, rt);
  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  // Tick 101 takes the binding up; its first probe is due at 104.
  for (uint64_t tick = 101; tick <= 103; tick++) {
    (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), tick);
    EXPECT_TRUE(h_.RunTask(rt, {0, 1})[1].empty()) << "not yet 25 ms, at " << tick;
  }
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 104);
  EXPECT_EQ(1u, h_.RunTask(rt, {0, 1})[1].size());
}

// Zero has one meaning per timing field: immediate for the first probe,
// "never" for the lifetimes, and refused where it has no sensible meaning.
TEST_F(RouterTest, NeighborTimingZerosAreRefusedWhereMeaningless) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.set_neighbor_probe_interval_ms(0);
  EXPECT_NE("", h_.CreateError("Router", arg));
  arg = ResolverArg(0);
  arg.set_neighbor_max_probe_interval_ms(0);
  EXPECT_NE("", h_.CreateError("Router", arg));
  arg = ResolverArg(0);
  arg.set_neighbor_max_probes(0);
  EXPECT_NE("", h_.CreateError("Router", arg));
  arg = ResolverArg(0);
  arg.set_neighbor_probe_interval_ms(2000);
  arg.set_neighbor_max_probe_interval_ms(1000);
  EXPECT_NE("", h_.CreateError("Router", arg)) << "ceiling below the interval";
  arg = ResolverArg(0);
  arg.set_neighbor_first_probe_ms(0);
  arg.set_neighbor_reachable_lifetime_ms(0);
  arg.set_neighbor_unreachable_retry_ms(0);
  auto *rt = CreateResolver(arg);
  ASSERT_NE(nullptr, rt);
  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 0);
  EXPECT_EQ(1u, h_.RunTask(rt, {0, 1})[1].size()) << "first probe at once";
}

// The ceiling is compared with the interval as configured, in milliseconds:
// with a 10 ms tick both of these round up to 101 ticks, and comparing ticks
// would have taken a ceiling below the interval.
TEST_F(RouterTest, AProbeCeilingBelowTheIntervalIsRefusedBeforeRounding) {
  bess::runtime::runtime().ticks().set_interval(std::chrono::milliseconds(10));
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.set_neighbor_probe_interval_ms(1009);
  arg.set_neighbor_max_probe_interval_ms(1001);
  EXPECT_NE("", h_.CreateError("Router", arg));
}

// The largest accepted duration rounds up to ticks without wrapping: with a
// 1 ms tick, adding the rounding term to its microseconds would overflow to a
// few ticks, and a first probe meant for the far future would go out at once.
TEST_F(RouterTest, TheLargestDurationDoesNotWrapWhenRounded) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.set_neighbor_first_probe_ms(std::numeric_limits<uint64_t>::max() / 1000);
  auto *rt = CreateResolver(arg);
  ASSERT_NE(nullptr, rt);
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1000);
  EXPECT_TRUE(h_.RunTask(rt, {0, 1})[1].empty());
  arg.set_neighbor_first_probe_ms(std::numeric_limits<uint64_t>::max() / 1000 + 1);
  EXPECT_NE("", h_.CreateError("Router", arg)) << "past the bound is refused";
}

// An interface id of 0 is derived from its gate (egress_gate + 1); any other
// id must equal that, and the MAC is the "aa:bb:cc:dd:ee:ff" text form.
TEST_F(RouterTest, AnInterfaceIdIsDerivedOrMustMatchItsGate) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.mutable_interfaces(0)->set_id(0);
  EXPECT_NE(nullptr, h_.Create("Router", arg, "derived"));
  arg.mutable_interfaces(0)->set_id(3);
  EXPECT_NE("", h_.CreateError("Router", arg)) << "gate 1 is interface 2";
  arg = ResolverArg(0);
  arg.mutable_interfaces(0)->set_mac_addr("02:00:00:00:00");
  EXPECT_NE("", h_.CreateError("Router", arg));
  arg.mutable_interfaces(0)->set_mac_addr("00:00:00:00:00:00");
  EXPECT_NE("", h_.CreateError("Router", arg));
}

// In destination mode, a disabled family's interface addresses are refused at
// Init, and its neighbor bindings at the resource: the router could never use
// them. So is a binding whose interface has no address of its family to
// solicit from (in either mode).
TEST_F(RouterTest, ADisabledFamilysAddressesAreRefused) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.MergeFrom(DualStackArg());
  arg.clear_max_ipv6_routes();
  arg.clear_max_ipv6_tbl8s();
  EXPECT_NE("", h_.CreateError("Router", arg)) << "IPv6 interface address";
  arg.mutable_interfaces(0)->clear_addresses();
  arg.mutable_interfaces(0)->add_addresses("192.0.2.1/24");
  ASSERT_NE(nullptr, h_.Create("Router", arg, "v4only"));
  EXPECT_NE("", BindNeighbor("v4only", 1, 2, "fe80::2")) << "IPv6 neighbor binding";
  EXPECT_EQ("", BindNeighbor("v4only", 1, 2, "192.0.2.2"));

  arg = ResolverArg(0);
  arg.MergeFrom(DualStackArg());
  arg.clear_max_ipv4_routes();
  arg.clear_max_ipv4_tbl8s();
  EXPECT_NE("", h_.CreateError("Router", arg)) << "IPv4 interface address";
  arg.mutable_interfaces(0)->clear_addresses();
  arg.mutable_interfaces(0)->add_addresses("fe80::1/64");
  ASSERT_NE(nullptr, h_.Create("Router", arg, "v6only"));
  EXPECT_NE("", BindNeighbor("v6only", 1, 2, "192.0.2.2")) << "IPv4 neighbor binding";

  arg = ResolverArg(0);
  arg.mutable_interfaces(0)->clear_addresses();
  arg.mutable_interfaces(0)->add_addresses("fe80::1/64");
  ASSERT_NE(nullptr, h_.Create("Router", arg, "nov4"));
  EXPECT_NE("", BindNeighbor("nov4", 1, 2, "192.0.2.2")) << "no IPv4 address there";
  EXPECT_NE("", BindNeighbor("nov4", 1, 3, "fe80::2")) << "no such interface";
  EXPECT_NE("", BindNeighbor("nov4", 1, 2, "fe80::1")) << "its own address";
}

// Interface addresses are CIDR, any number of each family: the prefix length
// is required and bounded by the family, an interface needs one address, and
// a repeated address is refused.
TEST_F(RouterTest, InterfaceAddressesAreCidrAndRefusedWhenMalformed) {
  for (const char *bad : {"192.0.2.1", "192.0.2.1/", "192.0.2.1/33", "192.0.2.1/2x",
                          "2001:db8::1/129", "not-an-address/24", "0.0.0.0/8",
                          "ff02::1/64"}) {
    bess::pb::RouterArg arg = ResolverArg(0);
    arg.mutable_interfaces(0)->add_addresses(bad);
    EXPECT_NE("", h_.CreateError("Router", arg)) << bad;
  }
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.mutable_interfaces(0)->add_addresses("192.0.2.1/16");
  EXPECT_NE("", h_.CreateError("Router", arg)) << "repeated address";
  arg = ResolverArg(0);
  arg.mutable_interfaces(0)->clear_addresses();
  EXPECT_NE("", h_.CreateError("Router", arg)) << "no address";
}

// A solicitation leaves from the interface address whose prefix holds the
// neighbor: a neighbor on a secondary subnet is asked from that subnet's
// address, a global IPv6 neighbor from the global address.
TEST_F(RouterTest, ASolicitationLeavesFromTheAddressOnTheNeighborsPrefix) {
  bess::pb::RouterArg arg = NextHopArg();
  auto *interface = arg.add_interfaces();
  interface->set_egress_gate(1);
  interface->set_mac_addr("02:00:00:00:00:01");
  for (const char *address :
       {"192.0.2.1/24", "198.51.100.1/24", "fe80::1/64", "2001:db8::1/64"}) {
    interface->add_addresses(address);
  }
  arg.set_neighbor_first_probe_ms(0);
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  ASSERT_EQ("", BindNeighbor("rt", 1, 2, "198.51.100.2"));
  ASSERT_EQ("", BindNeighbor("rt", 2, 2, "2001:db8::2"));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  const auto probes = h_.RunTask(rt, {0, 1});
  ASSERT_EQ(2u, probes.at(1).size());
  bool saw_arp = false;
  bool saw_ns = false;
  for (const Bytes &probe : probes.at(1)) {
    if (const auto arp = bess::route::ParseArp(probe)) {
      saw_arp = true;
      EXPECT_EQ("198.51.100.1", bess::utils::ToIpv4Address(arp->sender_ip));
    } else if (const auto ns = bess::route::ParseNeighborDiscovery(probe)) {
      saw_ns = true;
      EXPECT_EQ("2001:db8::1", bess::utils::ToIpv6Address(ns->source_ip));
    }
  }
  EXPECT_TRUE(saw_arp);
  EXPECT_TRUE(saw_ns);
}

// An ARP frame `op` (1 request, 2 reply) from 192.0.2.2 at `sender_mac`,
// sent to `eth_dst`, naming `target_ip`.
Bytes ArpFrom(int op, const char *sender_mac, const char *eth_dst,
              const char *target_ip) {
  return PacketBuilder()
      .Ether(eth_dst, sender_mac)
      .Arp(op, sender_mac, "192.0.2.2",
           op == 2 ? "02:00:00:00:00:01" : "00:00:00:00:00:00", target_ip)
      .PadTo(60)
      .Build();
}

// A gratuitous ARP from a bound neighbor moves its hop to the announced MAC
// at once (and the request still reaches the responder's gate); one from an
// address nobody bound creates nothing.
TEST_F(RouterTest, AGratuitousArpMovesABoundNeighbor) {
  auto *rt = CreateResolver(ResolverArg(3));
  ASSERT_NE(nullptr, rt);
  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  ASSERT_EQ(1u, h_.RunTask(rt, {0, 1})[1].size());
  (void)h_.RunPipeline(writer, 0,
                       {ArpFrom(2, "02:00:00:00:00:aa", "02:00:00:00:00:01",
                                "192.0.2.1")},
                       rt, {0});
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 2);
  ASSERT_EQ(Ethernet::Address(Mac("02:00:00:00:00:aa").data()),
            rt->router()->LookupNextHop(bess::route::NextHopId(1))->dst_mac);

  const Bytes announce =
      ArpFrom(1, "02:00:00:00:00:bb", "ff:ff:ff:ff:ff:ff", "192.0.2.2");
  auto out = h_.RunPipeline(writer, 0, {announce}, rt, {0, 1, 3});
  ASSERT_EQ(1u, out[3].size()) << "the responder still sees the request";
  EXPECT_EQ(announce, out[3][0]);
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 3);
  const auto *moved = rt->router()->LookupNextHop(bess::route::NextHopId(1));
  ASSERT_NE(nullptr, moved);
  EXPECT_EQ(bess::route::NeighborState::kResolved, moved->neighbor);
  EXPECT_EQ(Ethernet::Address(Mac("02:00:00:00:00:bb").data()), moved->dst_mac);

  const Bytes stranger = PacketBuilder()
                             .Ether("ff:ff:ff:ff:ff:ff", "02:00:00:00:00:cc")
                             .Arp(1, "02:00:00:00:00:cc", "192.0.2.9",
                                  "00:00:00:00:00:00", "192.0.2.9")
                             .PadTo(60)
                             .Build();
  (void)h_.RunPipeline(writer, 0, {stranger}, rt, {0, 1, 3});
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 4);
  EXPECT_NE(std::string::npos, rt->GetDesc().find(", 1 neighbors")) << rt->GetDesc();
}

// A broadcast ARP reply from a bound neighbor resolves it, like a unicast one.
TEST_F(RouterTest, ABroadcastArpReplyResolvesABoundNeighbor) {
  auto *rt = CreateResolver(ResolverArg(0));
  ASSERT_NE(nullptr, rt);
  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  ASSERT_EQ(1u, h_.RunTask(rt, {0, 1})[1].size());
  (void)h_.RunPipeline(writer, 0,
                       {ArpFrom(2, "02:00:00:00:00:aa", "ff:ff:ff:ff:ff:ff",
                                "192.0.2.1")},
                       rt, {0});
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 2);
  const auto *hop = rt->router()->LookupNextHop(bess::route::NextHopId(1));
  ASSERT_NE(nullptr, hop);
  EXPECT_EQ(bess::route::NeighborState::kResolved, hop->neighbor);
  EXPECT_EQ(Ethernet::Address(Mac("02:00:00:00:00:aa").data()), hop->dst_mac);
}

// -- "<module>/neighbors" ----------------------------------------------------

// Bindings come and go at runtime: a binding publishes its hop at once,
// incomplete, and the next tick starts soliciting; an unbind takes the hop
// away and stops the solicitations; a rebind moves the hop to another
// neighbor. The resource reads back as written.
TEST_F(RouterTest, NeighborBindingsComeAndGoAtRuntime) {
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", ResolverArg(0), "rt"));
  ASSERT_NE(nullptr, rt);
  EXPECT_EQ(nullptr, rt->router()->LookupNextHop(bess::route::NextHopId(5)));
  ASSERT_EQ("", BindNeighbor("rt", 5, 2, "192.0.2.7"));
  const auto *bound = rt->router()->LookupNextHop(bess::route::NextHopId(5));
  ASSERT_NE(nullptr, bound);
  EXPECT_EQ(bess::route::NeighborState::kIncomplete, bound->neighbor);
  EXPECT_EQ(bess::dataplane::InterfaceId(2), bound->egress);
  bess::pb::RouterNextHopIdKey key;
  key.set_id(5);
  bess::pb::RouterNeighborBinding written;
  written.set_interface_id(2);
  written.set_ipv4("192.0.2.7");
  EXPECT_EQ("", h_.ReadBackDiffers("rt/neighbors", {{&key, &written}}));
  EXPECT_EQ("", h_.ReadBackDiffers("rt/next_hops", {})) << "a bound hop is not static";

  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  auto probes = h_.RunTask(rt, {0, 1});
  ASSERT_EQ(1u, probes[1].size());
  EXPECT_EQ("192.0.2.7",
            bess::utils::ToIpv4Address(bess::route::ParseArp(probes[1][0])->target_ip));

  // A rebind to an IPv6 neighbor on the same id.
  ASSERT_EQ("", BindNeighbor("rt", 5, 2, "fe80::7"));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 2);
  probes = h_.RunTask(rt, {0, 1});
  ASSERT_EQ(1u, probes[1].size());
  EXPECT_TRUE(bess::route::ParseNeighborDiscovery(probes[1][0]).has_value());

  ASSERT_EQ("", UnbindNeighbor("rt", 5));
  EXPECT_EQ("", h_.ReadBackDiffers("rt/neighbors", {}));
  while (bess::runtime::runtime().transactions().ReclaimRetired() != 0) {
  }
  EXPECT_EQ(nullptr, rt->router()->LookupNextHop(bess::route::NextHopId(5)));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 5000);
  EXPECT_TRUE(h_.RunTask(rt, {0, 1})[1].empty()) << "an unbound neighbor is not solicited";
  EXPECT_NE("", UnbindNeighbor("rt", 5)) << "not found";
}

// A route or a group naming a bound hop references the binding: the unbind is
// refused while either does, and allowed once neither does.
TEST_F(RouterTest, AnUnbindIsRefusedWhileARouteOrGroupNamesTheHop) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.MergeFrom(DualStackArg());
  arg.set_max_groups(4);
  auto *rt = CreateResolver(arg);
  ASSERT_NE(nullptr, rt);
  bess::pb::RouterRouteKey route;
  route.set_ipv4("198.51.100.0");
  route.set_prefix_length(24);
  bess::pb::RouterRouteValue to_bound;
  to_bound.set_next_hop_id(1);
  ASSERT_EQ("", h_.UpsertResource("rt/routes", route, to_bound));
  EXPECT_NE("", UnbindNeighbor("rt", 1)) << "a route names it";
  ASSERT_EQ("", h_.EraseResource("rt/routes", route));

  bess::pb::RouterGroupIdKey group_key;
  group_key.set_id(1);
  bess::pb::RouterGroupValue group;
  group.add_member_ids(1);
  ASSERT_EQ("", h_.UpsertResource("rt/groups", group_key, group));
  EXPECT_NE("", UnbindNeighbor("rt", 1)) << "a group names it";
  ASSERT_EQ("", h_.EraseResource("rt/groups", group_key));
  EXPECT_EQ("", UnbindNeighbor("rt", 1));
}

// An id is static or bound, never both: each resource refuses the other's.
TEST_F(RouterTest, StaticAndBoundNextHopIdsAreDisjoint) {
  auto *rt = CreateResolver(ResolverArg(0));
  ASSERT_NE(nullptr, rt);
  bess::pb::RouterNextHopIdKey key;
  key.set_id(3);
  bess::pb::RouterNextHopValue hop;
  hop.set_egress_gate(1);
  ASSERT_EQ("", h_.UpsertResource("rt/next_hops", key, hop));
  EXPECT_NE("", BindNeighbor("rt", 3, 2, "192.0.2.3")) << "a static id";
  key.set_id(1);
  EXPECT_NE("", h_.UpsertResource("rt/next_hops", key, hop)) << "a bound id";
  EXPECT_NE("", h_.EraseResource("rt/next_hops", key)) << "a bound id";
}

// A binding, the route to it and a refused operation in one transaction: the
// whole transaction aborts, so neither the hop nor the binding appears, and
// the next tick has nothing to resolve. The same binding and route without
// the refused operation apply together.
TEST_F(RouterTest, ABindingInAnAbortedTransactionLeavesNothing) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.MergeFrom(DualStackArg());
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  const bess::route::Router &router = *rt->router();
  const auto prefix = bess::route::Ipv4Prefix::Make(0xc6336400, 24).value();
  const bess::route::NeighborKey neighbor(bess::dataplane::InterfaceId(2), 0xc0000207);
  std::vector<bess::dataplane::Op> ops = {
      router.BindNeighborOp(bess::route::NextHopId(5), neighbor),
      router.SetRouteOp(bess::route::kDefaultRouteDomainId, prefix,
                        bess::route::NextHopId(5)),
      router.SetRouteOp(bess::route::kDefaultRouteDomainId,
                        bess::route::Ipv4Prefix::Make(0xcb007100, 24).value(),
                        bess::route::NextHopId(9)),  // names nothing: refused
  };
  EXPECT_NE("", h_.Apply(ops));
  EXPECT_EQ(nullptr, router.LookupNextHop(bess::route::NextHopId(5)));
  EXPECT_FALSE(router.IsNeighborBound(bess::route::NextHopId(5)));
  EXPECT_EQ(0u, router.route_count());
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  EXPECT_TRUE(h_.RunTask(rt, {0, 1})[1].empty()) << "nothing to resolve";

  ops.pop_back();
  ASSERT_EQ("", h_.Apply(ops));
  EXPECT_TRUE(router.IsNeighborBound(bess::route::NextHopId(5)));
  EXPECT_EQ(1u, router.route_count());
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 2);
  EXPECT_EQ(1u, h_.RunTask(rt, {0, 1})[1].size());
}

// Without interfaces there is no resolver, and nothing can be bound.
TEST_F(RouterTest, ARouterWithoutInterfacesBindsNothing) {
  ASSERT_NE(nullptr, h_.Create("Router", NextHopArg(), "rt"));
  EXPECT_NE("", BindNeighbor("rt", 1, 2, "192.0.2.2"));
}

// get_neighbors reports every binding in id order: before the tick takes them
// up, incomplete with no probes; after the first solicitation, one probe; and
// once a reply resolves the IPv4 neighbor, resolved with its MAC.
TEST_F(RouterTest, GetNeighborsReportsEachBindingsResolution) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.mutable_interfaces(0)->add_addresses("2001:db8::1/64");
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  ASSERT_EQ("", BindNeighbor("rt", 2, 2, "2001:db8::2"));
  ASSERT_EQ("", BindNeighbor("rt", 1, 2, "192.0.2.2"));
  auto get = [&]() {
    CommandResponse got = h_.Action(rt, "get_neighbors", bess::pb::EmptyArg());
    EXPECT_EQ(0, got.error().code());
    bess::pb::RouterCommandGetNeighborsResponse response;
    EXPECT_TRUE(got.data().UnpackTo(&response));
    return response;
  };
  auto response = get();
  ASSERT_EQ(2, response.neighbors_size());
  EXPECT_EQ(1u, response.neighbors(0).next_hop_id());
  EXPECT_EQ("192.0.2.2", response.neighbors(0).ipv4());
  EXPECT_EQ(2u, response.neighbors(1).next_hop_id());
  EXPECT_EQ("2001:db8::2", response.neighbors(1).ipv6());
  for (const auto &neighbor : response.neighbors()) {
    EXPECT_EQ(2u, neighbor.interface_id());
    EXPECT_EQ(bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE, neighbor.state());
    EXPECT_EQ(0u, neighbor.probes());
    EXPECT_EQ("", neighbor.mac_addr());
  }

  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  ASSERT_EQ(2u, h_.RunTask(rt, {0, 1})[1].size());
  response = get();
  for (const auto &neighbor : response.neighbors()) {
    EXPECT_EQ(bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE, neighbor.state());
    EXPECT_EQ(1u, neighbor.probes());
  }

  const std::string local_mac = arg.interfaces(0).mac_addr();
  constexpr char kPeerMac[] = "02:00:00:00:00:02";
  const Bytes reply = PacketBuilder()
                          .Ether(local_mac, kPeerMac)
                          .Arp(2, kPeerMac, "192.0.2.2", local_mac, "192.0.2.1")
                          .PadTo(60)
                          .Build();
  (void)h_.RunPipeline(writer, 0, {reply}, rt, {0});
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 2);
  response = get();
  EXPECT_EQ(bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED, response.neighbors(0).state());
  EXPECT_EQ(Ethernet::Address(Mac(kPeerMac).data()).ToString(),
            response.neighbors(0).mac_addr());
  EXPECT_FALSE(response.neighbors(0).reconfirming());
  EXPECT_EQ(bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE, response.neighbors(1).state());
}

// The IPv4 address an ARP solicitation asks for.
std::string ArpTarget(const Bytes &probe) {
  const auto arp = bess::route::ParseArp(probe);
  return arp ? bess::utils::ToIpv4Address(arp->target_ip) : "";
}

// At max_neighbors, one transaction that unbinds a hop and binds another
// publishes the bind before the unbind; the tick still frees the resolver's
// room first, so the new neighbor is taken up and solicited.
TEST_F(RouterTest, AnUnbindAndABindInOneTransactionAtCapacityResolveTheNewNeighbor) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.set_max_neighbors(1);
  auto *rt = CreateResolver(arg);  // id 1 bound to 192.0.2.2
  ASSERT_NE(nullptr, rt);
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  ASSERT_EQ(1u, h_.RunTask(rt, {0, 1})[1].size());

  const bess::route::Router &router = *rt->router();
  const std::vector<bess::dataplane::Op> ops = {
      router.UnbindNeighborOp(bess::route::NextHopId(1)),
      router.BindNeighborOp(
          bess::route::NextHopId(2),
          bess::route::NeighborKey(bess::dataplane::InterfaceId(2), 0xc0000203)),
  };
  ASSERT_EQ("", h_.Apply(ops));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 2);
  const auto probes = h_.RunTask(rt, {0, 1});
  ASSERT_EQ(1u, probes.at(1).size());
  EXPECT_EQ("192.0.2.3", ArpTarget(probes.at(1)[0]));
  EXPECT_NE(std::string::npos, rt->GetDesc().find(", 1 neighbors")) << rt->GetDesc();
}

// Several transactions change one id before a tick: the tick applies only the
// final binding, so the intermediate neighbors are never solicited and never
// take the resolver's room.
TEST_F(RouterTest, SeveralTransactionsBeforeATickApplyTheFinalBinding) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.set_max_neighbors(1);
  auto *rt = CreateResolver(arg);  // id 1 bound to 192.0.2.2
  ASSERT_NE(nullptr, rt);
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  ASSERT_EQ(1u, h_.RunTask(rt, {0, 1})[1].size());

  ASSERT_EQ("", BindNeighbor("rt", 1, 2, "192.0.2.3"));
  ASSERT_EQ("", UnbindNeighbor("rt", 1));
  ASSERT_EQ("", BindNeighbor("rt", 2, 2, "192.0.2.4"));
  ASSERT_EQ("", UnbindNeighbor("rt", 2));
  ASSERT_EQ("", BindNeighbor("rt", 3, 2, "192.0.2.5"));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 2);
  const auto probes = h_.RunTask(rt, {0, 1});
  ASSERT_EQ(1u, probes.at(1).size());
  EXPECT_EQ("192.0.2.5", ArpTarget(probes.at(1)[0]));

  CommandResponse got = h_.Action(rt, "get_neighbors", bess::pb::EmptyArg());
  bess::pb::RouterCommandGetNeighborsResponse response;
  ASSERT_TRUE(got.data().UnpackTo(&response));
  ASSERT_EQ(1, response.neighbors_size());
  EXPECT_EQ(3u, response.neighbors(0).next_hop_id());
  EXPECT_EQ(1u, response.neighbors(0).probes());
}

// The last unbind forgets what was learned: binding the same address again
// starts incomplete and solicits, rather than republishing an old MAC.
TEST_F(RouterTest, ARebindOfAForgottenNeighborStartsIncompleteAndSolicits) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.set_neighbor_reachable_lifetime_ms(0);  // a kept MAC would never be rechecked
  auto *rt = CreateResolver(arg);  // id 1 bound to 192.0.2.2
  ASSERT_NE(nullptr, rt);
  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  ASSERT_EQ(1u, h_.RunTask(rt, {0, 1})[1].size());
  (void)h_.RunPipeline(writer, 0,
                       {ArpFrom(2, "02:00:00:00:00:aa", "02:00:00:00:00:01",
                                "192.0.2.1")},
                       rt, {0});
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 2);
  ASSERT_EQ(bess::route::NeighborState::kResolved,
            rt->router()->LookupNextHop(bess::route::NextHopId(1))->neighbor);

  ASSERT_EQ("", UnbindNeighbor("rt", 1));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 3);
  EXPECT_NE(std::string::npos, rt->GetDesc().find(", 0 neighbors")) << rt->GetDesc();

  ASSERT_EQ("", BindNeighbor("rt", 2, 2, "192.0.2.2"));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 4);
  const auto *rebound = rt->router()->LookupNextHop(bess::route::NextHopId(2));
  ASSERT_NE(nullptr, rebound);
  EXPECT_EQ(bess::route::NeighborState::kIncomplete, rebound->neighbor);
  EXPECT_TRUE(rebound->dst_mac.IsZero());
  const auto probes = h_.RunTask(rt, {0, 1});
  ASSERT_EQ(1u, probes.at(1).size());
  EXPECT_EQ("192.0.2.2", ArpTarget(probes.at(1)[0]));
}

// A resolved neighbor moved to another id in one transaction keeps what was
// learned: the new id is installed before the old one is released, so the
// neighbor is never forgotten, and it publishes resolved without a probe.
TEST_F(RouterTest, ANeighborMovedToAnotherIdStaysResolved) {
  auto *rt = CreateResolver(ResolverArg(0));  // id 1 bound to 192.0.2.2
  ASSERT_NE(nullptr, rt);
  Module *writer = MakeNeighborInterfaceWriter(2);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 1);
  ASSERT_EQ(1u, h_.RunTask(rt, {0, 1})[1].size());
  (void)h_.RunPipeline(writer, 0,
                       {ArpFrom(2, "02:00:00:00:00:aa", "02:00:00:00:00:01",
                                "192.0.2.1")},
                       rt, {0});
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 2);
  ASSERT_EQ(bess::route::NeighborState::kResolved,
            rt->router()->LookupNextHop(bess::route::NextHopId(1))->neighbor);

  const bess::route::Router &router = *rt->router();
  const std::vector<bess::dataplane::Op> ops = {
      router.UnbindNeighborOp(bess::route::NextHopId(1)),
      router.BindNeighborOp(
          bess::route::NextHopId(2),
          bess::route::NeighborKey(bess::dataplane::InterfaceId(2), 0xc0000202)),
  };
  ASSERT_EQ("", h_.Apply(ops));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 3);
  const auto *moved = rt->router()->LookupNextHop(bess::route::NextHopId(2));
  ASSERT_NE(nullptr, moved);
  EXPECT_EQ(bess::route::NeighborState::kResolved, moved->neighbor);
  EXPECT_EQ("02:00:00:00:00:aa", moved->dst_mac.ToString());
  EXPECT_TRUE(h_.RunTask(rt, {0, 1})[1].empty()) << "a moved neighbor is not probed";
  EXPECT_NE(std::string::npos, rt->GetDesc().find(", 1 neighbors")) << rt->GetDesc();
}

// Renumbering in one tick keeps every neighbor that is still named, whatever
// the id order: id 1 moves from A to B (leaving A unbound for a moment) while
// a higher id 3 takes A. Forgetting waits until every move is installed, so
// A and B both stay resolved, on their MACs, and neither is probed.
TEST_F(RouterTest, AChainOfMovesInOneTickKeepsEveryNeighborResolved) {
  Router *rt = CreateTwoResolvedNeighbors(ResolverArg(0));
  ASSERT_NE(nullptr, rt);
  const bess::route::Router &router = *rt->router();
  const bess::route::NeighborKey a(bess::dataplane::InterfaceId(2), 0xc0000202);
  const bess::route::NeighborKey b(bess::dataplane::InterfaceId(2), 0xc0000203);
  ASSERT_EQ("", h_.Apply({
                    router.BindNeighborOp(bess::route::NextHopId(1), b),
                    router.BindNeighborOp(bess::route::NextHopId(3), a),
                }));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 3);
  ExpectResolved(rt, 1, "02:00:00:00:00:bb");
  ExpectResolved(rt, 2, "02:00:00:00:00:bb");
  ExpectResolved(rt, 3, "02:00:00:00:00:aa");
  EXPECT_TRUE(h_.RunTask(rt, {0, 1})[1].empty()) << "no neighbor is probed again";
  EXPECT_NE(std::string::npos, rt->GetDesc().find(", 2 neighbors")) << rt->GetDesc();
}

// Two ids swap their neighbors in one transaction: both neighbors stay
// resolved on their MACs and neither is probed.
TEST_F(RouterTest, ASwapOfTwoNeighborsKeepsBothResolved) {
  Router *rt = CreateTwoResolvedNeighbors(ResolverArg(0));
  ASSERT_NE(nullptr, rt);
  const bess::route::Router &router = *rt->router();
  const bess::route::NeighborKey a(bess::dataplane::InterfaceId(2), 0xc0000202);
  const bess::route::NeighborKey b(bess::dataplane::InterfaceId(2), 0xc0000203);
  ASSERT_EQ("", h_.Apply({
                    router.BindNeighborOp(bess::route::NextHopId(1), b),
                    router.BindNeighborOp(bess::route::NextHopId(2), a),
                }));
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 3);
  ExpectResolved(rt, 1, "02:00:00:00:00:bb");
  ExpectResolved(rt, 2, "02:00:00:00:00:aa");
  EXPECT_TRUE(h_.RunTask(rt, {0, 1})[1].empty()) << "no neighbor is probed again";
  EXPECT_NE(std::string::npos, rt->GetDesc().find(", 2 neighbors")) << rt->GetDesc();
}

// A neighbor released and taken back up in one tick resumes where its
// resolution stood: renumbered in a chain while it is being reconfirmed, it
// sends its next probe on schedule, reports its reconfirmation, and still
// fails closed once the probe budget runs out.
TEST_F(RouterTest, ARenumberedReconfirmingNeighborKeepsItsProbeSchedule) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.set_neighbor_probe_interval_ms(1);
  arg.set_neighbor_max_probe_interval_ms(1);
  arg.set_neighbor_max_probes(2);
  arg.set_neighbor_reachable_lifetime_ms(4);
  arg.set_neighbor_unreachable_retry_ms(0);
  Router *rt = CreateTwoResolvedNeighbors(arg);  // both resolved at tick 2
  ASSERT_NE(nullptr, rt);
  // Tick 6: both lifetimes are over; each is reconfirmed, its first probe out.
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 6);
  ASSERT_EQ(2u, h_.RunTask(rt, {0, 1})[1].size());

  const bess::route::Router &router = *rt->router();
  const bess::route::NeighborKey a(bess::dataplane::InterfaceId(2), 0xc0000202);
  const bess::route::NeighborKey b(bess::dataplane::InterfaceId(2), 0xc0000203);
  ASSERT_EQ("", h_.Apply({
                    router.BindNeighborOp(bess::route::NextHopId(1), b),
                    router.BindNeighborOp(bess::route::NextHopId(3), a),
                }));
  // Tick 7 takes the moves up, then sends both second probes on schedule.
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 7);
  std::set<std::string> targets;
  for (const Bytes &probe : h_.RunTask(rt, {0, 1})[1]) {
    targets.insert(ArpTarget(probe));
  }
  EXPECT_EQ((std::set<std::string>{"192.0.2.2", "192.0.2.3"}), targets);
  ExpectResolved(rt, 3, "02:00:00:00:00:aa");
  CommandResponse got = h_.Action(rt, "get_neighbors", bess::pb::EmptyArg());
  bess::pb::RouterCommandGetNeighborsResponse response;
  ASSERT_TRUE(got.data().UnpackTo(&response));
  bool saw_a = false;
  for (const auto &n : response.neighbors()) {
    if (n.next_hop_id() == 3) {
      saw_a = true;
      EXPECT_TRUE(n.reconfirming());
      EXPECT_EQ(2u, n.probes());
    }
  }
  EXPECT_TRUE(saw_a);
  // Tick 8: both probes unanswered, so the moved neighbor fails closed with
  // the other, not a whole reachable lifetime later.
  (void)bess::runtime::runtime().ticks().Run(std::chrono::milliseconds(1), 8);
  for (uint32_t id : {1u, 2u, 3u}) {
    const auto *hop = rt->router()->LookupNextHop(bess::route::NextHopId(id));
    ASSERT_NE(nullptr, hop);
    EXPECT_EQ(bess::route::NeighborState::kUnreachable, hop->neighbor) << "id " << id;
  }
}

// Ownership does not depend on request order: a route or a group listed
// before the new binding it names references that binding (so the unbind is
// refused while it does), and results stay in request order.
TEST_F(RouterTest, ARouteOrGroupListedBeforeItsBindingNamesTheBinding) {
  bess::pb::RouterArg arg = ResolverArg(0);
  arg.MergeFrom(DualStackArg());
  arg.set_max_groups(4);
  auto *rt = dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  ASSERT_NE(nullptr, rt);
  const bess::route::Router &router = *rt->router();
  const auto neighbor = [](uint32_t address) {
    return bess::route::NeighborKey(bess::dataplane::InterfaceId(2), address);
  };

  ASSERT_EQ("", h_.Apply({router.SetRouteOp(bess::route::kDefaultRouteDomainId,
                                            bess::route::Ipv4Prefix::Make(0xc6336400, 24).value(),
                                            bess::route::NextHopId(5)),
                          router.BindNeighborOp(bess::route::NextHopId(5),
                                                neighbor(0xc0000207))}))
      << "route first";
  EXPECT_TRUE(router.IsNeighborBound(bess::route::NextHopId(5)));
  EXPECT_EQ(1u, router.route_count());
  EXPECT_NE("", UnbindNeighbor("rt", 5)) << "the route references the binding";

  const bess::route::NextHopId members[] = {bess::route::NextHopId(6)};
  ASSERT_EQ("", h_.Apply({router.SetNextHopGroupOp(bess::route::NextHopGroupId(1), members),
                          router.BindNeighborOp(bess::route::NextHopId(6),
                                                neighbor(0xc0000208))}))
      << "group first";
  EXPECT_TRUE(router.IsNeighborBound(bess::route::NextHopId(6)));
  EXPECT_NE("", UnbindNeighbor("rt", 6)) << "the group references the binding";

  // A refused binding listed after the route that names it is reported at
  // its own position, though it was reserved first.
  const std::vector<bess::dataplane::Op> refused = {
      router.SetRouteOp(bess::route::kDefaultRouteDomainId,
                        bess::route::Ipv4Prefix::Make(0xcb007100, 24).value(),
                        bess::route::NextHopId(7)),
      router.BindNeighborOp(bess::route::NextHopId(7), neighbor(0xc0000201)),  // its own
  };
  const auto result = bess::runtime::runtime().transactions().Apply(refused);
  ASSERT_EQ(bess::dataplane::TransactionEngine::Outcome::kRejected, result.outcome);
  ASSERT_EQ(2u, result.ops.size());
  EXPECT_TRUE(result.ops[0].error.empty()) << result.ops[0].error;
  EXPECT_FALSE(result.ops[1].error.empty());
  EXPECT_FALSE(router.IsNeighborBound(bess::route::NextHopId(7)));
}

}  // namespace
