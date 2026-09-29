// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_RTE_RING_ALLOC_H_
#define BESS_UTILS_RTE_RING_ALLOC_H_

// Helpers for `rte_ring`s that live in caller-owned memory (as opposed to
// DPDK memzones via `rte_ring_create`), the pattern `Queue`, `DRR`,
// `LockLessQueue`, and the ring benchmark all share.

#include <rte_ring.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace bess {
namespace utils {

// `struct rte_ring` is declared `alignas(RTE_CACHE_LINE_SIZE)`, which is
// 64 on this x86 build but larger on some ARM64 targets -- so the
// alignment must come from `alignof(rte_ring)`, never a hardcoded 64.
// (A hardcoded 64 happens to work here and would be silent UB elsewhere,
// exactly the portability bug class Phase D exists for.)
inline void *AllocRingMem(size_t bytes) {
  const size_t align = alignof(rte_ring);
  return std::aligned_alloc(align, (bytes + align - 1) / align * align);
}

// `rte_ring_init()` takes a name; every (re-)created ring gets a unique
// one so resized/per-flow rings can never collide.
inline std::string NewRingName(const char *prefix) {
  static std::atomic<uint64_t> id{0};
  char buf[64];
  snprintf(buf, sizeof(buf), "%s_%lu", prefix,
           static_cast<unsigned long>(id.fetch_add(1)));
  return buf;
}

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_RTE_RING_ALLOC_H_
