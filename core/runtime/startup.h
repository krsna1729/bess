// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_STARTUP_H_
#define BESS_RUNTIME_STARTUP_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

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
