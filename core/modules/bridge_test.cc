// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <any>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

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
using bess::pb::BridgeStaticKey;
using bess::pb::BridgeStaticValue;
using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;
using bess::utils::be16_t;
using bess::utils::Ethernet;

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
    ctx.wid = bess::dataplane::WorkerId(0);
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
    ctx.wid = bess::dataplane::WorkerId(0);
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
    ctx.wid = bess::dataplane::WorkerId(0);
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
  ModuleHarness h;
  EXPECT_EQ("", h.UpsertResource(br->name() + "/statics",
                                 StaticKey("02:00:00:00:00:01"),
                                 StaticValue(0)));

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
  ctx.wid = bess::dataplane::WorkerId(0);
  ctx.task = &task;
  ctx.current_igate = 0;
  br->ProcessBatch(&ctx, &batch);
  // Packet was dropped by hairpin filtering
}

// Static programming through the resource: DROP_GATE is accepted (a
// blackholed MAC), a multicast MAC is refused, and statics beyond `size` plus
// kStaticReserve are refused (the FDB is bounded, D-064).
TEST_F(BridgeTest, StaticProgrammingLimits) {
  BridgeArg arg;
  arg.set_size(1);
  Bridge *br = CreateBridge(arg);
  ASSERT_NE(nullptr, br);
  ModuleHarness h;
  const std::string statics = br->name() + "/statics";

  EXPECT_EQ("", h.UpsertResource(statics, StaticKey("02:00:00:00:00:01"),
                                 StaticValue(DROP_GATE)));
  EXPECT_NE("", h.UpsertResource(statics, StaticKey("01:00:5e:00:00:01"),
                                 StaticValue(1)));  // multicast
  std::string last;
  for (int i = 0; i < 1 + static_cast<int>(Bridge::kStaticReserve); i++) {
    last = h.UpsertResource(
        statics,
        StaticKey(bess::utils::Format("02:00:00:01:%02x:%02x", i >> 8, i & 0xff)),
        StaticValue(1));
    if (!last.empty()) {
      EXPECT_EQ(static_cast<int>(Bridge::kStaticReserve), i);  // the 1,026th entry
      break;
    }
  }
  EXPECT_NE(std::string::npos, last.find("FDB full")) << last;
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

// `flush` is an action that forgets learned MACs only; the statics stay.
TEST(BridgeResourceTest, FlushActionForgetsLearnedMacsOnly) {
  ModuleHarness h;
  Module *br = h.Create("Bridge", BridgeArg());
  ASSERT_NE(nullptr, br);
  const std::string statics = br->name() + "/statics";

  const BridgeStaticKey key = StaticKey("02:00:00:00:00:09");
  const BridgeStaticValue value = StaticValue(3);
  ASSERT_EQ("", h.UpsertResource(statics, key, value));
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
}

// A typed transaction can name a gate no RPC would accept. It must be refused
// before an FDB slot is reserved: a placeholder taken by a refused operation
// is never released, and repeated writes fill the table. A placeholder is a
// lookup miss, but the entry count shows it.
TEST(BridgeResourceTest, ATypedOutOfRangeGateIsRefusedBeforeReserving) {
  ModuleHarness h;
  Module *br = h.Create("Bridge", BridgeArg());
  ASSERT_NE(nullptr, br);
  const std::string statics = br->name() + "/statics";
  const Bridge::Mac mac{{0x02, 0x00, 0x00, 0x00, 0x00, 0x02}};
  const bess::dataplane::ResourceKey key = bess::dataplane::EncodeKey(mac);

  EXPECT_NE("", h.Apply({bess::dataplane::Op::Upsert(
                statics, key, std::any(gate_idx_t{65535}))}));
  // Above MAX_GATES but below the interface bound: the RPC refuses it, so the
  // typed path does too.
  EXPECT_NE("", h.Apply({bess::dataplane::Op::Upsert(
                statics, key, std::any(gate_idx_t{MAX_GATES + 1}))}));
  EXPECT_EQ("0 FDB entries, max 1024", br->GetDesc());  // no placeholder left
  // DROP_GATE is the one valid sentinel outside [0, MAX_GATES).
  EXPECT_EQ("", h.Apply({bess::dataplane::Op::Upsert(
                  statics, key, std::any(gate_idx_t{DROP_GATE}))}));
  EXPECT_EQ("", h.Apply({bess::dataplane::Op::Upsert(
                  statics, key, std::any(gate_idx_t{1}))}));
  EXPECT_EQ("1 FDB entries, max 1024", br->GetDesc());
}

// A frame from `src` to `dst`.
Bytes Frame(const std::string &dst, const std::string &src) {
  return PacketBuilder().Ether(dst, src).IPv4("10.0.0.1", "10.0.0.2").Build();
}

// The engine requires a static's publication not to fail, and Publish
// CHECKs it: Reserve takes the FDB slot a new MAC needs, and a MAC the FDB
// holds is reprogrammed in place. These tests fill the FDB to its capacity,
// so whether a new key fits is the size bound's answer, not cuckoo
// placement's. A bridge of size 1 holds 1 + kStaticReserve entries in a
// table at most half full by slots: host A, learned on gate 0 (the one
// learned entry `size` allows), and kStaticReserve statics on gate 1.
constexpr char kHostA[] = "02:00:00:00:00:01";
constexpr char kHostB[] = "02:00:00:00:00:02";
constexpr char kHostC[] = "02:00:00:00:00:03";
constexpr char kFiller0[] = "02:00:00:01:00:00";  // FillerMac(0)
constexpr size_t kFullFdb = 1 + Bridge::kStaticReserve;

Bridge::Mac FillerMac(size_t i) {
  return {{0x02, 0x00, 0x00, 0x01, static_cast<uint8_t>(i >> 8u), static_cast<uint8_t>(i)}};
}

std::string FdbEntries(size_t n) {
  return bess::utils::Format("%zu FDB entries, max 1", n);
}

Module *FullBridge(ModuleHarness &h) {
  BridgeArg arg;
  arg.set_size(1);
  Module *br = h.Create("Bridge", arg);
  if (br == nullptr) {
    return nullptr;
  }
  h.Run(br, 0, {Frame(kHostB, kHostA)}, {1, 2});  // learns A on gate 0
  std::vector<bess::dataplane::Op> fill;
  fill.reserve(Bridge::kStaticReserve);
  for (size_t i = 0; i < Bridge::kStaticReserve; i++) {
    fill.push_back(bess::dataplane::Op::Upsert(br->name() + "/statics",
                                               bess::dataplane::EncodeKey(FillerMac(i)),
                                               std::any(gate_idx_t{1})));
  }
  EXPECT_EQ("", h.Apply(fill));
  EXPECT_EQ(FdbEntries(kFullFdb), br->GetDesc());
  return br;
}

// A MAC the full FDB learned becomes static in place: it needs no slot, so
// the upsert publishes, the entry count stays, and the MAC goes to its static
// gate and is no longer moved by learning.
TEST(BridgeResourceTest, AFullFdbMakesALearnedMacStaticInPlace) {
  ModuleHarness h;
  Module *br = FullBridge(h);
  ASSERT_NE(nullptr, br);
  const std::string statics = br->name() + "/statics";
  auto out = h.Run(br, 1, {Frame(kHostA, kHostB)}, {0, 2});
  EXPECT_EQ(1u, out[0].size());  // learned on gate 0
  EXPECT_TRUE(out[2].empty());

  ASSERT_EQ("", h.UpsertResource(statics, StaticKey(kHostA), StaticValue(2)));
  EXPECT_EQ(FdbEntries(kFullFdb), br->GetDesc());
  EXPECT_EQ(kFullFdb, h.ReadResource(statics).size());
  out = h.Run(br, 1, {Frame(kHostA, kHostB)}, {0, 2});
  EXPECT_TRUE(out[0].empty());
  EXPECT_EQ(1u, out[2].size());

  // A frame from A on gate 0 does not move it back: it is static now.
  h.Run(br, 0, {Frame(kHostB, kHostA)}, {1, 2});
  out = h.Run(br, 1, {Frame(kHostA, kHostB)}, {0, 2});
  EXPECT_TRUE(out[0].empty());
  EXPECT_EQ(1u, out[2].size());
}

// The error the engine gives operation `i` of a transaction it refuses
// ("applied" if it applies): ModuleHarness::Apply reports operation 0's only.
std::string ApplyError(const std::vector<bess::dataplane::Op> &ops, size_t i) {
  const bess::dataplane::TransactionEngine::Result result =
      bess::runtime::runtime().transactions().Apply(std::span<const bess::dataplane::Op>(ops));
  if (result.outcome == bess::dataplane::TransactionEngine::Outcome::kApplied) {
    return "applied";
  }
  return i < result.ops.size() ? result.ops[i].error : "no result for the operation";
}

// A new MAC needs a slot the full FDB does not have: Reserve refuses it and
// nothing changes, also when an erase of a static is reserved before it in
// the same transaction (erases publish after upserts, so they free nothing at
// Reserve).
TEST(BridgeResourceTest, AFullFdbRefusesANewStaticMacAndChangesNothing) {
  ModuleHarness h;
  Module *br = FullBridge(h);
  ASSERT_NE(nullptr, br);
  const std::string statics = br->name() + "/statics";
  const Bridge::Mac host_c{{0x02, 0x00, 0x00, 0x00, 0x00, 0x03}};

  const std::string refused = h.UpsertResource(statics, StaticKey(kHostC), StaticValue(2));
  EXPECT_NE(std::string::npos, refused.find("FDB full")) << refused;
  const std::string with_erase = ApplyError(
      {bess::dataplane::Op::Erase(statics, bess::dataplane::EncodeKey(FillerMac(0))),
       bess::dataplane::Op::Upsert(statics, bess::dataplane::EncodeKey(host_c),
                                   std::any(gate_idx_t{2}))},
      1);
  EXPECT_NE(std::string::npos, with_erase.find("FDB full")) << with_erase;

  EXPECT_EQ(FdbEntries(kFullFdb), br->GetDesc());
  EXPECT_EQ(Bridge::kStaticReserve, h.ReadResource(statics).size());
  auto out = h.Run(br, 1, {Frame(kHostC, kHostB)}, {0, 2});
  EXPECT_EQ(1u, out[0].size());  // C is unknown: flooded
  EXPECT_EQ(1u, out[2].size());
  out = h.Run(br, 1, {Frame(kHostA, kHostB)}, {0, 2});
  EXPECT_EQ(1u, out[0].size());  // A is still learned on gate 0
  EXPECT_TRUE(out[2].empty());
  out = h.Run(br, 0, {Frame(kFiller0, kHostB)}, {1, 2});
  EXPECT_EQ(1u, out[1].size());  // the erase was not applied
  EXPECT_TRUE(out[2].empty());
}

// A transaction refused after a new MAC took the FDB's one free slot gives
// the slot back. The placeholder is a lookup miss, so traffic cannot show a
// leaked one; the entry count (which counts it) can, and so can the next new
// MAC, which needs that slot.
TEST(BridgeResourceTest, ARefusedTransactionGivesItsReservedSlotBack) {
  ModuleHarness h;
  Module *br = FullBridge(h);
  ASSERT_NE(nullptr, br);
  const std::string statics = br->name() + "/statics";
  const Bridge::Mac host_b{{0x02, 0x00, 0x00, 0x00, 0x00, 0x02}};
  const Bridge::Mac host_c{{0x02, 0x00, 0x00, 0x00, 0x00, 0x03}};
  ASSERT_EQ("", h.EraseResource(statics, StaticKey(kFiller0)));
  ASSERT_EQ(FdbEntries(kFullFdb - 1), br->GetDesc());

  // C reserves the free slot; erasing B, which is no static, refuses the
  // transaction.
  const std::string refused = ApplyError(
      {bess::dataplane::Op::Upsert(statics, bess::dataplane::EncodeKey(host_c),
                                   std::any(gate_idx_t{2})),
       bess::dataplane::Op::Erase(statics, bess::dataplane::EncodeKey(host_b))},
      1);
  EXPECT_NE(std::string::npos, refused.find("not found")) << refused;
  EXPECT_EQ(FdbEntries(kFullFdb - 1), br->GetDesc());
  EXPECT_EQ(Bridge::kStaticReserve - 1, h.ReadResource(statics).size());

  ASSERT_EQ("", h.UpsertResource(statics, StaticKey("02:00:00:00:00:04"), StaticValue(2)));
  EXPECT_EQ(FdbEntries(kFullFdb), br->GetDesc());
  const std::string full = h.UpsertResource(statics, StaticKey(kHostC), StaticValue(2));
  EXPECT_NE(std::string::npos, full.find("FDB full")) << full;
}

}  // namespace
