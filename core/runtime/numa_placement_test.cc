// SPDX-License-Identifier: BSD-3-Clause

// Process placement (Decision D-123): the resolver over fake sysfs trees.
// Every scenario of the D-123 table (docs/decisions.md and
// docs/placement.md) is one test here, named in the table; the scenarios
// differ only in their inputs -- a sysfs tree, the affinity, the allowed
// memory nodes, the flags -- as they do on real hosts.

#include "runtime/numa_placement.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace bess::runtime {
namespace {

namespace fs = std::filesystem;

// A sysfs tree in a temporary directory: devices/system/{cpu,node}.
class FakeSysfs {
 public:
  FakeSysfs() {
    static std::atomic<int> seq{0};
    root_ = fs::temp_directory_path() /
            ("bess_numa_" + std::to_string(getpid()) + "_" +
             std::to_string(seq++));
    fs::remove_all(root_);
    fs::create_directories(cpu_dir());
  }
  ~FakeSysfs() {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }
  FakeSysfs(const FakeSysfs &) = delete;
  FakeSysfs &operator=(const FakeSysfs &) = delete;

  // CPUs `first`..`last` on `node` (cpu/cpuN/nodeM link and node/nodeM/
  // cpulist), with `siblings(cpu)` as thread_siblings_list ("" for none).
  template <typename Siblings>
  FakeSysfs &Cpus(int first, int last, int node, Siblings siblings) {
    for (int cpu = first; cpu <= last; cpu++) {
      const fs::path dir = cpu_dir() / ("cpu" + std::to_string(cpu));
      fs::create_directories(dir / "topology");
      if (node >= 0) {
        fs::create_directories(node_dir() / ("node" + std::to_string(node)));
        fs::create_directory_symlink(
            "../../node/node" + std::to_string(node),
            dir / ("node" + std::to_string(node)));
      }
      const std::string list = siblings(cpu);
      if (!list.empty()) {
        Write(dir / "topology/thread_siblings_list", list);
      }
    }
    return *this;
  }
  FakeSysfs &Cpus(int first, int last, int node) {
    return Cpus(first, last, node,
                [](int cpu) { return std::to_string(cpu); });
  }
  // node/nodeM/cpulist (and the directory).
  FakeSysfs &NodeCpulist(int node, const std::string &list) {
    Write(node_dir() / ("node" + std::to_string(node)) / "cpulist", list);
    return *this;
  }
  // node/<name>, e.g. has_memory, online, possible.
  FakeSysfs &NodeFile(const std::string &name, const std::string &value) {
    Write(node_dir() / name, value);
    return *this;
  }
  // node/nodeM/hugepages/hugepages-2048kB/free_hugepages.
  FakeSysfs &FreeHugepages(int node, int pages) {
    Write(node_dir() / ("node" + std::to_string(node)) /
              "hugepages/hugepages-2048kB/free_hugepages",
          std::to_string(pages));
    return *this;
  }

  Topology Read() const { return ReadTopology(root_.string()); }

 private:
  fs::path cpu_dir() const { return root_ / "devices/system/cpu"; }
  fs::path node_dir() const { return root_ / "devices/system/node"; }
  static void Write(const fs::path &path, const std::string &value) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << value << "\n";
  }

  fs::path root_;
};

std::set<int> Range(int first, int last) {
  std::set<int> out;
  for (int i = first; i <= last; i++) {
    out.insert(i);
  }
  return out;
}

// Siblings i and i + half (Intel's usual numbering).
auto SplitSiblings(int half) {
  return [half](int cpu) {
    const int low = cpu % half;
    return std::to_string(low) + "," + std::to_string(low + half);
  };
}

// Siblings 2k and 2k+1 (AMD's and many VMs' numbering).
std::string AdjacentSiblings(int cpu) {
  const int low = cpu & ~1;
  return std::to_string(low) + "-" + std::to_string(low + 1);
}

// Two nodes, 8 CPUs each (0-7 on node 0, 8-15 on node 1), memory on both.
void TwoNodes(FakeSysfs &sys) {
  sys.Cpus(0, 7, 0).Cpus(8, 15, 1).NodeFile("has_memory", "0-1");
}

struct Expect {
  int node;
  std::set<int> control;
  std::vector<int> workers;  // the first automatic worker CPUs, in order
  size_t warnings;
};

// Resolves and checks node, control CPUs, the first auto worker CPUs and
// the number of warnings. Returns the placement for further checks.
Placement Check(const PlacementInput &input, const Expect &want) {
  auto resolved = ResolvePlacement(input);
  EXPECT_TRUE(resolved.has_value()) << resolved.error();
  if (!resolved) {
    return Placement{};
  }
  const Placement &p = *resolved;
  EXPECT_EQ(want.node, p.node) << p.Summary();
  EXPECT_EQ(want.control, p.control_cpus) << p.Summary();
  std::vector<int> used;
  for (int expected : want.workers) {
    const std::optional<int> cpu = p.NextWorkerCpu(used);
    EXPECT_EQ(std::optional<int>(expected), cpu) << p.Summary();
    if (!cpu) {
      break;
    }
    used.push_back(*cpu);
  }
  std::string all;
  for (const std::string &w : p.warnings) {
    all += "\n  " + w;
  }
  EXPECT_EQ(want.warnings, p.warnings.size()) << p.Summary() << all;
  return p;
}

PlacementInput Input(const Topology &topo, std::set<int> cpus,
                     std::set<int> mems) {
  PlacementInput in;
  in.topology = topo;
  in.cpus = std::move(cpus);
  in.mems_allowed = std::move(mems);
  return in;
}

// -- cpulists and the sysfs reader -----------------------------------------

TEST(NumaPlacementTest, CpuListsParseAndFormat) {
  EXPECT_EQ(std::optional<std::set<int>>(std::set<int>{0, 1, 2, 3, 8}),
            ParseCpuList("0-3,8"));
  EXPECT_EQ(std::optional<std::set<int>>(std::set<int>{}), ParseCpuList(""));
  EXPECT_EQ(std::optional<std::set<int>>(std::set<int>{5}), ParseCpuList("5"));
  for (const char *bad : {"a", "3-1", "1,", ",1", "1--2", "-1", "1-", "99999999"}) {
    EXPECT_FALSE(ParseCpuList(bad).has_value()) << bad;
  }
  EXPECT_EQ("0-3,8,10-11", FormatCpuList({0, 1, 2, 3, 8, 10, 11}));
  EXPECT_EQ("", FormatCpuList({}));
}

TEST(NumaPlacementTest, NodeCpulistPlacesCpusWithoutLinks) {
  FakeSysfs sys;
  sys.Cpus(0, 3, -1).NodeCpulist(0, "0-1").NodeCpulist(1, "2-3");
  const Topology t = sys.Read();
  EXPECT_EQ(0, t.NodeOf(1));
  EXPECT_EQ(1, t.NodeOf(2));
  EXPECT_EQ((std::set<int>{0, 1}), t.memory_nodes);  // the nodeM directories
  EXPECT_EQ(0, t.NodeOf(99));  // unknown CPU: node 0
}

// -- the D-123 scenario table -------------------------------------------------

// Row 1: bare metal, one node, SMT on (siblings i and i+10), all 20 CPUs.
TEST(NumaPlacementTest, BareMetalOneNodeSmt) {
  FakeSysfs sys;
  sys.Cpus(0, 19, 0, SplitSiblings(10)).NodeFile("has_memory", "0");
  Check(Input(sys.Read(), Range(0, 19), {0}), {0, {0}, {1, 2}, 0});
}

// Row 2: bare metal, one node, SMT off.
TEST(NumaPlacementTest, BareMetalOneNodeNoSmt) {
  FakeSysfs sys;
  sys.Cpus(0, 9, 0).NodeFile("has_memory", "0");
  const Placement p = Check(Input(sys.Read(), Range(0, 9), {0}), {0, {0}, {1, 2}, 0});
  EXPECT_FALSE(p.DescribeWorkerCpu(2, std::vector<int>{1}).shares_core);
}

// Row 3: two nodes, taskset to node 1's CPUs, memory allowed on both.
TEST(NumaPlacementTest, TwoNodesTasksetToNodeOne) {
  FakeSysfs sys;
  TwoNodes(sys);
  Check(Input(sys.Read(), Range(8, 15), {0, 1}), {1, {8}, {9, 10}, 0});
}

// Row 4: two nodes, every CPU and node: the lowest, with a warning.
TEST(NumaPlacementTest, TwoNodesAllCpusWarnsAmbiguity) {
  FakeSysfs sys;
  TwoNodes(sys);
  const Placement p =
      Check(Input(sys.Read(), Range(0, 15), {0, 1}), {0, {0}, {1, 2}, 1});
  EXPECT_NE(std::string::npos, p.warnings[0].find("--numa_node"));
  EXPECT_EQ(NodeReason::kLowestOfSeveral, p.reason);
}

// Row 5: two nodes, --numa_node=1: node 1, silent.
TEST(NumaPlacementTest, TwoNodesFlagChoosesNodeOne) {
  FakeSysfs sys;
  TwoNodes(sys);
  PlacementInput in = Input(sys.Read(), Range(0, 15), {0, 1});
  in.numa_node_flag = 1;
  const Placement p = Check(in, {1, {8}, {9, 10}, 0});
  EXPECT_EQ(NodeReason::kFlag, p.reason);
}

// Row 6: --numa_node outside the candidates: used, with a warning.
TEST(NumaPlacementTest, FlagOutsideCandidatesWarns) {
  FakeSysfs sys;
  TwoNodes(sys);
  PlacementInput in = Input(sys.Read(), Range(0, 7), {0, 1});
  in.numa_node_flag = 1;
  // No CPU of node 1 in the set: control and workers come from the set.
  const Placement p = Check(in, {1, {0}, {1, 2}, 1});
  EXPECT_TRUE(p.DescribeWorkerCpu(1, {}).cross_numa);
}

// Row 7: node ids 0 and 2 (non-contiguous).
TEST(NumaPlacementTest, NonContiguousNodeIds) {
  FakeSysfs sys;
  sys.Cpus(0, 3, 0).Cpus(4, 7, 2).NodeFile("has_memory", "0,2");
  Check(Input(sys.Read(), Range(4, 7), {0, 2}), {2, {4}, {5, 6}, 0});
}

// Row 8: a memory-only node (CXL: memory, no CPUs) is never chosen.
TEST(NumaPlacementTest, MemoryOnlyNodeIgnored) {
  FakeSysfs sys;
  sys.Cpus(0, 7, 0).NodeFile("has_memory", "0-1").NodeCpulist(1, "");
  const Placement p = Check(Input(sys.Read(), Range(0, 7), {0, 1}), {0, {0}, {1, 2}, 0});
  EXPECT_EQ((std::set<int>{0}), p.candidates);
}

// Row 9: CPUs only on a node without memory: the lowest node whose memory
// may be used, with a warning (a CPU-only node can hold nothing).
TEST(NumaPlacementTest, CpuOnlyNodeUsesAllowedMemoryNode) {
  FakeSysfs sys;
  sys.Cpus(0, 3, 0).Cpus(4, 7, 1).NodeFile("has_memory", "0");
  const Placement p = Check(Input(sys.Read(), Range(4, 7), {0}), {0, {4}, {5, 6}, 1});
  EXPECT_EQ(NodeReason::kNoCandidate, p.reason);
}

// Row 10: a container cpuset without CPU 0: control is the set's first CPU.
TEST(NumaPlacementTest, ContainerCpusetWithoutCpuZero) {
  FakeSysfs sys;
  sys.Cpus(0, 15, 0).NodeFile("has_memory", "0");
  Check(Input(sys.Read(), Range(4, 7), {0}), {0, {4}, {5, 6}, 0});
}

// Row 11: cpuset.mems restricted to node 1, CPUs on both nodes: node 1.
TEST(NumaPlacementTest, ContainerMemsRestrictToNodeOne) {
  FakeSysfs sys;
  TwoNodes(sys);
  Check(Input(sys.Read(), Range(0, 15), {1}), {1, {8}, {9, 10}, 0});
}

// Row 12: CPUs on node 0, memory only on node 1: no candidate; memory from
// node 1 (the only node allowed), warned; workers are on the set's CPUs.
TEST(NumaPlacementTest, ContainerCpusAndMemsDisjoint) {
  FakeSysfs sys;
  TwoNodes(sys);
  const Placement p = Check(Input(sys.Read(), Range(0, 7), {1}), {1, {0}, {1, 2}, 1});
  EXPECT_EQ(NodeReason::kNoCandidate, p.reason);
}

// Row 13: a one-CPU container: no reservation; the worker shares it, warned.
TEST(NumaPlacementTest, OneCpuContainerSharesTheCpu) {
  FakeSysfs sys;
  sys.Cpus(0, 7, 0).NodeFile("has_memory", "0");
  const Placement p = Check(Input(sys.Read(), {5}, {0}), {0, {}, {5}, 1});
  EXPECT_EQ(std::nullopt, p.NextWorkerCpu(std::vector<int>{5}));
}

// Row 15: hugepages are not an input: with or without free hugepages on
// either node the placement is the same (the no-hugepage EAL path is
// unchanged; see dpdk.cc and utils/dpdk_memory.h).
TEST(NumaPlacementTest, HugepagesDoNotChangeThePlacement) {
  FakeSysfs with;
  TwoNodes(with);
  with.FreeHugepages(0, 0).FreeHugepages(1, 512);
  FakeSysfs without;
  TwoNodes(without);
  const Placement a = Check(Input(with.Read(), Range(8, 15), {0, 1}), {1, {8}, {9, 10}, 0});
  const Placement b = Check(Input(without.Read(), Range(8, 15), {0, 1}), {1, {8}, {9, 10}, 0});
  EXPECT_EQ(a.Summary(), b.Summary());
}

// Row 16: Kubernetes Guaranteed pod, CPU Manager static, Memory Manager
// static, Topology Manager single-numa-node: one node's CPUs and memory.
TEST(NumaPlacementTest, K8sGuaranteedSingleNumaNode) {
  FakeSysfs sys;
  TwoNodes(sys);
  Check(Input(sys.Read(), Range(10, 13), {1}), {1, {10}, {11, 12}, 0});
}

// Row 17: Guaranteed, Memory Manager none (every node's memory allowed).
TEST(NumaPlacementTest, K8sGuaranteedWithoutMemoryManager) {
  FakeSysfs sys;
  TwoNodes(sys);
  Check(Input(sys.Read(), Range(10, 13), {0, 1}), {1, {10}, {11, 12}, 0});
}

// Row 18: Burstable, the shared pool: every CPU and node; lowest, warned.
TEST(NumaPlacementTest, K8sBurstableSharedPoolWarns) {
  FakeSysfs sys;
  TwoNodes(sys);
  Check(Input(sys.Read(), Range(0, 15), {0, 1}), {0, {0}, {1, 2}, 1});
}

// Row 20: a VM that hides NUMA (no devices/system/node): node 0, silent.
TEST(NumaPlacementTest, VmWithoutNodeDirectory) {
  FakeSysfs sys;
  sys.Cpus(0, 3, -1);
  const Topology t = sys.Read();
  EXPECT_EQ((std::set<int>{0}), t.memory_nodes);
  Check(Input(t, Range(0, 3), {0}), {0, {0}, {1, 2}, 0});
}

// Row 21: the node directory exists but places no CPU: node 0, silent.
TEST(NumaPlacementTest, VmNodeDirectoryWithoutCpuLinks) {
  FakeSysfs sys;
  sys.Cpus(0, 3, -1).NodeFile("online", "0");
  Check(Input(sys.Read(), Range(0, 3), {}), {0, {0}, {1, 2}, 0});
}

// Row 22: a NIC whose node DPDK cannot tell (-1): "unknown", never warned.
TEST(NumaPlacementTest, DeviceWithUnknownNode) {
  EXPECT_EQ(DeviceNuma::kUnknown, ClassifyDevice(-1, 0));
  EXPECT_STREQ("unknown", DeviceNumaName(ClassifyDevice(-1, 1)));
}

// Row 23: vCPUs whose thread_siblings_list is just themselves: no SMT.
TEST(NumaPlacementTest, VmVcpusWithoutSmt) {
  FakeSysfs sys;
  sys.Cpus(0, 3, 0).NodeFile("has_memory", "0");
  const Placement p = Check(Input(sys.Read(), Range(0, 3), {0}), {0, {0}, {1, 2}, 0});
  EXPECT_FALSE(p.DescribeWorkerCpu(2, std::vector<int>{1}).shares_core);
}

// Row 24: a NIC on the other node: cross (warned once by the port).
TEST(NumaPlacementTest, DeviceOnOtherNodeIsCross) {
  EXPECT_EQ(DeviceNuma::kCross, ClassifyDevice(1, 0));
  EXPECT_EQ(DeviceNuma::kLocal, ClassifyDevice(1, 1));
  EXPECT_STREQ("cross", DeviceNumaName(DeviceNuma::kCross));
}

// Row 25: an explicit worker on the other node's CPU: accepted, warned.
TEST(NumaPlacementTest, ExplicitWorkerOnOtherNodeWarns) {
  FakeSysfs sys;
  TwoNodes(sys);
  const Placement p = Check(Input(sys.Read(), Range(8, 15), {0, 1}), {1, {8}, {9}, 0});
  PlacementInput all = Input(sys.Read(), Range(0, 15), {0, 1});
  all.numa_node_flag = 1;
  const Placement q = *ResolvePlacement(all);
  const WorkerCpuReport r = q.DescribeWorkerCpu(3, {});
  EXPECT_TRUE(r.cross_numa);
  EXPECT_EQ(0, r.numa_node);
  EXPECT_FALSE(r.outside_set);
  ASSERT_EQ(1u, r.warnings.size());
  EXPECT_NE(std::string::npos, r.warnings[0].find("cross-NUMA"));
  EXPECT_TRUE(p.DescribeWorkerCpu(3, {}).outside_set);
}

// Row 26: an explicit worker on the reserved control CPU: accepted, warned.
TEST(NumaPlacementTest, ExplicitWorkerOnControlCpuWarns) {
  FakeSysfs sys;
  sys.Cpus(0, 7, 0).NodeFile("has_memory", "0");
  const Placement p = Check(Input(sys.Read(), Range(0, 7), {0}), {0, {0}, {1}, 0});
  const WorkerCpuReport r = p.DescribeWorkerCpu(0, {});
  EXPECT_TRUE(r.on_control_cpu);
  EXPECT_EQ(1u, r.warnings.size());
}

// Row 27: an explicit worker outside the affinity: reported outside (and
// refused by AddWorker, D-027; see control/thread_placement_test.cc).
TEST(NumaPlacementTest, ExplicitWorkerOutsideAffinityIsOutside) {
  FakeSysfs sys;
  sys.Cpus(0, 7, 0).NodeFile("has_memory", "0");
  const Placement p = Check(Input(sys.Read(), Range(2, 5), {0}), {0, {2}, {3}, 0});
  EXPECT_TRUE(p.DescribeWorkerCpu(7, {}).outside_set);
  EXPECT_FALSE(p.DescribeWorkerCpu(4, {}).outside_set);
}

// Row 28: the allocator runs out: nullopt (AddWorker answers ENOSPC).
TEST(NumaPlacementTest, AllocatorExhaustion) {
  FakeSysfs sys;
  sys.Cpus(0, 3, 0).NodeFile("has_memory", "0");
  const Placement p = Check(Input(sys.Read(), Range(0, 3), {0}), {0, {0}, {1, 2, 3}, 0});
  EXPECT_EQ(std::nullopt, p.NextWorkerCpu(std::vector<int>{1, 2, 3}));
  // Explicit workers on the control CPU do not change what is free.
  EXPECT_EQ(std::nullopt, p.NextWorkerCpu(std::vector<int>{0, 1, 2, 3}));
}

// Row 29: the choice is a function of the CPUs in use: a live worker keeps
// its CPU and a later choice never takes it.
TEST(NumaPlacementTest, AllocatorIsStable) {
  FakeSysfs sys;
  sys.Cpus(0, 7, 0).NodeFile("has_memory", "0");
  const Placement p = *ResolvePlacement(Input(sys.Read(), Range(0, 7), {0}));
  EXPECT_EQ(std::optional<int>(4), p.NextWorkerCpu(std::vector<int>{1, 2, 3}));
  EXPECT_EQ(p.NextWorkerCpu(std::vector<int>{3, 1, 2}),
            p.NextWorkerCpu(std::vector<int>{1, 2, 3}));
  EXPECT_EQ(std::optional<int>(1), p.NextWorkerCpu(std::vector<int>{5, 6}));
}

// Row 30: a destroyed worker's CPU is free again; the next automatic
// worker takes the lowest free CPU.
TEST(NumaPlacementTest, FreedCpuIsReused) {
  FakeSysfs sys;
  sys.Cpus(0, 7, 0).NodeFile("has_memory", "0");
  const Placement p = *ResolvePlacement(Input(sys.Read(), Range(0, 7), {0}));
  EXPECT_EQ(std::optional<int>(4), p.NextWorkerCpu(std::vector<int>{1, 2, 3}));
  EXPECT_EQ(std::optional<int>(2), p.NextWorkerCpu(std::vector<int>{1, 3}));
}

// Every automatic worker for `count` picks, and each one's warnings given
// the workers before it.
std::vector<int> AutoWorkers(const Placement &p, int count,
                             std::vector<size_t> *warnings = nullptr) {
  std::vector<int> used;
  for (int i = 0; i < count; i++) {
    const std::optional<int> cpu = p.NextWorkerCpu(used);
    if (!cpu) {
      break;
    }
    if (warnings != nullptr) {
      warnings->push_back(p.DescribeWorkerCpu(*cpu, used).warnings.size());
    }
    used.push_back(*cpu);
  }
  return used;
}

// Row (i): N+1 whole cores (2N+2 CPUs, siblings i and i+4), N = 3 automatic
// workers: one per core, every sibling idle, no warning.
TEST(NumaPlacementTest, StripedWholeCoresOnePerCore) {
  FakeSysfs sys;
  sys.Cpus(0, 7, 0, SplitSiblings(4)).NodeFile("has_memory", "0");
  const Placement p = *ResolvePlacement(Input(sys.Read(), Range(0, 7), {0}));
  std::vector<size_t> warnings;
  EXPECT_EQ((std::vector<int>{1, 2, 3}), AutoWorkers(p, 3, &warnings));
  EXPECT_EQ((std::vector<size_t>{0, 0, 0}), warnings);
}

// Row (ii): N whole cores (2N CPUs, siblings i and i+3), N = 3 workers: the
// last lands on the control CPU's sibling, warned.
TEST(NumaPlacementTest, StripedLastWorkerOnControlSibling) {
  FakeSysfs sys;
  sys.Cpus(0, 5, 0, SplitSiblings(3)).NodeFile("has_memory", "0");
  const Placement p = *ResolvePlacement(Input(sys.Read(), Range(0, 5), {0}));
  std::vector<size_t> warnings;
  EXPECT_EQ((std::vector<int>{1, 2, 3}), AutoWorkers(p, 3, &warnings));
  EXPECT_EQ((std::vector<size_t>{0, 0, 1}), warnings);
  EXPECT_NE(std::string::npos,
            p.DescribeWorkerCpu(3, std::vector<int>{1, 2}).warnings[0].find(
                "control CPU 0"));
}

// Row (iii): unrestricted 10 cores / 20 threads (siblings i and i+10), 12
// workers: the 9 free first threads (core 0's is the control CPU), then
// second threads from core 0 on -- each of those warned.
TEST(NumaPlacementTest, StripedFirstThreadsThenSiblings) {
  FakeSysfs sys;
  sys.Cpus(0, 19, 0, SplitSiblings(10)).NodeFile("has_memory", "0");
  const Placement p = *ResolvePlacement(Input(sys.Read(), Range(0, 19), {0}));
  std::vector<size_t> warnings;
  EXPECT_EQ((std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}),
            AutoWorkers(p, 12, &warnings));
  EXPECT_EQ((std::vector<size_t>{0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1}), warnings);
}

// Row (iii), the other common numbering (siblings 2k and 2k+1): cores in
// order of their lowest CPU, first threads before any second one.
TEST(NumaPlacementTest, StripedOrderWithAdjacentSiblings) {
  FakeSysfs sys;
  sys.Cpus(0, 7, 0, AdjacentSiblings).NodeFile("has_memory", "0");
  const Placement p = *ResolvePlacement(Input(sys.Read(), Range(0, 7), {0}));
  EXPECT_EQ((std::vector<int>{2, 4, 6, 1, 3, 5, 7}), AutoWorkers(p, 8));
  // A core whose first thread is outside the set: its second is a first.
  const Placement q = *ResolvePlacement(Input(sys.Read(), {0, 1, 3, 4, 5}, {0}));
  EXPECT_EQ((std::vector<int>{3, 4, 1, 5}), AutoWorkers(q, 4));
}

// Row (iv): a VM without sibling information: lowest first.
TEST(NumaPlacementTest, StripedWithoutSiblingsIsLowestFirst) {
  FakeSysfs sys;
  sys.Cpus(0, 5, 0, [](int) { return std::string(); }).NodeFile("has_memory", "0");
  const Placement p = *ResolvePlacement(Input(sys.Read(), Range(0, 5), {0}));
  EXPECT_EQ((std::vector<int>{1, 2, 3, 4, 5}), AutoWorkers(p, 6));
}

// Row (v): a destroyed first-thread worker's CPU is taken again by the
// next automatic worker, not a sibling.
TEST(NumaPlacementTest, StripedReusesFreedFirstThread) {
  FakeSysfs sys;
  sys.Cpus(0, 19, 0, SplitSiblings(10)).NodeFile("has_memory", "0");
  const Placement p = *ResolvePlacement(Input(sys.Read(), Range(0, 19), {0}));
  std::vector<int> used = AutoWorkers(p, 12);
  std::erase(used, 5);
  EXPECT_EQ(std::optional<int>(5), p.NextWorkerCpu(used));
}

// Row (vi): an explicit core equal to an automatic worker's CPU: both run
// there (the explicit one warned), the automatic one is not moved.
TEST(NumaPlacementTest, ExplicitCoreOnAnAutoWorkersCpuWarns) {
  FakeSysfs sys;
  sys.Cpus(0, 7, 0).NodeFile("has_memory", "0");
  const Placement p = *ResolvePlacement(Input(sys.Read(), Range(0, 7), {0}));
  const std::vector<int> autos = AutoWorkers(p, 1);
  ASSERT_EQ((std::vector<int>{1}), autos);
  const WorkerCpuReport r = p.DescribeWorkerCpu(1, autos);
  ASSERT_EQ(1u, r.warnings.size());
  EXPECT_NE(std::string::npos, r.warnings[0].find("already runs another worker"));
  EXPECT_EQ(std::optional<int>(2), p.NextWorkerCpu(std::vector<int>{1, 1}));
}

// Row (vii): an explicit worker, then an automatic one: the automatic one
// skips the explicitly held CPU.
TEST(NumaPlacementTest, AutoWorkerSkipsExplicitCpu) {
  FakeSysfs sys;
  sys.Cpus(0, 7, 0).NodeFile("has_memory", "0");
  const Placement p = *ResolvePlacement(Input(sys.Read(), Range(0, 7), {0}));
  EXPECT_EQ(std::optional<int>(2), p.NextWorkerCpu(std::vector<int>{1}));
  EXPECT_EQ(std::optional<int>(1), p.NextWorkerCpu(std::vector<int>{3}));
}

// Row (b): an explicit worker on the control CPU's sibling: warned.
TEST(NumaPlacementTest, ExplicitWorkerOnControlSiblingWarns) {
  FakeSysfs sys;
  sys.Cpus(0, 19, 0, SplitSiblings(10)).NodeFile("has_memory", "0");
  const Placement p = Check(Input(sys.Read(), Range(0, 19), {0}), {0, {0}, {1}, 0});
  const WorkerCpuReport r = p.DescribeWorkerCpu(10, {});
  EXPECT_TRUE(r.shares_core);
  EXPECT_FALSE(r.on_control_cpu);
  EXPECT_EQ(1u, r.warnings.size());
}

// Row (c): no thread_siblings_list at all (some VMs): no SMT warning.
TEST(NumaPlacementTest, MissingSiblingListsNeverWarn) {
  FakeSysfs sys;
  sys.Cpus(0, 3, 0, [](int) { return std::string(); }).NodeFile("has_memory", "0");
  const Placement p = Check(Input(sys.Read(), Range(0, 3), {0}), {0, {0}, {1, 2}, 0});
  EXPECT_TRUE(p.DescribeWorkerCpu(2, std::vector<int>{1}).warnings.empty());
  EXPECT_TRUE(p.DescribeWorkerCpu(1, {}).warnings.empty());
}

// -- --control_cpus -----------------------------------------------------------

TEST(NumaPlacementTest, ControlCpusFlag) {
  FakeSysfs sys;
  sys.Cpus(0, 7, 0).NodeFile("has_memory", "0");
  PlacementInput in = Input(sys.Read(), Range(0, 7), {0});
  in.control_cpus_flag = "6-7";
  Check(in, {0, {6, 7}, {0, 1}, 0});
  in.control_cpus_flag = "none";
  Check(in, {0, {}, {0, 1}, 0});
  in.control_cpus_flag = "0-7";  // nothing left for automatic workers
  Check(in, {0, {0, 1, 2, 3, 4, 5, 6, 7}, {}, 1});
  in.control_cpus_flag = "x";
  EXPECT_FALSE(ResolvePlacement(in).has_value());
}

// Row (ix): --control_cpus naming a CPU outside the set: refused (startup
// fails), in the words AddWorker uses for a worker outside it.
TEST(NumaPlacementTest, ControlCpusOutsideSetRefused) {
  FakeSysfs sys;
  sys.Cpus(0, 15, 0).NodeFile("has_memory", "0");
  PlacementInput in = Input(sys.Read(), Range(4, 7), {0});
  in.control_cpus_flag = "3-4";
  const auto resolved = ResolvePlacement(in);
  ASSERT_FALSE(resolved.has_value());
  EXPECT_NE(std::string::npos, resolved.error().find("core 3 is not in bessd's CPU set (4-7)"))
      << resolved.error();
}

TEST(NumaPlacementTest, SummaryIsOneDiagnosableLine) {
  FakeSysfs sys;
  TwoNodes(sys);
  const Placement p = *ResolvePlacement(Input(sys.Read(), Range(8, 11), {0, 1}));
  EXPECT_EQ(
      "placement: node 1 (candidates {1}, reason: the only node with both "
      "bessd's CPUs and its memory), control CPUs 8, worker CPUs available 9-11",
      p.Summary());
}

// The real host resolves without error (whatever it is).
TEST(NumaPlacementTest, ThisHostResolves) {
  const Topology t = ReadTopology("/sys");
  EXPECT_FALSE(t.memory_nodes.empty());
  PlacementInput in;
  in.topology = t;
  in.cpus = {0};
  EXPECT_TRUE(ResolvePlacement(in).has_value());
}

}  // namespace
}  // namespace bess::runtime
