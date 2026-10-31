// SPDX-License-Identifier: BSD-3-Clause
#ifndef BESS_TESTING_SCALE_PROBE_H_
#define BESS_TESTING_SCALE_PROBE_H_

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <vector>

#include "testing/allocation_faults.h"

// Count-based scaling checks (DSA audit, tools/check_scaling.py): a step's
// cost must follow what it changes, not how much came before it. Built on
// AllocationFaults, so a test using it links testing/allocation_faults.cc;
// DPDK memory is not seen here (count MemoryGrowth::Grow for that).
//
// The shape, for a container grown by one entry per call:
//
//   const auto costs = CostsOfSteps(1000, {99, 999}, [&](size_t i) { Stage(i);
//   }); EXPECT_LE(costs[1].bytes, costs[0].bytes);  // step 1000 copies nothing
//   more
//
// An exact-size reserve per call reallocates the whole container on every
// step, so the late step requests ~10x the early one's bytes; a geometric one
// reallocates only at powers of two, which steps 100 and 1000 avoid.

namespace bess::scale_probe {

struct Cost {
  size_t allocations = 0;
  size_t bytes = 0;
};

// What `op` allocates on this thread.
template <typename Op>
Cost CostOf(Op &&op) {
  fault_injection::AllocationFaults window;
  op();
  return {window.allocations(), window.bytes()};
}

// Runs step(i) for i = 0 .. steps - 1 and returns the cost of each step
// listed in `watched`, in that order.
template <typename Step>
std::vector<Cost> CostsOfSteps(size_t steps,
                               std::initializer_list<size_t> watched,
                               Step &&step) {
  std::vector<Cost> costs(watched.size());
  for (size_t i = 0; i < steps; i++) {
    const auto it = std::find(watched.begin(), watched.end(), i);
    if (it == watched.end()) {
      step(i);
      continue;
    }
    costs[static_cast<size_t>(it - watched.begin())] = CostOf([&] { step(i); });
  }
  return costs;
}

}  // namespace bess::scale_probe

#endif  // BESS_TESTING_SCALE_PROBE_H_
