// SPDX-License-Identifier: BSD-3-Clause

// L2Forward (D-114): the default gate at creation, and "<name>/macs" entries
// written through the resource's codec -- the path the transaction RPC and
// manifests take -- deciding the gates of the module's packet path.

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

}  // namespace
