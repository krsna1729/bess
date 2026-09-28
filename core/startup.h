// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_STARTUP_H_
#define BESS_STARTUP_H_

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

#endif  // BESS_STARTUP_H_
