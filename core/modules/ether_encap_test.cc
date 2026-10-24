// SPDX-License-Identifier: BSD-3-Clause

// EtherEncap in process (was bessctl/module_tests/etherencap.py): the
// header comes from the ether_src, ether_dst and ether_type attributes,
// set upstream by SetMetadata.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "module_graph.h"
#include "modules/ether_encap.h"
#include "packet.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"
#include "utils/endian.h"
#include "utils/ether.h"

namespace {

using bess::testing::Bytes;
using bess::testing::FillHeadroom;
using bess::testing::HeadroomBytes;
using bess::testing::MakePacket;
using bess::testing::MakePacketWithoutHeadroom;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;
using bess::testing::PacketBytes;

void AddAttr(bess::pb::SetMetadataArg &arg, const std::string &name, const std::string &value) {
  auto *a = arg.add_attrs();
  a->set_name(name);
  a->set_size(value.size());
  a->set_value_bin(value);
}

// An IP packet leaves wrapped in the Ethernet header the metadata gave.
TEST(EtherEncapTest, WrapsInTheHeaderFromMetadata) {
  ModuleHarness h;
  bess::pb::SetMetadataArg md_arg;
  AddAttr(md_arg, "ether_src", std::string("\xde\xad\xbe\xef\x12\x34", 6));
  AddAttr(md_arg, "ether_dst", std::string("\x12\x34\xde\xad\xbe\xef", 6));
  AddAttr(md_arg, "ether_type", std::string("\x08\x00", 2));
  Module *md = h.Create("SetMetadata", md_arg);
  Module *encap = h.Create("EtherEncap", bess::pb::EtherEncapArg());
  ASSERT_NE(nullptr, md);
  ASSERT_NE(nullptr, encap);
  // No default hooks: Track needs a worker thread, and this test runs on none.
  ASSERT_EQ(0, ModuleGraph::ConnectModules(md, 0, encap, 0, true));

  auto ip_udp = [](PacketBuilder b) {
    b.IPv4("1.2.3.4", "2.3.4.5");
    b.Last<PacketBuilder::IPv4Hdr>().ttl = 98;
    return b.Udp(10001, 10002).Payload("helloworld").Build();
  };
  const Bytes in = ip_udp(PacketBuilder());
  const Bytes want = ip_udp(PacketBuilder().Ether("12:34:de:ad:be:ef", "de:ad:be:ef:12:34"));

  auto out = h.RunPipeline(md, 0, {in}, encap, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(want, out[0][0]);
}

// A packet with no headroom cannot take the header. The module drops it (the
// old code skipped the write and forwarded the bare payload), and a refused
// header writes nothing.
TEST(EtherEncapTest, NoHeadroomIsADropAndWritesNothing) {
  bess::PlainPacketPool pool(4);
  bess::PacketHandle handle = pool.Alloc();
  ASSERT_NE(nullptr, handle);
  bess::PacketRef pkt(handle);
  ASSERT_NE(nullptr, pkt.append(64));
  // Spend the whole headroom: a 14-byte prepend now has nowhere to go.
  ASSERT_NE(nullptr, pkt.prepend(pkt.headroom()));
  ASSERT_EQ(0, pkt.headroom());
  const uint32_t before = pkt.total_len();

  bess::utils::Ethernet::Address src;
  bess::utils::Ethernet::Address dst;
  EXPECT_FALSE(EtherEncap::Encap(handle, src, dst, bess::utils::be16_t(0x0800)));
  EXPECT_EQ(before, pkt.total_len()) << "a refused header writes nothing";
}

// The same refusal through ProcessBatch: a chain of VLAN pushes spends the
// first segment's headroom, and the encapsulation that has none left drops the
// packet instead of forwarding it bare.
TEST(EtherEncapTest, ANoHeadroomPacketIsDroppedNotForwarded) {
  ModuleHarness h;
  bess::pb::SetMetadataArg md_arg;
  AddAttr(md_arg, "ether_src", std::string("\xde\xad\xbe\xef\x12\x34", 6));
  AddAttr(md_arg, "ether_dst", std::string("\x12\x34\xde\xad\xbe\xef", 6));
  AddAttr(md_arg, "ether_type", std::string("\x08\x00", 2));
  AddAttr(md_arg, "vlan_tag", std::string("\x00\x64", 2));

  auto run = [&](int pushes)
      -> std::map<gate_idx_t, std::vector<Bytes>> {
    Module *first = h.Create("SetMetadata", md_arg);
    Module *prev = first;
    EXPECT_NE(nullptr, prev);
    for (int i = 0; i < pushes; i++) {
      Module *push = h.Create("VLANPush", bess::pb::VLANPushArg());
      EXPECT_NE(nullptr, push);
      if (push == nullptr) {
        return {};
      }
      EXPECT_EQ(0, ModuleGraph::ConnectModules(prev, 0, push, 0, true));
      prev = push;
    }
    Module *encap = h.Create("EtherEncap", bess::pb::EtherEncapArg());
    EXPECT_NE(nullptr, encap);
    if (encap == nullptr) {
      return {};
    }
    EXPECT_EQ(0, ModuleGraph::ConnectModules(prev, 0, encap, 0, true));
    return h.RunPipeline(first, 0, {Bytes(64, 0)}, encap, {0});
  };

  EXPECT_EQ(1u, run(20)[0].size()) << "room for the header: forwarded";
  EXPECT_TRUE(run(31)[0].empty()) << "no headroom: dropped, not forwarded";
}

// SetMetadata (the header's attributes) -> EtherEncap, in `h`; returns the
// SetMetadata, and the EtherEncap in `*encap`.
Module *Pipeline(ModuleHarness &h, Module **encap) {
  bess::pb::SetMetadataArg md_arg;
  AddAttr(md_arg, "ether_src", std::string("\xde\xad\xbe\xef\x12\x34", 6));
  AddAttr(md_arg, "ether_dst", std::string("\x12\x34\xde\xad\xbe\xef", 6));
  AddAttr(md_arg, "ether_type", std::string("\x08\x00", 2));
  Module *md = h.Create("SetMetadata", md_arg);
  *encap = h.Create("EtherEncap", bess::pb::EtherEncapArg());
  if (md == nullptr || *encap == nullptr) {
    return nullptr;
  }
  EXPECT_EQ(0, ModuleGraph::ConnectModules(md, 0, *encap, 0, true));
  return md;
}

// 0, 1, 2, ... as `n` bytes: what EtherEncap wraps (it reads none of them).
Bytes Inner(size_t n) {
  Bytes b(n);
  for (size_t i = 0; i < n; i++) {
    b[i] = static_cast<uint8_t>(i);
  }
  return b;
}

// `inner` behind Pipeline's header.
Bytes Wrapped(const Bytes &inner) {
  Bytes b = {0x12, 0x34, 0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef, 0x12, 0x34, 0x08, 0x00};
  b.insert(b.end(), inner.begin(), inner.end());
  return b;
}

// A clone's headroom is its source's buffer, so the header goes into a copy,
// in one segment, in a chain and for an empty packet: the other holder keeps
// its bytes and its headroom, whichever of the two is sent. The old code
// wrote the header into the shared headroom.
TEST(EtherEncapTest, ASharedPacketIsCopiedBeforeTheHeader) {
  ModuleHarness h;
  Module *encap = nullptr;
  Module *md = Pipeline(h, &encap);
  ASSERT_NE(nullptr, md);
  const Bytes in = Inner(60);
  for (bool send_clone : {true, false}) {
    for (size_t split : {0u, 20u}) {
      SCOPED_TRACE(::testing::Message() << "clone " << send_clone << ", split " << split);
      bess::PacketHandle original = MakePacket(in, split);
      ASSERT_NE(nullptr, original);
      FillHeadroom(original, 14, 0x11);
      bess::PacketHandle clone = bess::PacketClone(original);
      ASSERT_NE(nullptr, clone);
      bess::PacketHandle sent = send_clone ? clone : original;
      bess::PacketHandle kept = send_clone ? original : clone;
      auto out = h.RunPackets(md, 0, {sent}, encap, {0});
      ASSERT_EQ(1u, out[0].size());
      EXPECT_EQ(Wrapped(in), out[0][0]);
      EXPECT_EQ(in, PacketBytes(kept)) << "the other holder's bytes are untouched";
      EXPECT_EQ(Bytes(14, 0x11), HeadroomBytes(kept, 14))
          << "the other holder's headroom is untouched";
      bess::PacketFree(kept);
    }
  }

  // An empty clone holds no payload byte, but its headroom is still the
  // source's buffer.
  bess::PacketHandle empty = MakePacket(Bytes());
  ASSERT_NE(nullptr, empty);
  FillHeadroom(empty, 14, 0x11);
  bess::PacketHandle empty_clone = bess::PacketClone(empty);
  ASSERT_NE(nullptr, empty_clone);
  auto out = h.RunPackets(md, 0, {empty_clone}, encap, {0});
  ASSERT_EQ(1u, out[0].size()) << "an empty shared packet is wrapped, not dropped";
  EXPECT_EQ(Wrapped(Bytes()), out[0][0]);
  EXPECT_EQ(Bytes(14, 0x11), HeadroomBytes(empty, 14));
  bess::PacketFree(empty);
}

// Every packet leaves wrapped on gate 0 or is dropped: one segment, a chain,
// a clone, an empty packet (EtherEncap reads nothing of the packet, so an
// empty one is wrapped too) and one without headroom, which is the one
// dropped. A shared packet without headroom is dropped too, not copied.
TEST(EtherEncapTest, EveryPacketIsAccountedFor) {
  ModuleHarness h;
  Module *encap = nullptr;
  Module *md = Pipeline(h, &encap);
  ASSERT_NE(nullptr, md);
  const Bytes in = Inner(60);
  bess::PacketHandle source = MakePacket(in);
  ASSERT_NE(nullptr, source);
  bess::PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(nullptr, clone);
  auto out = h.RunPackets(md, 0,
                          {MakePacket(in), MakePacket(in, 20), clone, MakePacket(Bytes()),
                           MakePacketWithoutHeadroom(in)},
                          encap, {0});
  EXPECT_EQ((std::vector<Bytes>{Wrapped(in), Wrapped(in), Wrapped(in), Wrapped(Bytes())}), out[0])
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
