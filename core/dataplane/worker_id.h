// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_WORKER_ID_H_
#define BESS_DATAPLANE_WORKER_ID_H_

#include <cstddef>
#include <cstdint>

#include "dataplane/strong_id.h"

namespace bess {
namespace dataplane {

// A BESS worker's stable logical identity (`Worker::wid()`), as a type (K6,
// the first Phase I step for workers).
//
// It is index-like: zero is the first worker, not an invalid value, and every
// value below kMaxWorkers names a worker slot. It is *not* a CPU id or a DPDK
// lcore id; worker.h explains why those three differ.
struct WorkerIdTag;
using WorkerId = StrongId<WorkerIdTag, uint16_t>;

// Must equal Worker::kMaxWorkers; checked where both are visible
// (stats/current_worker.h) so this header stays free of worker.h.
inline constexpr size_t kMaxWorkers = 64;

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_WORKER_ID_H_
