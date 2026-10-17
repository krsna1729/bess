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

  // CreateStore asserts success, so a refusal needs its own path: the code
  // comes back raw.
  int CreateStoreRefused(const PacketStoreArg &arg,
                         const std::string &name = "store0") {
    const auto &builders = ModuleBuilder::all_module_builders();
    const auto it = builders.find("PacketStore");
    EXPECT_NE(it, builders.end());
    if (it == builders.end()) {
      return -1;
    }
    google::protobuf::Any packed;
    EXPECT_TRUE(packed.PackFrom(arg));
    pb_error_t perr;
    ModuleGraph::CreateModule(it->second, name, packed, &perr);
    return perr.code();
  }
};

TEST_F(PacketStoreTest, PacketStoreInitAndCommands) {
  PacketStoreArg arg;
  arg.set_max_packets(100);
  arg.set_max_packets_per_flow(10);
  arg.set_timeout_ms(1000);

  PacketStore *ps = CreateStore(arg);
  ASSERT_NE(nullptr, ps);

  // Release and drop act on partitions control cannot see, so a flow that
  // holds nothing is accepted, not refused: the request goes to the workers
  // that may hold it, and a refusal would be a guess about state only a worker
  // can see.
  PacketStoreCommandReleaseArg rel;
  rel.set_id(42);
  rel.set_gate(0);
  EXPECT_EQ(0, ps->CommandRelease(rel).error().code());

  PacketStoreCommandDropArg drp;
  drp.set_id(42);
  EXPECT_EQ(0, ps->CommandDrop(drp).error().code());

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
  ctx.wid = bess::dataplane::WorkerId(0);
  ctx.task = &task;
  ps->ProcessBatch(&ctx, &batch);

  // ProcessBatch hands the batch to the owner; the store happens when the
  // owner's task runs, which is the module's own task.
  PacketBatch drained;
  drained.clear();
  ps->RunTask(&ctx, &drained, nullptr);

  EXPECT_EQ("3 packets stored across 1 flows (max 100)", ps->GetDesc());

  // Release flow 0 to gate 0
  PacketStoreCommandReleaseArg rel;
  rel.set_id(0);
  rel.set_gate(0);
  EXPECT_EQ(0, ps->CommandRelease(rel).error().code());

  PacketBatch empty_batch;
  empty_batch.clear();
  ps->RunTask(&ctx, &empty_batch, nullptr);

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
  ctx.wid = bess::dataplane::WorkerId(0);
  ctx.task = &task;
  ps->ProcessBatch(&ctx, &batch);

  PacketBatch drained;
  drained.clear();
  ps->RunTask(&ctx, &drained, nullptr);

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
  // Nothing is stored until the owner's task takes the batch off the ring.
  EXPECT_EQ("0 packets stored across 0 flows (max 100)", ps->GetDesc());
  h.RunTask(ps, {0, 1});
  EXPECT_EQ("3 packets stored across 1 flows (max 100)", ps->GetDesc());

  PacketStoreCommandReleaseArg rel;
  rel.set_id(0);
  rel.set_gate(0);
  CommandResponse r = h.Action(ps, "release", rel);
  ASSERT_EQ(0, r.error().code()) << r.error().errmsg();
  EXPECT_EQ(3u, h.RunTask(ps, {0, 1})[0].size());
  EXPECT_EQ("0 packets stored across 0 flows (max 100)", ps->GetDesc());

  // The released flow is gone, and a drop of it is still accepted: control
  // cannot see that no worker holds it, so it delivers the request and the
  // workers find nothing to free.
  PacketStoreCommandDropArg drp;
  drp.set_id(0);
  EXPECT_EQ(0, h.Action(ps, "drop", drp).error().code());

  h.Run(ps, 0, in, {0, 1});
  h.RunTask(ps, {0, 1});
  r = h.Action(ps, "drop", drp);
  EXPECT_EQ(0, r.error().code()) << r.error().errmsg();
  EXPECT_EQ("0 packets stored across 0 flows (max 100)", ps->GetDesc());

  h.Run(ps, 0, in, {0, 1});
  h.RunTask(ps, {0, 1});
  r = h.Action(ps, "clear", PacketStoreCommandClearArg());
  EXPECT_EQ(0, r.error().code()) << r.error().errmsg();
  EXPECT_EQ("0 packets stored across 0 flows (max 100)", ps->GetDesc());
}

// The flow key is whichever attribute the graph provides first from
// `key_attrs`, not a fixed pair: a store keyed on the classifier's action_id
// (D-141) holds one queue per rule, which is the granularity a BAR's release
// and a Downlink Data Report are per.
TEST(PacketStoreKeyAttrTest, TheFlowKeyIsTheConfiguredAttribute) {
  bess::testing::ModuleHarness h;
  PacketStoreArg arg;
  arg.set_max_packets(100);
  arg.set_max_packets_per_flow(10);
  arg.add_key_attrs("action_id");
  Module *ps = h.Create("PacketStore", arg);
  ASSERT_NE(nullptr, ps);

  // What WildcardMatch writes for a rule; a constant here.
  bess::pb::SetMetadataArg tag;
  auto *attr = tag.add_attrs();
  attr->set_name("action_id");
  attr->set_size(sizeof(uint32_t));
  attr->set_value_int(7);
  Module *writer = h.Create("SetMetadata", tag);
  ASSERT_NE(nullptr, writer);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, ps, 0, true));

  const std::vector<bess::testing::Bytes> in(2, bess::testing::Bytes(64, 0xab));
  auto held = h.RunPipeline(writer, 0, in, ps, {0, 1});
  EXPECT_TRUE(held[0].empty());
  EXPECT_TRUE(held[1].empty());
  h.RunTask(ps, {0, 1});
  EXPECT_EQ("2 packets stored across 1 flows (max 100)", ps->GetDesc());

  // The key is the attribute's value and not 0: releasing 7 hands the packets
  // back, while 0 - what every packet would share without a key attribute -
  // holds nothing, so it releases nothing.
  PacketStoreCommandReleaseArg release;
  release.set_id(0);
  release.set_gate(0);
  ASSERT_EQ(0, h.Action(ps, "release", release).error().code());
  EXPECT_TRUE(h.RunTask(ps, {0, 1})[0].empty());
  release.set_id(7);
  CommandResponse r = h.Action(ps, "release", release);
  ASSERT_EQ(0, r.error().code()) << r.error().errmsg();
  EXPECT_EQ(2u, h.RunTask(ps, {0, 1})[0].size());
}

// The default is the historical pair, so every existing graph keys exactly as
// it did: no key_attrs means store_id, then vif_id.
TEST(PacketStoreKeyAttrTest, TheDefaultKeyIsStoreIdThenVifId) {
  bess::testing::ModuleHarness h;
  PacketStoreArg arg;
  arg.set_max_packets(100);
  arg.set_max_packets_per_flow(10);
  Module *ps = h.Create("PacketStore", arg);
  ASSERT_NE(nullptr, ps);

  bess::pb::SetMetadataArg tag;
  auto *attr = tag.add_attrs();
  attr->set_name("store_id");
  attr->set_size(sizeof(uint32_t));
  attr->set_value_int(9);
  Module *writer = h.Create("SetMetadata", tag);
  ASSERT_NE(nullptr, writer);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, ps, 0, true));

  const std::vector<bess::testing::Bytes> in(1, bess::testing::Bytes(64, 0xab));
  auto held = h.RunPipeline(writer, 0, in, ps, {0, 1});
  EXPECT_TRUE(held[0].empty());
  h.RunTask(ps, {0, 1});
  EXPECT_EQ("1 packets stored across 1 flows (max 100)", ps->GetDesc());

  PacketStoreCommandReleaseArg release;
  release.set_id(9);
  release.set_gate(0);
  CommandResponse r = h.Action(ps, "release", release);
  ASSERT_EQ(0, r.error().code()) << r.error().errmsg();
  EXPECT_EQ(1u, h.RunTask(ps, {0, 1})[0].size());
}

// A key attribute the module cannot register is refused at Init, naming it.
// Without the check the id is the error code and ProcessBatch indexes
// attr_offsets_ with it: a debug build aborts, a release build reads out of
// bounds. Both of these are ordinary ApplyPipeline input.
TEST_F(PacketStoreTest, InitRefusesAKeyAttributeItCannotRegister) {
  PacketStoreArg duplicate;
  duplicate.add_key_attrs("action_id");
  duplicate.add_key_attrs("action_id");
  EXPECT_EQ(EEXIST, CreateStoreRefused(duplicate));

  PacketStoreArg empty;
  empty.add_key_attrs("");
  EXPECT_EQ(EINVAL, CreateStoreRefused(empty));
}

// The per-flow limits resource (D-129): "<module>/flows", key the flow id.
// An entry carries exactly the fields it set, so a client can tell an absent
// field from a present zero, and the table is live: workers read one published
// table per batch.
class PacketStoreFlowsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    PacketStoreArg arg;
    arg.set_max_packets(100);
    arg.set_max_packets_per_flow(10);
    store_ = h_.Create("PacketStore", arg);
    ASSERT_NE(nullptr, store_);
    flows_ = std::string(store_->name()) + "/flows";
  }

  static bess::pb::PacketStoreFlowKey Key(uint32_t id) {
    bess::pb::PacketStoreFlowKey key;
    key.set_id(id);
    return key;
  }

  bess::testing::ModuleHarness h_;
  Module *store_ = nullptr;
  std::string flows_;
};

// D-129 amendment: the table is live. A worker reads one immutable published
// table per batch, and an erase posts the flow's packets to the workers that
// hold them, so nothing here needs the dataplane stopped.
TEST_F(PacketStoreFlowsTest, TheTableDoesNotRequirePausedWorkers) {
  const bess::framework::ResourceCodec *codec = h_.Codec(flows_);
  ASSERT_NE(nullptr, codec);
  EXPECT_FALSE(codec->requires_paused_workers())
      << "workers read a published table, not a mutable one";
}

TEST_F(PacketStoreFlowsTest, AnEntryKeepsTheFieldsItSet) {
  bess::pb::PacketStoreFlowValue both;
  both.set_timeout_ns(1'000'000);
  both.set_max_packets_per_flow(4);
  const auto k1 = Key(1);
  EXPECT_EQ("", h_.UpsertResource(flows_, k1, both));

  // Present and 0: this flow's packets never expire, which is not the same as
  // leaving the field out.
  bess::pb::PacketStoreFlowValue timeout_only;
  timeout_only.set_timeout_ns(0);
  const auto k2 = Key(2);
  EXPECT_EQ("", h_.UpsertResource(flows_, k2, timeout_only));

  // The round trip keeps presence: a client that sent an absent field does not
  // read back the module-wide default it never named.
  EXPECT_EQ("", h_.ReadBackDiffers(flows_, {{&k1, &both}, {&k2, &timeout_only}}));

  // An entry replaces the whole value, not the fields it names.
  bess::pb::PacketStoreFlowValue cap_only;
  cap_only.set_max_packets_per_flow(2);
  EXPECT_EQ("", h_.UpsertResource(flows_, k1, cap_only));
  EXPECT_EQ("", h_.ReadBackDiffers(flows_, {{&k1, &cap_only}, {&k2, &timeout_only}}));
}

TEST_F(PacketStoreFlowsTest, ErasingAFlowThatIsNotThereIsANoOp) {
  // The deletion path erases a session's flow whether or not it ever buffered,
  // so a missing entry is nothing to do, not a refusal.
  EXPECT_EQ("", h_.EraseResource(flows_, Key(7)));
  EXPECT_EQ("", h_.ReadBackDiffers(flows_, {}));
}

TEST_F(PacketStoreFlowsTest, TheKeylessFlowIdIsRefused) {
  // 0 is every packet that has no key: all keyless sessions share that bucket,
  // so one session's limits must not be written to it.
  bess::pb::PacketStoreFlowValue value;
  value.set_max_packets_per_flow(1);
  EXPECT_NE("", h_.UpsertResource(flows_, Key(0), value));
}

// Two workers feed the owner: what one worker handed over and what another did
// are in the same store, and one action reaches both. Under the old pin
// (max_allowed_workers_ = 1) the second worker could not attach at all.
TEST_F(PacketStoreTest, TwoWorkersFeedTheOwner) {
  PacketStoreArg arg;
  arg.set_max_packets(100);
  arg.set_max_packets_per_flow(10);
  PacketStore *ps = CreateStore(arg);
  ASSERT_NE(nullptr, ps);

  PlainPacketPool pool(16);
  Task task(ps, nullptr);
  Context ctx0{};
  ctx0.wid = bess::dataplane::WorkerId(0);
  ctx0.task = &task;
  Context ctx1{};
  ctx1.wid = bess::dataplane::WorkerId(1);
  ctx1.task = &task;

  const auto feed = [&](Context *ctx, int count) {
    PacketBatch batch;
    batch.clear();
    for (int i = 0; i < count; i++) {
      bess::PacketHandle pkt = pool.Alloc();
      ASSERT_NE(nullptr, pkt);
      PacketRef ref(pkt);
      ref.append(64);
      batch.add(ref);
    }
    ps->ProcessBatch(ctx, &batch);
  };

  // Two packets handed over by worker 0 and one by worker 1: one flow, one
  // store, and nothing is held until the owner's task takes them off the ring.
  feed(&ctx0, 2);
  feed(&ctx1, 1);
  EXPECT_EQ("0 packets stored across 0 flows (max 100)", ps->GetDesc());

  PacketBatch drained;
  drained.clear();
  ps->RunTask(&ctx0, &drained, nullptr);
  EXPECT_EQ("3 packets stored across 1 flows (max 100)", ps->GetDesc());

  // One action reaches the whole store, whichever worker buffered a packet.
  PacketStoreCommandReleaseArg rel;
  rel.set_id(0);
  rel.set_gate(0);
  ASSERT_EQ(0, ps->CommandRelease(rel).error().code());
  EXPECT_EQ("0 packets stored across 0 flows (max 100)", ps->GetDesc());

  // The owner's task emits all three; gate 0 is unconnected here, so they land
  // in the context's deadend.
  PacketBatch batch0;
  batch0.clear();
  EXPECT_EQ(3u, ps->RunTask(&ctx0, &batch0, nullptr).packets);
  EXPECT_EQ(3u, ctx0.silent_drops);
}

// The table is configuration: erasing a flow removes its limits and leaves its
// packets where they are, so a bulk erase is one table edit rather than one
// request per flow. `drop` is what frees packets, and a deletion path that
// means both issues both.
TEST_F(PacketStoreFlowsTest, AnEraseRemovesTheLimitsAndLeavesThePackets) {
  bess::pb::SetMetadataArg tag;
  auto *attr = tag.add_attrs();
  attr->set_name("store_id");
  attr->set_size(sizeof(uint32_t));
  attr->set_value_int(5);
  Module *writer = h_.Create("SetMetadata", tag);
  ASSERT_NE(nullptr, writer);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, store_, 0, true));

  const std::vector<bess::testing::Bytes> in(2, bess::testing::Bytes(64, 0xab));
  h_.RunPipeline(writer, 0, in, store_, {0, 1});
  h_.RunTask(store_, {0, 1});
  EXPECT_EQ("2 packets stored across 1 flows (max 100)", store_->GetDesc());

  EXPECT_EQ("", h_.EraseResource(flows_, Key(5)));
  EXPECT_EQ("", h_.ReadBackDiffers(flows_, {}));
  EXPECT_EQ("2 packets stored across 1 flows (max 100)", store_->GetDesc());

  // What the flow held leaves when something drops it.
  PacketStoreCommandDropArg drop;
  drop.set_id(5);
  ASSERT_EQ(0, h_.Action(store_, "drop", drop).error().code());
  EXPECT_EQ("0 packets stored across 0 flows (max 100)", store_->GetDesc());
}

// The published table is what the packet path reads: a flow's own cap applies
// from the next batch, with no pause in between.
TEST_F(PacketStoreFlowsTest, APerFlowCapIsEnforcedOnThePacketPath) {
  bess::pb::SetMetadataArg tag;
  auto *attr = tag.add_attrs();
  attr->set_name("store_id");
  attr->set_size(sizeof(uint32_t));
  attr->set_value_int(6);
  Module *writer = h_.Create("SetMetadata", tag);
  ASSERT_NE(nullptr, writer);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(writer, 0, store_, 0, true));

  bess::pb::PacketStoreFlowValue value;
  value.set_max_packets_per_flow(1);
  EXPECT_EQ("", h_.UpsertResource(flows_, Key(6), value));

  const std::vector<bess::testing::Bytes> in(3, bess::testing::Bytes(64, 0xab));
  h_.RunPipeline(writer, 0, in, store_, {0, 1});
  // The owner stores the batch, and the cap the upsert set evicts two to
  // gate 1: one stays.
  auto held = h_.RunTask(store_, {0, 1});
  EXPECT_TRUE(held[0].empty());
  EXPECT_EQ(2u, held[1].size());
  EXPECT_EQ("1 packets stored across 1 flows (max 100)", store_->GetDesc());
}

// Teardown frees what the store holds and what is still in flight to it: the
// ingress ring usually holds a partial burst, and freeing a whole burst would
// free stale handles.
TEST_F(PacketStoreTest, DeInitFreesEverything) {
  PacketStoreArg arg;
  arg.set_max_packets(100);
  arg.set_max_packets_per_flow(10);
  PacketStore *ps = CreateStore(arg);
  ASSERT_NE(nullptr, ps);

  PlainPacketPool pool(16);
  Task task(ps, nullptr);
  Context ctx{};
  ctx.wid = bess::dataplane::WorkerId(0);
  ctx.task = &task;

  const auto feed = [&](int count) {
    PacketBatch batch;
    batch.clear();
    for (int i = 0; i < count; i++) {
      bess::PacketHandle pkt = pool.Alloc();
      ASSERT_NE(nullptr, pkt);
      PacketRef ref(pkt);
      ref.append(64);
      batch.add(ref);
    }
    ps->ProcessBatch(&ctx, &batch);
  };

  feed(3);
  PacketBatch drained;
  drained.clear();
  ps->RunTask(&ctx, &drained, nullptr);  // 3 stored
  feed(2);                               // 2 still on the ring
  EXPECT_EQ("3 packets stored across 1 flows (max 100)", ps->GetDesc());

  ps->DeInit();
  EXPECT_EQ(16u, pool.Size());
}

// A request must act on every packet that reached the module before it,
// including one a worker handed over that the owner has not stored yet. Draining
// the requests first made a release apply to an empty store, and the packets
// stayed until they expired - the defect a code review found.
TEST_F(PacketStoreFlowsTest, AReleaseSeesPacketsStillInFlight) {
  // More packets than one run stores (kIngressPerRun is 1024) and a store that
  // holds them all: the release must wait for every packet that was in the ring
  // before it, however many runs that takes, and must not pass any of them.
  constexpr size_t kFed = 2048;
  PacketStoreArg arg;
  arg.set_max_packets(static_cast<uint32_t>(kFed));
  arg.set_max_packets_per_flow(static_cast<uint32_t>(kFed));
  Module *big = h_.Create("PacketStore", arg);
  ASSERT_NE(nullptr, big);
  const std::vector<bess::testing::Bytes> chunk(bess::PacketBatch::kMaxBurst,
                                                bess::testing::Bytes(64, 0xab));
  for (size_t fed = 0; fed < kFed; fed += chunk.size()) {
    h_.Run(big, 0, chunk);  // handed over, not stored: the owner has not run
  }
  EXPECT_EQ("0 packets stored across 0 flows (max 2048)", big->GetDesc());

  PacketStoreCommandReleaseArg release;
  release.set_id(0);
  release.set_gate(0);
  ASSERT_EQ(0, h_.Action(big, "release", release).error().code());

  // The owner stores what it owes the ingress, at most kIngressPerRun a run, and
  // applies the release only once that is in: every packet goes out on gate 0
  // and the store ends empty.
  // The first run stores at most kIngressPerRun of what it owes and must not
  // apply the release yet: that ordering is what this test exists for.
  auto first = h_.RunTask(big, {0, 1});
  EXPECT_TRUE(first[0].empty())
      << "the release ran before the owed ingress was stored";
  EXPECT_EQ("1024 packets stored across 1 flows (max 2048)", big->GetDesc());

  size_t released = 0;
  // A release leaves in bursts of kMaxBurst, so 2048 packets take about 64
  // runs; the loop is bounded well above that.
  for (int run = 0; run < 80 && released < kFed; run++) {
    auto out = h_.RunTask(big, {0, 1});
    released += out[0].size();
    EXPECT_TRUE(out[1].empty());
  }
  EXPECT_EQ(kFed, released);
  EXPECT_EQ("0 packets stored across 0 flows (max 2048)", big->GetDesc());
}

// A run that stored a batch is work: the scheduler's backoff counts an idle run
// and charges it no cycles, so reporting 0 packets would let the owner sleep
// while its ingress ring filled and dropped.
TEST_F(PacketStoreTest, AStoredBatchIsWorkNotAnIdleRun) {
  PacketStoreArg arg;
  arg.set_max_packets(100);
  arg.set_max_packets_per_flow(10);
  PacketStore *ps = CreateStore(arg);
  ASSERT_NE(nullptr, ps);

  PlainPacketPool pool(16);
  Task task(ps, nullptr);
  Context ctx{};
  ctx.wid = bess::dataplane::WorkerId(0);
  ctx.task = &task;

  const auto feed = [&](int count) {
    PacketBatch batch;
    batch.clear();
    for (int i = 0; i < count; i++) {
      bess::PacketHandle pkt = pool.Alloc();
      ASSERT_NE(nullptr, pkt);
      PacketRef ref(pkt);
      ref.append(64);
      batch.add(ref);
    }
    ps->ProcessBatch(&ctx, &batch);
  };

  feed(3);
  PacketBatch drained;
  drained.clear();
  const task_result stored = ps->RunTask(&ctx, &drained, nullptr);
  // Storing is work (block false, or the scheduler would back off and let the
  // ring fill) but it is not counted as packets sent downstream: a stored packet
  // is charged to this traffic class again when it is released, and a rate limit
  // here must not throttle storing.
  EXPECT_EQ(0u, stored.packets);
  EXPECT_FALSE(stored.block);
  EXPECT_EQ("3 packets stored across 1 flows (max 100)", ps->GetDesc());

  PacketBatch idle_batch;
  idle_batch.clear();
  const task_result idle = ps->RunTask(&ctx, &idle_batch, nullptr);
  EXPECT_EQ(0u, idle.packets);
  EXPECT_TRUE(idle.block);

  // The store still holds the three packets, and this test's pool goes before
  // the fixture's teardown does: free them here (DeInitFreesEverything's
  // pattern), while the pool they came from is alive.
  ps->DeInit();
  EXPECT_EQ(16u, pool.Size());
}

// What the ingress ring refuses is freed and counted, not silent: the store's
// own bound is what holds packets, so a full ring is the one place the packet
// path can lose them.
TEST_F(PacketStoreTest, AFullIngressRingCountsWhatItRefuses) {
  constexpr int kFed = 5000;
  constexpr size_t kRingCapacity = 4095;  // rte_ring: slots - 1

  PacketStoreArg arg;
  arg.set_max_packets(kFed);
  arg.set_max_packets_per_flow(kFed);
  PacketStore *ps = CreateStore(arg);
  ASSERT_NE(nullptr, ps);

  PlainPacketPool pool(kFed);
  Task task(ps, nullptr);
  Context ctx{};
  ctx.wid = bess::dataplane::WorkerId(0);
  ctx.task = &task;

  PacketBatch batch;
  batch.clear();
  for (int i = 0; i < kFed; i++) {
    bess::PacketHandle pkt = pool.Alloc();
    ASSERT_NE(nullptr, pkt);
    PacketRef ref(pkt);
    ref.append(64);
    batch.add(ref);
    if (batch.cnt() == static_cast<int>(bess::PacketBatch::kMaxBurst)) {
      ps->ProcessBatch(&ctx, &batch);
      batch.clear();
    }
  }
  if (batch.cnt() > 0) {
    ps->ProcessBatch(&ctx, &batch);
  }

  // Nothing has run the owner, so everything is on the ring: 4095 fit and the
  // rest were freed on the way in.
  const size_t refused = kFed - kRingCapacity;
  EXPECT_EQ("0 packets stored across 0 flows (max 5000), " +
                std::to_string(refused) + " dropped (ingress full)",
            ps->GetDesc());
  EXPECT_EQ(refused, pool.Size());

  // The ring still holds 4095 packets, and this test's pool goes before the
  // fixture's teardown does: free them here, while the pool is alive.
  ps->DeInit();
  EXPECT_EQ(static_cast<size_t>(kFed), pool.Size());
}

// Two operations in one transaction are one private copy of the table and one
// publish, so both are visible after it (the engine clones the staged table and
// swaps it in once).
TEST_F(PacketStoreFlowsTest, ATransactionOfTwoUpsertsPublishesBoth) {
  const bess::framework::ResourceCodec *codec = h_.Codec(flows_);
  ASSERT_NE(nullptr, codec);

  bess::pb::PacketStoreFlowValue v1;
  v1.set_max_packets_per_flow(1);
  bess::pb::PacketStoreFlowValue v2;
  v2.set_timeout_ns(0);
  const auto k1 = Key(1);
  const auto k2 = Key(2);
  google::protobuf::Any pk1;
  ASSERT_TRUE(pk1.PackFrom(k1));
  google::protobuf::Any pv1;
  ASSERT_TRUE(pv1.PackFrom(v1));
  google::protobuf::Any pk2;
  ASSERT_TRUE(pk2.PackFrom(k2));
  google::protobuf::Any pv2;
  ASSERT_TRUE(pv2.PackFrom(v2));
  auto key1 = codec->Key(pk1.type_url(), pk1.value());
  auto val1 = codec->Value(pv1.type_url(), pv1.value());
  auto key2 = codec->Key(pk2.type_url(), pk2.value());
  auto val2 = codec->Value(pv2.type_url(), pv2.value());
  ASSERT_TRUE(key1 && val1 && key2 && val2);

  std::vector<bess::dataplane::Op> ops;
  ops.push_back(
      bess::dataplane::Op::Upsert(flows_, std::move(*key1), std::move(*val1)));
  ops.push_back(
      bess::dataplane::Op::Upsert(flows_, std::move(*key2), std::move(*val2)));
  EXPECT_EQ("", h_.Apply(ops));
  EXPECT_EQ("", h_.ReadBackDiffers(flows_, {{&k1, &v1}, {&k2, &v2}}));
}

// All-or-nothing: a transaction whose later operation is refused publishes
// nothing, and the private copy it built does not leak into the next one.
TEST_F(PacketStoreFlowsTest, ARefusedTransactionPublishesNothing) {
  const bess::framework::ResourceCodec *codec = h_.Codec(flows_);
  ASSERT_NE(nullptr, codec);

  bess::pb::PacketStoreFlowValue value;
  value.set_max_packets_per_flow(1);
  const auto k1 = Key(1);
  const auto k2 = Key(2);  // the refused operation's key
  google::protobuf::Any pk1;
  ASSERT_TRUE(pk1.PackFrom(k1));
  google::protobuf::Any pv1;
  ASSERT_TRUE(pv1.PackFrom(value));
  google::protobuf::Any pk2;
  ASSERT_TRUE(pk2.PackFrom(k2));
  auto key1 = codec->Key(pk1.type_url(), pk1.value());
  auto val1 = codec->Value(pv1.type_url(), pv1.value());
  auto key2 = codec->Key(pk2.type_url(), pk2.value());
  ASSERT_TRUE(key1 && val1 && key2);

  std::vector<bess::dataplane::Op> ops;
  // The keys are copied into each op: moving one into the first would leave the
  // other with a moved-from key.
  ops.push_back(bess::dataplane::Op::Upsert(flows_, *key1, *val1));
  // The second operation cannot be applied: its value is not a FlowLimits, so
  // the resource refuses it and the engine aborts the transaction. It targets a
  // different key on purpose - a transaction that touched one key twice could be
  // refused before its value was ever looked at.
  ops.push_back(bess::dataplane::Op::Upsert(flows_, *key2,
                                            std::any(std::string("nope"))));
  EXPECT_NE("", h_.Apply(ops));
  EXPECT_EQ("", h_.ReadBackDiffers(flows_, {}));

  // And the next transaction starts from the published table, not the copy the
  // aborted one left behind: a third key, so a leaked private copy still holding
  // k1 or k2 would show up in the read-back.
  const auto k3 = Key(3);
  EXPECT_EQ("", h_.UpsertResource(flows_, k3, value));
  EXPECT_EQ("", h_.ReadBackDiffers(flows_, {{&k3, &value}}));
}

}  // namespace
