// SPDX-License-Identifier: BSD-3-Clause

#include "framework/plugin_check.h"

#include <cstdio>

namespace bess::framework {

std::string CheckPluginDescriptor(const BessPluginDescriptor &d,
                                  uint32_t api_version,
                                  uint64_t supported_capabilities) {
  char buf[192];
  if (d.abi_version != BESS_PLUGIN_ABI_VERSION) {
    std::snprintf(buf, sizeof(buf),
                  "descriptor layout version %u, this daemon reads version %u",
                  d.abi_version, BESS_PLUGIN_ABI_VERSION);
    return buf;
  }
  if (d.api_min > d.api_max) {
    std::snprintf(buf, sizeof(buf), "invalid API range [%u, %u]", d.api_min,
                  d.api_max);
    return buf;
  }
  if (api_version < d.api_min || api_version > d.api_max) {
    std::snprintf(buf, sizeof(buf),
                  "plugin supports BESS API [%u, %u], this daemon is API %u",
                  d.api_min, d.api_max, api_version);
    return buf;
  }
  const uint64_t missing = d.required_capabilities & ~supported_capabilities;
  if (missing != 0) {
    std::snprintf(buf, sizeof(buf),
                  "plugin requires capabilities 0x%llx this daemon lacks",
                  static_cast<unsigned long long>(missing));
    return buf;
  }
  return {};
}

}  // namespace bess::framework
