// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_PLUGIN_H_
#define BESS_FRAMEWORK_PLUGIN_H_

#include <cstdint>

// Decisions D-041 and D-047 (docs/decisions.md): the descriptor is a small C
// ABI that lets the daemon diagnose an incompatible plugin before it runs any
// of the plugin's code beyond static constructors. The C++ Module API has no
// stable binary ABI; external plugins are rebuilt for their target BESS
// release, and the API range below says which releases that is.

// Layout version of BessPluginDescriptor itself.
#define BESS_PLUGIN_ABI_VERSION 1

// Version of the supported source API (module, packet and port headers and the
// framework headers listed in plugin-api.md). It increases when a change to
// those headers can break a plugin's source. A daemon at version N loads a
// plugin whose [api_min, api_max] contains N.
#define BESS_PLUGIN_API_VERSION 1

// Optional daemon facilities a plugin can require. A daemon refuses to load a
// plugin that requires a facility it was not built with.
#define BESS_CAP_INIT_CONTEXT (UINT64_C(1) << 0)  // Module::init_context()
#define BESS_CAP_INSTANCES (UINT64_C(1) << 1)     // init_context().instances()
#define BESS_CAP_RESOURCES (UINT64_C(1) << 2)     // transactional resources
#define BESS_CAP_METRICS (UINT64_C(1) << 3)       // init_context().metrics()

struct BessPluginDescriptor {
  uint32_t abi_version;  // Must be BESS_PLUGIN_ABI_VERSION
  const char *name;
  const char *version;
  uint32_t api_min;                // lowest BESS_PLUGIN_API_VERSION supported
  uint32_t api_max;                // highest BESS_PLUGIN_API_VERSION supported
  uint64_t required_capabilities;  // OR of BESS_CAP_* the plugin needs
};

// Declares a plugin that supports exactly the API version it was built
// against and needs no optional capability.
#define BESS_PLUGIN(_name, _version) \
  BESS_PLUGIN_REQUIRES(_name, _version, 0)

// As BESS_PLUGIN, naming the capabilities (BESS_CAP_* OR-ed) it requires.
#define BESS_PLUGIN_REQUIRES(_name, _version, _capabilities)               \
  extern "C" const BessPluginDescriptor *bess_plugin_descriptor_v1() {     \
    static const BessPluginDescriptor desc = {                             \
        .abi_version = BESS_PLUGIN_ABI_VERSION,                            \
        .name = _name,                                                     \
        .version = _version,                                               \
        .api_min = BESS_PLUGIN_API_VERSION,                                \
        .api_max = BESS_PLUGIN_API_VERSION,                                \
        .required_capabilities = (_capabilities),                          \
    };                                                                     \
    return &desc;                                                          \
  }

#endif  // BESS_FRAMEWORK_PLUGIN_H_
