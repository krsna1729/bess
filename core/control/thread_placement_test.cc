// SPDX-License-Identifier: BSD-3-Clause

// Thread and CPU placement (Decision D-027): everything bessd places stays
// inside the CPU set it was started with (a container's cpuset, a taskset),
// and its own threads stay off the workers' CPUs. Run it restricted too
// (`taskset -c 2-3 control_thread_placement_test`) to see it as a container
// would.

#include "runtime/thread_placement.h"
#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <rte_eal.h>
#include <rte_malloc.h>

#include <cerrno>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "control/control_plane.h"
#include "dpdk.h"
#include "runtime/runtime_state.h"
#include "runtime/numa_placement.h"
#include "runtime/worker_manager.h"
#include "runtime/opts.h"
#include "packet_pool.h"
#include "port.h"
#include "worker.h"
#include "utils/dpdk_memory.h"

namespace {

using bess::runtime::ControlCpus;
using bess::runtime::CpuAllowed;
using bess::runtime::CpuId;
using bess::runtime::CpuList;
using bess::runtime::ProcessCpus;

cpu_set_t Set(std::initializer_list<int> cpus) {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int c : cpus) {
    CPU_SET(c, &set);
  }
  return set;
}

cpu_set_t ThisThread() {
  cpu_set_t set;
  CPU_ZERO(&set);
  EXPECT_EQ(0, sched_getaffinity(0, sizeof(set), &set));
  return set;
}

void InitRuntimeOnce() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;
  FLAGS_m = 0;  // malloc-backed, sandbox-safe
  bess::PacketPool::CreateDefaultPools(32767);
  PortBuilder::InitDrivers();
}

TEST(ThreadPlacementTest, ControlCpusAreTheRestWithinTheBase) {
  const cpu_set_t base = Set({2, 3, 4, 5});
  const int w[] = {3, 9};  // 9 is outside the base: ignored
  cpu_set_t got = ControlCpus(base, w);
  EXPECT_TRUE(CPU_EQUAL(&got, &(const cpu_set_t &)Set({2, 4, 5})));
  // Workers on every base CPU: control threads keep the base.
  const int all[] = {2, 3, 4, 5};
  got = ControlCpus(base, all);
  EXPECT_TRUE(CPU_EQUAL(&got, &base));
  EXPECT_EQ(CpuList(Set({0, 1, 2, 5, 7, 8})), "0-2,5,7-8");
  EXPECT_EQ(CpuList(Set({4})), "4");
}

TEST(ThreadPlacementTest, EverythingStaysInsideTheInheritedSet) {
  const cpu_set_t now = ThisThread();
  // Inside the captured set, whatever ran before: other tests may already
  // have narrowed this thread to the control CPUs (PlaceControlThreads).
  cpu_set_t inside;
  CPU_AND(&inside, &now, &ProcessCpus());
  EXPECT_TRUE(CPU_EQUAL(&inside, &now)) << CpuList(now) << " vs " << CpuList(ProcessCpus());
  EXPECT_TRUE(CpuAllowed(bess::runtime::DefaultWorkerCore()));
}

// A test or benchmark that brings DPDK up lazily is not bessd: nothing is
// reserved, so rte_eal_init leaves the main thread -- and every thread made
// after it -- on the inherited set, not pinned to one control CPU.
TEST(ThreadPlacementTest, LazyEalBringUpKeepsTheInheritedSet) {
  // ApplyProcessPlacement is one-way and process-global: once another test
  // (ControlThreadsLeaveWorkerCpus) ran it, under --gtest_shuffle or
  // --gtest_repeat, this process is no longer a lazily initialised one.
  if (bess::runtime::ProcessPlacementApplied()) {
    GTEST_SKIP() << "process placement already applied in this process";
  }
  InitRuntimeOnce();  // brings the EAL up unless an earlier test did
  ASSERT_TRUE(bess::IsDpdkInitialized());
  const cpu_set_t reserved = bess::runtime::ReservedControlCpus();
  EXPECT_EQ(0, CPU_COUNT(&reserved));
  const cpu_set_t main_now = ThisThread();
  EXPECT_TRUE(CPU_EQUAL(&main_now, &ProcessCpus())) << CpuList(main_now);
  cpu_set_t later;
  std::thread([&] { later = ThisThread(); }).join();
  EXPECT_TRUE(CPU_EQUAL(&later, &ProcessCpus())) << CpuList(later);
}

// Row 15 (D-123): without hugepages the node choice is unchanged, and DPDK's
// one normal-page heap is socket 0, so every table asks for socket 0 there
// and its allocation succeeds.
TEST(ThreadPlacementTest, WithoutHugepagesTablesAllocateOnSocketZero) {
  InitRuntimeOnce();  // FLAGS_m = 0: --no-huge
  ASSERT_EQ(0, rte_eal_has_hugepages());
  EXPECT_EQ(0, bess::utils::MemoryNode());
  void *p = bess::utils::DpdkAllocate(4096, 64);
  ASSERT_NE(nullptr, p);
  rte_free(p);
}

// A worker on the last allowed CPU, in a process placed as bessd places
// itself: the other threads run on the reserved control CPU (D-123; with one
// CPU nothing is reserved and they share it), threads created afterwards
// inherit that, and destroying the worker leaves them there.
TEST(ThreadPlacementTest, ControlThreadsLeaveWorkerCpus) {
  InitRuntimeOnce();
  bess::runtime::ApplyProcessPlacement();  // what bessd's main does
  const cpu_set_t &process = ProcessCpus();
  int core = -1;
  for (int i = 0; i < CPU_SETSIZE; i++) {
    if (CPU_ISSET(i, &process)) {
      core = i;
    }
  }
  ASSERT_GE(core, 0);
  const cpu_set_t reserved = bess::runtime::ReservedControlCpus();
  const cpu_set_t control = CPU_COUNT(&reserved) > 0 ? reserved : process;
  launch_worker(0, core);
  const cpu_set_t main_now = ThisThread();
  cpu_set_t later;
  std::thread([&] { later = ThisThread(); }).join();
  if (CPU_COUNT(&process) > 1) {
    EXPECT_EQ(1, CPU_COUNT(&reserved)) << "auto reserves one CPU";
    EXPECT_FALSE(CPU_ISSET(core, &main_now)) << "main thread on worker CPU";
    EXPECT_FALSE(CPU_ISSET(core, &later)) << "new thread on worker CPU";
    EXPECT_TRUE(CPU_EQUAL(&main_now, &reserved)) << CpuList(main_now);
  } else {
    EXPECT_TRUE(CPU_EQUAL(&main_now, &process));  // nowhere else to go
  }
  destroy_worker(0);
  const cpu_set_t after = ThisThread();
  EXPECT_TRUE(CPU_EQUAL(&after, &control)) << CpuList(after);
}

// A worker CPU outside the set is refused with a clear error, not a crash.
TEST(ThreadPlacementTest, WorkersOutsideTheSetAreRefused) {
  InitRuntimeOnce();
  const long present = sysconf(_SC_NPROCESSORS_CONF);
  int outside = -1;
  for (int i = 0; i < present; i++) {
    const CpuId cpu(static_cast<uint32_t>(i));
    if (!CpuAllowed(cpu) && bess::runtime::CpuPresent(cpu)) {
      outside = i;
      break;
    }
  }
  if (outside < 0) {
    GTEST_SKIP() << "every CPU is allowed; run under taskset to exercise";
  }
  bess::control::ControlPlane control_plane;
  auto added = control_plane.AddWorker(0, static_cast<uint64_t>(outside), "");
  ASSERT_FALSE(added);
  EXPECT_NE(added.error().message.find("not in bessd's CPU set"),
            std::string::npos)
      << added.error().message;
  EXPECT_EQ(added.error().err, EINVAL);
  EXPECT_FALSE(is_worker_active(0));
}

// A core id is not truncated on its way to the checks: 2^32 + an allowed CPU
// names no CPU, so it is refused rather than started on that CPU.
TEST(ThreadPlacementTest, CoreIdsBeyond32BitsAreRefused) {
  InitRuntimeOnce();
  const CpuId allowed = bess::runtime::DefaultWorkerCore();
  ASSERT_TRUE(CpuAllowed(allowed));
  bess::control::ControlPlane control_plane;
  auto added = control_plane.AddWorker(
      1, (uint64_t{1} << 32) + allowed.value(), "");
  ASSERT_FALSE(added) << "core 2^32 + " << allowed.value() << " was accepted";
  EXPECT_EQ(added.error().err, EINVAL);
  EXPECT_FALSE(is_worker_active(1));
}

// -- automatic workers (D-123) ------------------------------------------------
//
// A worker given no core takes Placement::NextWorkerCpu over the live
// workers' CPUs; an explicit core inside the set is always honoured and only
// diagnosed. The CPUs come from the real ProcessPlacement(), so a test that
// needs more CPUs than this machine (or taskset) gives skips.

class AutomaticWorkerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    InitRuntimeOnce();
    (void)control_plane_.ResetWorkers();
  }
  void TearDown() override { (void)control_plane_.ResetWorkers(); }

  static const bess::runtime::Placement &Placement() {
    return bess::runtime::ProcessPlacement();
  }
  static int CoreOf(int wid) {
    const Worker *worker = bess::runtime::runtime().workers().Get(wid);
    return worker != nullptr ? worker->core() : -1;
  }
  static std::string Warnings(const bess::runtime::WorkerCpuReport &report) {
    std::string all;
    for (const std::string &w : report.warnings) {
      all += w + "\n";
    }
    return all;
  }

  bess::control::ControlPlane control_plane_;
};

TEST_F(AutomaticWorkerTest, AWorkerWithoutACoreGetsAWorkerCpuThatIsNotAControlCpu) {
  if (Placement().WorkerCpus().empty()) {
    GTEST_SKIP() << "no CPU for automatic workers: " << Placement().Summary();
  }
  auto added = control_plane_.AddWorker(0, std::nullopt, "");
  ASSERT_TRUE(added) << added.error().message;
  const int core = CoreOf(0);
  ASSERT_GE(core, 0);
  EXPECT_TRUE(CPU_ISSET(core, &ProcessCpus())) << core;
  EXPECT_EQ(1u, Placement().WorkerCpus().count(core)) << Placement().Summary();
  EXPECT_EQ(0u, Placement().control_cpus.count(core)) << Placement().Summary();
  EXPECT_EQ(std::optional<int>(core), Placement().NextWorkerCpu({}));
}

TEST_F(AutomaticWorkerTest, ASecondAutomaticWorkerGetsAnotherCpu) {
  if (Placement().WorkerCpus().size() < 2) {
    GTEST_SKIP() << "needs two CPUs for automatic workers: " << Placement().Summary();
  }
  ASSERT_TRUE(control_plane_.AddWorker(0, std::nullopt, ""));
  ASSERT_TRUE(control_plane_.AddWorker(1, std::nullopt, ""));
  EXPECT_NE(CoreOf(0), CoreOf(1));
  for (const int wid : {0, 1}) {
    EXPECT_EQ(1u, Placement().WorkerCpus().count(CoreOf(wid))) << wid;
    EXPECT_EQ(0u, Placement().control_cpus.count(CoreOf(wid))) << wid;
  }
}

TEST_F(AutomaticWorkerTest, ADestroyedWorkersCpuGoesToTheNextAutomaticWorker) {
  if (Placement().WorkerCpus().size() < 2) {
    GTEST_SKIP() << "needs two CPUs for automatic workers: " << Placement().Summary();
  }
  ASSERT_TRUE(control_plane_.AddWorker(0, std::nullopt, ""));
  ASSERT_TRUE(control_plane_.AddWorker(1, std::nullopt, ""));
  const int first = CoreOf(0);
  const int second = CoreOf(1);
  ASSERT_TRUE(control_plane_.DestroyWorker(0));
  ASSERT_TRUE(control_plane_.AddWorker(2, std::nullopt, ""));
  EXPECT_EQ(first, CoreOf(2)) << "the freed CPU is the first free one again";
  EXPECT_EQ(second, CoreOf(1)) << "the other worker did not move";
}

// Every worker CPU taken (by explicit workers here: as many as there are):
// one more automatic worker is ENOSPC, and nothing is started.
TEST_F(AutomaticWorkerTest, AnAutomaticWorkerWithNoFreeCpuIsRefusedWithEnospc) {
  const std::set<int> cpus = Placement().WorkerCpus();
  if (cpus.size() + 1 > static_cast<size_t>(Worker::kMaxWorkers)) {
    GTEST_SKIP() << cpus.size() << " worker CPUs: more than the workers there can be";
  }
  int wid = 0;
  for (const int cpu : cpus) {
    auto added = control_plane_.AddWorker(static_cast<uint64_t>(wid), static_cast<uint64_t>(cpu), "");
    ASSERT_TRUE(added) << added.error().message;
    wid++;
  }
  auto added = control_plane_.AddWorker(static_cast<uint64_t>(wid), std::nullopt, "");
  ASSERT_FALSE(added) << "worker " << wid << " got CPU " << CoreOf(wid);
  EXPECT_EQ(ENOSPC, added.error().err);
  EXPECT_NE(std::string::npos, added.error().message.find("no free CPU"))
      << added.error().message;
  EXPECT_FALSE(is_worker_active(wid));
}

// An explicit core is honoured even on an automatic worker's CPU: both run
// there, the automatic one does not move, and the second is diagnosed.
TEST_F(AutomaticWorkerTest, AnExplicitCoreOnAnAutomaticWorkersCpuIsHonouredBesideIt) {
  if (Placement().WorkerCpus().empty()) {
    GTEST_SKIP() << "no CPU for automatic workers: " << Placement().Summary();
  }
  ASSERT_TRUE(control_plane_.AddWorker(0, std::nullopt, ""));
  const int cpu = CoreOf(0);
  auto added = control_plane_.AddWorker(1, static_cast<uint64_t>(cpu), "");
  ASSERT_TRUE(added) << added.error().message;
  EXPECT_EQ(cpu, CoreOf(1));
  EXPECT_EQ(cpu, CoreOf(0)) << "the automatic worker moved";
  const bess::runtime::WorkerCpuReport report =
      bess::runtime::runtime().workers().CpuReport(1);
  EXPECT_NE(std::string::npos, Warnings(report).find("already runs another worker"))
      << Warnings(report);
}

// An automatic worker never takes a CPU an explicit worker holds.
TEST_F(AutomaticWorkerTest, AnAutomaticWorkerSkipsTheCpuOfAnExplicitOne) {
  if (Placement().WorkerCpus().size() < 2) {
    GTEST_SKIP() << "needs two CPUs for automatic workers: " << Placement().Summary();
  }
  const std::optional<int> first = Placement().NextWorkerCpu({});
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(control_plane_.AddWorker(0, static_cast<uint64_t>(*first), ""));
  ASSERT_TRUE(control_plane_.AddWorker(1, std::nullopt, ""));
  EXPECT_NE(*first, CoreOf(1));
  const int used[] = {*first};
  EXPECT_EQ(Placement().NextWorkerCpu(used), std::optional<int>(CoreOf(1)));
}

// An explicit core on the reserved control CPU is honoured, and diagnosed.
TEST_F(AutomaticWorkerTest, AnExplicitCoreOnTheControlCpuIsHonouredWithAWarning) {
  if (Placement().control_cpus.empty()) {
    GTEST_SKIP() << "no reserved control CPU: " << Placement().Summary();
  }
  const int control = *Placement().control_cpus.begin();
  auto added = control_plane_.AddWorker(0, static_cast<uint64_t>(control), "");
  ASSERT_TRUE(added) << added.error().message;
  EXPECT_EQ(control, CoreOf(0));
  const bess::runtime::WorkerCpuReport report =
      bess::runtime::runtime().workers().CpuReport(0);
  EXPECT_TRUE(report.on_control_cpu);
  EXPECT_NE(std::string::npos, Warnings(report).find("control CPU")) << Warnings(report);
}

}  // namespace
