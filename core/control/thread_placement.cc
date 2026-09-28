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

#include "control/thread_placement.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

#include <glog/logging.h>

#include "opts.h"

namespace bess::control {

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

bool CpuAllowed(int core) {
  return core >= 0 && core < CPU_SETSIZE && CPU_ISSET(core, &kProcessCpus);
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

int DefaultWorkerCore() {
  if (FLAGS_c >= 0) {
    return FLAGS_c;  // validated against ProcessCpus() when parsed
  }
  if (CpuAllowed(0)) {
    return 0;  // the historical default
  }
  for (int i = 0; i < CPU_SETSIZE; i++) {
    if (CPU_ISSET(i, &kProcessCpus)) {
      return i;
    }
  }
  return FLAGS_c;
}

int PlaceControlThreads(std::span<const WorkerPlacement> workers) {
  const cpu_set_t &base = kProcessCpus;
  std::vector<int> cores;
  std::vector<pid_t> worker_tids;
  for (const WorkerPlacement &w : workers) {
    cores.push_back(w.core);
    worker_tids.push_back(w.tid);
  }
  const cpu_set_t control = ControlCpus(base, cores);
  if (!workers.empty() && CPU_EQUAL(&control, &base)) {
    static bool warned = false;
    if (!warned) {
      LOG(WARNING) << "every CPU available to bessd hosts a worker: control "
                      "threads share CPUs with packet workers";
      warned = true;
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

}  // namespace bess::control
