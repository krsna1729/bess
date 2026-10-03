// SPDX-License-Identifier: BSD-3-Clause

#include "runtime/startup.h"

#include <dirent.h>
#include <pthread.h>
#include <signal.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "utils/logging.h"
#include <gflags/gflags.h>

namespace bess::startup {

namespace {

std::string Upper(std::string s) {
  for (char &c : s) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return s;
}

// The value of NAME in a NAME=VALUE list, or nullptr.
const char *Lookup(char **environ_list, const std::string &name) {
  for (char **e = environ_list; e != nullptr && *e != nullptr; e++) {
    const std::string entry(*e);
    if (entry.size() > name.size() && entry.compare(0, name.size(), name) == 0 &&
        entry[name.size()] == '=') {
      return *e + name.size() + 1;
    }
  }
  return nullptr;
}

// dddd:bb:dd.f (a PCI address in DPDK's -a form), hex digits.
bool IsPciAddress(const std::string &a) {
  if (a.size() != 12 || a[4] != ':' || a[7] != ':' || a[10] != '.') {
    return false;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if (i == 4 || i == 7 || i == 10) {
      continue;
    }
    if (!std::isxdigit(static_cast<unsigned char>(a[i]))) {
      return false;
    }
  }
  return true;
}

bool ReadFirstLine(const std::string &path, std::string *out) {
  std::ifstream in(path);
  return static_cast<bool>(std::getline(in, *out));
}

bool ParseUint64(const std::string &text, uint64_t *value) {
  uint64_t parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  *value = parsed;
  return true;
}

std::filesystem::path CurrentCgroupPath(const std::string &cgroup_mount_root,
                                        const std::string &proc_cgroup_path) {
  const std::filesystem::path mount_root =
      std::filesystem::path(cgroup_mount_root).lexically_normal();
  std::ifstream in(proc_cgroup_path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("0::", 0) != 0) {
      continue;
    }
    const std::filesystem::path hierarchy(line.substr(3));
    if (!hierarchy.is_absolute()) {
      return mount_root;
    }
    for (const auto &part : hierarchy) {
      if (part == "..") {
        return mount_root;
      }
    }
    return (mount_root / hierarchy.relative_path()).lexically_normal();
  }
  return mount_root;
}

}  // namespace

std::vector<std::string> ApplyEnvironmentFlags(char **environ_list) {
  std::vector<std::string> taken;
  std::vector<gflags::CommandLineFlagInfo> flags;
  gflags::GetAllFlags(&flags);
  for (const auto &flag : flags) {
    if (!flag.is_default) {
      continue;  // given on the command line: it wins
    }
    const std::string env = "BESSD_" + Upper(flag.name);
    const char *value = Lookup(environ_list, env);
    if (value == nullptr) {
      continue;
    }
    if (gflags::SetCommandLineOption(flag.name.c_str(), value).empty()) {
      LOG(FATAL) << env << "=" << value << " is not a valid value for -"
                 << flag.name;
    }
    taken.push_back(env);
  }
  return taken;
}

uint64_t UsableHugepageMB(const std::string &hugepages_root,
                          const std::string &cgroup_mount_root,
                          const std::string &proc_cgroup_path) {
  uint64_t total_mb = 0;
  DIR *dir = opendir(hugepages_root.c_str());
  if (dir == nullptr) {
    return 0;
  }
  const std::filesystem::path mount_root =
      std::filesystem::path(cgroup_mount_root).lexically_normal();
  const std::filesystem::path process_cgroup =
      CurrentCgroupPath(cgroup_mount_root, proc_cgroup_path);
  while (const dirent *entry = readdir(dir)) {
    // hugepages-<size>kB
    const std::string name(entry->d_name);
    if (name.rfind("hugepages-", 0) != 0 || name.size() < 13 ||
        name.substr(name.size() - 2) != "kB") {
      continue;
    }
    const uint64_t page_kb =
        std::strtoull(name.substr(10, name.size() - 12).c_str(), nullptr, 10);
    std::string free_pages;
    if (page_kb == 0 ||
        !ReadFirstLine(hugepages_root + "/" + name + "/free_hugepages",
                       &free_pages)) {
      continue;
    }
    uint64_t bytes = std::strtoull(free_pages.c_str(), nullptr, 10) * page_kb *
                     1024;
    // Each finite cgroup v2 ancestor limit leaves max - current bytes.
    const std::string size = page_kb >= 1024 * 1024
                                 ? std::to_string(page_kb / 1024 / 1024) + "GB"
                                 : std::to_string(page_kb / 1024) + "MB";
    for (auto cgroup = process_cgroup;; cgroup = cgroup.parent_path()) {
      std::string limit;
      if (ReadFirstLine((cgroup / ("hugetlb." + size + ".max")).string(),
                        &limit) &&
          limit != "max") {
        uint64_t max_bytes = 0;
        uint64_t current_bytes = 0;
        std::string current;
        if (!ParseUint64(limit, &max_bytes) ||
            !ReadFirstLine((cgroup / ("hugetlb." + size + ".current")).string(),
                           &current) ||
            !ParseUint64(current, &current_bytes)) {
          bytes = 0;
        } else {
          const uint64_t remaining =
              current_bytes < max_bytes ? max_bytes - current_bytes : 0;
          bytes = std::min(bytes, remaining);
        }
      }
      if (cgroup == mount_root) {
        break;
      }
      const auto parent = cgroup.parent_path();
      if (parent == cgroup) {
        break;
      }
    }
    total_mb += bytes >> 20;
  }
  closedir(dir);
  return total_mb;
}

std::vector<std::string> DevicePluginPciAddresses(char **environ_list) {
  std::vector<std::string> addresses;
  for (char **e = environ_list; e != nullptr && *e != nullptr; e++) {
    const std::string entry(*e);
    if (entry.rfind("PCIDEVICE_", 0) != 0) {
      continue;
    }
    const size_t eq = entry.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::stringstream list(entry.substr(eq + 1));
    std::string address;
    while (std::getline(list, address, ',')) {
      // Plugins also set PCIDEVICE_<RESOURCE>_INFO (JSON): not addresses.
      if (IsPciAddress(address)) {
        addresses.push_back(address);
      }
    }
  }
  std::sort(addresses.begin(), addresses.end());
  addresses.erase(std::unique(addresses.begin(), addresses.end()),
                  addresses.end());
  return addresses;
}

void BlockTerminationSignals() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGINT);
  PCHECK(pthread_sigmask(SIG_BLOCK, &set, nullptr) == 0);
}

TerminationWatcher::TerminationWatcher(std::function<void()> on_signal)
    : thread_([this, on_signal = std::move(on_signal)] {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGTERM);
        sigaddset(&set, SIGINT);
        const timespec tick = {0, 200 * 1000 * 1000};
        while (!stop_.load()) {
          const int sig = sigtimedwait(&set, nullptr, &tick);
          if (sig > 0) {
            LOG(WARNING) << "Received " << strsignal(sig)
                         << ": shutting down gracefully";
            on_signal();
            return;
          }
        }
      }) {}

TerminationWatcher::~TerminationWatcher() {
  stop_ = true;
  thread_.join();
}

}  // namespace bess::startup
