// SPDX-License-Identifier: BSD-3-Clause

// PortInc's "<name>/settings" singleton (D-129) in process: the burst set
// through the resource's codec is what each receive asks the port for, it
// reads back canonically, and a burst set_burst refuses is refused. And
// mark_attr (D-185): each packet's rte_flow MARK, as a reader sees it.

#include <gtest/gtest.h>

#include <cerrno>
#include <memory>
#include <string>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "module.h"
#include "module_graph.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "pktbatch.h"
#include "port.h"
#include "runtime/runtime_state.h"
#include "testing/marked_rx_port.h"
#include "testing/module_harness.h"
#include "worker.h"

namespace {

using bess::testing::MarkedRxPort;
using bess::testing::ModuleHarness;

// A port with one incoming queue that always has as many 60-byte packets as
// asked for.
class PortIncTestPort final : public Port {
 public:
  CommandResponse Init(const google::protobuf::Any &) { return CommandSuccess(); }
  void DeInit() override {}
  int RecvPackets(queue_t, bess::PacketHandle *pkts, int cnt) override {
    if (cnt == 0 || !current_worker.packet_pool()->AllocBulk(pkts, cnt, 60)) {
      return 0;
    }
    return cnt;
  }
  int SendPackets(queue_t, bess::PacketHandle *, int) override { return 0; }
};

ADD_DRIVER(PortIncTestPort, "port_inc_test", "PortInc test port");
ADD_DRIVER(MarkedRxPort, "marked_rx", "a port whose packets carry RX MARKs");

// True when "<resource>"'s codec says applying it requires paused workers.
bool RequiresPausedWorkers(const std::string &resource) {
  const bess::dataplane::Resource *r =
      bess::runtime::runtime().transactions().FindResource(resource);
  EXPECT_NE(nullptr, r) << resource;
  if (r == nullptr) {
    return false;
  }
  const bess::framework::ResourceCodec *codec =
      bess::framework::ResourceBindings::ProcessDefault().Find(*r);
  EXPECT_NE(nullptr, codec) << resource;
  return codec != nullptr && codec->requires_paused_workers();
}

bess::pb::PortIncSettings Settings(uint64_t burst) {
  bess::pb::PortIncSettings s;
  s.set_burst(burst);
  return s;
}

class PortIncSettingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto &drivers = PortBuilder::all_port_builders();
    const auto builder = drivers.find("PortIncTestPort");
    ASSERT_NE(builder, drivers.end());
    std::unique_ptr<Port> port(builder->second.CreatePort("port-inc-test"));
    port->num_queues[PACKET_DIR_INC] = 1;
    ASSERT_TRUE(bess::runtime::runtime().ports().Add(std::move(port)));
  }

  void TearDown() override { bess::runtime::runtime().ports().Clear(); }

  static bess::pb::PortIncArg Arg() {
    bess::pb::PortIncArg arg;
    arg.set_port("port-inc-test");
    return arg;
  }
};

TEST_F(PortIncSettingsTest, BurstIsWhatEachReceiveAsksFor) {
  ModuleHarness h;
  Module *inc = h.Create("PortInc", Arg(), "pi");
  ASSERT_NE(nullptr, inc);
  EXPECT_EQ(bess::PacketBatch::kMaxBurst, h.RunTask(inc, {0})[0].size());

  ASSERT_EQ("", h.SetResource("pi/settings", Settings(4)));
  EXPECT_EQ(4u, h.RunTask(inc, {0})[0].size());
}

TEST_F(PortIncSettingsTest, ReadBackRoundTrips) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("PortInc", Arg(), "pi"));
  const bess::pb::PortIncSettings initial = Settings(bess::PacketBatch::kMaxBurst);
  EXPECT_EQ("", h.ReadBackDiffers("pi/settings", {{nullptr, &initial}}));
  const bess::pb::PortIncSettings s = Settings(9);
  ASSERT_EQ("", h.SetResource("pi/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("pi/settings", {{nullptr, &s}}));
}

TEST_F(PortIncSettingsTest, BadBurstRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("PortInc", Arg(), "pi"));
  EXPECT_NE("", h.SetResource("pi/settings", Settings(bess::PacketBatch::kMaxBurst + 1)));
  EXPECT_NE("", h.SetResource("pi/settings", Settings(uint64_t{1} << 32)));
  const bess::pb::PortIncSettings unchanged = Settings(bess::PacketBatch::kMaxBurst);
  EXPECT_EQ("", h.ReadBackDiffers("pi/settings", {{nullptr, &unchanged}}));
}

TEST_F(PortIncSettingsTest, AppliesLive) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("PortInc", Arg(), "pi"));
  EXPECT_FALSE(RequiresPausedWorkers("pi/settings"));
}

class PortIncMarkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto &drivers = PortBuilder::all_port_builders();
    const auto builder = drivers.find("MarkedRxPort");
    ASSERT_NE(builder, drivers.end());
    std::unique_ptr<Port> port(builder->second.CreatePort("marked-rx"));
    port->num_queues[PACKET_DIR_INC] = 1;
    ASSERT_TRUE(bess::runtime::runtime().ports().Add(std::move(port)));
  }

  void TearDown() override { bess::runtime::runtime().ports().Clear(); }
};

// D-185: a Split on the 4-byte attribute sends each packet to the gate of its
// MARK: hash.fdir.hi where RTE_MBUF_F_RX_FDIR_ID is set, else 0, even when
// hash.fdir.hi holds a value.
TEST_F(PortIncMarkTest, MarkAttrCarriesEachPacketsMark) {
  ModuleHarness h;
  bess::pb::PortIncArg arg;
  arg.set_port("marked-rx");
  arg.set_mark_attr("rx_mark");
  Module *inc = h.Create("PortInc", arg);
  ASSERT_NE(nullptr, inc);
  bess::pb::SplitArg split_arg;
  split_arg.set_attribute("rx_mark");
  split_arg.set_size(4);
  Module *split = h.Create("Split", split_arg);
  ASSERT_NE(nullptr, split);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(inc, 0, split, 0, true));

  EXPECT_EQ(bess::testing::MarkedRxExpected(),
            h.RunTaskPipeline(inc, split, bess::testing::MarkedRxGates()));
}

// Without mark_attr nothing is registered or written, and the packets pass
// as before.
TEST_F(PortIncMarkTest, NoMarkAttrRegistersNothing) {
  ModuleHarness h;
  bess::pb::PortIncArg arg;
  arg.set_port("marked-rx");
  Module *inc = h.Create("PortInc", arg);
  ASSERT_NE(nullptr, inc);
  EXPECT_TRUE(inc->all_attrs().empty());
  EXPECT_EQ(bess::testing::MarkedRxPackets().size(), h.RunTask(inc, {0})[0].size());
}

}  // namespace
