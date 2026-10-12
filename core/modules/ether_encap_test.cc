// SPDX-License-Identifier: BSD-3-Clause

// EtherEncap in process (was bessctl/module_tests/etherencap.py): the
// header comes from the ether_src, ether_dst and ether_type attributes,
// set upstream by SetMetadata.

#include <gtest/gtest.h>

#include <string>

#include "module_graph.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

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

}  // namespace
