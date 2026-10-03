// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_STATS_CURRENT_WORKER_H_
#define BESS_STATS_CURRENT_WORKER_H_

#include "utils/logging.h"

#include "dataplane/worker_id.h"
#include "worker.h"

namespace bess::stats {

static_assert(dataplane::kMaxWorkers == Worker::kMaxWorkers,
              "dataplane::kMaxWorkers must match Worker::kMaxWorkers");

// The calling worker's id, for code that runs on a worker thread and does
// not have `Context::wid` at hand (gate hooks, for instance). Calling it from
// a thread that is not a worker is a bug; every build checks.
inline dataplane::WorkerId CurrentWorkerId() noexcept {
  const int wid = current_worker.wid();
  CHECK(wid >= 0 && wid < Worker::kMaxWorkers)  // else an out-of-range slot (D-061)
      << "CurrentWorkerId() called off a worker thread (wid " << wid << ")";
  return dataplane::WorkerId(static_cast<uint16_t>(wid));
}

}  // namespace bess::stats

#endif  // BESS_STATS_CURRENT_WORKER_H_
