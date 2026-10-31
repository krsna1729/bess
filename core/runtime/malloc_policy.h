// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_MALLOC_POLICY_H_
#define BESS_RUNTIME_MALLOC_POLICY_H_

// glibc's heap in bessd (RT-1). By default glibc gives every thread its own
// arena, up to 8 per CPU, and an arena keeps what was freed in it: control
// threads that each held a transient copy left that much resident per arena,
// unaccounted, until the daemon ran out of its reserve. bessd caps the arenas
// before any thread starts and, after large releases or once glibc holds a
// bound's worth of new free bytes, hands freed pages back.

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace bess::runtime {

inline constexpr int kMallocArenas = 2;
// Released bytes that make the maintenance loop trim: a large teardown, not
// the churn of a single command.
inline constexpr size_t kMallocTrimAfterBytes = size_t{64} * 1024 * 1024;
// How often the maintenance loop reads glibc's free bytes (MallocFreeCheck).
inline constexpr std::chrono::seconds kMallocFreeCheckPeriod{1};

// mallopt(M_ARENA_MAX, kMallocArenas). An arena glibc made stays, and glibc
// fixes its limit once threads made more than eight, so bessd's main() calls
// it before any thread exists.
void CapMallocArenas() noexcept;

// Admitted bytes given back (framework::MemoryScope's release paths: an
// erase, a shrink, a module's teardown). Any thread; one relaxed atomic add.
void NoteReleasedBytes(size_t bytes) noexcept;

// malloc_trim(0) if at least `threshold` bytes were released since the last
// trim; returns whether it trimmed. The maintenance loop calls it once per
// tick, outside the control-plane lock: a trim walks the arenas' free chunks,
// so it runs only after a teardown of that size, never per command.
bool TrimAfterRelease(size_t threshold = kMallocTrimAfterBytes) noexcept;

// How many times glibc was trimmed, by either trigger (tests and diagnostics).
uint64_t malloc_trims() noexcept;

// The free bytes MallocFreeCheck lets glibc gain before it trims:
// max(kMallocTrimAfterBytes, limit_bytes / 32), `limit_bytes` the memory
// policy's limit (0: none known).
size_t MallocFreeBound(size_t limit_bytes) noexcept;

// The second trigger: glibc's free bytes (mallinfo2().fordblks), which also
// count what never went through admission (a teardown's transient copies).
// mallinfo2 walks every free chunk, so Check reads it at most once per
// `period`, and trims (at most once per read) when the free bytes grew by more
// than `bound` since the last trim. Not the free bytes themselves: a trim
// returns the pages but leaves the chunks free, so those would trim every
// period. Free bytes reused since (resident again) lower the base; a trim by
// TrimAfterRelease rebases it. Control thread only.
class MallocFreeCheck {
 public:
  explicit MallocFreeCheck(std::chrono::steady_clock::duration period) : period_(period) {}

  // Returns whether it trimmed.
  bool Check(std::chrono::steady_clock::time_point now, size_t bound) noexcept;

 private:
  const std::chrono::steady_clock::duration period_;
  std::chrono::steady_clock::time_point next_{};
  size_t base_ = 0;  // free bytes after the last trim, or fewer since
  uint64_t trims_seen_ = 0;
};

}  // namespace bess::runtime

#endif  // BESS_RUNTIME_MALLOC_POLICY_H_
