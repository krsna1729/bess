// SPDX-License-Identifier: BSD-3-Clause

#include "runtime/thread_placement.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "utils/logging.h"

#include "runtime/numa_placement.h"
#include "runtime/opts.h"

#define SYS_CPU_DIR "/sys/devices/system/cpu/cpu%u"
#define CORE_ID_FILE "topology/core_id"

int is_cpu_present(unsigned int core_id) {
  const auto core = bess::runtime::CpuIdFrom(core_id);
  return core.has_value() && bess::runtime::CpuPresent(*core);
}

namespace bess::runtime {

// Present when the kernel exposes the CPU's topology.
bool CpuPresent(CpuId core) {
  char path[PATH_MAX];
  int len = snprintf(path, sizeof(path), SYS_CPU_DIR "/" CORE_ID_FILE,
                     static_cast<unsigned>(core.value()));
  if (len <= 0 || (unsigned)len >= sizeof(path)) {
    return false;
  }
  return access(path, F_OK) == 0;
}

cpu_set_t ControlCpus(const cpu_set_t &base, std::span<const int> cores) {
  cpu_set_t out = base;
  for (int core : cores) {
    if (core >= 0 && core < CPU_SETSIZE) {
      CPU_CLR(core, &out);
    }
  }
  return CPU_COUNT(&out) == 0 ? base : out;
}

namespace {

// Captured during static initialization, before main() and DPDK's EAL.
const cpu_set_t kProcessCpus = [] {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0) {
    CPU_SET(0, &set);
  }
  return set;
}();

std::vector<pid_t> ThreadIds() {
  std::vector<pid_t> tids;
  if (DIR *dir = opendir("/proc/self/task")) {
    while (const dirent *entry = readdir(dir)) {
      if (entry->d_name[0] != '.') {
        tids.push_back(static_cast<pid_t>(std::atoi(entry->d_name)));
      }
    }
    closedir(dir);
  }
  return tids;
}

}  // namespace

const cpu_set_t &ProcessCpus() { return kProcessCpus; }

bool CpuAllowed(CpuId core) {
  return CPU_ISSET(core.value(), &kProcessCpus);
}

std::string CpuList(const cpu_set_t &set) {
  std::string out;
  for (int i = 0; i < CPU_SETSIZE; i++) {
    if (!CPU_ISSET(i, &set)) {
      continue;
    }
    const int start = i;
    while (i + 1 < CPU_SETSIZE && CPU_ISSET(i + 1, &set)) {
      i++;
    }
    out += (out.empty() ? "" : ",") + std::to_string(start);
    if (i > start) {
      out += "-" + std::to_string(i);
    }
  }
  return out;
}

CpuId DefaultWorkerCore(std::span<const int> used) {
  if (FLAGS_c >= 0) {
    return CpuId(static_cast<uint32_t>(FLAGS_c));  // validated when parsed
  }
  // Any worker given no core: the first free CPU of the memory node (D-123).
  if (const std::optional<int> cpu = ProcessPlacement().NextWorkerCpu(used)) {
    return CpuId(static_cast<uint32_t>(*cpu));
  }
  // Nothing free (every CPU reserved or busy): the first CPU of the set,
  // shared, rather than no default worker at all.
  for (int i = 0; i < CPU_SETSIZE; i++) {
    if (CPU_ISSET(i, &kProcessCpus)) {
      LOG(WARNING) << "no free CPU for the default worker: it shares CPU " << i;
      return CpuId(static_cast<uint32_t>(i));
    }
  }
  LOG(FATAL) << "this process may run on no CPU";
  __builtin_unreachable();
}

cpu_set_t ReservedControlCpus() {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int cpu : ProcessPlacement().control_cpus) {
    if (cpu >= 0 && cpu < CPU_SETSIZE) {
      CPU_SET(cpu, &set);
    }
  }
  return set;
}

int PlaceControlThreads(std::span<const WorkerPlacement> workers) {
  const cpu_set_t &base = kProcessCpus;
  std::vector<int> cores;
  std::vector<pid_t> worker_tids;
  for (const WorkerPlacement &w : workers) {
    cores.push_back(w.core);
    worker_tids.push_back(w.tid);
  }
  // The reserved control CPUs when there are any (D-123); else the rest of
  // the set (D-027).
  cpu_set_t control = ReservedControlCpus();
  if (CPU_COUNT(&control) == 0) {
    control = ControlCpus(base, cores);
    if (!workers.empty() && CPU_EQUAL(&control, &base)) {
      static bool warned = false;
      if (!warned) {
        LOG(WARNING) << "every CPU available to bessd hosts a worker: control "
                        "threads share CPUs with packet workers";
        warned = true;
      }
    }
  }
  int moved = 0;
  for (pid_t tid : ThreadIds()) {
    if (std::find(worker_tids.begin(), worker_tids.end(), tid) !=
        worker_tids.end()) {
      continue;
    }
    // A thread that exited meanwhile (ESRCH) is fine to skip.
    if (sched_setaffinity(tid, sizeof(control), &control) == 0) {
      moved++;
    }
  }
  return moved;
}

}  // namespace bess::runtime
