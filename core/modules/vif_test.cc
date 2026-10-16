// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "module.h"
#include "module_graph.h"
#include "modules/vif.h"
#include "packet.h"
#include "task.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"
#include "utils/arp.h"
#include "utils/endian.h"
#include "utils/ether.h"

namespace {

using bess::PacketBatch;
using bess::PacketRef;
using bess::PlainPacketPool;
using bess::pb::VifArg;
using bess::pb::VifCommandAddArg;
using bess::pb::VifCommandClearArg;
using bess::pb::VifCommandDeleteArg;
using bess::pb::VifInterfaceKey;
using bess::pb::VifInterfaceValue;
using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;
using bess::utils::Arp;
using bess::utils::be16_t;
using bess::utils::be32_t;
using bess::utils::Ethernet;

class VifTest : public ::testing::Test {
 protected:
  void TearDown() override { ModuleGraph::DestroyAllModules(); }

  Vif *CreateVif(const VifArg &arg, const std::string &name = "vif0") {
    const auto &builders = ModuleBuilder::all_module_builders();
    const auto it = builders.find("Vif");
    EXPECT_NE(it, builders.end());
    if (it == builders.end()) {
      return nullptr;
    }
    google::protobuf::Any packed;
    EXPECT_TRUE(packed.PackFrom(arg));
    pb_error_t perr;
    Module *m = ModuleGraph::CreateModule(it->second, name, packed, &perr);
    EXPECT_EQ(0, perr.code()) << perr.errmsg();
    return dynamic_cast<Vif *>(m);
  }
};

TEST_F(VifTest, VlanDemuxAndTagStripping) {
  VifArg arg;
  arg.set_default_gate(5);

  auto *iface1 = arg.add_interfaces();
  iface1->set_id(1);
  iface1->set_vlan(100);
  iface1->set_mac_addr("02:00:00:00:01:01");
  iface1->set_route_domain(10);
  iface1->set_gate(1);

  auto *iface2 = arg.add_interfaces();
  iface2->set_id(2);
  iface2->set_vlan(200);
  iface2->set_mac_addr("02:00:00:00:02:02");
  iface2->set_route_domain(20);
  iface2->set_gate(2);

  Vif *vif = CreateVif(arg);
  ASSERT_NE(nullptr, vif);

  const auto &builders = ModuleBuilder::all_module_builders();
  Module *sink1 = ModuleGraph::CreateModule(builders.find("Sink")->second, "sink1", {}, nullptr);
  Module *sink2 = ModuleGraph::CreateModule(builders.find("Sink")->second, "sink2", {}, nullptr);
  Module *sink_def = ModuleGraph::CreateModule(builders.find("Sink")->second, "sink_def", {}, nullptr);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(vif, 1, sink1, 0));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(vif, 2, sink2, 0));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(vif, 5, sink_def, 0));
  bess::metadata::default_pipeline.ComputeMetadataOffsets();
  PlainPacketPool pool(16);
  bess::PacketHandle pkt1 = pool.Alloc();
  ASSERT_NE(nullptr, pkt1);
  PacketRef ref1(pkt1);

  // Construct VLAN 100 tagged packet: [Dst MAC (6)] [Src MAC (6)] [0x8100 (2)] [TCI (2)] [0x0800 (2)] [Payload (20)]
  uint8_t tagged_pkt[40];
  std::memset(tagged_pkt, 0, sizeof(tagged_pkt));
  Ethernet::Address dst1;
  dst1.FromString("02:00:00:00:01:01");
  Ethernet::Address src;
  src.FromString("02:11:22:33:44:55");
  std::memcpy(tagged_pkt, dst1.bytes, 6);
  std::memcpy(tagged_pkt + 6, src.bytes, 6);
  tagged_pkt[12] = 0x81;
  tagged_pkt[13] = 0x00;
  tagged_pkt[14] = 0x00;
  tagged_pkt[15] = 100;  // VLAN 100
  tagged_pkt[16] = 0x08;
  tagged_pkt[17] = 0x00;  // IPv4

  std::memcpy(ref1.append(sizeof(tagged_pkt)), tagged_pkt, sizeof(tagged_pkt));

  PacketBatch batch;
  batch.clear();
  batch.add(ref1);
  Task task(vif, nullptr);
  Context ctx;
  ctx.task = &task;
  vif->ProcessBatch(&ctx, &batch);

  // Verify that the 4-byte 802.1Q header was stripped!
  EXPECT_EQ(sizeof(tagged_pkt) - 4, ref1.total_len());
  const Ethernet *eth = ref1.head_data<const Ethernet *>();
  EXPECT_EQ(be16_t(Ethernet::Type::kIpv4), eth->ether_type);

  bess::PacketFree(pkt1);
}

TEST_F(VifTest, LocalArpResponder) {
  VifArg arg;
  arg.set_default_gate(5);

  auto *iface = arg.add_interfaces();
  iface->set_id(1);
  iface->set_vlan(100);
  iface->set_mac_addr("02:00:00:00:01:01");
  iface->set_ip_addr("192.168.1.1");
  iface->set_route_domain(10);
  iface->set_gate(1);

  Vif *vif = CreateVif(arg);
  ASSERT_NE(nullptr, vif);

  const auto &builders = ModuleBuilder::all_module_builders();
  Module *sink1 = ModuleGraph::CreateModule(builders.find("Sink")->second, "sink_arp", {}, nullptr);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(vif, 1, sink1, 0));
  bess::metadata::default_pipeline.ComputeMetadataOffsets();
  PlainPacketPool pool(16);
  bess::PacketHandle pkt = pool.Alloc();
  ASSERT_NE(nullptr, pkt);
  PacketRef ref(pkt);

  // Construct tagged ARP Request on VLAN 100 asking for 192.168.1.1
  const size_t arp_pkt_len = sizeof(Ethernet) + 4 + sizeof(Arp);
  uint8_t raw[128];
  std::memset(raw, 0, sizeof(raw));

  Ethernet::Address broadcast;
  broadcast.FromString("ff:ff:ff:ff:ff:ff");
  Ethernet::Address sender_mac;
  sender_mac.FromString("02:aa:bb:cc:dd:ee");

  std::memcpy(raw, broadcast.bytes, 6);
  std::memcpy(raw + 6, sender_mac.bytes, 6);
  raw[12] = 0x81;
  raw[13] = 0x00;
  raw[14] = 0x00;
  raw[15] = 100;  // VLAN 100
  raw[16] = 0x08;
  raw[17] = 0x06;  // ARP

  Arp *arp = reinterpret_cast<Arp *>(raw + 18);
  arp->hw_addr = be16_t(Arp::HardwareAddress::kEthernet);
  arp->proto_addr = be16_t(Ethernet::Type::kIpv4);
  arp->hw_addr_length = 6;
  arp->proto_addr_length = 4;
  arp->opcode = be16_t(Arp::Opcode::kRequest);
  arp->sender_hw_addr = sender_mac;
  bess::utils::ParseIpv4Address("192.168.1.100", &arp->sender_ip_addr);
  arp->target_hw_addr = broadcast;
  bess::utils::ParseIpv4Address("192.168.1.1", &arp->target_ip_addr);

  std::memcpy(ref.append(arp_pkt_len), raw, arp_pkt_len);

  PacketBatch batch;
  batch.clear();
  batch.add(ref);
  Task task(vif, nullptr);
  Context ctx;
  ctx.task = &task;
  vif->ProcessBatch(&ctx, &batch);

  // Verify that the ARP Request was transformed into an ARP Reply in-place!
  const Ethernet *eth = ref.head_data<const Ethernet *>();
  const Arp *reply = reinterpret_cast<const Arp *>(eth + 1);

  EXPECT_EQ(be16_t(Arp::Opcode::kReply), reply->opcode);
  Ethernet::Address expected_vif_mac;
  expected_vif_mac.FromString("02:00:00:00:01:01");
  EXPECT_EQ(expected_vif_mac, reply->sender_hw_addr);
  EXPECT_EQ(sender_mac, reply->target_hw_addr);

  be32_t expected_vif_ip;
  bess::utils::ParseIpv4Address("192.168.1.1", &expected_vif_ip);
  EXPECT_EQ(expected_vif_ip, reply->sender_ip_addr);

  bess::PacketFree(pkt);
}

// Whether `resource`'s codec says it requires paused workers.
bool RequiresPausedWorkers(const std::string &resource) {
  const bess::dataplane::Resource *r =
      bess::runtime::runtime().transactions().FindResource(resource);
  const bess::framework::ResourceCodec *codec =
      r == nullptr ? nullptr
                   : bess::framework::ResourceBindings::ProcessDefault().Find(*r);
  return codec != nullptr && codec->requires_paused_workers();
}

VifInterfaceKey IfaceKey(uint32_t id) {
  VifInterfaceKey key;
  key.set_id(id);
  return key;
}

// Canonical: lower-case MAC, dotted-decimal address.
VifInterfaceValue IfaceValue() {
  VifInterfaceValue value;
  value.set_vlan(5);
  value.set_mac_addr("02:00:00:00:00:10");
  value.set_ip_addr("10.0.0.10");
  value.set_route_domain(7);
  value.set_gate(1);
  return value;
}

Bytes ToIface() {
  return PacketBuilder()
      .Ether("02:00:00:00:00:10", "02:00:00:00:00:01")
      .Dot1Q(5)
      .IPv4("10.0.0.1", "10.0.0.10")
      .Build();
}

// The interfaces resource (D-129) steers traffic: a frame to the interface's
// (VLAN, MAC) leaves on its gate; erased, it falls back to the default gate.
TEST(VifResourceTest, InterfacesSteerTrafficAndReadBack) {
  ModuleHarness h;
  VifArg arg;
  arg.set_default_gate(0);
  Module *vif = h.Create("Vif", arg);
  ASSERT_NE(nullptr, vif);
  const std::string interfaces = vif->name() + "/interfaces";
  EXPECT_TRUE(RequiresPausedWorkers(interfaces));

  auto out = h.Run(vif, 0, {ToIface()}, {0, 1});
  EXPECT_EQ(1u, out[0].size());
  EXPECT_TRUE(out[1].empty());

  const VifInterfaceKey key = IfaceKey(3);
  const VifInterfaceValue value = IfaceValue();
  ASSERT_EQ("", h.UpsertResource(interfaces, key, value));
  out = h.Run(vif, 0, {ToIface()}, {0, 1});
  EXPECT_TRUE(out[0].empty());
  EXPECT_EQ(1u, out[1].size());
  EXPECT_EQ("", h.ReadBackDiffers(interfaces, {{&key, &value}}));

  // The interface's address is answered for on its VLAN.
  const Bytes request = PacketBuilder()
                            .Ether("ff:ff:ff:ff:ff:ff", "02:00:00:00:00:01")
                            .Dot1Q(5)
                            .Arp(1, "02:00:00:00:00:01", "10.0.0.1",
                                 "00:00:00:00:00:00", "10.0.0.10")
                            .Build();
  out = h.Run(vif, 0, {request}, {0, 1});
  ASSERT_EQ(1u, out[1].size());

  ASSERT_EQ("", h.EraseResource(interfaces, key));
  out = h.Run(vif, 0, {ToIface()}, {0, 1});
  EXPECT_EQ(1u, out[0].size());
  EXPECT_TRUE(out[1].empty());
  EXPECT_EQ("", h.ReadBackDiffers(interfaces, {}));
  EXPECT_NE("", h.EraseResource(interfaces, key));  // not found
}

// Zero addresses read back omitted, as a client leaves them out; an upper-case
// MAC reads back in lower case.
TEST(VifResourceTest, InterfacesReadBackCanonically) {
  ModuleHarness h;
  Module *vif = h.Create("Vif", VifArg());
  ASSERT_NE(nullptr, vif);
  const std::string interfaces = vif->name() + "/interfaces";
  VifInterfaceValue bare;
  bare.set_gate(2);
  const VifInterfaceKey bare_key = IfaceKey(1);
  ASSERT_EQ("", h.UpsertResource(interfaces, bare_key, bare));
  VifInterfaceValue upper = IfaceValue();
  upper.set_mac_addr("02:AA:BB:CC:DD:EE");
  const VifInterfaceKey upper_key = IfaceKey(2);
  ASSERT_EQ("", h.UpsertResource(interfaces, upper_key, upper));
  VifInterfaceValue lower = upper;
  lower.set_mac_addr("02:aa:bb:cc:dd:ee");
  EXPECT_EQ("", h.ReadBackDiffers(interfaces, {{&bare_key, &bare}, {&upper_key, &lower}}));
}

TEST(VifResourceTest, InterfacesRefuseBadKeysAndValues) {
  ModuleHarness h;
  Module *vif = h.Create("Vif", VifArg());
  ASSERT_NE(nullptr, vif);
  const std::string interfaces = vif->name() + "/interfaces";
  EXPECT_NE("", h.UpsertResource(interfaces, IfaceKey(0), IfaceValue()));
  VifInterfaceValue bad = IfaceValue();
  bad.set_vlan(4096);
  EXPECT_NE("", h.UpsertResource(interfaces, IfaceKey(1), bad));
  bad = IfaceValue();
  bad.set_mac_addr("zz:00:00:00:00:00");
  EXPECT_NE("", h.UpsertResource(interfaces, IfaceKey(1), bad));
  bad = IfaceValue();
  bad.set_ip_addr("10.0.0");
  EXPECT_NE("", h.UpsertResource(interfaces, IfaceKey(1), bad));
  bad = IfaceValue();
  bad.set_gate(DROP_GATE + 1);
  EXPECT_NE("", h.UpsertResource(interfaces, IfaceKey(1), bad));
  EXPECT_EQ("", h.ReadBackDiffers(interfaces, {}));
}

// The init argument's interfaces are what the resource reads back.
TEST(VifResourceTest, InitInterfacesAreTheResource) {
  ModuleHarness h;
  VifArg arg;
  VifArg::Interface *init = arg.add_interfaces();
  init->set_id(1);
  init->set_mac_addr("02:00:00:00:00:01");
  init->set_gate(1);
  Module *vif = h.Create("Vif", arg);
  ASSERT_NE(nullptr, vif);
  const std::string interfaces = vif->name() + "/interfaces";

  const VifInterfaceKey init_key = IfaceKey(1);
  VifInterfaceValue init_value;
  init_value.set_mac_addr("02:00:00:00:00:01");
  init_value.set_gate(1);
  EXPECT_EQ("", h.ReadBackDiffers(interfaces, {{&init_key, &init_value}}));
}

}  // namespace
