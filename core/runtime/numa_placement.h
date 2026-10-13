// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_NUMA_PLACEMENT_H_
#define BESS_RUNTIME_NUMA_PLACEMENT_H_

#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

// Process placement (Decision D-123): bessd picks ONE memory node at startup
// and everything it allocates lives there; control threads get a reserved
// CPU; workers given no core get a free CPU of that node, one per physical
// core before any shares one (an explicit core is always honoured). Anything
// on another node (a worker's CPU, a NIC) is diagnosed, never refused.
//
// One resolver for every environment -- bare metal, a container's cpuset, a
// Kubernetes pod, a VM with NUMA hidden: its only inputs are a sysfs tree,
// the process affinity, the allowed memory nodes and two flags. There is no
// environment detection; the scenarios differ only in what those inputs say.

namespace bess::runtime {

// What a sysfs tree says about CPUs and NUMA nodes.
struct Topology {
  // Every CPU sysfs lists (cpu/cpuN) -> its node: the cpu/cpuN/nodeM link,
  // else node/nodeM/cpulist, else 0 (what DPDK assumes for a CPU sysfs does
  // not place, e.g. a VM that hides NUMA).
  std::map<int, int> cpu_node;
  // CPU -> the hardware threads of its physical core, itself included
  // (cpu/cpuN/topology/thread_siblings_list; just itself when absent).
  std::map<int, std::vector<int>> siblings;
  // Nodes with memory: node/has_memory, else node/online, else the nodeM
  // directories, else {0}.
  std::set<int> memory_nodes;

  // 0 for a CPU sysfs does not list.
  int NodeOf(int cpu) const;
  // True when `a` and `b` are different hardware threads of one core.
  bool SharesCore(int a, int b) const;
};

// Reads `sysfs_root`/devices/system/{cpu,node}. Missing files are not
// errors: each falls back as described above.
Topology ReadTopology(const std::string &sysfs_root);

// "0-3,8" -> {0,1,2,3,8}; nullopt when malformed. Empty text -> empty set.
std::optional<std::set<int>> ParseCpuList(const std::string &text);
std::string FormatCpuList(const std::set<int> &cpus);

struct PlacementInput {
  Topology topology;
  std::set<int> cpus;          // the process affinity (ProcessCpus())
  std::set<int> mems_allowed;  // get_mempolicy(MPOL_F_MEMS_ALLOWED); empty: unknown, i.e. all
  int numa_node_flag = -1;     // --numa_node; -1: automatic
  std::string control_cpus_flag = "auto";  // --control_cpus: "auto" or a cpulist
};

enum class NodeReason {
  kOnlyCandidate,      // exactly one node has both our CPUs and usable memory
  kFlag,               // --numa_node
  kLowestOfSeveral,    // several candidates: the lowest (warned)
  kNoCandidate,        // CPUs and usable memory share no node (warned)
};
const char *NodeReasonName(NodeReason reason);

// What a worker on a CPU would be, for diagnostics; never a refusal except
// `outside_set`, which AddWorker already refuses (D-027).
struct WorkerCpuReport {
  int numa_node = 0;
  bool cross_numa = false;      // not on the memory node
  bool on_control_cpu = false;  // a reserved control CPU
  bool shares_core = false;     // SMT sibling of another worker or a control CPU
  bool outside_set = false;     // not in the process affinity
  std::vector<std::string> warnings;  // one line per finding, empty: none
};

enum class DeviceNuma { kUnknown, kLocal, kCross };
// `device_socket` as DPDK reports it (rte_eth_dev_socket_id); < 0: unknown,
// which is never warned about.
DeviceNuma ClassifyDevice(int device_socket, int memory_node);
const char *DeviceNumaName(DeviceNuma numa);  // "unknown", "local", "cross"

struct Placement {
  Topology topology;
  std::set<int> cpus;
  int node = 0;
  std::set<int> candidates;
  NodeReason reason = NodeReason::kOnlyCandidate;
  // The CPU reserved for control threads (auto: the first CPU of the memory
  // node in the set; none with a one-CPU set), or the --control_cpus list.
  std::set<int> control_cpus;
  std::vector<std::string> warnings;

  // CPUs automatic workers may use: the set's CPUs on the memory node (all
  // of the set when none is on it), minus the control CPUs.
  std::set<int> WorkerCpus() const;
  // The CPU for a worker given no core: SMT-striped over WorkerCpus() minus
  // `used` (every live worker's CPU, explicit or automatic) -- the first
  // thread of every physical core (cores in order of their lowest CPU),
  // then every core's second thread, and so on; the control CPU holds its
  // core's first thread. Without sibling information, lowest first. nullopt
  // when none is left (the caller answers ENOSPC). Never co-locates; an
  // explicit core is the caller's to honour, never this function's.
  std::optional<int> NextWorkerCpu(std::span<const int> used) const;
  // Diagnostics for a worker on `cpu`, given the other workers' CPUs.
  WorkerCpuReport DescribeWorkerCpu(int cpu,
                                    std::span<const int> other_workers) const;
  // "placement: node N (candidates {..}, reason ...), control CPUs X, worker
  // CPUs available Y" -- every scenario diagnosable from one line.
  std::string Summary() const;
};

// The pure resolver. Errors only for an unusable --control_cpus (not a
// cpulist, or naming CPUs outside the set).
std::expected<Placement, std::string> ResolvePlacement(
    const PlacementInput &input);

// -- the process's placement, resolved once ---------------------------------

// Resolved on first use from /sys, ProcessCpus(), the allowed memory nodes
// and --numa_node/--control_cpus; logs Summary() and each warning once. An
// unusable --control_cpus is fatal (a startup flag error).
const Placement &ProcessPlacement();
// ProcessPlacement().node: the node every allocation uses.
int MemoryNode();
// Resolves (if not yet) and prefers the memory node for this thread's -- and
// every thread it later creates' -- normal (libc) memory: set_mempolicy
// (MPOL_PREFERRED); a refusal is a warning. Call once, early in bessd's
// main, before any thread exists. Marks the process as placed: only then
// are the control CPUs reserved (ReservedControlCpus()).
void ApplyProcessPlacement();
// Whether ApplyProcessPlacement() ran: bessd, not a test or benchmark that
// brings DPDK up lazily (whose threads keep the inherited CPU set).
bool ProcessPlacementApplied();
// The memory nodes this process may allocate on: get_mempolicy
// (MPOL_F_MEMS_ALLOWED), else (refused, e.g. by seccomp) /proc/self/status's
// Mems_allowed_list; empty when neither says.
std::set<int> MemsAllowed();
// Mems_allowed_list from a /proc/<pid>/status file; empty when absent.
std::set<int> MemsAllowedFromStatus(const std::string &status_path);

}  // namespace bess::runtime

#endif  // BESS_RUNTIME_NUMA_PLACEMENT_H_
