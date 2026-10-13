// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_STATS_CURRENT_WORKER_H_
#define BESS_STATS_CURRENT_WORKER_H_

#include "utils/logging.h"

#include "dataplane/worker_id.h"
#include "worker.h"

namespace bess::stats {

// Out of line and cold: an inlined CHECK stream gives every caller a stack
// frame, a stack-protector canary and saved registers on the packet path.
[[noreturn, gnu::cold, gnu::noinline]] inline void CurrentWorkerIdFailed(
    dataplane::WorkerId wid) {
  LOG(FATAL) << "CurrentWorkerId() called off a worker thread (wid "
             << wid.value() << ")";
  __builtin_unreachable();
}

// The calling worker's id, for code that runs on a worker thread and does
// not have `Context::wid` at hand (gate hooks, for instance). Calling it from
// a thread that is not a worker is a bug; every build checks.
inline dataplane::WorkerId CurrentWorkerId() noexcept {
  const dataplane::WorkerId wid = current_worker.wid();
  // Else an out-of-range slot (D-061): a non-worker thread's is UINT16_MAX.
  if (wid.value() >= Worker::kMaxWorkers) [[unlikely]] {
    CurrentWorkerIdFailed(wid);
  }
  return wid;
}

}  // namespace bess::stats

#endif  // BESS_STATS_CURRENT_WORKER_H_
