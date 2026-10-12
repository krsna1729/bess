// SPDX-License-Identifier: BSD-3-Clause

// L2Forward (D-114): the default gate at creation, and "<name>/macs" entries
// written through the resource's codec -- the path the transaction RPC and
// manifests take -- deciding the gates of the module's packet path. Its
// commands (add, delete, lookup, populate, set_default_gate) and Init's
// limits in process (was bessctl/module_tests/l2forward.py; the cases that
// need a running worker are tests/live/l2_forward_test.cc).

#include "modules/l2_forward.h"

#include <gtest/gtest.h>

#include <any>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "module.h"
#include "module_graph.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::dataplane::Op;
using Outcome = bess::dataplane::TransactionEngine::Outcome;

bess::dataplane::TransactionEngine &Engine() { return bess::runtime::runtime().transactions(); }

// nullptr and the error in *perr when the module is refused.
L2Forward *Create(const bess::pb::L2ForwardArg &arg, pb_error_t *perr) {
  google::protobuf::Any packed;
  EXPECT_TRUE(packed.PackFrom(arg));
  return static_cast<L2Forward *>(ModuleGraph::CreateModule(
      ModuleBuilder::all_module_builders().at("L2Forward"), "l2", packed, perr));
}

// An upsert (gate >= 0) or erase (gate < 0) of `addr`, decoded by the codec
// bound to "l2/macs" from the messages a client sends.
Op MacOp(const std::string &addr, int64_t gate) {
  const bess::dataplane::Resource *resource = Engine().FindResource("l2/macs");
  EXPECT_NE(nullptr, resource);
  const bess::framework::ResourceCodec *codec =
      bess::framework::ResourceBindings::ProcessDefault().Find(*resource);
  EXPECT_NE(nullptr, codec);
  bess::pb::L2ForwardMacKey key;
  key.set_addr(addr);
  google::protobuf::Any k;
  EXPECT_TRUE(k.PackFrom(key));
  auto rkey = codec->Key(k.type_url(), k.value());
  EXPECT_TRUE(rkey.has_value()) << rkey.error();
  if (gate < 0) {
    return Op::Erase("l2/macs", *rkey);
  }
  bess::pb::L2ForwardMacValue value;
  value.set_gate(static_cast<uint64_t>(gate));
  google::protobuf::Any v;
  EXPECT_TRUE(v.PackFrom(value));
  auto rvalue = codec->Value(v.type_url(), v.value());
  EXPECT_TRUE(rvalue.has_value()) << rvalue.error();
  return Op::Upsert("l2/macs", *rkey, *rvalue);
}

// Packets with the given destination MACs, classified by the module.
std::vector<gate_idx_t> Classify(const L2Forward *m, const std::vector<std::string> &dsts) {
  bess::PlainPacketPool pool(dsts.size() + 64);
  bess::PacketBatch batch;
  batch.clear();
  for (const std::string &dst : dsts) {
    bess::PacketHandle pkt = pool.Alloc(60);
    EXPECT_NE(pkt, nullptr);
    uint8_t *p = bess::PacketRef(pkt).head_data<uint8_t *>();
    std::memset(p, 0, 60);
    unsigned b[6];
    EXPECT_EQ(6, std::sscanf(dst.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3],
                             &b[4], &b[5]));
    for (int i = 0; i < 6; i++) {
      p[i] = static_cast<uint8_t>(b[i]);
    }
    batch.add(bess::PacketRef(pkt));
  }
  std::vector<gate_idx_t> gates(dsts.size());
  m->ClassifyBatch(&batch, gates.data());
  for (bess::PacketRef pkt : batch.packets()) {
    bess::PacketFree(pkt.handle());
  }
  return gates;
}

class L2ForwardTest : public ::testing::Test {
 protected:
  void TearDown() override { ModuleGraph::DestroyAllModules(); }
};

// The default gate may be given at creation (manifests have no commands); an
// invalid one refuses the module.
TEST_F(L2ForwardTest, DefaultGateAtCreation) {
  bess::pb::L2ForwardArg arg;
  arg.set_default_gate(uint64_t{1} << 20);
  pb_error_t perr;
  EXPECT_EQ(nullptr, Create(arg, &perr));
  EXPECT_EQ(EINVAL, perr.code()) << perr.errmsg();

  arg.set_default_gate(7);
  L2Forward *m = Create(arg, &perr);
  ASSERT_NE(nullptr, m) << perr.errmsg();
  EXPECT_EQ((std::vector<gate_idx_t>{7}), Classify(m, {"00:00:00:00:00:09"}));
}

// Entries written through the codec send their MACs to the gates given; a
// MAC with no entry takes the default gate, also after its entry is erased.
TEST_F(L2ForwardTest, MacsResourceDecidesTheGates) {
  bess::pb::L2ForwardArg arg;
  arg.set_default_gate(5);
  pb_error_t perr;
  L2Forward *m = Create(arg, &perr);
  ASSERT_NE(nullptr, m) << perr.errmsg();

  std::vector<Op> add = {MacOp("00:00:00:00:00:01", 0), MacOp("02:00:00:00:00:02", 3),
                         MacOp("aa:bb:cc:dd:ee:ff", 8192)};  // 8192: DROP_GATE
  ASSERT_EQ(Outcome::kApplied, Engine().Apply(add).outcome);
  EXPECT_EQ((std::vector<gate_idx_t>{0, 3, 8192, 5}),
            Classify(m, {"00:00:00:00:00:01", "02:00:00:00:00:02", "aa:bb:cc:dd:ee:ff",
                         "00:00:00:00:00:04"}));

  std::vector<Op> erase = {MacOp("02:00:00:00:00:02", -1)};
  ASSERT_EQ(Outcome::kApplied, Engine().Apply(erase).outcome);
  EXPECT_EQ((std::vector<gate_idx_t>{0, 5}),
            Classify(m, {"00:00:00:00:00:01", "02:00:00:00:00:02"}));
}

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;

bess::pb::L2ForwardCommandAddArg Entries(
    const std::vector<std::pair<std::string, int64_t>> &entries) {
  bess::pb::L2ForwardCommandAddArg arg;
  for (const auto &[addr, gate] : entries) {
    auto *e = arg.add_entries();
    e->set_addr(addr);
    e->set_gate(gate);
  }
  return arg;
}

template <typename Arg>
Arg Addrs(const std::vector<std::string> &addrs) {
  Arg arg;
  for (const std::string &a : addrs) {
    arg.add_addrs(a);
  }
  return arg;
}

// The gates lookup answers for `addrs` (the test fails if it refuses).
std::vector<uint64_t> Lookup(ModuleHarness &h, Module *m, const std::vector<std::string> &addrs) {
  bess::pb::L2ForwardCommandLookupResponse resp;
  EXPECT_TRUE(h.MustCommand(m, "lookup", Addrs<bess::pb::L2ForwardCommandLookupArg>(addrs))
                  .UnpackTo(&resp));
  return {resp.gates().begin(), resp.gates().end()};
}

// The errno lookup refuses `addrs` with (0: answered).
int LookupError(ModuleHarness &h, Module *m, const std::vector<std::string> &addrs) {
  return h.Command(m, "lookup", Addrs<bess::pb::L2ForwardCommandLookupArg>(addrs))
      .error()
      .code();
}

bess::pb::L2ForwardCommandPopulateArg Populate(const std::string &base, int64_t count,
                                               int64_t gate_count) {
  bess::pb::L2ForwardCommandPopulateArg arg;
  arg.set_base(base);
  arg.set_count(count);
  arg.set_gate_count(gate_count);
  return arg;
}

bess::pb::L2ForwardArg TableArg(int64_t size, int64_t bucket) {
  bess::pb::L2ForwardArg arg;
  arg.set_size(size);
  arg.set_bucket(bucket);
  return arg;
}

Bytes ToMac(const std::string &dst) {
  return PacketBuilder()
      .Ether(dst, "02:00:00:00:00:99")
      .IPv4("10.0.0.1", "10.0.0.2")
      .Udp(1000, 53)
      .PadTo(60)
      .Build();
}

// Entries added by command: a MAC already present is refused (EEXIST);
// lookup answers each MAC's gate and packets to those MACs leave there;
// delete removes an entry, and deleting it again is refused (ENOENT).
TEST(L2ForwardCommandTest, AddLookupDelete) {
  ModuleHarness h;
  Module *l2 = h.Create("L2Forward", bess::pb::L2ForwardArg());
  ASSERT_NE(nullptr, l2);
  h.MustCommand(l2, "add",
                Entries({{"00:01:02:03:04:05", 64}, {"aa:bb:cc:dd:ee:ff", 1},
                         {"11:11:11:11:11:22", 2}}));
  EXPECT_EQ(EEXIST, h.Command(l2, "add", Entries({{"00:01:02:03:04:05", 0}})).error().code());

  EXPECT_EQ((std::vector<uint64_t>{1, 64}), Lookup(h, l2, {"aa:bb:cc:dd:ee:ff", "00:01:02:03:04:05"}));
  const Bytes to_1 = ToMac("aa:bb:cc:dd:ee:ff");
  const Bytes to_64 = ToMac("00:01:02:03:04:05");
  const Bytes to_2 = ToMac("11:11:11:11:11:22");
  auto out = h.Run(l2, 0, {to_1, to_64, to_2}, {0, 1, 2, 64});
  EXPECT_TRUE(out[0].empty());
  EXPECT_EQ((std::vector<Bytes>{to_1}), out[1]);
  EXPECT_EQ((std::vector<Bytes>{to_2}), out[2]);
  EXPECT_EQ((std::vector<Bytes>{to_64}), out[64]);

  h.MustCommand(l2, "delete", Addrs<bess::pb::L2ForwardCommandDeleteArg>({"00:01:02:03:04:05"}));
  EXPECT_EQ(ENOENT, h.Command(l2, "delete",
                              Addrs<bess::pb::L2ForwardCommandDeleteArg>({"00:01:02:03:04:05"}))
                        .error()
                        .code());
  EXPECT_EQ(ENOENT, LookupError(h, l2, {"00:01:02:03:04:05"}));
  // No entry and no default gate: dropped.
  out = h.Run(l2, 0, {to_64}, {0, 1, 2, 64});
  EXPECT_TRUE(out[64].empty());
}

// Commands validate everything before applying anything: int64 wire gates
// are checked before narrowing (65536 is not gate 0, -1 not 65535; 8193 is
// past DROP_GATE), a bad entry adds nothing (not even the good one before
// it), a MAC given twice in one add is refused, and populate with
// gate_count 0 is refused (it used to divide by zero and crash bessd).
TEST(L2ForwardCommandTest, CommandsValidateBeforeApplying) {
  ModuleHarness h;
  Module *l2 = h.Create("L2Forward", bess::pb::L2ForwardArg());
  ASSERT_NE(nullptr, l2);
  for (const int64_t gate : {int64_t{65536}, int64_t{-1}, int64_t{8193}}) {
    EXPECT_EQ(EINVAL, h.Command(l2, "add", Entries({{"00:01:02:03:04:05", gate}})).error().code())
        << gate;
  }
  EXPECT_EQ(ENOENT, LookupError(h, l2, {"00:01:02:03:04:05"}));

  EXPECT_EQ(EINVAL,
            h.Command(l2, "add", Entries({{"00:01:02:03:04:06", 1}, {"not-a-mac", 2}}))
                .error()
                .code());
  EXPECT_EQ(ENOENT, LookupError(h, l2, {"00:01:02:03:04:06"})) << "nothing was added";

  EXPECT_EQ(EEXIST,
            h.Command(l2, "add", Entries({{"00:01:02:03:04:07", 1}, {"00:01:02:03:04:07", 2}}))
                .error()
                .code());
  EXPECT_EQ(ENOENT, LookupError(h, l2, {"00:01:02:03:04:07"}));

  EXPECT_EQ(EINVAL,
            h.Command(l2, "populate", Populate("00:01:02:03:00:00", 10, 0)).error().code());
  EXPECT_EQ(ENOENT, LookupError(h, l2, {"00:01:02:03:00:00"}));

  bess::pb::L2ForwardCommandSetDefaultGateArg dflt;
  dflt.set_gate(65536);
  EXPECT_EQ(EINVAL, h.Command(l2, "set_default_gate", dflt).error().code());
  // Still the default (drop), not gate 0.
  EXPECT_TRUE(h.Run(l2, 0, {ToMac("02:00:00:00:00:42")}, {0})[0].empty());

  h.MustCommand(l2, "add", Entries({{"00:01:02:03:04:08", 5}}));
  EXPECT_EQ((std::vector<uint64_t>{5}), Lookup(h, l2, {"00:01:02:03:04:08"}));
}

// Init's table limits: size 1 used to compute an alternate bucket far
// outside the table, and int64 sizes narrowed to int (2^32 + 1 became 1);
// sizes must be powers of two. A two-bucket table of 4 works.
TEST(L2ForwardCommandTest, TableSizeLimits) {
  ModuleHarness h;
  for (const auto &bad : {TableArg(1, 0), TableArg((int64_t{1} << 32) + 1, 0), TableArg(-4, 0),
                          TableArg(0, (int64_t{1} << 32) + 4), TableArg(6, 0)}) {
    EXPECT_NE("", h.CreateError("L2Forward", bad)) << bad.ShortDebugString();
  }
  Module *l2 = h.Create("L2Forward", TableArg(2, 4));
  ASSERT_NE(nullptr, l2);
  h.MustCommand(l2, "add", Entries({{"02:00:00:00:00:01", 3}}));
  EXPECT_EQ((std::vector<uint64_t>{3}), Lookup(h, l2, {"02:00:00:00:00:01"}));
}

// populate starts at `base` (an extra shift used to start two bytes lower),
// counts up across byte boundaries, cycles the gates, and refuses more
// entries than the table has slots.
TEST(L2ForwardCommandTest, PopulateStartsAtBaseAndCyclesGates) {
  ModuleHarness h;
  Module *l2 = h.Create("L2Forward", TableArg(64, 4));
  ASSERT_NE(nullptr, l2);
  h.MustCommand(l2, "populate", Populate("aa:bb:cc:dd:ee:fe", 4, 3));
  EXPECT_EQ((std::vector<uint64_t>{0, 1, 2, 0}),
            Lookup(h, l2, {"aa:bb:cc:dd:ee:fe", "aa:bb:cc:dd:ee:ff", "aa:bb:cc:dd:ef:00",
                           "aa:bb:cc:dd:ef:01"}));
  EXPECT_EQ(ENOENT, LookupError(h, l2, {"00:00:aa:bb:cc:dd"}));
  EXPECT_EQ(EINVAL,
            h.Command(l2, "populate", Populate("02:00:00:00:00:00", 64 * 4 + 1, 1)).error().code());
  EXPECT_EQ(ENOENT, LookupError(h, l2, {"02:00:00:00:00:00"}));
}

// A multi-entry add that runs out of space takes back what it added.
TEST(L2ForwardCommandTest, AddRollsBackWhenFull) {
  ModuleHarness h;
  Module *l2 = h.Create("L2Forward", TableArg(2, 1));  // two slots
  ASSERT_NE(nullptr, l2);
  std::vector<std::pair<std::string, int64_t>> entries;
  for (int i = 1; i <= 5; i++) {
    char addr[18];
    std::snprintf(addr, sizeof(addr), "02:00:00:00:00:%02x", i);
    entries.emplace_back(addr, i);
  }
  EXPECT_EQ(ENOMEM, h.Command(l2, "add", Entries(entries)).error().code());
  for (const auto &[addr, gate] : entries) {
    EXPECT_EQ(ENOENT, LookupError(h, l2, {addr})) << addr;
  }
}

}  // namespace
