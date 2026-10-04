// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_PLUGIN_CHECK_H_
#define BESS_FRAMEWORK_PLUGIN_CHECK_H_

#include <string>

#include "framework/plugin.h"

namespace bess::framework {

// The optional facilities this daemon provides.
inline constexpr uint64_t kSupportedPluginCapabilities =
    BESS_CAP_INIT_CONTEXT | BESS_CAP_INSTANCES | BESS_CAP_RESOURCES | BESS_CAP_METRICS |
    BESS_CAP_REQUESTS | BESS_CAP_EVENTS | BESS_CAP_EVENT_SOURCES;

// Checks a plugin's descriptor against this daemon. Returns an empty string
// if the plugin may load, otherwise a one-line reason naming what is
// incompatible. Internal: used by the plugin loader, not installed.
std::string CheckPluginDescriptor(
    const BessPluginDescriptor &descriptor,
    uint32_t api_version = BESS_PLUGIN_API_VERSION,
    uint64_t supported_capabilities = kSupportedPluginCapabilities);

}  // namespace bess::framework

#endif  // BESS_FRAMEWORK_PLUGIN_CHECK_H_
