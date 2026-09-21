#include "gate.h"
#include "module.h"
#include "port.h"
#include "resume_hook.h"

#include <gtest/gtest.h>

TEST(BuiltInRegistryTest, DiscoversLinkedComponents) {
  const auto &modules = ModuleBuilder::all_module_builders();
  const auto &ports = PortBuilder::all_port_builders();
  const auto &gate_hooks = bess::GateHookBuilder::all_gate_hook_builders();

  EXPECT_NE(modules.find("ACL"), modules.end());
  EXPECT_NE(ports.find("PMDPort"), ports.end());
  EXPECT_NE(gate_hooks.find("Track"), gate_hooks.end());
  EXPECT_FALSE(bess::global_resume_hooks.empty());
}
