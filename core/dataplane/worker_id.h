// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_WORKER_ID_H_
#define BESS_DATAPLANE_WORKER_ID_H_

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

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

// The id of a worker slot from untrusted input (a request's 64-bit wid, an
// int from an old API): nothing outside [0, kMaxWorkers), never truncated.
template <std::integral T>
constexpr std::optional<WorkerId> WorkerIdFrom(T value) {
  if (std::cmp_less(value, 0) || std::cmp_greater_equal(value, kMaxWorkers)) {
    return std::nullopt;
  }
  return WorkerId(static_cast<uint16_t>(value));
}

// Every worker slot id in order: `for (WorkerId wid : AllWorkerIds())`.
struct AllWorkerIds {
  struct iterator {
    uint16_t i;
    constexpr WorkerId operator*() const { return WorkerId(i); }
    constexpr iterator &operator++() {
      ++i;
      return *this;
    }
    constexpr bool operator==(const iterator &) const = default;
  };
  constexpr iterator begin() const { return {0}; }
  constexpr iterator end() const { return {static_cast<uint16_t>(kMaxWorkers)}; }
};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_WORKER_ID_H_
