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

#ifndef BESS_CONTROL_THREAD_PLACEMENT_H_
#define BESS_CONTROL_THREAD_PLACEMENT_H_

#include <sched.h>
#include <sys/types.h>

#include <span>
#include <string>

namespace bess::control {

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

}  // namespace bess::control

#endif  // BESS_CONTROL_THREAD_PLACEMENT_H_
