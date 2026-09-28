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

// Thread and CPU placement (Decision D-027): everything bessd places stays
// inside the CPU set it was started with (a container's cpuset, a taskset),
// and its own threads stay off the workers' CPUs. Run it restricted too
// (`taskset -c 2-3 control_thread_placement_test`) to see it as a container
// would.

#include "control/thread_placement.h"

#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <thread>
#include <vector>

#include "control/control_plane.h"
#include "control/runtime_state.h"
#include "opts.h"
#include "packet_pool.h"
#include "port.h"
#include "worker.h"

namespace {

using bess::control::ControlCpus;
using bess::control::CpuAllowed;
using bess::control::CpuList;
using bess::control::ProcessCpus;

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
  // Nothing narrowed this thread yet: the captured set is what we run with.
  EXPECT_TRUE(CPU_EQUAL(&now, &ProcessCpus())) << CpuList(ProcessCpus());
  EXPECT_TRUE(CpuAllowed(bess::control::DefaultWorkerCore()));
}

// A worker on the last allowed CPU: the other threads leave it, threads
// created afterwards inherit that, and destroying the worker gives it back.
TEST(ThreadPlacementTest, ControlThreadsLeaveWorkerCpus) {
  InitRuntimeOnce();
  const cpu_set_t &process = ProcessCpus();
  int core = -1;
  for (int i = 0; i < CPU_SETSIZE; i++) {
    if (CPU_ISSET(i, &process)) {
      core = i;
    }
  }
  ASSERT_GE(core, 0);
  launch_worker(0, core);
  const cpu_set_t main_now = ThisThread();
  cpu_set_t later;
  std::thread([&] { later = ThisThread(); }).join();
  if (CPU_COUNT(&process) > 1) {
    EXPECT_FALSE(CPU_ISSET(core, &main_now)) << "main thread on worker CPU";
    EXPECT_FALSE(CPU_ISSET(core, &later)) << "new thread on worker CPU";
    EXPECT_EQ(CPU_COUNT(&main_now), CPU_COUNT(&process) - 1);
  } else {
    EXPECT_TRUE(CPU_EQUAL(&main_now, &process));  // nowhere else to go
  }
  destroy_worker(0);
  const cpu_set_t after = ThisThread();
  EXPECT_TRUE(CPU_EQUAL(&after, &process)) << "the CPU was not given back";
}

// A worker CPU outside the set is refused with a clear error, not a crash.
TEST(ThreadPlacementTest, WorkersOutsideTheSetAreRefused) {
  InitRuntimeOnce();
  const long present = sysconf(_SC_NPROCESSORS_CONF);
  int outside = -1;
  for (int i = 0; i < present; i++) {
    if (!CpuAllowed(i) && is_cpu_present(static_cast<unsigned>(i))) {
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
  EXPECT_FALSE(is_worker_active(0));
}

}  // namespace
