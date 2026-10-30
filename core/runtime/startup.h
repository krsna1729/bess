// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_STARTUP_H_
#define BESS_RUNTIME_STARTUP_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "utils/dpdk_memory.h"

// Container- and orchestrator-friendly startup (Decision D-030): bessd needs
// as little configuration as possible and takes what it needs from its
// environment.
namespace bess::startup {

// Every command-line flag can also come from the environment as
// BESSD_<FLAG> (uppercase, e.g. BESSD_GRPC_URL, BESSD_BUFFERS); a flag given
// on the command line wins. Values go through the flag's own validator.
// Returns the flags taken from the environment (for the log); an invalid
// value is fatal, like an invalid command-line value.
std::vector<std::string> ApplyEnvironmentFlags(char **environ_list);

// Hugepages this process could use, in MB total: free host pages bounded by the
// remaining cgroup v2 hugetlb capacity (`max - current`) at this cgroup and its
// ancestors. `cgroup_mount_root` is normally /sys/fs/cgroup; `proc_cgroup_path`
// is normally /proc/self/cgroup. A missing limit is unlimited.
uint64_t UsableHugepageMB(const std::string &hugepages_root,
                          const std::string &cgroup_mount_root,
                          const std::string &proc_cgroup_path);

// The memory limit bessd runs under (D-198): the smallest finite cgroup v2
// memory.max of this process's cgroup and its ancestors ("max" is
// unlimited), with that cgroup's directory, whose memory.current is what the
// kernel holds against it; else -- or if that is above the host's memory --
// MemTotal from `meminfo_path` (normally /proc/meminfo), with no cgroup
// directory. bytes is 0 if neither can be read.
struct MemoryLimit {
  uint64_t bytes = 0;
  std::string source;      // "cgroup memory.max (/pod/container)", "MemTotal"
  std::string cgroup_dir;  // absolute, under cgroup_mount_root; or empty
};
MemoryLimit DiscoverMemoryLimit(const std::string &cgroup_mount_root,
                                const std::string &proc_cgroup_path,
                                const std::string &meminfo_path);

// Whether hugetlb pages are charged to the memory controller (D-198): the
// cgroup v2 mount at `cgroup_mount_root` (normally /sys/fs/cgroup; else any
// cgroup2 mount) lists `memory_hugetlb_accounting` among its super options
// in `mountinfo_path` (normally /proc/self/mountinfo; Linux >= 6.6). Then a
// hugepage counts against memory.max when it is faulted in, and a charge
// past memory.max is SIGBUS even with free pages in the pool -- so bessd
// admits hugepage growth against its memory limit too. Kubernetes assumes
// the option is off (KEP-1539: hugepages are limited by hugetlb.<size>.max,
// memory by memory.max).
bool HugetlbChargedToMemory(const std::string &mountinfo_path,
                            const std::string &cgroup_mount_root);

// The hugepage cap the hugetlb controller sets on this process (D-198): for
// each page size with free pages under `hugepages_root`, the smallest finite
// hugetlb.<size>.max of this cgroup and its ancestors; the cap is their sum
// when every such size is limited, else none (bytes 0: an unlimited size
// can grow past any sum). source names the files, e.g. "cgroup
// hugetlb.1GB.max (/pod)"; cgroup_dir is unused.
MemoryLimit DiscoverHugetlbLimit(const std::string &hugepages_root,
                                 const std::string &cgroup_mount_root,
                                 const std::string &proc_cgroup_path);

// Dataplane memory (D-198): the five --mem_* flags as given, before they are
// resolved against the host. The defaults are bessd's; library users and
// tests (InitDpdk() without bessd) start from LibraryMemorySettings().
struct MemorySettings {
  utils::MemoryBacking backing = utils::MemoryBacking::kAuto;  // --mem_backing
  int64_t hugepage_mb = 0;  // --mem_hugepage_mb: 0 no cap, N MB
  bool fallback = true;     // --mem_fallback (D-206: on unless forbidden)
  int64_t limit_mb = 0;     // --mem_limit_mb: 0 discovered, -1 none, N MB
  int64_t reserve_mb = -1;  // --mem_reserve_mb: -1 automatic, N MB
};

// What a process without bessd's flags gets when no BESSD_MEM_* variable is
// set: normal pages, no limit -- a 512 MB --no-huge heap, as before D-198.
MemorySettings LibraryMemorySettings();

// The parsers and range checks the flag validators and the environment
// share, so --mem_backing=x and BESSD_MEM_BACKING=x accept the same values.
// "auto", "hugepages" or "normal".
std::optional<utils::MemoryBacking> ParseMemoryBacking(std::string_view text);
const char *MemoryBackingName(utils::MemoryBacking backing);
// --mem_limit_mb and --mem_reserve_mb: -1 or more.
bool ValidMemoryMegabytes(int64_t mb);
// --mem_hugepage_mb: 0 or more.
bool ValidHugepageMegabytes(int64_t mb);

// BESSD_MEM_BACKING, BESSD_MEM_HUGEPAGE_MB, BESSD_MEM_FALLBACK,
// BESSD_MEM_LIMIT_MB and BESSD_MEM_RESERVE_MB (D-030's names for the flags)
// from `environ_list`, over what *settings holds; the library path's
// equivalent of bessd's ApplyEnvironmentFlags(). False, with *error naming
// the variable, for a value the flag would refuse.
bool MemorySettingsFromEnvironment(char **environ_list,
                                   MemorySettings *settings,
                                   std::string *error);

// The memory policy, resolved once: the backing (auto: hugepages when
// `usable_hugepage_mb` > 0, else normal pages), the hugepage cap (hugepage
// backing only: --mem_hugepage_mb, bounded by `hugetlb_limit`, the cgroup's
// hugetlb.<size>.max, which is the cap when the flag is 0), the fallback,
// the limit (--mem_limit_mb 0: `discovered`; -1: none; N: N MB of this
// process's RSS), the reserve (-1: max(128 MB, limit/16), at most half the
// limit) and whether hugepages are charged to memory.max
// (`hugetlb_charged`). The policy's backing is never kAuto. False, with
// *error naming the flag, for --mem_backing=hugepages without usable
// hugepages, a cap with --mem_backing=normal, or a reserve that leaves
// nothing of the limit.
bool ResolveMemoryPolicy(const MemorySettings &settings,
                         const MemoryLimit &discovered,
                         uint64_t usable_hugepage_mb,
                         utils::MemoryPolicy *policy, std::string *error,
                         const MemoryLimit &hugetlb_limit = {},
                         bool hugetlb_charged = false);

// ResolveMemoryPolicy() against this host (the memory limit from
// /sys/fs/cgroup and /proc/meminfo, usable hugepages and the hugetlb
// controller's cap from /sys/kernel/mm/hugepages and the cgroup unless the
// backing is normal, hugetlb charging from /proc/self/mountinfo), then
// utils::SetMemoryPolicy(): what bessd's main() and a lazy InitDpdk() both
// do before the EAL starts. Warns when auto resolves to normal pages, and
// when hugepages are charged to a memory limit too small to hold them.
bool ApplyMemorySettings(const MemorySettings &settings, std::string *error);

// PCI addresses a device plugin assigned to this container: the Kubernetes
// SR-IOV network device plugin (and others following its convention) set
// PCIDEVICE_<RESOURCE>=<addr>[,<addr>...]. Sorted, deduplicated.
std::vector<std::string> DevicePluginPciAddresses(char **environ_list);

// Termination (SIGTERM from `docker stop` or a Kubernetes pod deletion,
// SIGINT from a terminal) becomes a graceful shutdown: the server stops, the
// dataplane is reset in order, bessd exits 0. BlockTerminationSignals() runs
// first in main(), before any thread exists, so every thread inherits the
// mask and only the watcher takes the signal.
void BlockTerminationSignals();

class TerminationWatcher {
 public:
  // Calls `on_signal` (once) when SIGTERM or SIGINT arrives.
  explicit TerminationWatcher(std::function<void()> on_signal);
  ~TerminationWatcher();  // stops watching
  TerminationWatcher(const TerminationWatcher &) = delete;
  TerminationWatcher &operator=(const TerminationWatcher &) = delete;

 private:
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace bess::startup

#endif  // BESS_RUNTIME_STARTUP_H_
