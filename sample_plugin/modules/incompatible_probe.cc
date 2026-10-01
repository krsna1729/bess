// SPDX-License-Identifier: BSD-3-Clause

// Test-only plugin: it declares a BESS API range no daemon supports, so the
// daemon must refuse it and roll back the module its static constructor
// registered (tools/check_sample_plugin.py checks that it never appears).

#include "framework/plugin.h"
#include "module.h"

class IncompatibleProbe final : public Module {
 public:
  static const Commands cmds;
  CommandResponse Init(const bess::pb::EmptyArg &) { return CommandSuccess(); }
};

const Commands IncompatibleProbe::cmds = {};
ADD_MODULE(IncompatibleProbe, "incompatible_probe",
           "test module of a plugin the daemon must refuse")

extern "C" const BessPluginDescriptor *bess_plugin_descriptor_v1() {
  static const BessPluginDescriptor desc = {
      .abi_version = BESS_PLUGIN_ABI_VERSION,
      .name = "incompatible_probe",
      .version = "0",
      .api_min = 9999,
      .api_max = 9999,
      .required_capabilities = 0,
  };
  return &desc;
}
