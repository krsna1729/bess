// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "module.h"
#include "module_graph.h"
#include "modules/packet_store.h"
#include "packet.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "task.h"
#include "testing/module_harness.h"
#include "utils/endian.h"

namespace {

using bess::PacketBatch;
using bess::PacketRef;
using bess::PlainPacketPool;
using bess::pb::PacketStoreArg;
using bess::pb::PacketStoreCommandClearArg;
using bess::pb::PacketStoreCommandDropArg;
using bess::pb::PacketStoreCommandReleaseArg;

class PacketStoreTest : public ::testing::Test {
 protected:
  void TearDown() override { ModuleGraph::DestroyAllModules(); }

  PacketStore *CreateStore(const PacketStoreArg &arg,
                           const std::string &name = "store0") {
    const auto &builders = ModuleBuilder::all_module_builders();
    const auto it = builders.find("PacketStore");
    EXPECT_NE(it, builders.end());
    if (it == builders.end()) {
      return nullptr;
    }
    google::protobuf::Any packed;
    EXPECT_TRUE(packed.PackFrom(arg));
    pb_error_t perr;
    Module *m = ModuleGraph::CreateModule(it->second, name, packed, &perr);
    EXPECT_EQ(0, perr.code()) << perr.errmsg();
    return dynamic_cast<PacketStore *>(m);
  }
};

TEST_F(PacketStoreTest, PacketStoreInitAndCommands) {
  PacketStoreArg arg;
  arg.set_max_packets(100);
  arg.set_max_packets_per_flow(10);
  arg.set_timeout_ms(1000);

  PacketStore *ps = CreateStore(arg);
  ASSERT_NE(nullptr, ps);

  // Release non-existent flow must fail (ENOENT)
  PacketStoreCommandReleaseArg rel;
  rel.set_id(42);
  rel.set_gate(0);
  EXPECT_NE(0, ps->CommandRelease(rel).error().code());

  // Drop non-existent flow must fail (ENOENT)
  PacketStoreCommandDropArg drp;
  drp.set_id(42);
  EXPECT_NE(0, ps->CommandDrop(drp).error().code());

  // Clear empty store succeeds
  PacketStoreCommandClearArg clr;
  EXPECT_EQ(0, ps->CommandClear(clr).error().code());
}

TEST_F(PacketStoreTest, BufferAndReleaseFlow) {
  PacketStoreArg arg;
  arg.set_max_packets(100);
  arg.set_max_packets_per_flow(10);

  PacketStore *ps = CreateStore(arg);
  ASSERT_NE(nullptr, ps);

  // ProcessBatch is invoked synchronously here, not by a worker; the default
  // Track hook requires a worker identity and is irrelevant to this test.
  const auto &builders = ModuleBuilder::all_module_builders();
  Module *sink0 = ModuleGraph::CreateModule(builders.find("Sink")->second,
                                            "sink0", {}, nullptr);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(ps, 0, sink0, 0, true));

  PlainPacketPool pool(16);
  Task task(ps, nullptr);

  // Buffer 3 packets
  PacketBatch batch;
  batch.clear();
  for (int i = 0; i < 3; i++) {
    bess::PacketHandle pkt = pool.Alloc();
    ASSERT_NE(nullptr, pkt);
    PacketRef ref(pkt);
    ref.append(64);
    batch.add(ref);
  }

  Context ctx;
  ctx.wid = 0;
  ctx.task = &task;
  ps->ProcessBatch(&ctx, &batch);

  EXPECT_EQ("3 packets stored across 1 flows (max 100)", ps->GetDesc());

  // Release flow 0 to gate 0
  PacketStoreCommandReleaseArg rel;
  rel.set_id(0);
  rel.set_gate(0);
  EXPECT_EQ(0, ps->CommandRelease(rel).error().code());

  PacketBatch empty_batch;
  empty_batch.clear();
  ps->ProcessBatch(&ctx, &empty_batch);

  EXPECT_EQ("0 packets stored across 0 flows (max 100)", ps->GetDesc());
}

TEST_F(PacketStoreTest, PerFlowCapacityEviction) {
  PacketStoreArg arg;
  arg.set_max_packets(100);
  arg.set_max_packets_per_flow(2);  // Limit 2 per flow

  PacketStore *ps = CreateStore(arg);
  ASSERT_NE(nullptr, ps);

  // ProcessBatch is invoked synchronously here, not by a worker; omit the
  // worker-only default Track hook.
  const auto &builders = ModuleBuilder::all_module_builders();
  Module *sink1 = ModuleGraph::CreateModule(builders.find("Sink")->second,
                                            "sink1", {}, nullptr);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(ps, 1, sink1, 0, true));

  PlainPacketPool pool(16);
  Task task(ps, nullptr);

  // Push 3 packets (exceeding limit of 2)
  PacketBatch batch;
  batch.clear();
  for (int i = 0; i < 3; i++) {
    bess::PacketHandle pkt = pool.Alloc();
    ASSERT_NE(nullptr, pkt);
    PacketRef ref(pkt);
    ref.append(64);
    batch.add(ref);
  }

  Context ctx;
  ctx.wid = 0;
  ctx.task = &task;
  ps->ProcessBatch(&ctx, &batch);

  // Exactly 2 packets should remain stored, 1 evicted to gate 1!
  EXPECT_EQ("2 packets stored across 1 flows (max 100)", ps->GetDesc());

  // Clear remaining
  PacketStoreCommandClearArg clr;
  EXPECT_EQ(0, ps->CommandClear(clr).error().code());
}

// D-129: release, drop and clear act on held packets, not on configuration:
// all three are actions (none answers data), so InvokeAction runs them. With
// no store_id or vif_id writer every packet is held under flow 0.
TEST(PacketStoreActionTest, ReleaseDropAndClearAreActions) {
  bess::testing::ModuleHarness h;
  PacketStoreArg arg;
  arg.set_max_packets(100);
  arg.set_max_packets_per_flow(10);
  Module *ps = h.Create("PacketStore", arg);
  ASSERT_NE(nullptr, ps);

  const std::vector<bess::testing::Bytes> in(3, bess::testing::Bytes(64, 0xab));
  auto held = h.Run(ps, 0, in, {0, 1});
  EXPECT_TRUE(held[0].empty());
  EXPECT_TRUE(held[1].empty());
  EXPECT_EQ("3 packets stored across 1 flows (max 100)", ps->GetDesc());

  PacketStoreCommandReleaseArg rel;
  rel.set_id(0);
  rel.set_gate(0);
  CommandResponse r = h.Action(ps, "release", rel);
  ASSERT_EQ(0, r.error().code()) << r.error().errmsg();
  EXPECT_EQ(3u, h.RunTask(ps, {0, 1})[0].size());
  EXPECT_EQ("0 packets stored across 0 flows (max 100)", ps->GetDesc());

  // The released flow is gone: drop now finds nothing.
  PacketStoreCommandDropArg drp;
  drp.set_id(0);
  EXPECT_EQ(ENOENT, h.Action(ps, "drop", drp).error().code());

  h.Run(ps, 0, in, {0, 1});
  r = h.Action(ps, "drop", drp);
  EXPECT_EQ(0, r.error().code()) << r.error().errmsg();
  EXPECT_EQ("0 packets stored across 0 flows (max 100)", ps->GetDesc());

  h.Run(ps, 0, in, {0, 1});
  r = h.Action(ps, "clear", PacketStoreCommandClearArg());
  EXPECT_EQ(0, r.error().code()) << r.error().errmsg();
  EXPECT_EQ("0 packets stored across 0 flows (max 100)", ps->GetDesc());
}

}  // namespace
