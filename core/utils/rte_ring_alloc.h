// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_RTE_RING_ALLOC_H_
#define BESS_UTILS_RTE_RING_ALLOC_H_

// Helpers for `rte_ring`s that live in caller-owned memory (as opposed to
// DPDK memzones via `rte_ring_create`), the pattern `Queue`, `DRR`,
// `LockLessQueue`, and the ring benchmark all share.

#include <rte_ring.h>

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

// DPDK 25.11's rte_ring_elem.h lacks its own `extern "C"` (rte_ring.h
// includes it before opening its block), so rte_ring_get_memsize_elem is
// declared there with C++ linkage and does not link; this declares the C
// function under another name (as handoff.cc does).
extern "C" ssize_t BessRingAllocMemsizeElem(unsigned esize, unsigned count)
    __asm__("rte_ring_get_memsize_elem");

namespace bess {
namespace utils {

// `struct rte_ring` is declared `alignas(RTE_CACHE_LINE_SIZE)`, which is
// 64 on this x86 build but larger on some ARM64 targets -- so the
// alignment must come from `alignof(rte_ring)`, never a hardcoded 64.
// (A hardcoded 64 happens to work here and would be silent UB elsewhere,
// exactly the portability bug class Phase D exists for.)
//
// What AllocRingMem(bytes) takes from glibc: `bytes` rounded up to that
// alignment.
constexpr size_t RingMemBytes(size_t bytes) noexcept {
  constexpr size_t align = alignof(rte_ring);
  return (bytes + align - 1) / align * align;
}

inline void *AllocRingMem(size_t bytes) {
  return std::aligned_alloc(alignof(rte_ring), RingMemBytes(bytes));
}

// The glibc bytes AllocRingMem takes for a ring of `count` slots
// (rte_ring_get_memsize(count), rounded as above), so an owner admits a
// ring before it allocates one (D-198): all of it glibc. 0 for a count DPDK
// refuses.
inline size_t RingBytes(size_t count) noexcept {
  if (count > UINT_MAX) {
    return 0;
  }
  const ssize_t bytes = rte_ring_get_memsize(static_cast<unsigned>(count));
  return bytes < 0 ? 0 : RingMemBytes(static_cast<size_t>(bytes));
}

// The same for a ring of `count` elements of `esize` bytes
// (rte_ring_get_memsize_elem).
inline size_t RingElemBytes(size_t esize, size_t count) noexcept {
  if (esize > UINT_MAX || count > UINT_MAX) {
    return 0;
  }
  const ssize_t bytes = BessRingAllocMemsizeElem(
      static_cast<unsigned>(esize), static_cast<unsigned>(count));
  return bytes < 0 ? 0 : RingMemBytes(static_cast<size_t>(bytes));
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
