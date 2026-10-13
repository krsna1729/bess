// SPDX-License-Identifier: BSD-3-Clause

// PortOut's commands as control_v2 runs them (D-129): it has no actions;
// get_initial_arg is legacy-only (GetPipeline's Module.arg replaces it).
// InvokeAction refuses it, the legacy command still answers the argument.

#include <gtest/gtest.h>

#include <cerrno>
#include <memory>

#include "module.h"
#include "pb/module_msg.pb.h"
#include "port.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"

namespace {

using bess::testing::ModuleHarness;

// A port with one outgoing queue that sends nothing anywhere.
class PortOutTestPort final : public Port {
 public:
  CommandResponse Init(const google::protobuf::Any &) { return CommandSuccess(); }
  void DeInit() override {}
  int RecvPackets(queue_t, bess::PacketHandle *, int) override { return 0; }
  int SendPackets(queue_t, bess::PacketHandle *, int) override { return 0; }
};

ADD_DRIVER(PortOutTestPort, "port_out_test", "PortOut test port");

class PortOutTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto &drivers = PortBuilder::all_port_builders();
    const auto builder = drivers.find("PortOutTestPort");
    ASSERT_NE(builder, drivers.end());
    std::unique_ptr<Port> port(builder->second.CreatePort("port-out-test"));
    port->num_queues[PACKET_DIR_OUT] = 1;
    ASSERT_TRUE(bess::runtime::runtime().ports().Add(std::move(port)));
  }

  void TearDown() override { bess::runtime::runtime().ports().Clear(); }
};

TEST_F(PortOutTest, InitialArgIsLegacyOnly) {
  ModuleHarness h;
  bess::pb::PortOutArg arg;
  arg.set_port("port-out-test");
  Module *out = h.Create("PortOut", arg);
  ASSERT_NE(nullptr, out);

  EXPECT_EQ(ENOTSUP, h.Action(out, "get_initial_arg", bess::pb::EmptyArg()).error().code());
  bess::pb::PortOutArg back;
  ASSERT_TRUE(h.MustCommand(out, "get_initial_arg", bess::pb::EmptyArg()).UnpackTo(&back));
  EXPECT_EQ("port-out-test", back.port());
}

}  // namespace
