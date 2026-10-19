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
#include <map>
#include <span>
#include <string>
#include <vector>

#include "gate.h"
#include "module.h"
#include "module_graph.h"
#include "modules/router.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"
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

// The frame the tests send: the addresses it arrives with (a gNB's toward the
// N3 port, say), an IPv4 payload.
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

class RouterTest : public ::testing::Test {
 protected:
  // A router named "rt" with one next hop, written as a client writes it (the
  // codec path, so the MAC fields are the RPC's; a null `dst_mac`/`src_mac`
  // leaves the field out, which reads back as the zero address, D-129).
  Router *MakeRouter(uint64_t egress_gate, const char *dst_mac, const char *src_mac,
                     bess::pb::RouterNeighborState neighbor) {
    Router *rt = dynamic_cast<Router *>(h_.Create("Router", bess::pb::RouterArg(), "rt"));
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
  // a graph is what an ActionTable writes.
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
  // graph a deployment has (an ActionTable upstream of the Router).
  std::map<gate_idx_t, std::vector<Bytes>> Run(Router *rt, const Bytes &frame,
                                              std::vector<gate_idx_t> ogates = {0, 1}) {
    Module *writer = MakeWriter(1);
    EXPECT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
    return h_.RunPipeline(writer, 0, {frame}, rt, std::move(ogates));
  }

  // A router that routes by destination (D-155): the mode is configuration,
  // not a guess about the graph's wiring. `groups` sets max_groups (D-156).
  Router *MakeDestinationRouter(uint64_t domains = 1, uint64_t groups = 0) {
    bess::pb::RouterArg arg;
    arg.set_mode(bess::pb::ROUTE_MODE_DESTINATION);
    arg.set_max_domains(domains);
    if (groups != 0) {
      arg.set_max_groups(groups);
    }
    return dynamic_cast<Router *>(h_.Create("Router", arg, "rt"));
  }

  // A destination-mode router that punts what it cannot forward (D-162): a miss
  // to `miss_gate`, an unresolved neighbor to `unresolved_gate`.
  Router *MakePuntingRouter(uint64_t miss_gate, uint64_t unresolved_gate) {
    bess::pb::RouterArg arg;
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
  Module *MakeDomainWriter(uint32_t domain) {
    bess::pb::SetMetadataArg arg;
    bess::pb::SetMetadataArg::Attribute *attr = arg.add_attrs();
    attr->set_name("route_domain");
    attr->set_size(4);
    const uint8_t bytes[4] = {static_cast<uint8_t>(domain >> 24), static_cast<uint8_t>(domain >> 16),
                              static_cast<uint8_t>(domain >> 8), static_cast<uint8_t>(domain)};
    attr->set_value_bin(reinterpret_cast<const char *>(bytes), sizeof(bytes));
    return h_.Create("SetMetadata", arg, "vif" + std::to_string(next_writer_++));
  }

  // A frame in the domain it carries, through the VIF that writes it.
  std::map<gate_idx_t, std::vector<Bytes>> RunToDestination(Router *rt, const Bytes &frame,
                                                           uint32_t domain = 0,
                                                           std::vector<gate_idx_t> ogates = {0, 1}) {
    Module *vif = MakeDomainWriter(domain);
    EXPECT_EQ(0, ModuleGraph::ConnectModules(vif, 0, rt, 0, true));
    return h_.RunPipeline(vif, 0, {frame}, rt, std::move(ogates));
  }

  // Names for the metadata writers, one per call (see MakeWriter).
  int next_writer_ = 0;
  ModuleHarness h_;
};

// A hop that names both addresses: the frame leaves with them, on the gate the
// hop's egress names, and every other byte of the frame is untouched.
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
  EXPECT_TRUE(std::equal(frame.begin() + 12, frame.end(), got.begin() + 12))
      << "the ethertype and everything after it are the frame's own";
  EXPECT_EQ(0u, rt->header_write_failures());
}

// A hop that names no addresses: the frame keeps the header it arrived with.
// Every graph in the tree is this shape today, so this is the compatibility
// case, not an edge one.
TEST_F(RouterTest, AHopThatNamesNoAddressesIsForwardedUntouched) {
  Router *rt = MakeRouter(1, nullptr, nullptr, bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
  ASSERT_NE(nullptr, rt);
  const Bytes frame = Frame();
  auto out = Run(rt, frame);

  ASSERT_EQ(1u, out[1].size());
  EXPECT_EQ(frame, out[1][0]) << "byte for byte what arrived";
  EXPECT_EQ(0u, rt->header_write_failures());
}

// Half a pair is not a rewrite: writing the destination alone would put a zero
// source address on the wire.
TEST_F(RouterTest, AHopThatNamesOnlyOneAddressIsForwardedUntouched) {
  Router *rt = MakeRouter(1, "02:00:00:00:00:bb", nullptr,
                          bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
  ASSERT_NE(nullptr, rt);
  const Bytes frame = Frame();
  auto out = Run(rt, frame);

  ASSERT_EQ(1u, out[1].size());
  EXPECT_EQ(frame, out[1][0]);
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
// packet and does not consult the FIB it holds (D-155).
TEST_F(RouterTest, TheIdModeStillFailsClosedWithoutAWriter) {
  Router *rt = dynamic_cast<Router *>(h_.Create("Router", bess::pb::RouterArg(), "rt"));
  ASSERT_NE(nullptr, rt);
  EXPECT_FALSE(rt->routes_by_destination());
  MakeHop(rt, 1, 1, "02:00:00:00:00:bb", "02:00:00:00:00:aa");
  MakeRoute(rt, "10.0.0.0", 8, 0, 1);

  auto out = h_.RunPipeline(rt, 0, {FrameTo("10.1.2.3")}, rt, {0, 1});

  EXPECT_TRUE(out[0].empty());
  EXPECT_TRUE(out[1].empty()) << "dropped, not routed by destination";
  EXPECT_EQ(0u, rt->no_route_frames()) << "the FIB was never consulted";
}

TEST_F(RouterTest, AnUnknownModeIsRefused) {
  bess::pb::RouterArg arg;
  arg.set_mode(static_cast<bess::pb::RouteMode>(7));
  EXPECT_NE("", h_.CreateError("Router", arg));
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

// A punt gate that is not a gate this graph can have is refused at Init, like
// an unknown mode (D-162).
TEST_F(RouterTest, AnInvalidPuntGateIsRefused) {
  bess::pb::RouterArg arg;
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

// What destination routing cannot route: a frame that is not IP, one whose
// headers do not parse, and a non-initial fragment (its L4 header, and so the
// flow hash a group route needs, is in an earlier fragment).
TEST_F(RouterTest, FramesWithoutARoutableIpv4HeaderAreDroppedAndCounted) {
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

  for (const Bytes *frame : {&arp, &ipv6, &no_header, &fragment}) {
    auto out = RunToDestination(rt, *frame);
    EXPECT_TRUE(out[0].empty());
    EXPECT_TRUE(out[1].empty());
  }
  EXPECT_EQ(2u, rt->not_ipv4_frames()) << "the ARP frame and the IPv6 one";
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

}  // namespace
