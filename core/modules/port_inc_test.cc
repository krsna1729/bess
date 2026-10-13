// SPDX-License-Identifier: BSD-3-Clause

// PortInc's "<name>/settings" singleton (D-129) in process: the burst set
// through the resource's codec is what each receive asks the port for, it
// reads back canonically, a burst set_burst refuses is refused, and set_burst
// and get_initial_arg are legacy only.

#include <gtest/gtest.h>

#include <cerrno>
#include <memory>
#include <string>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "module.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "pktbatch.h"
#include "port.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"
#include "worker.h"

namespace {

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

TEST_F(PortIncSettingsTest, SetBurstAndInitialArgAreLegacyOnly) {
  ModuleHarness h;
  Module *inc = h.Create("PortInc", Arg(), "pi");
  ASSERT_NE(nullptr, inc);
  bess::pb::PortIncCommandSetBurstArg burst;
  burst.set_burst(2);
  EXPECT_EQ(ENOTSUP, h.Action(inc, "set_burst", burst).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(inc, "get_initial_arg", bess::pb::EmptyArg()).error().code());
  // The legacy commands still run, and the resource reads what they set.
  h.MustCommand(inc, "set_burst", burst);
  const bess::pb::PortIncSettings s = Settings(2);
  EXPECT_EQ("", h.ReadBackDiffers("pi/settings", {{nullptr, &s}}));
  bess::pb::PortIncCommandSetBurstArg too_big;
  too_big.set_burst(bess::PacketBatch::kMaxBurst + 1);
  EXPECT_EQ(EINVAL, h.Command(inc, "set_burst", too_big).error().code());
  bess::pb::PortIncArg back;
  ASSERT_TRUE(h.MustCommand(inc, "get_initial_arg", bess::pb::EmptyArg()).UnpackTo(&back));
  EXPECT_EQ("port-inc-test", back.port());
}

}  // namespace
