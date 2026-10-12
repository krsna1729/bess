// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_STATS_CURRENT_WORKER_H_
#define BESS_STATS_CURRENT_WORKER_H_

#include "utils/logging.h"

#include "dataplane/worker_id.h"
#include "worker.h"

namespace bess::stats {

static_assert(dataplane::kMaxWorkers == Worker::kMaxWorkers,
              "dataplane::kMaxWorkers must match Worker::kMaxWorkers");

// Out of line and cold: an inlined CHECK stream gives every caller a stack
// frame, a stack-protector canary and saved registers on the packet path.
[[noreturn, gnu::cold, gnu::noinline]] inline void CurrentWorkerIdFailed(
    int wid) {
  LOG(FATAL) << "CurrentWorkerId() called off a worker thread (wid " << wid
             << ")";
  __builtin_unreachable();
}

// The calling worker's id, for code that runs on a worker thread and does
// not have `Context::wid` at hand (gate hooks, for instance). Calling it from
// a thread that is not a worker is a bug; every build checks.
inline dataplane::WorkerId CurrentWorkerId() noexcept {
  const int wid = current_worker.wid();
  // Else an out-of-range slot (D-061). Unsigned compare: one branch for both bounds.
  if (static_cast<unsigned>(wid) >= Worker::kMaxWorkers) [[unlikely]] {
    CurrentWorkerIdFailed(wid);
  }
  return dataplane::WorkerId(static_cast<uint16_t>(wid));
}

}  // namespace bess::stats

#endif  // BESS_STATS_CURRENT_WORKER_H_
