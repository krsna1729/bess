// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_CPU_ID_H_
#define BESS_DATAPLANE_CPU_ID_H_

#include <sched.h>

#include <concepts>
#include <cstdint>
#include <optional>
#include <utility>

#include "dataplane/strong_id.h"

namespace bess::dataplane {

// A Linux CPU number that fits a cpu_set_t: [0, CPU_SETSIZE). Not a
// WorkerId and not a DPDK lcore id (worker.h explains the three). Made from
// untrusted input by CpuIdFrom, which refuses an out-of-range value instead
// of truncating it: a request's 64-bit core 2^32 + 3 must be refused, not
// pinned to CPU 3.
struct CpuIdTag;
using CpuId = StrongId<CpuIdTag, uint32_t>;

template <std::integral T>
constexpr std::optional<CpuId> CpuIdFrom(T value) {
  if (std::cmp_less(value, 0) || std::cmp_greater_equal(value, CPU_SETSIZE)) {
    return std::nullopt;
  }
  return CpuId(static_cast<uint32_t>(value));
}

// A NUMA node, as DPDK names it (a socket id): where a worker's CPU sits and
// its memory comes from. Not a CPU. DPDK's own "any socket" (-1) is not a
// SocketId; an API that allows it takes a std::optional<SocketId>.
struct SocketIdTag;
using SocketId = StrongId<SocketIdTag, uint32_t>;

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_CPU_ID_H_
