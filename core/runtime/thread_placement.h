// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_THREAD_PLACEMENT_H_
#define BESS_RUNTIME_THREAD_PLACEMENT_H_

#include <sched.h>
#include <sys/types.h>

#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "dataplane/strong_id.h"

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

// A Linux CPU number that fits a cpu_set_t: [0, CPU_SETSIZE). Not a
// WorkerId and not a DPDK lcore id (worker.h explains the three). Made from
// untrusted input by CpuIdFrom, which refuses an out-of-range value instead
// of truncating it: a request's 64-bit core 2^32 + 3 must be refused, not
// pinned to CPU 3.
struct CpuIdTag;
using CpuId = dataplane::StrongId<CpuIdTag, uint32_t>;

template <std::integral T>
constexpr std::optional<CpuId> CpuIdFrom(T value) {
  if (std::cmp_less(value, 0) || std::cmp_greater_equal(value, CPU_SETSIZE)) {
    return std::nullopt;
  }
  return CpuId(static_cast<uint32_t>(value));
}

// Whether the kernel exposes CPU `core` (topology present).
bool CpuPresent(CpuId core);
bool CpuAllowed(CpuId core);
// "0-3,8" form, for messages and DPDK's --lcores.
std::string CpuList(const cpu_set_t &set);
// The default worker's CPU: -c if given, else the first CPU of the memory
// node no worker in `used` has and no control thread is reserved (D-123),
// else (nothing free) the first CPU of the set, shared.
CpuId DefaultWorkerCore(std::span<const int> used = {});
// The CPUs reserved for control threads (ProcessPlacement().control_cpus)
// once bessd applied its placement (ApplyProcessPlacement()); empty when
// none is reserved, and in any process that did not (a test or benchmark
// bringing DPDK up lazily keeps every thread on the inherited set).
cpu_set_t ReservedControlCpus();

struct WorkerPlacement {
  pid_t tid;
  int core;
};

// The CPUs non-worker threads may use: `base` minus the workers' cores, or
// all of `base` if that would leave none (a single-CPU deployment).
cpu_set_t ControlCpus(const cpu_set_t &base, std::span<const int> cores);

// Moves every thread of this process that is not one of `workers` to the
// reserved control CPUs (D-123), or, with none reserved, to
// ControlCpus(ProcessCpus(), worker cores); threads created later inherit
// their creator's set. Returns how many threads it moved.
int PlaceControlThreads(std::span<const WorkerPlacement> workers);

}  // namespace bess::runtime

#endif  // BESS_RUNTIME_THREAD_PLACEMENT_H_
