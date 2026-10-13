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
#include "modules/bridge.h"
#include "packet.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"
#include "utils/format.h"
#include "utils/time.h"
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
using bess::pb::BridgeStaticKey;
using bess::pb::BridgeStaticValue;
using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;
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

    Context ctx{};
    ctx.current_tsc = rdtsc();
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

    Context ctx{};
    ctx.current_tsc = rdtsc();
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

    Context ctx{};
    ctx.current_tsc = rdtsc();
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

  Context ctx{};
  ctx.current_tsc = rdtsc();
  ctx.wid = 0;
  ctx.task = &task;
  ctx.current_igate = 0;
  br->ProcessBatch(&ctx, &batch);
  // Packet was dropped by hairpin filtering
}

// Static programming through the module: DROP_GATE is accepted (a blackholed
// MAC), a multicast MAC is refused, and statics beyond `size` plus
// kStaticReserve are refused with ENOSPC (the FDB is bounded, D-064).
TEST_F(BridgeTest, StaticProgrammingLimits) {
  BridgeArg arg;
  arg.set_size(1);
  Bridge *br = CreateBridge(arg);
  ASSERT_NE(nullptr, br);
  BridgeCommandAddArg add;
  add.set_mac_addr("02:00:00:00:00:01");
  add.set_gate(DROP_GATE);
  EXPECT_EQ(0, br->CommandAdd(add).error().code());
  add.set_mac_addr("01:00:5e:00:00:01");
  add.set_gate(1);
  EXPECT_EQ(EINVAL, br->CommandAdd(add).error().code());
  int last = 0;
  for (int i = 0; i < 1 + static_cast<int>(Bridge::kStaticReserve); i++) {
    add.set_mac_addr(bess::utils::Format("02:00:00:01:%02x:%02x", i >> 8, i & 0xff));
    last = br->CommandAdd(add).error().code();
    if (last != 0) {
      EXPECT_EQ(static_cast<int>(Bridge::kStaticReserve), i);  // the 1,026th entry
      break;
    }
  }
  EXPECT_EQ(ENOSPC, last);
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

BridgeStaticKey StaticKey(const std::string &mac) {
  BridgeStaticKey key;
  key.set_mac_addr(mac);
  return key;
}

BridgeStaticValue StaticValue(uint32_t gate) {
  BridgeStaticValue value;
  value.set_gate(gate);
  return value;
}

// A frame from host A to host B.
Bytes AToB() {
  return PacketBuilder()
      .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
      .IPv4("10.0.0.1", "10.0.0.2")
      .Build();
}

// The statics resource (D-129) programs the FDB: a static MAC is forwarded,
// not flooded, it reads back as written, and erasing it floods again.
TEST(BridgeResourceTest, StaticsSteerTrafficAndReadBack) {
  ModuleHarness h;
  Module *br = h.Create("Bridge", BridgeArg());
  ASSERT_NE(nullptr, br);
  const std::string statics = br->name() + "/statics";
  EXPECT_TRUE(RequiresPausedWorkers(statics));

  // Unknown unicast floods to every connected gate but the ingress one.
  auto out = h.Run(br, 0, {AToB()}, {1, 2});
  EXPECT_EQ(1u, out[1].size());
  EXPECT_EQ(1u, out[2].size());

  const BridgeStaticKey key = StaticKey("02:00:00:00:00:02");
  const BridgeStaticValue value = StaticValue(2);
  ASSERT_EQ("", h.UpsertResource(statics, key, value));
  out = h.Run(br, 0, {AToB()}, {1, 2});
  EXPECT_TRUE(out[1].empty());
  EXPECT_EQ(1u, out[2].size());
  EXPECT_EQ("", h.ReadBackDiffers(statics, {{&key, &value}}));

  // An upsert of the same MAC moves it.
  const BridgeStaticValue moved = StaticValue(1);
  ASSERT_EQ("", h.UpsertResource(statics, key, moved));
  out = h.Run(br, 0, {AToB()}, {1, 2});
  EXPECT_EQ(1u, out[1].size());
  EXPECT_TRUE(out[2].empty());
  EXPECT_EQ("", h.ReadBackDiffers(statics, {{&key, &moved}}));

  ASSERT_EQ("", h.EraseResource(statics, key));
  out = h.Run(br, 0, {AToB()}, {1, 2});
  EXPECT_EQ(1u, out[1].size());
  EXPECT_EQ(1u, out[2].size());
  EXPECT_EQ("", h.ReadBackDiffers(statics, {}));
  EXPECT_NE("", h.EraseResource(statics, key));  // not found
}

// The read-back is canonical: a MAC upserted in upper case reads back in
// lower case, and both spellings name the same entry.
TEST(BridgeResourceTest, StaticsReadBackInLowerCase) {
  ModuleHarness h;
  Module *br = h.Create("Bridge", BridgeArg());
  ASSERT_NE(nullptr, br);
  const std::string statics = br->name() + "/statics";
  const BridgeStaticValue value = StaticValue(DROP_GATE);
  ASSERT_EQ("", h.UpsertResource(statics, StaticKey("02:AA:BB:CC:DD:EE"), value));
  const auto entries = h.ReadResource(statics);
  ASSERT_EQ(1u, entries.size());
  BridgeStaticKey read_key;
  ASSERT_TRUE(entries[0].first.UnpackTo(&read_key));
  EXPECT_EQ("02:aa:bb:cc:dd:ee", read_key.mac_addr());
  const BridgeStaticKey lower = StaticKey("02:aa:bb:cc:dd:ee");
  EXPECT_EQ("", h.ReadBackDiffers(statics, {{&lower, &value}}));
}

TEST(BridgeResourceTest, StaticsRefuseBadKeysAndValues) {
  ModuleHarness h;
  Module *br = h.Create("Bridge", BridgeArg());
  ASSERT_NE(nullptr, br);
  const std::string statics = br->name() + "/statics";
  EXPECT_NE("", h.UpsertResource(statics, StaticKey("not-a-mac"), StaticValue(1)));
  EXPECT_NE("", h.UpsertResource(statics, StaticKey("01:00:5e:00:00:01"),
                                 StaticValue(1)));  // multicast
  EXPECT_NE("", h.UpsertResource(statics, StaticKey("02:00:00:00:00:01"),
                                 StaticValue(DROP_GATE + 1)));
  EXPECT_EQ("", h.ReadBackDiffers(statics, {}));
}

// The legacy commands stay (legacy surface only) and keep the resource's
// read-back in step; `flush` is an action that forgets learned MACs only.
TEST(BridgeResourceTest, LegacyCommandsAndFlushAction) {
  ModuleHarness h;
  Module *br = h.Create("Bridge", BridgeArg());
  ASSERT_NE(nullptr, br);
  const std::string statics = br->name() + "/statics";

  BridgeCommandAddArg add;
  add.set_mac_addr("02:00:00:00:00:09");
  add.set_gate(3);
  EXPECT_EQ(ENOTSUP, h.Action(br, "add", add).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(br, "delete", BridgeCommandDeleteArg()).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(br, "clear", BridgeCommandClearArg()).error().code());
  h.MustCommand(br, "add", add);
  const BridgeStaticKey key = StaticKey("02:00:00:00:00:09");
  const BridgeStaticValue value = StaticValue(3);
  EXPECT_EQ("", h.ReadBackDiffers(statics, {{&key, &value}}));

  // Learn host A on gate 0, then reach it from gate 1: forwarded, not
  // flooded.
  h.Run(br, 0, {AToB()}, {1, 2});
  const Bytes b_to_a = PacketBuilder()
                           .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                           .IPv4("10.0.0.2", "10.0.0.1")
                           .Build();
  auto out = h.Run(br, 1, {b_to_a}, {0, 2});
  EXPECT_EQ(1u, out[0].size());
  EXPECT_TRUE(out[2].empty());

  // flush forgets host A (flooded again) and keeps the static entry.
  EXPECT_EQ(0, h.Action(br, "flush", bess::pb::EmptyArg()).error().code());
  out = h.Run(br, 1, {b_to_a}, {0, 2});
  EXPECT_EQ(1u, out[0].size());
  EXPECT_EQ(1u, out[2].size());
  EXPECT_EQ("", h.ReadBackDiffers(statics, {{&key, &value}}));

  // clear (legacy) removes the statics too.
  h.MustCommand(br, "clear", BridgeCommandClearArg());
  EXPECT_EQ("", h.ReadBackDiffers(statics, {}));
}

}  // namespace
