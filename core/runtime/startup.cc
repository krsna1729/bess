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

// The hugetlb controller's name for a page size: hugetlb.<this>.max.
std::string HugetlbSizeName(uint64_t page_kb) {
  return page_kb >= 1024 * 1024 ? std::to_string(page_kb / 1024 / 1024) + "GB"
                                : std::to_string(page_kb / 1024) + "MB";
}

// The page sizes under `hugepages_root` (hugepages-<size>kB), in kB, with
// their free pages.
std::vector<std::pair<uint64_t, uint64_t>> HugepageSizes(
    const std::string &hugepages_root) {
  std::vector<std::pair<uint64_t, uint64_t>> sizes;
  DIR *dir = opendir(hugepages_root.c_str());
  if (dir == nullptr) {
    return sizes;
  }
  while (const dirent *entry = readdir(dir)) {
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
    sizes.emplace_back(page_kb,
                       std::strtoull(free_pages.c_str(), nullptr, 10));
  }
  closedir(dir);
  std::sort(sizes.begin(), sizes.end());
  return sizes;
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
  const std::filesystem::path mount_root =
      std::filesystem::path(cgroup_mount_root).lexically_normal();
  const std::filesystem::path process_cgroup =
      CurrentCgroupPath(cgroup_mount_root, proc_cgroup_path);
  for (const auto &[page_kb, free_pages] : HugepageSizes(hugepages_root)) {
    uint64_t bytes = free_pages * page_kb * 1024;
    // Each finite cgroup v2 ancestor limit leaves max - current bytes.
    const std::string size = HugetlbSizeName(page_kb);
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
  return total_mb;
}

bool HugetlbChargedToMemory(const std::string &mountinfo_path,
                            const std::string &cgroup_mount_root) {
  // 36 25 0:31 / /sys/fs/cgroup rw,nosuid shared:9 - cgroup2 cgroup2 rw,nsdelegate,memory_hugetlb_accounting
  // Fields after " - ": the filesystem type, the source, the super options.
  const std::string mount_root =
      std::filesystem::path(cgroup_mount_root).lexically_normal().string();
  std::ifstream in(mountinfo_path);
  std::string line;
  std::optional<bool> any;  // the first cgroup2 mount's answer
  while (std::getline(in, line)) {
    const size_t dash = line.find(" - ");
    if (dash == std::string::npos) {
      continue;
    }
    std::istringstream head(line.substr(0, dash));
    std::string id, parent, device, root, mount_point;
    std::istringstream tail(line.substr(dash + 3));
    std::string fstype, source, options;
    if (!(head >> id >> parent >> device >> root >> mount_point) ||
        !(tail >> fstype >> source >> options) || fstype != "cgroup2") {
      continue;
    }
    bool charged = false;
    std::istringstream list(options);
    std::string option;
    while (std::getline(list, option, ',')) {
      charged = charged || option == "memory_hugetlb_accounting";
    }
    if (std::filesystem::path(mount_point).lexically_normal().string() ==
        mount_root) {
      return charged;
    }
    if (!any) {
      any = charged;
    }
  }
  return any.value_or(false);
}

MemoryLimit DiscoverHugetlbLimit(const std::string &hugepages_root,
                                 const std::string &cgroup_mount_root,
                                 const std::string &proc_cgroup_path) {
  const std::filesystem::path mount_root =
      std::filesystem::path(cgroup_mount_root).lexically_normal();
  const std::filesystem::path process_cgroup =
      CurrentCgroupPath(cgroup_mount_root, proc_cgroup_path);
  MemoryLimit cap;
  for (const auto &[page_kb, free_pages] : HugepageSizes(hugepages_root)) {
    if (free_pages == 0) {
      continue;  // a size bessd cannot use does not bound what it can
    }
    const std::string size = HugetlbSizeName(page_kb);
    uint64_t smallest = 0;
    std::filesystem::path where;
    for (auto cgroup = process_cgroup;; cgroup = cgroup.parent_path()) {
      std::string text;
      uint64_t max_bytes = 0;
      if (ReadFirstLine((cgroup / ("hugetlb." + size + ".max")).string(),
                        &text) &&
          text != "max" && ParseUint64(text, &max_bytes) &&
          (where.empty() || max_bytes < smallest)) {
        smallest = max_bytes;
        where = cgroup;
      }
      if (cgroup == mount_root || cgroup.parent_path() == cgroup) {
        break;
      }
    }
    if (where.empty()) {
      return MemoryLimit{};  // this size is unlimited: no cap
    }
    cap.bytes += smallest;
    cap.source += (cap.source.empty() ? "cgroup " : " + ") +
                  ("hugetlb." + size + ".max (/" +
                   where.lexically_relative(mount_root).generic_string() + ")");
  }
  return cap;
}

MemoryLimit DiscoverMemoryLimit(const std::string &cgroup_mount_root,
                                const std::string &proc_cgroup_path,
                                const std::string &meminfo_path) {
  const std::filesystem::path mount_root =
      std::filesystem::path(cgroup_mount_root).lexically_normal();
  MemoryLimit cgroup_limit;
  for (auto cgroup = CurrentCgroupPath(cgroup_mount_root, proc_cgroup_path);;
       cgroup = cgroup.parent_path()) {
    std::string text;
    uint64_t max_bytes = 0;
    if (ReadFirstLine((cgroup / "memory.max").string(), &text) &&
        text != "max" && ParseUint64(text, &max_bytes) &&
        (cgroup_limit.bytes == 0 || max_bytes < cgroup_limit.bytes)) {
      cgroup_limit.bytes = max_bytes;
      cgroup_limit.cgroup_dir = cgroup.string();
      cgroup_limit.source =
          "cgroup memory.max (/" +
          cgroup.lexically_relative(mount_root).generic_string() + ")";
    }
    if (cgroup == mount_root || cgroup.parent_path() == cgroup) {
      break;
    }
  }

  // MemTotal:       16303428 kB
  uint64_t total_bytes = 0;
  std::ifstream meminfo(meminfo_path);
  std::string key;
  uint64_t kb = 0;
  std::string unit;
  while (meminfo >> key >> kb >> unit) {
    if (key == "MemTotal:") {
      total_bytes = kb * 1024;
      break;
    }
  }

  // A cgroup limit above the host's memory never binds: the host does.
  if (cgroup_limit.bytes != 0 &&
      (total_bytes == 0 || cgroup_limit.bytes <= total_bytes)) {
    return cgroup_limit;
  }
  MemoryLimit host;
  if (total_bytes != 0) {
    host.bytes = total_bytes;
    host.source = "MemTotal";
  }
  return host;
}

MemorySettings LibraryMemorySettings() {
  MemorySettings settings;
  settings.backing = utils::MemoryBacking::kNormal;
  settings.limit_mb = -1;
  return settings;
}

std::optional<utils::MemoryBacking> ParseMemoryBacking(std::string_view text) {
  if (text == "auto") {
    return utils::MemoryBacking::kAuto;
  }
  if (text == "hugepages") {
    return utils::MemoryBacking::kHugepages;
  }
  if (text == "normal") {
    return utils::MemoryBacking::kNormal;
  }
  return std::nullopt;
}

const char *MemoryBackingName(utils::MemoryBacking backing) {
  switch (backing) {
    case utils::MemoryBacking::kAuto:
      return "auto";
    case utils::MemoryBacking::kHugepages:
      return "hugepages";
    case utils::MemoryBacking::kNormal:
      return "normal";
  }
  return "?";
}

bool ValidMemoryMegabytes(int64_t mb) {
  return mb >= -1;
}

bool ValidHugepageMegabytes(int64_t mb) {
  return mb >= 0;
}

namespace {

// gflags' integer syntax for the int64 flags: decimal, the whole string.
bool ParseInt64(std::string_view text, int64_t *value) {
  int64_t parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (text.empty() || result.ec != std::errc{} ||
      result.ptr != text.data() + text.size()) {
    return false;
  }
  *value = parsed;
  return true;
}

// gflags' boolean syntax: true/false, t/f, yes/no, y/n, 1/0, any case.
bool ParseBool(std::string_view text, bool *value) {
  const std::string lower = [&] {
    std::string s(text);
    for (char &c : s) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
  }();
  if (lower == "true" || lower == "t" || lower == "yes" || lower == "y" ||
      lower == "1") {
    *value = true;
    return true;
  }
  if (lower == "false" || lower == "f" || lower == "no" || lower == "n" ||
      lower == "0") {
    *value = false;
    return true;
  }
  return false;
}

std::string Megabytes(uint64_t bytes) {
  return std::to_string(bytes >> 20) + " MB";
}

}  // namespace

bool MemorySettingsFromEnvironment(char **environ_list,
                                   MemorySettings *settings,
                                   std::string *error) {
  MemorySettings s = *settings;
  // The variable is the flag's name in D-030's form: BESSD_ and uppercase.
  const auto refuse = [&](const char *flag, const char *value) {
    *error = "BESSD_" + Upper(flag) + "=" + value +
             " is not a valid value for --" + flag;
    return false;
  };
  const auto env = [&](const char *flag) {
    return Lookup(environ_list, "BESSD_" + Upper(flag));
  };
  if (const char *v = env("mem_backing")) {
    const auto backing = ParseMemoryBacking(v);
    if (!backing) {
      return refuse("mem_backing", v);
    }
    s.backing = *backing;
  }
  if (const char *v = env("mem_hugepage_mb")) {
    if (!ParseInt64(v, &s.hugepage_mb) || !ValidHugepageMegabytes(s.hugepage_mb)) {
      return refuse("mem_hugepage_mb", v);
    }
  }
  if (const char *v = env("mem_fallback")) {
    if (!ParseBool(v, &s.fallback)) {
      return refuse("mem_fallback", v);
    }
  }
  if (const char *v = env("mem_limit_mb")) {
    if (!ParseInt64(v, &s.limit_mb) || !ValidMemoryMegabytes(s.limit_mb)) {
      return refuse("mem_limit_mb", v);
    }
  }
  if (const char *v = env("mem_reserve_mb")) {
    if (!ParseInt64(v, &s.reserve_mb) || !ValidMemoryMegabytes(s.reserve_mb)) {
      return refuse("mem_reserve_mb", v);
    }
  }
  *settings = s;
  return true;
}

bool ResolveMemoryPolicy(const MemorySettings &settings,
                         const MemoryLimit &discovered,
                         uint64_t usable_hugepage_mb,
                         utils::MemoryPolicy *policy, std::string *error,
                         const MemoryLimit &hugetlb_limit,
                         bool hugetlb_charged) {
  constexpr uint64_t kMB = uint64_t{1} << 20;
  utils::MemoryPolicy p;
  switch (settings.backing) {
    case utils::MemoryBacking::kAuto:
      p.backing = usable_hugepage_mb > 0 ? utils::MemoryBacking::kHugepages
                                         : utils::MemoryBacking::kNormal;
      break;
    case utils::MemoryBacking::kHugepages:
      if (usable_hugepage_mb == 0) {
        *error =
            "--mem_backing=hugepages, but no hugepages are usable (none free "
            "on the host, or this cgroup's hugetlb limit is reached): give "
            "bessd hugepages, or start it with --mem_backing=auto or normal";
        return false;
      }
      p.backing = utils::MemoryBacking::kHugepages;
      break;
    case utils::MemoryBacking::kNormal:
      if (settings.hugepage_mb > 0) {
        *error = "--mem_hugepage_mb=" + std::to_string(settings.hugepage_mb) +
                 " caps the hugepage heap: it cannot be given with "
                 "--mem_backing=normal";
        return false;
      }
      p.backing = utils::MemoryBacking::kNormal;
      break;
  }
  // A cap on hugepages bessd does not use would say something untrue in the
  // log and the status: kept only with hugepage backing. The hugetlb
  // controller's limit caps the heap whether or not the flag does: DPDK must
  // refuse at it rather than fault a page past it (D-198).
  if (p.backing == utils::MemoryBacking::kHugepages) {
    if (settings.hugepage_mb > 0) {
      p.hugepage_cap_bytes = static_cast<uint64_t>(settings.hugepage_mb) * kMB;
      p.hugepage_cap_source = "--mem_hugepage_mb";
    }
    if (hugetlb_limit.bytes != 0 &&
        (p.hugepage_cap_bytes == 0 || hugetlb_limit.bytes < p.hugepage_cap_bytes)) {
      p.hugepage_cap_bytes = hugetlb_limit.bytes;
      p.hugepage_cap_source = hugetlb_limit.source;
    }
  }
  p.hugetlb_charged = hugetlb_charged;
  p.fallback = settings.fallback;
  if (settings.limit_mb > 0) {
    // An operator's limit is bessd's own: measured as its RSS.
    p.limit_bytes = static_cast<uint64_t>(settings.limit_mb) * kMB;
    p.limit_source = "--mem_limit_mb";
  } else if (settings.limit_mb == 0) {
    p.limit_bytes = discovered.bytes;
    p.limit_source = discovered.bytes != 0
                         ? discovered.source
                         : "none found (no cgroup memory.max, no MemTotal)";
    p.cgroup_dir = discovered.cgroup_dir;
  } else {
    p.limit_source = "none (--mem_limit_mb=-1)";
  }
  if (settings.reserve_mb >= 0) {
    p.reserve_bytes = static_cast<uint64_t>(settings.reserve_mb) * kMB;
  } else {
    // Room for what is not admitted (glibc, gRPC, protobuf, thread stacks):
    // 1/16 of the limit, at least 128 MB, at most half of a small limit.
    p.reserve_bytes = std::min(std::max<uint64_t>(128 * kMB, p.limit_bytes / 16),
                               p.limit_bytes / 2);
  }
  if (p.limit_bytes != 0 && p.reserve_bytes >= p.limit_bytes) {
    *error = "--mem_reserve_mb=" + std::to_string(settings.reserve_mb) +
             " leaves nothing of the " + std::to_string(p.limit_bytes / kMB) +
             " MB memory limit (" + p.limit_source + ")";
    return false;
  }
  *policy = std::move(p);
  return true;
}

bool ApplyMemorySettings(const MemorySettings &settings, std::string *error) {
  // Only --mem_limit_mb=0 reads the limit, and only a backing that may be
  // hugepages counts them.
  const MemoryLimit discovered =
      settings.limit_mb == 0
          ? DiscoverMemoryLimit("/sys/fs/cgroup", "/proc/self/cgroup",
                                "/proc/meminfo")
          : MemoryLimit{};
  const bool may_use_hugepages =
      settings.backing != utils::MemoryBacking::kNormal;
  const uint64_t usable =
      may_use_hugepages ? UsableHugepageMB("/sys/kernel/mm/hugepages",
                                           "/sys/fs/cgroup", "/proc/self/cgroup")
                        : 0;
  const MemoryLimit hugetlb_limit =
      may_use_hugepages
          ? DiscoverHugetlbLimit("/sys/kernel/mm/hugepages", "/sys/fs/cgroup",
                                 "/proc/self/cgroup")
          : MemoryLimit{};
  const bool hugetlb_charged =
      HugetlbChargedToMemory("/proc/self/mountinfo", "/sys/fs/cgroup");
  utils::MemoryPolicy policy;
  if (!ResolveMemoryPolicy(settings, discovered, usable, &policy, error,
                           hugetlb_limit, hugetlb_charged)) {
    return false;
  }
  const bool hugepages = policy.backing == utils::MemoryBacking::kHugepages;
  if (settings.backing == utils::MemoryBacking::kAuto && !hugepages) {
    LOG(WARNING) << "No usable hugepages: DPDK memory is normal pages "
                    "(--mem_backing=auto). Give bessd hugepages for "
                    "production.";
  }
  // Charged hugepages count against memory.max like any page (D-198): a
  // limit that cannot hold the cap and the reserve refuses tables the cap
  // allows -- a pod's memory limit must include its hugepages on such a node.
  if (hugepages && policy.hugetlb_charged && policy.limit_bytes != 0 &&
      policy.hugepage_cap_bytes != 0 &&
      policy.limit_bytes < policy.hugepage_cap_bytes + policy.reserve_bytes) {
    LOG(WARNING) << "Hugepages are charged to the memory limit here (cgroup2 "
                    "memory_hugetlb_accounting): the "
                 << Megabytes(policy.limit_bytes) << " limit ("
                 << policy.limit_source << ") is below the "
                 << Megabytes(policy.hugepage_cap_bytes) << " hugepage cap ("
                 << policy.hugepage_cap_source << ") plus the "
                 << Megabytes(policy.reserve_bytes)
                 << " reserve, so tables are refused before the cap is "
                    "reached; raise the memory limit to include the "
                    "hugepages";
  }
  // The one line naming every resolved setting (D-198).
  LOG(INFO) << "Memory: backing " << MemoryBackingName(policy.backing)
            << " (--mem_backing=" << MemoryBackingName(settings.backing)
            << (hugepages ? "; " + std::to_string(usable) +
                                " MB of hugepages usable, mapped as needed"
                          : std::string())
            << "), hugepage cap "
            << (policy.hugepage_cap_bytes == 0
                    ? std::string("none")
                    : Megabytes(policy.hugepage_cap_bytes) + " (" +
                          policy.hugepage_cap_source + ")")
            << ", hugetlb "
            << (policy.hugetlb_charged ? "charged to memory.max"
                                       : "not charged to memory.max")
            << ", fallback " << (policy.fallback ? "on" : "off") << ", limit "
            << (policy.limit_bytes == 0 ? std::string("none")
                                        : Megabytes(policy.limit_bytes))
            << " (" << policy.limit_source << "), reserve "
            << Megabytes(policy.reserve_bytes) << " (D-198)";
  utils::SetMemoryPolicy(std::move(policy));
  return true;
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
