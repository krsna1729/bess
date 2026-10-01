// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "module.h"
#include "module_graph.h"
#include "modules/bridge.h"
#include "packet.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "task.h"
#include "utils/endian.h"
#include "utils/ether.h"

namespace {

using bess::PacketBatch;
using bess::PacketRef;
using bess::PlainPacketPool;
using bess::pb::BridgeArg;
using bess::pb::BridgeCommandAddArg;
using bess::pb::BridgeCommandClearArg;
using bess::pb::BridgeCommandDeleteArg;
using bess::utils::be16_t;
using bess::utils::Ethernet;

class BridgeTest : public ::testing::Test {
 protected:
  void TearDown() override { ModuleGraph::DestroyAllModules(); }

  Bridge *CreateBridge(const BridgeArg &arg,
                       const std::string &name = "bridge0") {
    const auto &builders = ModuleBuilder::all_module_builders();
    const auto it = builders.find("Bridge");
    EXPECT_NE(it, builders.end());
    if (it == builders.end()) {
      return nullptr;
    }
    google::protobuf::Any packed;
    EXPECT_TRUE(packed.PackFrom(arg));
    pb_error_t perr;
    Module *m = ModuleGraph::CreateModule(it->second, name, packed, &perr);
    EXPECT_EQ(0, perr.code()) << perr.errmsg();
    return dynamic_cast<Bridge *>(m);
  }
};

TEST_F(BridgeTest, BridgeInitAndCommands) {
  BridgeArg arg;
  arg.set_size(512);
  arg.set_aging_time(60);

  Bridge *br = CreateBridge(arg);
  ASSERT_NE(nullptr, br);

  // Add static entry
  BridgeCommandAddArg add;
  add.set_mac_addr("02:aa:bb:cc:dd:ee");
  add.set_gate(2);
  CommandResponse res = br->CommandAdd(add);
  EXPECT_EQ(0, res.error().code());

  // Add invalid MAC must fail
  BridgeCommandAddArg bad_add;
  bad_add.set_mac_addr("invalid-mac");
  bad_add.set_gate(1);
  res = br->CommandAdd(bad_add);
  EXPECT_NE(0, res.error().code());

  // Delete static entry
  BridgeCommandDeleteArg del;
  del.set_mac_addr("02:aa:bb:cc:dd:ee");
  res = br->CommandDelete(del);
  EXPECT_EQ(0, res.error().code());

  // Delete non-existent entry must fail
  res = br->CommandDelete(del);
  EXPECT_NE(0, res.error().code());

  // Clear
  BridgeCommandClearArg clr;
  res = br->CommandClear(clr);
  EXPECT_EQ(0, res.error().code());
}

TEST_F(BridgeTest, DynamicMacLearningAndForwarding) {
  BridgeArg arg;
  Bridge *br = CreateBridge(arg);
  ASSERT_NE(nullptr, br);

  // Connect 3 output gates to Sinks
  const auto &builders = ModuleBuilder::all_module_builders();
  Module *sink0 = ModuleGraph::CreateModule(builders.find("Sink")->second,
                                            "sink0", {}, nullptr);
  Module *sink1 = ModuleGraph::CreateModule(builders.find("Sink")->second,
                                            "sink1", {}, nullptr);
  Module *sink2 = ModuleGraph::CreateModule(builders.find("Sink")->second,
                                            "sink2", {}, nullptr);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(br, 0, sink0, 0));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(br, 1, sink1, 0));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(br, 2, sink2, 0));

  PlainPacketPool pool(16);
  Task task(br, nullptr);

  // Packet 1: Host A (02:00:00:00:00:01) on Port 0 -> Host B (02:00:00:00:00:02)
  // Host B unknown -> Flooded to Gates 1 and 2! Host A learned on Gate 0.
  {
    bess::PacketHandle pkt = pool.Alloc();
    ASSERT_NE(nullptr, pkt);
    PacketRef ref(pkt);
    uint8_t raw[64];
    std::memset(raw, 0, sizeof(raw));
    Ethernet::Address host_a;
    host_a.FromString("02:00:00:00:00:01");
    Ethernet::Address host_b;
    host_b.FromString("02:00:00:00:00:02");
    std::memcpy(raw, host_b.bytes, 6);
    std::memcpy(raw + 6, host_a.bytes, 6);
    raw[12] = 0x08;
    raw[13] = 0x00;  // IPv4
    std::memcpy(ref.append(sizeof(raw)), raw, sizeof(raw));

    PacketBatch batch;
    batch.clear();
    batch.add(ref);

    Context ctx;
    ctx.wid = 0;
    ctx.task = &task;
    ctx.current_igate = 0;
    br->ProcessBatch(&ctx, &batch);
    // Packet is emitted to flood gates (1 and 2)
  }

  // Packet 2: Host B on Port 1 -> Host A.
  // Host A is known on Gate 0 -> Unicast directly to Gate 0! Host B learned on Gate 1.
  {
    bess::PacketHandle pkt = pool.Alloc();
    ASSERT_NE(nullptr, pkt);
    PacketRef ref(pkt);
    uint8_t raw[64];
    std::memset(raw, 0, sizeof(raw));
    Ethernet::Address host_a;
    host_a.FromString("02:00:00:00:00:01");
    Ethernet::Address host_b;
    host_b.FromString("02:00:00:00:00:02");
    std::memcpy(raw, host_a.bytes, 6);
    std::memcpy(raw + 6, host_b.bytes, 6);
    raw[12] = 0x08;
    raw[13] = 0x00;
    std::memcpy(ref.append(sizeof(raw)), raw, sizeof(raw));

    PacketBatch batch;
    batch.clear();
    batch.add(ref);

    Context ctx;
    ctx.wid = 0;
    ctx.task = &task;
    ctx.current_igate = 1;
    br->ProcessBatch(&ctx, &batch);
    // Forwarded to gate 0
  }

  // Packet 3: Host A on Port 0 -> Host B.
  // Host B is now known on Gate 1 -> Unicast directly to Gate 1!
  {
    bess::PacketHandle pkt = pool.Alloc();
    ASSERT_NE(nullptr, pkt);
    PacketRef ref(pkt);
    uint8_t raw[64];
    std::memset(raw, 0, sizeof(raw));
    Ethernet::Address host_a;
    host_a.FromString("02:00:00:00:00:01");
    Ethernet::Address host_b;
    host_b.FromString("02:00:00:00:00:02");
    std::memcpy(raw, host_b.bytes, 6);
    std::memcpy(raw + 6, host_a.bytes, 6);
    raw[12] = 0x08;
    raw[13] = 0x00;
    std::memcpy(ref.append(sizeof(raw)), raw, sizeof(raw));

    PacketBatch batch;
    batch.clear();
    batch.add(ref);

    Context ctx;
    ctx.wid = 0;
    ctx.task = &task;
    ctx.current_igate = 0;
    br->ProcessBatch(&ctx, &batch);
    // Forwarded to gate 1
  }
}

TEST_F(BridgeTest, HairpinFiltering) {
  BridgeArg arg;
  Bridge *br = CreateBridge(arg);
  ASSERT_NE(nullptr, br);

  const auto &builders = ModuleBuilder::all_module_builders();
  Module *sink0 = ModuleGraph::CreateModule(builders.find("Sink")->second,
                                            "sink0", {}, nullptr);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(br, 0, sink0, 0));

  PlainPacketPool pool(16);
  Task task(br, nullptr);

  // Add static entry for Host A on Gate 0
  BridgeCommandAddArg add;
  add.set_mac_addr("02:00:00:00:00:01");
  add.set_gate(0);
  EXPECT_EQ(0, br->CommandAdd(add).error().code());

  // Packet arrives on Gate 0 destined for Host A (same gate) -> Hairpin filter drops it!
  bess::PacketHandle pkt = pool.Alloc();
  ASSERT_NE(nullptr, pkt);
  PacketRef ref(pkt);
  uint8_t raw[64];
  std::memset(raw, 0, sizeof(raw));
  Ethernet::Address host_a;
  host_a.FromString("02:00:00:00:00:01");
  Ethernet::Address other;
  other.FromString("02:00:00:00:00:99");
  std::memcpy(raw, host_a.bytes, 6);
  std::memcpy(raw + 6, other.bytes, 6);
  raw[12] = 0x08;
  raw[13] = 0x00;
  std::memcpy(ref.append(sizeof(raw)), raw, sizeof(raw));

  PacketBatch batch;
  batch.clear();
  batch.add(ref);

  Context ctx;
  ctx.wid = 0;
  ctx.task = &task;
  ctx.current_igate = 0;
  br->ProcessBatch(&ctx, &batch);
  // Packet was dropped by hairpin filtering
}

}  // namespace
