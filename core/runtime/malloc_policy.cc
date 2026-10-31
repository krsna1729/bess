// SPDX-License-Identifier: BSD-3-Clause

#include "runtime/malloc_policy.h"

#include <malloc.h>

#include <algorithm>
#include <atomic>

namespace bess::runtime {

namespace {

std::atomic<size_t> released_bytes{0};
std::atomic<uint64_t> trims{0};

void Trim() noexcept {
  malloc_trim(0);
  trims.fetch_add(1, std::memory_order_relaxed);
}

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
  Trim();
  return true;
}

uint64_t malloc_trims() noexcept { return trims.load(std::memory_order_relaxed); }

size_t MallocFreeBound(size_t limit_bytes) noexcept {
  return std::max(kMallocTrimAfterBytes, limit_bytes / 32);
}

bool MallocFreeCheck::Check(std::chrono::steady_clock::time_point now, size_t bound) noexcept {
  if (now < next_) {
    return false;
  }
  next_ = now + period_;
  const size_t free_bytes = mallinfo2().fordblks;
  const uint64_t seen = malloc_trims();
  if (seen != trims_seen_) {  // TrimAfterRelease gave these pages back
    trims_seen_ = seen;
    base_ = free_bytes;
    return false;
  }
  base_ = std::min(base_, free_bytes);
  if (free_bytes - base_ <= bound) {
    return false;
  }
  Trim();
  trims_seen_ = malloc_trims();
  base_ = free_bytes;
  return true;
}

}  // namespace bess::runtime
