// SPDX-License-Identifier: BSD-3-Clause

#include "framework/plugin_loader.h"

#include <dirent.h>
#include <dlfcn.h>

#include "utils/logging.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "framework/plugin_check.h"
#include "gate.h"
#include "module.h"
#include "port.h"

namespace bess::framework {

// Return true if string s has specified suffix.
static inline bool HasSuffix(const std::string &s, const std::string &suffix) {
  return (s.size() >= suffix.size()) &&
         std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

// Store handles of loaded plugins
// key: plugin path (std::string), value: handle (void *)
static std::unordered_map<std::string, void *> plugin_handles;
// What each loaded plugin registered.
static std::unordered_map<std::string, PluginContents> plugin_contents;

namespace {

template <typename Map>
std::vector<std::string> KeysOf(const Map &m) {
  std::vector<std::string> keys;
  keys.reserve(m.size());
  for (const auto &[key, value] : m) {
    keys.push_back(key);
  }
  return keys;
}

// Keys of `after` absent from `before` (both sorted: map order).
std::vector<std::string> Added(const std::vector<std::string> &before,
                               const std::vector<std::string> &after) {
  std::vector<std::string> added;
  std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                      std::back_inserter(added));
  return added;
}

}  // namespace

std::vector<std::string> ListPlugins() {
  std::vector<std::string> list;
  for (auto &kv : plugin_handles) {
    list.push_back(kv.first);
  }
  return list;
}

std::vector<LoadedPlugin> LoadedPlugins() {
  std::vector<LoadedPlugin> list;
  for (const auto &[path, handle] : plugin_handles) {
    LoadedPlugin p{path, "", "", 0};
    using Get = const BessPluginDescriptor *(*)();
    if (void *symbol = dlsym(handle, "bess_plugin_descriptor_v1")) {
      if (const BessPluginDescriptor *d = reinterpret_cast<Get>(symbol)()) {
        p.name = d->name != nullptr ? d->name : "";
        p.version = d->version != nullptr ? d->version : "";
        p.required_capabilities = d->required_capabilities;
      }
    }
    list.push_back(std::move(p));
  }
  std::sort(list.begin(), list.end(),
            [](const LoadedPlugin &a, const LoadedPlugin &b) { return a.path < b.path; });
  return list;
}

namespace {

enum class PluginLoad { kLoaded, kRetry, kRefused };

// dlopen()s `path`. A plugin that exports a descriptor must match this daemon
// (D-047); a refused plugin is closed again, which deregisters the modules its
// static constructors registered. A plugin without a descriptor is a legacy
// plugin and loads as before.
PluginLoad TryLoadPlugin(const std::string &path) {
  const auto modules_before = KeysOf(ModuleBuilder::all_module_builders());
  const auto drivers_before = KeysOf(PortBuilder::all_port_builders());
  const auto hooks_before = KeysOf(bess::GateHookBuilder::all_gate_hook_builders());
  void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (handle == nullptr) {
    return PluginLoad::kRetry;
  }
  PluginContents contents{
      Added(modules_before, KeysOf(ModuleBuilder::all_module_builders())),
      Added(drivers_before, KeysOf(PortBuilder::all_port_builders())),
      Added(hooks_before, KeysOf(bess::GateHookBuilder::all_gate_hook_builders()))};
  if (void *symbol = dlsym(handle, "bess_plugin_descriptor_v1")) {
    const auto *descriptor =
        reinterpret_cast<const BessPluginDescriptor *(*)()>(symbol)();
    std::string reason = descriptor == nullptr
                             ? "descriptor function returned null"
                             : framework::CheckPluginDescriptor(*descriptor);
    if (!reason.empty()) {
      LOG(ERROR) << "Plugin " << path << " refused: " << reason;
      for (const std::string &d : contents.port_drivers) {
        PortBuilder::DeregisterPortClass(d);
      }
      for (const std::string &h : contents.gate_hooks) {
        bess::GateHookBuilder::all_gate_hook_builders_holder().erase(h);
      }
      dlclose(handle);
      return PluginLoad::kRefused;
    }
    LOG(INFO) << "Plugin " << path << " declares "
              << (descriptor->name ? descriptor->name : "?") << " "
              << (descriptor->version ? descriptor->version : "?");
  }
  plugin_handles.emplace(path, handle);
  plugin_contents[path] = std::move(contents);
  return PluginLoad::kLoaded;
}

}  // namespace

bool LoadPlugin(const std::string &path) {
  return TryLoadPlugin(path) == PluginLoad::kLoaded;
}

bool PluginContentsOf(const std::string &path, PluginContents *out) {
  auto it = plugin_contents.find(path);
  if (it == plugin_contents.end()) {
    return false;
  }
  *out = it->second;
  return true;
}

bool CodeInPlugin(const std::string &path, const void *address) {
  Dl_info info{};
  return plugin_handles.contains(path) && dladdr(address, &info) != 0 &&
         info.dli_fname != nullptr && path == info.dli_fname;
}

bool UnloadPlugin(const std::string &path) {
  auto it = plugin_handles.find(path);
  if (it == plugin_handles.end()) {
    VLOG(1) << "Plugin " << path << " not found.";
    return false;
  }
  // Drivers and gate hooks register without a destructor to undo it: the
  // builders would keep calling into unmapped code.
  // (Removed before dlclose: destroying a builder runs the plugin's code.)
  std::vector<std::string> module_classes;
  if (auto c = plugin_contents.find(path); c != plugin_contents.end()) {
    for (const std::string &d : c->second.port_drivers) {
      PortBuilder::DeregisterPortClass(d);
    }
    for (const std::string &h : c->second.gate_hooks) {
      bess::GateHookBuilder::all_gate_hook_builders_holder().erase(h);
    }
    module_classes = std::move(c->second.module_classes);
    plugin_contents.erase(c);
  }
  bool success = (dlclose(it->second) == 0);
  if (success) {
    plugin_handles.erase(it);
    // dlclose succeeds without unmapping a library another loaded object
    // still needs, or one marked NODELETE: its static destructors did not
    // run, so its module classes stay registered. Keep it as loaded (with
    // what is still registered) and say so, rather than report it gone.
    if (void *still = dlopen(path.c_str(), RTLD_NOLOAD | RTLD_NOW)) {
      LOG(ERROR) << "Plugin " << path << " stays mapped after dlclose (another loaded "
                 << "object needs it, or it is NODELETE); its port drivers and gate "
                 << "hooks were removed, its module classes remain";
      plugin_handles.emplace(path, still);
      plugin_contents[path] = PluginContents{std::move(module_classes), {}, {}};
      return false;
    }
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
