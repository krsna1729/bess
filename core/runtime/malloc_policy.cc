// SPDX-License-Identifier: BSD-3-Clause

#include "runtime/malloc_policy.h"

#include <fcntl.h>
#include <malloc.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>

// Linux 6.18, "prctl: extend PR_SET_THP_DISABLE to optionally exclude
// VM_HUGEPAGE" (David Hildenbrand): older headers lack it.
#ifndef PR_THP_DISABLE_EXCEPT_ADVISED
#define PR_THP_DISABLE_EXCEPT_ADVISED (1 << 1)
#endif

namespace bess::runtime {

namespace {

std::atomic<size_t> released_bytes{0};
std::atomic<uint64_t> trims{0};

void Trim() noexcept {
  (void)malloc_trim(0);  // 1 when it released memory; trims count calls
  trims.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void CapMallocArenas() noexcept { mallopt(M_ARENA_MAX, kMallocArenas); }

std::string_view ThpModeOf(std::string_view enabled) noexcept {
  const size_t open = enabled.find('[');
  const size_t close = enabled.find(']', open);
  if (open == std::string_view::npos || close == std::string_view::npos) {
    return {};
  }
  return enabled.substr(open + 1, close - open - 1);
}

ThpConfinement ConfineThpToAdvised() noexcept {
  char text[128];
  ssize_t got = -1;
  const int fd = open("/sys/kernel/mm/transparent_hugepage/enabled", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    got = read(fd, text, sizeof(text));
    close(fd);
  }
  if (got <= 0 || ThpModeOf({text, static_cast<size_t>(got)}) != "always") {
    return ThpConfinement::kNotNeeded;
  }
  return prctl(PR_SET_THP_DISABLE, 1, PR_THP_DISABLE_EXCEPT_ADVISED, 0, 0) == 0
             ? ThpConfinement::kConfined
             : ThpConfinement::kRefused;
}

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

size_t MallocFreeBound(size_t reserve_bytes) noexcept {
  return reserve_bytes == 0 ? kMallocTrimAfterBytes : reserve_bytes / 4;
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
