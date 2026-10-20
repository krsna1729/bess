// SPDX-License-Identifier: BSD-3-Clause

// EtherEncap in process (was bessctl/module_tests/etherencap.py): the
// header comes from the ether_src, ether_dst and ether_type attributes,
// set upstream by SetMetadata.

#include <gtest/gtest.h>

#include <string>

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
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;

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
  EXPECT_FALSE(EtherEncap::Encap(pkt, src, dst, bess::utils::be16_t(0x0800)));
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

}  // namespace
