// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_THREAD_PLACEMENT_H_
#define BESS_RUNTIME_THREAD_PLACEMENT_H_

#include <sched.h>
#include <sys/types.h>

#include <span>
#include <string>

// Whether the kernel exposes CPU `core_id` (topology present). Declared for
// the whole program in worker.h as well; defined here, in the EAL layer, so
// runtime/opts.cc does not depend on the worker.
int is_cpu_present(unsigned int core_id);

namespace bess::runtime {

// Keeps the daemon's own threads -- main, gRPC, DPDK's service threads --
// off the CPUs packet workers are pinned to (Decision D-027). Unpinned, the
// kernel schedules them onto a busy-polling worker's CPU, and every control
// call (a command, a transaction) steals the worker's time: on a live bessd,
// ~8K transactions/s cut a worker's packet rate by 45%; with control
// threads kept off, the rate did not change measurably.

// The CPUs this process was started with -- its inherited affinity, which
// a container runtime (a cgroup cpuset, Kubernetes' CPU manager) or a
// taskset set. Captured before main(), ahead of anything bessd or DPDK
// changes. Everything bessd places -- workers, DPDK's lcores, control
// threads -- stays inside it.
const cpu_set_t &ProcessCpus();
bool CpuAllowed(int core);
// "0-3,8" form, for messages and DPDK's --lcores.
std::string CpuList(const cpu_set_t &set);
// The default worker's CPU: -c if given, else CPU 0 if allowed, else the
// first allowed CPU (a container rarely owns CPU 0).
int DefaultWorkerCore();

struct WorkerPlacement {
  pid_t tid;
  int core;
};

// The CPUs non-worker threads may use: `base` minus the workers' cores, or
// all of `base` if that would leave none (a single-CPU deployment).
cpu_set_t ControlCpus(const cpu_set_t &base, std::span<const int> cores);

// Applies ControlCpus(ProcessCpus(), worker cores) to every thread of this
// process that is not one of `workers`; threads created later inherit their
// creator's set. Returns how many threads it moved.
int PlaceControlThreads(std::span<const WorkerPlacement> workers);

}  // namespace bess::runtime

#endif  // BESS_RUNTIME_THREAD_PLACEMENT_H_
