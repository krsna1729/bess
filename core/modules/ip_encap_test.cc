// SPDX-License-Identifier: BSD-3-Clause

// IPEncap in process: the IPv4 header it prepends from the ip_src, ip_dst
// and ip_proto attributes (set upstream by SetMetadata), the headroom of a
// shared packet, the total-length limit, and a packet it cannot encapsulate.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "module.h"
#include "module_graph.h"
#include "packet.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

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

void AddAttr(bess::pb::SetMetadataArg &arg, const std::string &name, const std::string &value) {
  auto *a = arg.add_attrs();
  a->set_name(name);
  a->set_size(value.size());
  a->set_value_bin(value);
}

// SetMetadata (192.0.2.1 -> 198.51.100.7, UDP) -> IPEncap, in `h`; returns
// the SetMetadata, and the IPEncap in `*encap`.
Module *Pipeline(ModuleHarness &h, Module **encap) {
  bess::pb::SetMetadataArg md_arg;
  AddAttr(md_arg, "ip_src", std::string("\xc0\x00\x02\x01", 4));
  AddAttr(md_arg, "ip_dst", std::string("\xc6\x33\x64\x07", 4));
  AddAttr(md_arg, "ip_proto", std::string("\x11", 1));
  Module *md = h.Create("SetMetadata", md_arg);
  *encap = h.Create("IPEncap", bess::pb::IPEncapArg());
  if (md == nullptr || *encap == nullptr) {
    return nullptr;
  }
  EXPECT_EQ(0, ModuleGraph::ConnectModules(md, 0, *encap, 0, true));
  return md;
}

// A UDP datagram of `n` bytes (at least 8): what IPEncap wraps (it reads none
// of it).
Bytes Inner(size_t n) { return PacketBuilder().Udp(1000, 2000).PadTo(n).Build(); }

// `inner` behind Pipeline's IPv4 header: total length, identification 0,
// don't fragment, TTL 64, the header checksum (PacketBuilder's own sum).
Bytes Encapsulated(const Bytes &inner) {
  return PacketBuilder()
      .Add(PacketBuilder::IPv4Hdr{.src = {192, 0, 2, 1},
                                  .dst = {198, 51, 100, 7},
                                  .proto = 17,
                                  .ttl = 64,
                                  .id = 0,
                                  .frag = 0x4000})
      .Payload(inner)
      .Build();
}

TEST(IPEncapTest, PrependsTheIpv4HeaderFromMetadata) {
  ModuleHarness h;
  Module *encap = nullptr;
  Module *md = Pipeline(h, &encap);
  ASSERT_NE(nullptr, md);
  const Bytes in = Inner(40);
  Gates out = h.RunPackets(md, 0, {MakePacket(in)}, encap, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(Encapsulated(in), out[0][0]);
}

// The identification is 0, never what the headroom held before (bytes of an
// earlier packet: the old code left them in the header).
TEST(IPEncapTest, TheHeaderHoldsNoEarlierBytes) {
  ModuleHarness h;
  Module *encap = nullptr;
  Module *md = Pipeline(h, &encap);
  ASSERT_NE(nullptr, md);
  const Bytes in = Inner(40);
  bess::PacketHandle pkt = MakePacket(in);
  ASSERT_NE(nullptr, pkt);
  FillHeadroom(pkt, 20, 0xa5);
  Gates out = h.RunPackets(md, 0, {pkt}, encap, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(Encapsulated(in), out[0][0]);
}

// A clone's headroom is its source's buffer, so the header goes into a copy,
// in one segment and in a chain: the other holder keeps its bytes and its
// headroom, whichever of the two is sent. The old code wrote the header into
// the shared headroom.
TEST(IPEncapTest, ASharedPacketIsCopiedBeforeTheHeader) {
  ModuleHarness h;
  Module *encap = nullptr;
  Module *md = Pipeline(h, &encap);
  ASSERT_NE(nullptr, md);
  const Bytes in = Inner(80);
  for (bool send_clone : {true, false}) {
    for (size_t split : {0u, 30u}) {
      SCOPED_TRACE(::testing::Message() << "clone " << send_clone << ", split " << split);
      bess::PacketHandle original = MakePacket(in, split);
      ASSERT_NE(nullptr, original);
      FillHeadroom(original, 20, 0x11);
      bess::PacketHandle clone = bess::PacketClone(original);
      ASSERT_NE(nullptr, clone);
      Gates out = h.RunPackets(md, 0, {send_clone ? clone : original}, encap, {0});
      bess::PacketHandle kept = send_clone ? original : clone;
      ASSERT_EQ(1u, out[0].size());
      EXPECT_EQ(Encapsulated(in), out[0][0]);
      EXPECT_EQ(in, PacketBytes(kept)) << "the other holder's bytes are untouched";
      EXPECT_EQ(Bytes(20, 0x11), HeadroomBytes(kept, 20))
          << "the other holder's headroom is untouched";
      bess::PacketFree(kept);
    }
  }
}

// The IPv4 total length counts the header and the packet in 16 bits: 65,515
// bytes of packet fit (total length 65,535); one more is dropped, not sent
// with a wrapped length (the old code wrote 0).
TEST(IPEncapTest, APacketTooLongForTheTotalLengthIsDropped) {
  ModuleHarness h;
  Module *encap = nullptr;
  Module *md = Pipeline(h, &encap);
  ASSERT_NE(nullptr, md);
  const Bytes longest = Inner(65515);
  Gates out = h.RunPackets(md, 0, {MakeChain(longest, 1500)}, encap, {0});
  ASSERT_EQ(1u, out[0].size());
  ASSERT_EQ(65535u, out[0][0].size());
  EXPECT_EQ(Encapsulated(longest), out[0][0]);

  out = h.RunPackets(md, 0, {MakeChain(Inner(65516), 1500)}, encap, {0});
  EXPECT_TRUE(out[0].empty());
}

// Every packet leaves encapsulated on gate 0 or is dropped: one segment, a
// chain, a clone, an empty packet (IPEncap reads nothing of the packet, so
// an empty one is encapsulated too) and one without headroom, which is
// dropped (the old code sent it on without a header). A shared packet
// without headroom is dropped too, not copied.
TEST(IPEncapTest, EveryPacketIsAccountedFor) {
  ModuleHarness h;
  Module *encap = nullptr;
  Module *md = Pipeline(h, &encap);
  ASSERT_NE(nullptr, md);
  const Bytes in = Inner(60);
  bess::PacketHandle source = MakePacket(in);
  ASSERT_NE(nullptr, source);
  bess::PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(nullptr, clone);
  Gates out = h.RunPackets(md, 0,
                           {MakePacket(in), MakePacket(in, 20), clone, MakePacket(Bytes()),
                            MakePacketWithoutHeadroom(in)},
                           encap, {0});
  EXPECT_EQ((std::vector<Bytes>{Encapsulated(in), Encapsulated(in), Encapsulated(in),
                                Encapsulated(Bytes())}),
            out[0])
      << "5 in, the one without headroom dropped";
  EXPECT_EQ(in, PacketBytes(source));
  bess::PacketFree(source);

  bess::PacketHandle bare = MakePacketWithoutHeadroom(in);
  ASSERT_NE(nullptr, bare);
  bess::PacketHandle bare_clone = bess::PacketClone(bare);
  ASSERT_NE(nullptr, bare_clone);
  out = h.RunPackets(md, 0, {bare_clone}, encap, {0});
  EXPECT_TRUE(out[0].empty()) << "a shared packet without headroom is dropped";
  EXPECT_EQ(in, PacketBytes(bare));
  bess::PacketFree(bare);
}

}  // namespace
