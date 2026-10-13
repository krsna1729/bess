// SPDX-License-Identifier: BSD-3-Clause

#include "runtime/numa_placement.h"

#include <linux/mempolicy.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "runtime/opts.h"
#include "runtime/thread_placement.h"
#include "utils/logging.h"

namespace bess::runtime {

namespace {

namespace fs = std::filesystem;

// The first line of `path`, whitespace-trimmed; empty when unreadable.
std::string ReadLine(const fs::path &path) {
  std::ifstream in(path);
  std::string line;
  if (!in || !std::getline(in, line)) {
    return "";
  }
  const auto blank = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!line.empty() && blank(static_cast<unsigned char>(line.back()))) {
    line.pop_back();
  }
  const auto first = std::find_if_not(line.begin(), line.end(), [&](char c) {
    return blank(static_cast<unsigned char>(c));
  });
  return std::string(first, line.end());
}

// N for a directory entry named `prefix`N (digits only), else nullopt.
std::optional<int> NumberedEntry(const std::string &name,
                                 std::string_view prefix) {
  if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
    return std::nullopt;
  }
  int value = 0;
  const char *begin = name.data() + prefix.size();
  const char *end = name.data() + name.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc() || ptr != end || value < 0) {
    return std::nullopt;
  }
  return value;
}

// The N of every `prefix`N entry of `dir`; empty when `dir` is absent.
std::set<int> NumberedEntries(const fs::path &dir, std::string_view prefix) {
  std::set<int> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (const auto n = NumberedEntry(it->path().filename().string(), prefix)) {
      out.insert(*n);
    }
  }
  return out;
}

std::string NodeSet(const std::set<int> &nodes) {
  return "{" + FormatCpuList(nodes) + "}";
}

std::string CpusOrNone(const std::set<int> &cpus) {
  return cpus.empty() ? std::string("none") : FormatCpuList(cpus);
}

// A cpulist value bound: well past any real CPU or node id, small enough
// that a malformed "0-2000000000" cannot make a huge set.
constexpr int kMaxListValue = 1 << 16;

}  // namespace

int Topology::NodeOf(int cpu) const {
  const auto it = cpu_node.find(cpu);
  return it != cpu_node.end() ? it->second : 0;
}

bool Topology::SharesCore(int a, int b) const {
  if (a == b) {
    return false;
  }
  const auto it = siblings.find(a);
  return it != siblings.end() &&
         std::find(it->second.begin(), it->second.end(), b) != it->second.end();
}

std::optional<std::set<int>> ParseCpuList(const std::string &text) {
  std::set<int> out;
  size_t pos = 0;
  const auto number = [&](int *value) {
    const char *begin = text.data() + pos;
    const char *end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, *value);
    if (ec != std::errc() || *value < 0 || *value > kMaxListValue) {
      return false;
    }
    pos = static_cast<size_t>(ptr - text.data());
    return true;
  };
  while (pos < text.size()) {
    int first = 0;
    if (!number(&first)) {
      return std::nullopt;
    }
    int last = first;
    if (pos < text.size() && text[pos] == '-') {
      pos++;
      if (!number(&last) || last < first) {
        return std::nullopt;
      }
    }
    for (int i = first; i <= last; i++) {
      out.insert(i);
    }
    if (pos < text.size()) {
      if (text[pos] != ',' || pos + 1 == text.size()) {
        return std::nullopt;
      }
      pos++;
    }
  }
  return out;
}

std::string FormatCpuList(const std::set<int> &cpus) {
  std::string out;
  for (auto it = cpus.begin(); it != cpus.end();) {
    const int start = *it;
    int end = start;
    for (++it; it != cpus.end() && *it == end + 1; ++it) {
      end = *it;
    }
    out += (out.empty() ? "" : ",") + std::to_string(start);
    if (end > start) {
      out += "-" + std::to_string(end);
    }
  }
  return out;
}

Topology ReadTopology(const std::string &sysfs_root) {
  const fs::path cpu_dir = fs::path(sysfs_root) / "devices/system/cpu";
  const fs::path node_dir = fs::path(sysfs_root) / "devices/system/node";
  Topology topo;

  // Node -> CPUs from node/nodeM/cpulist, for CPUs without a nodeM link.
  const std::set<int> node_ids = NumberedEntries(node_dir, "node");
  std::map<int, int> listed_node;
  for (int node : node_ids) {
    const auto cpus = ParseCpuList(
        ReadLine(node_dir / ("node" + std::to_string(node)) / "cpulist"));
    for (int cpu : cpus.value_or(std::set<int>{})) {
      listed_node.emplace(cpu, node);
    }
  }

  std::set<int> cpus = NumberedEntries(cpu_dir, "cpu");
  for (const auto &[cpu, node] : listed_node) {
    cpus.insert(cpu);
  }
  for (int cpu : cpus) {
    const fs::path dir = cpu_dir / ("cpu" + std::to_string(cpu));
    const std::set<int> link = NumberedEntries(dir, "node");
    if (!link.empty()) {
      topo.cpu_node[cpu] = *link.begin();
    } else if (const auto it = listed_node.find(cpu); it != listed_node.end()) {
      topo.cpu_node[cpu] = it->second;
    } else {
      topo.cpu_node[cpu] = 0;  // what DPDK assumes too
    }
    const auto threads =
        ParseCpuList(ReadLine(dir / "topology/thread_siblings_list"));
    std::vector<int> siblings;
    if (threads && threads->count(cpu) != 0) {
      siblings.assign(threads->begin(), threads->end());
    } else {
      siblings.push_back(cpu);
    }
    topo.siblings[cpu] = std::move(siblings);
  }

  for (const char *file : {"has_memory", "online"}) {
    const auto nodes = ParseCpuList(ReadLine(node_dir / file));
    if (nodes && !nodes->empty()) {
      topo.memory_nodes = *nodes;
      break;
    }
  }
  if (topo.memory_nodes.empty()) {
    topo.memory_nodes = node_ids.empty() ? std::set<int>{0} : node_ids;
  }
  return topo;
}

const char *NodeReasonName(NodeReason reason) {
  switch (reason) {
    case NodeReason::kOnlyCandidate:
      return "the only node with both bessd's CPUs and its memory";
    case NodeReason::kFlag:
      return "--numa_node";
    case NodeReason::kLowestOfSeveral:
      return "lowest of several candidates";
    case NodeReason::kNoCandidate:
      return "no node has both bessd's CPUs and its memory";
  }
  return "?";
}

DeviceNuma ClassifyDevice(int device_socket, int memory_node) {
  if (device_socket < 0) {
    return DeviceNuma::kUnknown;
  }
  return device_socket == memory_node ? DeviceNuma::kLocal : DeviceNuma::kCross;
}

const char *DeviceNumaName(DeviceNuma numa) {
  switch (numa) {
    case DeviceNuma::kUnknown:
      return "unknown";
    case DeviceNuma::kLocal:
      return "local";
    case DeviceNuma::kCross:
      return "cross";
  }
  return "?";
}

std::expected<Placement, std::string> ResolvePlacement(
    const PlacementInput &input) {
  Placement p;
  p.topology = input.topology;
  p.cpus = input.cpus;

  std::set<int> cpu_nodes;
  for (int cpu : input.cpus) {
    cpu_nodes.insert(input.topology.NodeOf(cpu));
  }
  // The nodes whose memory bessd may use: allowed by the kernel (a cpuset's
  // mems) and with memory at all (a CPU-only node has none).
  std::set<int> memory;
  if (input.mems_allowed.empty()) {
    memory = input.topology.memory_nodes;
  } else {
    std::ranges::set_intersection(input.topology.memory_nodes,
                                  input.mems_allowed,
                                  std::inserter(memory, memory.end()));
    if (memory.empty()) {
      memory = input.mems_allowed;  // the kernel's word over sysfs's
    }
  }
  std::ranges::set_intersection(cpu_nodes, memory,
                                std::inserter(p.candidates, p.candidates.end()));

  const std::string context = "bessd's CPUs are on nodes " + NodeSet(cpu_nodes) +
                              ", its memory may come from nodes " +
                              NodeSet(memory);
  if (input.numa_node_flag >= 0) {
    p.node = input.numa_node_flag;
    p.reason = NodeReason::kFlag;
    if (p.candidates.count(p.node) == 0) {
      p.warnings.push_back(
          "--numa_node=" + std::to_string(p.node) +
          " is not a candidate node " + NodeSet(p.candidates) + " (" + context +
          "): workers or memory there are remote, cross-NUMA");
    }
  } else if (p.candidates.size() == 1) {
    p.node = *p.candidates.begin();
    p.reason = NodeReason::kOnlyCandidate;
  } else if (p.candidates.size() > 1) {
    p.node = *p.candidates.begin();
    p.reason = NodeReason::kLowestOfSeveral;
    p.warnings.push_back(
        "bessd may use nodes " + NodeSet(p.candidates) + "; using node " +
        std::to_string(p.node) +
        ". One bessd drives one node: choose it with --numa_node=<node> or "
        "give bessd the CPUs (and memory) of one node");
  } else {
    // Memory has to come from a node bessd may allocate on, or nothing can
    // be allocated at all; remote CPUs are only slower.
    p.node = !memory.empty() ? *memory.begin()
             : !cpu_nodes.empty() ? *cpu_nodes.begin()
                                  : 0;
    p.reason = NodeReason::kNoCandidate;
    p.warnings.push_back("no node has both bessd's CPUs and memory it may use (" +
                         context + "); using node " + std::to_string(p.node) +
                         ": workers there access memory cross-NUMA");
  }

  const std::string &spec = input.control_cpus_flag;
  if (spec.empty() || spec == "auto") {
    if (input.cpus.size() < 2) {
      p.warnings.push_back(
          "bessd has one CPU (" + CpusOrNone(input.cpus) +
          "): control threads share it with the packet worker");
    } else {
      auto first = std::ranges::find_if(input.cpus, [&](int cpu) {
        return input.topology.NodeOf(cpu) == p.node;
      });
      p.control_cpus.insert(first != input.cpus.end() ? *first
                                                      : *input.cpus.begin());
    }
  } else if (spec != "none") {
    const auto listed = ParseCpuList(spec);
    if (!listed || listed->empty()) {
      return std::unexpected("--control_cpus='" + spec +
                             "' is neither 'auto', 'none' nor a CPU list");
    }
    // The one hard placement error, as for workers (D-027): the CPU set is
    // the operator's grant, whatever enforces it.
    for (int cpu : *listed) {
      if (input.cpus.count(cpu) == 0) {
        return std::unexpected("--control_cpus=" + spec + ": core " +
                               std::to_string(cpu) +
                               " is not in bessd's CPU set (" +
                               CpusOrNone(input.cpus) + ")");
      }
    }
    p.control_cpus = *listed;
  }
  if (!p.cpus.empty() && p.WorkerCpus().empty()) {
    p.warnings.push_back("no CPU of node " + std::to_string(p.node) +
                         " is left for automatic workers (control CPUs " +
                         CpusOrNone(p.control_cpus) +
                         "); workers need an explicit core");
  }
  return p;
}

namespace {

// The set's CPUs on the memory node; all of the set when none is on it
// (they are all remote then, warned at resolution).
std::set<int> NodeCpus(const Placement &p) {
  std::set<int> out;
  for (int cpu : p.cpus) {
    if (p.topology.NodeOf(cpu) == p.node) {
      out.insert(cpu);
    }
  }
  return out.empty() ? p.cpus : out;
}

}  // namespace

std::set<int> Placement::WorkerCpus() const {
  std::set<int> pool = NodeCpus(*this);
  for (int cpu : control_cpus) {
    pool.erase(cpu);
  }
  return pool;
}

std::optional<int> Placement::NextWorkerCpu(std::span<const int> used) const {
  // SMT-striped: the first thread of every physical core, cores in order of
  // their lowest CPU, then every core's second thread, and so on. A thread's
  // rank counts every thread of its core in the node's part of the set --
  // the control CPU included -- so the control CPU's sibling comes after
  // every free first thread.
  const std::set<int> node_cpus = NodeCpus(*this);
  std::optional<std::array<int, 3>> best;  // {rank, core's lowest CPU, cpu}
  for (int cpu : WorkerCpus()) {
    if (std::find(used.begin(), used.end(), cpu) != used.end()) {
      continue;
    }
    std::vector<int> threads{cpu};
    if (const auto it = topology.siblings.find(cpu);
        it != topology.siblings.end()) {
      for (int t : it->second) {
        if (t != cpu && node_cpus.count(t) != 0) {
          threads.push_back(t);
        }
      }
    }
    std::ranges::sort(threads);
    const auto rank =
        static_cast<int>(std::ranges::find(threads, cpu) - threads.begin());
    const std::array<int, 3> key{rank, threads.front(), cpu};
    if (!best || key < *best) {
      best = key;
    }
  }
  if (!best) {
    return std::nullopt;
  }
  return (*best)[2];
}

WorkerCpuReport Placement::DescribeWorkerCpu(
    int cpu, std::span<const int> other_workers) const {
  WorkerCpuReport r;
  const std::string name = "worker CPU " + std::to_string(cpu);
  r.numa_node = topology.NodeOf(cpu);
  r.outside_set = cpus.count(cpu) == 0;
  if (r.outside_set) {
    r.warnings.push_back(name + " is outside bessd's CPU set (" +
                         CpusOrNone(cpus) + ")");
  }
  r.cross_numa = r.numa_node != node;
  if (r.cross_numa) {
    r.warnings.push_back(
        name + " is on node " + std::to_string(r.numa_node) +
        ", bessd's memory on node " + std::to_string(node) +
        ": every packet and table access crosses the interconnect "
        "(cross-NUMA). Use a CPU of node " + std::to_string(node) +
        " or run this bessd with --numa_node=" + std::to_string(r.numa_node));
  }
  r.on_control_cpu = control_cpus.count(cpu) != 0;
  if (r.on_control_cpu) {
    r.warnings.push_back(name +
                         " is the reserved control CPU: control threads "
                         "(gRPC, maintenance) take time from this worker");
  }
  for (int other : other_workers) {
    if (other == cpu) {
      r.warnings.push_back(name + " already runs another worker");
    } else if (topology.SharesCore(cpu, other)) {
      r.shares_core = true;
      r.warnings.push_back(name + " shares a physical core with worker CPU " +
                           std::to_string(other) +
                           " (SMT siblings compete for one core; give bessd "
                           "whole cores)");
    }
  }
  for (int control : control_cpus) {
    if (topology.SharesCore(cpu, control)) {
      r.shares_core = true;
      r.warnings.push_back(name +
                           " shares a physical core with control CPU " +
                           std::to_string(control) +
                           " (SMT siblings compete for one core; give bessd "
                           "whole cores)");
    }
  }
  return r;
}

std::string Placement::Summary() const {
  return "placement: node " + std::to_string(node) + " (candidates " +
         NodeSet(candidates) + ", reason: " + NodeReasonName(reason) +
         "), control CPUs " + CpusOrNone(control_cpus) +
         ", worker CPUs available " + CpusOrNone(WorkerCpus());
}

std::set<int> MemsAllowed() {
  constexpr unsigned long kMaxNodes = 1024;
  constexpr size_t kBits = sizeof(unsigned long) * CHAR_BIT;
  std::array<unsigned long, kMaxNodes / kBits> mask{};
  std::set<int> nodes;
  if (syscall(SYS_get_mempolicy, nullptr, mask.data(), kMaxNodes, nullptr,
              MPOL_F_MEMS_ALLOWED) != 0) {
    return nodes;
  }
  for (size_t i = 0; i < kMaxNodes; i++) {
    if ((mask[i / kBits] >> (i % kBits)) & 1UL) {
      nodes.insert(static_cast<int>(i));
    }
  }
  return nodes;
}

const Placement &ProcessPlacement() {
  static const Placement placement = [] {
    PlacementInput input;
    input.topology = ReadTopology("/sys");
    const cpu_set_t &set = ProcessCpus();
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
      if (CPU_ISSET(cpu, &set)) {
        input.cpus.insert(cpu);
      }
    }
    input.mems_allowed = MemsAllowed();
    input.numa_node_flag = FLAGS_numa_node;
    input.control_cpus_flag = FLAGS_control_cpus;
    auto resolved = ResolvePlacement(input);
    if (!resolved) {
      LOG(FATAL) << resolved.error();
    }
    LOG(INFO) << resolved->Summary();
    for (const std::string &warning : resolved->warnings) {
      LOG(WARNING) << warning;
    }
    return std::move(*resolved);
  }();
  return placement;
}

int MemoryNode() { return ProcessPlacement().node; }

void ApplyProcessPlacement() {
  const Placement &p = ProcessPlacement();
  if (p.topology.memory_nodes.size() <= 1) {
    return;  // one node: nothing to prefer
  }
  constexpr size_t kBits = sizeof(unsigned long) * CHAR_BIT;
  std::array<unsigned long, 1024 / kBits> mask{};
  const auto node = static_cast<size_t>(p.node);
  if (node >= mask.size() * kBits) {
    LOG(WARNING) << "node " << p.node << " is beyond the memory policy mask";
    return;
  }
  mask[node / kBits] |= 1UL << (node % kBits);
  if (syscall(SYS_set_mempolicy, MPOL_PREFERRED, mask.data(), node + 2) != 0) {
    PLOG(WARNING) << "set_mempolicy(MPOL_PREFERRED, node " << p.node
                  << ") failed: normal memory follows the kernel's default";
  }
}

}  // namespace bess::runtime
