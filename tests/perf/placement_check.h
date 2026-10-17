// SPDX-License-Identifier: BSD-3-Clause

// Placement diagnostics for the live benches (D-123).
//
// The daemon owns this problem: it resolves placement once
// (bess::runtime::Placement, core/runtime/numa_placement.h) -- it picks the
// memory node, reserves control CPUs, gives an automatic worker a free CPU,
// and describes the result as a WorkerCpuReport (shares_core, cross_numa,
// on_control_cpu, outside_set). GetPipeline returns that description per
// worker (control_v2.WorkerPlacement, "observed, by wid"), so a bench reads
// the daemon's own verdict and never re-derives topology in the client.
//
// Why it matters: a worker that shares a physical core with another worker or
// with a control thread competes for issue slots and caches, and the run then
// reports that contention as a property of the code under test. (Measured
// 2026-10-17: a co-located control thread cost a worker 19-21% of its packet
// rate; separate physical cores cost nothing.) It is a warning, never a
// refusal: under `omarchy-benchmark --isolate` every CPU is already inside the
// run's own set, and an operator who pins siblings on purpose is entitled to
// the numbers plus the caveat.

#ifndef BESS_TESTS_PERF_PLACEMENT_CHECK_H_
#define BESS_TESTS_PERF_PLACEMENT_CHECK_H_

#include <cstdio>
#include <string>
#include <vector>

#include "control_v2.pb.h"

namespace bess::perf {

// Prints the daemon's one-line placement summary (GetCapabilities), which
// names the memory node, the control CPUs and the CPUs automatic workers may
// use. It already carries its own "placement: " prefix.
inline void PrintPlacement(const std::string &summary) {
  if (!summary.empty()) {
    std::fprintf(stderr, "%s\n", summary.c_str());
  }
}

// Warns about every observed worker the daemon placed on a shared physical
// core, on a control CPU, or off its memory node; prints nothing for a clean
// placement. Returns whether any worker was reported. `silence` (from
// --allow-dirty-placement) keeps the verdict without the texts.
inline bool ReportWorkerPlacement(const std::vector<pb::v2::WorkerPlacement> &workers,
                                  bool silence = false) {
  bool reported = false;
  for (const pb::v2::WorkerPlacement &w : workers) {
    if (!w.shares_core() && !w.on_control_cpu() && !w.cross_numa()) {
      continue;
    }
    reported = true;
    if (silence) {
      continue;
    }
    std::fprintf(stderr, "placement: warning: worker %d on core %d", w.wid(), w.core());
    if (w.shares_core()) {
      std::fprintf(stderr, " shares a physical core with another worker or a control thread");
    }
    if (w.on_control_cpu()) {
      std::fprintf(stderr, " is on a CPU reserved for control threads");
    }
    if (w.cross_numa()) {
      std::fprintf(stderr, " is on NUMA node %d, not bessd's memory node", w.numa_node());
    }
    std::fprintf(stderr, ", so these numbers include that contention\n");
  }
  if (reported && !silence) {
    std::fprintf(stderr,
                 "placement: the daemon's line above names the control CPUs; "
                 "--allow-dirty-placement silences this\n");
  }
  return reported;
}

}  // namespace bess::perf

#endif  // BESS_TESTS_PERF_PLACEMENT_CHECK_H_
