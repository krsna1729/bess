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
#include <string>
#include <vector>

#include "module.h"
#include "module_graph.h"
#include "modules/router.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"
#include "utils/ether.h"

namespace {

using bess::testing::Bytes;
using bess::testing::Mac;
using bess::testing::ModuleHarness;
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
  Module *MakeWriter(uint32_t id) {
    bess::pb::SetMetadataArg arg;
    bess::pb::SetMetadataArg::Attribute *attr = arg.add_attrs();
    attr->set_name("next_hop_id");
    attr->set_size(4);
    const uint8_t bytes[4] = {static_cast<uint8_t>(id >> 24), static_cast<uint8_t>(id >> 16),
                              static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
    attr->set_value_bin(reinterpret_cast<const char *>(bytes), sizeof(bytes));
    return h_.Create("SetMetadata", arg, "writer");
  }

  // One frame through a writer that sets next_hop_id = 1, into `rt`: the
  // graph a deployment has (an ActionTable upstream of the Router).
  std::map<gate_idx_t, std::vector<Bytes>> Run(Router *rt, const Bytes &frame,
                                              std::vector<gate_idx_t> ogates = {0, 1}) {
    Module *writer = MakeWriter(1);
    EXPECT_EQ(0, ModuleGraph::ConnectModules(writer, 0, rt, 0, true));
    return h_.RunPipeline(writer, 0, {frame}, rt, std::move(ogates));
  }

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
  EXPECT_EQ(0u, rt->l2_rewrite_failures());
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
  EXPECT_EQ(0u, rt->l2_rewrite_failures());
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
  EXPECT_EQ(0u, rt->l2_rewrite_failures()) << "not a failure: the hop named nothing to write";
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
  EXPECT_EQ(0u, rt->l2_rewrite_failures());
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
  EXPECT_EQ(1u, rt->l2_rewrite_failures());
  EXPECT_NE(std::string::npos, rt->GetDesc().find("1 l2 rewrite failures"))
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
  EXPECT_EQ(0u, rt->l2_rewrite_failures()) << "a drop for another reason, not a failed write";
}

}  // namespace
