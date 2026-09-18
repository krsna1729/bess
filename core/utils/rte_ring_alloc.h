// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

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
