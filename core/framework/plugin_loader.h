// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_PLUGIN_LOADER_H_
#define BESS_FRAMEWORK_PLUGIN_LOADER_H_

#include <string>
#include <vector>

namespace bess::framework {

// When Modules extend other Modules, they may reference a shared object
// that has not yet been loaded by the BESS daemon. kInheritanceLimit is
// the number of passes that will be made while loading Module shared objects,
// and thus the maximum inheritance depth of any Module.
inline constexpr int kInheritanceLimit = 10;

// Load an individual plugin specified by path. Returns true upon success.
bool LoadPlugin(const std::string &path);

// Unload a loaded plugin specified by path. Returns true upon success.
bool UnloadPlugin(const std::string &path);

// Load all the .so files in the specified directory. Returns true upon
// success.
bool LoadPlugins(const std::string &directory);

// List all imported .so files.
std::vector<std::string> ListPlugins();

// A loaded plugin as its descriptor names it.
struct LoadedPlugin {
  std::string path;
  std::string name;
  std::string version;
  uint64_t required_capabilities = 0;
};
std::vector<LoadedPlugin> LoadedPlugins();

}  // namespace bess::framework

#endif  // BESS_FRAMEWORK_PLUGIN_LOADER_H_
