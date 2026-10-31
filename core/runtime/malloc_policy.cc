// SPDX-License-Identifier: BSD-3-Clause

#include "runtime/malloc_policy.h"

#include <malloc.h>

#include <atomic>

namespace bess::runtime {

namespace {

std::atomic<size_t> released_bytes{0};
std::atomic<uint64_t> trims{0};

}  // namespace

void CapMallocArenas() noexcept { mallopt(M_ARENA_MAX, kMallocArenas); }

void NoteReleasedBytes(size_t bytes) noexcept {
  released_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

bool TrimAfterRelease(size_t threshold) noexcept {
  const size_t released = released_bytes.load(std::memory_order_relaxed);
  if (released < threshold) {
    return false;
  }
  // Releases noted meanwhile count toward the next trim.
  released_bytes.fetch_sub(released, std::memory_order_relaxed);
  malloc_trim(0);
  trims.fetch_add(1, std::memory_order_relaxed);
  return true;
}

uint64_t malloc_trims() noexcept { return trims.load(std::memory_order_relaxed); }

}  // namespace bess::runtime
