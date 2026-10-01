// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_PLUGIN_H_
#define BESS_FRAMEWORK_PLUGIN_H_

#include <cstdint>

#define BESS_PLUGIN_ABI_VERSION 1

// Decision D-041 (docs/decisions.md): descriptor v1 is metadata only. The C++
// Module API has no stable binary ABI; external plugins must be rebuilt for
// their target BESS release.
struct BessPluginDescriptor {
  uint32_t abi_version;  // Must be BESS_PLUGIN_ABI_VERSION
  const char *name;
  const char *version;
};

#define BESS_PLUGIN(_name, _version)                                      \
  extern "C" const BessPluginDescriptor *bess_plugin_descriptor_v1() {    \
    static const BessPluginDescriptor desc = {                            \
        .abi_version = BESS_PLUGIN_ABI_VERSION,                           \
        .name = _name,                                                    \
        .version = _version,                                              \
    };                                                                    \
    return &desc;                                                         \
  }


#endif  // BESS_FRAMEWORK_PLUGIN_H_
