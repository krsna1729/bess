// SPDX-License-Identifier: BSD-3-Clause

#include "framework/plugin_loader.h"

#include <dirent.h>
#include <dlfcn.h>

#include <glog/logging.h>

#include <algorithm>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "framework/plugin_check.h"

namespace bess::framework {

// Return true if string s has specified suffix.
static inline bool HasSuffix(const std::string &s, const std::string &suffix) {
  return (s.size() >= suffix.size()) &&
         std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

// Store handles of loaded plugins
// key: plugin path (std::string), value: handle (void *)
static std::unordered_map<std::string, void *> plugin_handles;

std::vector<std::string> ListPlugins() {
  std::vector<std::string> list;
  for (auto &kv : plugin_handles) {
    list.push_back(kv.first);
  }
  return list;
}

namespace {

enum class PluginLoad { kLoaded, kRetry, kRefused };

// dlopen()s `path`. A plugin that exports a descriptor must match this daemon
// (D-047); a refused plugin is closed again, which deregisters the modules its
// static constructors registered. A plugin without a descriptor is a legacy
// plugin and loads as before.
PluginLoad TryLoadPlugin(const std::string &path) {
  void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (handle == nullptr) {
    return PluginLoad::kRetry;
  }
  if (void *symbol = dlsym(handle, "bess_plugin_descriptor_v1")) {
    const auto *descriptor =
        reinterpret_cast<const BessPluginDescriptor *(*)()>(symbol)();
    std::string reason = descriptor == nullptr
                             ? "descriptor function returned null"
                             : framework::CheckPluginDescriptor(*descriptor);
    if (!reason.empty()) {
      LOG(ERROR) << "Plugin " << path << " refused: " << reason;
      dlclose(handle);
      return PluginLoad::kRefused;
    }
    LOG(INFO) << "Plugin " << path << " declares "
              << (descriptor->name ? descriptor->name : "?") << " "
              << (descriptor->version ? descriptor->version : "?");
  }
  plugin_handles.emplace(path, handle);
  return PluginLoad::kLoaded;
}

}  // namespace

bool LoadPlugin(const std::string &path) {
  return TryLoadPlugin(path) == PluginLoad::kLoaded;
}

bool UnloadPlugin(const std::string &path) {
  auto it = plugin_handles.find(path);
  if (it == plugin_handles.end()) {
    VLOG(1) << "Plugin " << path << " not found.";
    return false;
  }
  bool success = (dlclose(it->second) == 0);
  if (success) {
    plugin_handles.erase(it);
  } else {
    LOG(WARNING) << "Error unloading module " << path << ": " << dlerror();
  }
  return success;
}

bool LoadPlugins(const std::string &directory) {
  DIR *dir = opendir(directory.c_str());
  if (!dir) {
    return false;
  }

  std::list<std::string> remaining;
  size_t refused = 0;
  dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if ((entry->d_type == DT_REG || entry->d_type == DT_LNK) &&
        HasSuffix(entry->d_name, ".so")) {
      const std::string full_path = directory + "/" + entry->d_name;
      remaining.push_back(full_path);
    }
  }

  for (int pass = 1; pass <= kInheritanceLimit && remaining.size() > 0;
       ++pass) {
    for (auto it = remaining.begin(); it != remaining.end();) {
      const std::string full_path = *it;
      LOG(INFO) << "Loading plugin (attempt " << pass << "): " << full_path;
      const PluginLoad result = TryLoadPlugin(full_path);
      if (result == PluginLoad::kRetry) {
        VLOG(1) << "Error loading plugin " << full_path
                << "dlerror=" << dlerror();
        ++it;
      } else {
        // Loaded, or refused for good: another pass cannot change either.
        if (result == PluginLoad::kRefused) {
          refused++;
        }
        it = remaining.erase(it);
      }
    }
  }

  for (auto it = remaining.begin(); it != remaining.end(); ++it) {
    LOG(ERROR)
        << "Failed to load plugin " << *it
        << ". Run daemon in verbose mode (--v=1) to see dlopen() attempts.";
  }

  closedir(dir);
  return (remaining.size() == 0 && refused == 0);
}

}  // namespace bess::framework
