// SPDX-License-Identifier: BSD-3-Clause

// QueueInc's "<name>/settings" singleton (D-129) in process: the burst set
// through the resource's codec is what each receive asks the port's queue
// for, it reads back canonically, and a burst set_burst refuses is refused
// (set_burst went with the legacy service, D-140).

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
class QueueIncTestPort final : public Port {
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

ADD_DRIVER(QueueIncTestPort, "queue_inc_test", "QueueInc test port");

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

bess::pb::QueueIncSettings Settings(uint64_t burst) {
  bess::pb::QueueIncSettings s;
  s.set_burst(burst);
  return s;
}

class QueueIncSettingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto &drivers = PortBuilder::all_port_builders();
    const auto builder = drivers.find("QueueIncTestPort");
    ASSERT_NE(builder, drivers.end());
    std::unique_ptr<Port> port(builder->second.CreatePort("queue-inc-test"));
    port->num_queues[PACKET_DIR_INC] = 1;
    ASSERT_TRUE(bess::runtime::runtime().ports().Add(std::move(port)));
  }

  void TearDown() override { bess::runtime::runtime().ports().Clear(); }

  static bess::pb::QueueIncArg Arg() {
    bess::pb::QueueIncArg arg;
    arg.set_port("queue-inc-test");
    arg.set_qid(0);
    return arg;
  }
};

TEST_F(QueueIncSettingsTest, BurstIsWhatEachReceiveAsksFor) {
  ModuleHarness h;
  Module *inc = h.Create("QueueInc", Arg(), "qi");
  ASSERT_NE(nullptr, inc);
  EXPECT_EQ(bess::PacketBatch::kMaxBurst, h.RunTask(inc, {0})[0].size());

  ASSERT_EQ("", h.SetResource("qi/settings", Settings(3)));
  EXPECT_EQ(3u, h.RunTask(inc, {0})[0].size());
}

TEST_F(QueueIncSettingsTest, ReadBackRoundTrips) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("QueueInc", Arg(), "qi"));
  const bess::pb::QueueIncSettings initial = Settings(bess::PacketBatch::kMaxBurst);
  EXPECT_EQ("", h.ReadBackDiffers("qi/settings", {{nullptr, &initial}}));
  const bess::pb::QueueIncSettings s = Settings(11);
  ASSERT_EQ("", h.SetResource("qi/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("qi/settings", {{nullptr, &s}}));
}

TEST_F(QueueIncSettingsTest, BadBurstRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("QueueInc", Arg(), "qi"));
  EXPECT_NE("", h.SetResource("qi/settings", Settings(bess::PacketBatch::kMaxBurst + 1)));
  EXPECT_NE("", h.SetResource("qi/settings", Settings(uint64_t{1} << 32)));
  const bess::pb::QueueIncSettings unchanged = Settings(bess::PacketBatch::kMaxBurst);
  EXPECT_EQ("", h.ReadBackDiffers("qi/settings", {{nullptr, &unchanged}}));
}

TEST_F(QueueIncSettingsTest, AppliesLive) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("QueueInc", Arg(), "qi"));
  EXPECT_FALSE(RequiresPausedWorkers("qi/settings"));
}

}  // namespace
