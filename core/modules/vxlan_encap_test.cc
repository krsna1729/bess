// SPDX-License-Identifier: BSD-3-Clause

// VXLANEncap in process: the UDP and VXLAN headers it prepends, the headroom
// of a shared packet, and the UDP length limit. A real metadata producer
// supplies the tunnel inputs; VNI 42 must reach the encapsulated frame.

#include <arpa/inet.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "module.h"
#include "module_graph.h"
#include "packet.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"
#include "tunnel/tunnel.h"

namespace {

using bess::testing::Bytes;
using bess::testing::FillHeadroom;
using bess::testing::HeadroomBytes;
using bess::testing::MakeChain;
using bess::testing::MakePacket;
using bess::testing::MakePacketWithoutHeadroom;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;
using bess::testing::PacketBytes;

using Gates = std::map<gate_idx_t, std::vector<Bytes>>;

// An inner Ethernet frame of `n` bytes (at least 42: Ethernet, IPv4, UDP).
Bytes Inner(size_t n) {
  return PacketBuilder()
      .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
      .IPv4("10.0.0.1", "10.0.0.2")
      .Udp(1000, 2000)
      .PadTo(n)
      .Build();
}

// `inner` behind the UDP (to 4789, no checksum) and VXLAN (VNI 42) headers.
// The source port is the flow entropy of the inner frame's first `head`
// bytes (all of them by default): the first segment's, for a chain.
Bytes Encapsulated(const Bytes &inner, size_t head = SIZE_MAX) {
  const size_t n = std::min(head, inner.size());
  PacketBuilder b;
  b.Udp(bess::tunnel::FlowEntropyPort(std::span<const uint8_t>(inner.data(), n)),
        bess::tunnel::kVxlanPort);
  b.Last<PacketBuilder::UdpHdr>().checksum = 0;
  return b.Vxlan(42).Payload(inner).Build();
}

Gates Through(ModuleHarness &h, Module *m, const std::vector<bess::PacketHandle> &packets) {
  bess::pb::SetMetadataArg arg;
  for (const auto &[name, value] :
       {std::pair{"tun_ip_src", 0x0a000001u},
        std::pair{"tun_ip_dst", 0x0a000002u}, std::pair{"tun_id", 42u}}) {
    auto *attr = arg.add_attrs();
    attr->set_name(name);
    attr->set_size(4);
    const uint32_t encoded = htonl(value);
    attr->set_value_bin(reinterpret_cast<const char *>(&encoded),
                        sizeof(encoded));
  }
  Module *writer = h.Create("SetMetadata", arg);
  EXPECT_NE(nullptr, writer);
  if (writer == nullptr) {
    return {};
  }
  EXPECT_EQ(0, ModuleGraph::ConnectModules(writer, 0, m, 0, true));
  return h.RunPackets(writer, 0, packets, m, {0});
}

TEST(VXLANEncapTest, PrependsTheUdpAndVxlanHeaders) {
  ModuleHarness h;
  Module *vx = h.Create("VXLANEncap", bess::pb::VXLANEncapArg());
  ASSERT_NE(nullptr, vx);
  const Bytes in = Inner(60);
  Gates out = Through(h, vx, {MakePacket(in)});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(Encapsulated(in), out[0][0]);
}

// A clone's headroom is its source's buffer, so the headers go into a copy,
// in one segment and in a chain: the other holder keeps its bytes and its
// headroom, whichever of the two is sent. The old code wrote the headers
// into the shared headroom.
TEST(VXLANEncapTest, ASharedPacketIsCopiedBeforeTheHeaders) {
  ModuleHarness h;
  Module *vx = h.Create("VXLANEncap", bess::pb::VXLANEncapArg());
  ASSERT_NE(nullptr, vx);
  const Bytes in = Inner(80);
  for (bool send_clone : {true, false}) {
    for (size_t split : {0u, 50u}) {
      SCOPED_TRACE(::testing::Message() << "clone " << send_clone << ", split " << split);
      bess::PacketHandle original = MakePacket(in, split);
      ASSERT_NE(nullptr, original);
      FillHeadroom(original, 16, 0x11);
      bess::PacketHandle clone = bess::PacketClone(original);
      ASSERT_NE(nullptr, clone);
      Gates out = Through(h, vx, {send_clone ? clone : original});
      bess::PacketHandle kept = send_clone ? original : clone;
      ASSERT_EQ(1u, out[0].size());
      EXPECT_EQ(Encapsulated(in), out[0][0]);
      EXPECT_EQ(in, PacketBytes(kept)) << "the other holder's bytes are untouched";
      EXPECT_EQ(Bytes(16, 0x11), HeadroomBytes(kept, 16))
          << "the other holder's headroom is untouched";
      bess::PacketFree(kept);
    }
  }
}

// The UDP length counts the headers and the inner frame in 16 bits: 65,519
// bytes of inner frame fit (length 65,535); one more is dropped, not sent
// with a wrapped length (the old code wrote 0).
TEST(VXLANEncapTest, AnInnerFrameTooLongForTheUdpLengthIsDropped) {
  ModuleHarness h;
  Module *vx = h.Create("VXLANEncap", bess::pb::VXLANEncapArg());
  ASSERT_NE(nullptr, vx);
  const Bytes longest = Inner(65519);
  Gates out = Through(h, vx, {MakeChain(longest, 1500)});
  ASSERT_EQ(1u, out[0].size());
  ASSERT_EQ(65535u, out[0][0].size());
  EXPECT_EQ(0xff, out[0][0][4]);
  EXPECT_EQ(0xff, out[0][0][5]);
  EXPECT_EQ(Encapsulated(longest), out[0][0]);

  out = Through(h, vx, {MakeChain(Inner(65520), 1500)});
  EXPECT_TRUE(out[0].empty());
}

// Every packet leaves encapsulated on gate 0 or is dropped: one segment, a
// chain (its entropy is its first segment's: 20 bytes, no IPv4 header), a
// clone, a runt (shorter than an Ethernet header: the flow entropy reads none
// of it, so it is encapsulated with the fixed port) and one without
// headroom, which is dropped. A shared packet without headroom is dropped
// too, not copied.
TEST(VXLANEncapTest, EveryPacketIsAccountedFor) {
  ModuleHarness h;
  Module *vx = h.Create("VXLANEncap", bess::pb::VXLANEncapArg());
  ASSERT_NE(nullptr, vx);
  const Bytes in = Inner(60);
  const Bytes runt(5, 0xab);
  bess::PacketHandle source = MakePacket(in);
  ASSERT_NE(nullptr, source);
  bess::PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(nullptr, clone);
  Gates out = Through(h, vx,
                      {MakePacket(in), MakePacket(in, 20), clone, MakePacket(runt),
                       MakePacketWithoutHeadroom(in)});
  EXPECT_EQ((std::vector<Bytes>{Encapsulated(in), Encapsulated(in, 20), Encapsulated(in),
                                Encapsulated(runt)}),
            out[0])
      << "5 in, the one without headroom dropped";
  EXPECT_EQ(in, PacketBytes(source));
  bess::PacketFree(source);

  bess::PacketHandle bare = MakePacketWithoutHeadroom(in);
  ASSERT_NE(nullptr, bare);
  bess::PacketHandle bare_clone = bess::PacketClone(bare);
  ASSERT_NE(nullptr, bare_clone);
  out = Through(h, vx, {bare_clone});
  EXPECT_TRUE(out[0].empty()) << "a shared packet without headroom is dropped";
  EXPECT_EQ(in, PacketBytes(bare));
  bess::PacketFree(bare);
}

}  // namespace
