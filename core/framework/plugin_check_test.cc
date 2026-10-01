// SPDX-License-Identifier: BSD-3-Clause

#include "framework/plugin_check.h"

#include <gtest/gtest.h>

namespace {

using bess::framework::CheckPluginDescriptor;

BessPluginDescriptor Valid() {
  return BessPluginDescriptor{
      .abi_version = BESS_PLUGIN_ABI_VERSION,
      .name = "p",
      .version = "1",
      .api_min = 3,
      .api_max = 5,
      .required_capabilities = BESS_CAP_INIT_CONTEXT,
  };
}

TEST(PluginCheckTest, AcceptsADescriptorWhoseRangeContainsTheDaemonApi) {
  EXPECT_EQ("", CheckPluginDescriptor(Valid(), /*api_version=*/3,
                                      BESS_CAP_INIT_CONTEXT));
  EXPECT_EQ("", CheckPluginDescriptor(Valid(), 5, BESS_CAP_INIT_CONTEXT));
}

TEST(PluginCheckTest, RefusesAnApiOutsideTheRangeOnEitherSide) {
  const std::string too_new =
      CheckPluginDescriptor(Valid(), 6, BESS_CAP_INIT_CONTEXT);
  EXPECT_NE(std::string::npos, too_new.find("[3, 5]")) << too_new;
  EXPECT_NE(std::string::npos, too_new.find("API 6")) << too_new;
  EXPECT_NE("", CheckPluginDescriptor(Valid(), 2, BESS_CAP_INIT_CONTEXT));
}

TEST(PluginCheckTest, RefusesAnInvertedRange) {
  BessPluginDescriptor d = Valid();
  d.api_min = 5;
  d.api_max = 3;
  EXPECT_NE(std::string::npos,
            CheckPluginDescriptor(d, 4, BESS_CAP_INIT_CONTEXT)
                .find("invalid API range"));
}

TEST(PluginCheckTest, RefusesAnUnknownDescriptorLayout) {
  BessPluginDescriptor d = Valid();
  d.abi_version = BESS_PLUGIN_ABI_VERSION + 1;
  EXPECT_NE(std::string::npos,
            CheckPluginDescriptor(d, 4, BESS_CAP_INIT_CONTEXT)
                .find("layout version"));
}

TEST(PluginCheckTest, NamesTheMissingCapabilities) {
  BessPluginDescriptor d = Valid();
  d.required_capabilities = BESS_CAP_INIT_CONTEXT | BESS_CAP_INSTANCES;
  const std::string why = CheckPluginDescriptor(d, 4, BESS_CAP_INIT_CONTEXT);
  EXPECT_NE(std::string::npos, why.find("0x2")) << why;  // only INSTANCES
}

TEST(PluginCheckTest, ADaemonWithEveryCapabilityAcceptsTheMacroDefaults) {
  // BESS_PLUGIN declares exactly the API it was built against, no capability.
  BessPluginDescriptor d = Valid();
  d.api_min = d.api_max = BESS_PLUGIN_API_VERSION;
  d.required_capabilities = 0;
  EXPECT_EQ("", CheckPluginDescriptor(d));
}

}  // namespace
