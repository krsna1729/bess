// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "module.h"
#include "module_graph.h"
#include "modules/vif.h"
#include "packet.h"
#include "task.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
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

TEST_F(VifTest, VifInitAndCommands) {
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

  // Add a second interface dynamically
  VifCommandAddArg add2;
  add2.set_id(2);
  add2.set_vlan(200);
  add2.set_mac_addr("02:00:00:00:02:02");
  add2.set_ip_addr("192.168.2.1");
  add2.set_route_domain(20);
  add2.set_gate(2);
  CommandResponse res = vif->CommandAdd(add2);
  EXPECT_EQ(0, res.error().code());

  // Adding with ID 0 must fail
  VifCommandAddArg add_bad;
  add_bad.set_id(0);
  add_bad.set_gate(1);
  res = vif->CommandAdd(add_bad);
  EXPECT_NE(0, res.error().code());

  // Delete interface 1
  VifCommandDeleteArg del1;
  del1.set_id(1);
  res = vif->CommandDelete(del1);
  EXPECT_EQ(0, res.error().code());

  // Deleting again must fail (ENOENT)
  res = vif->CommandDelete(del1);
  EXPECT_NE(0, res.error().code());

  // Clear all
  VifCommandClearArg clr;
  res = vif->CommandClear(clr);
  EXPECT_EQ(0, res.error().code());
}

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

}  // namespace
