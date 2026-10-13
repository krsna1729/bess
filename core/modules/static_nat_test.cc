// SPDX-License-Identifier: BSD-3-Clause

// StaticNAT's commands as control_v2 runs them (D-129): it has no actions;
// get_initial_arg (GetPipeline's Module.arg replaces it) and the runtime
// config pair are legacy-only. InvokeAction refuses them, the legacy command
// still runs them.

#include <gtest/gtest.h>

#include <cerrno>

#include "module.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"

namespace {

using bess::testing::ModuleHarness;

TEST(StaticNatTest, CommandsAreLegacyOnly) {
  ModuleHarness h;
  bess::pb::StaticNATArg arg;
  auto *pair = arg.add_pairs();
  pair->mutable_int_range()->set_start("10.0.0.1");
  pair->mutable_int_range()->set_end("10.0.0.4");
  pair->mutable_ext_range()->set_start("192.168.1.1");
  pair->mutable_ext_range()->set_end("192.168.1.4");
  Module *nat = h.Create("StaticNAT", arg);
  ASSERT_NE(nullptr, nat);

  for (const char *legacy : {"get_initial_arg", "get_runtime_config", "set_runtime_config"}) {
    SCOPED_TRACE(legacy);
    EXPECT_EQ(ENOTSUP, h.Action(nat, legacy, bess::pb::EmptyArg()).error().code());
    h.MustCommand(nat, legacy, bess::pb::EmptyArg());
  }

  bess::pb::StaticNATArg back;
  ASSERT_TRUE(h.MustCommand(nat, "get_initial_arg", bess::pb::EmptyArg()).UnpackTo(&back));
  ASSERT_EQ(1, back.pairs_size());
  EXPECT_EQ("10.0.0.1", back.pairs(0).int_range().start());
  EXPECT_EQ("192.168.1.1", back.pairs(0).ext_range().start());
}

}  // namespace
