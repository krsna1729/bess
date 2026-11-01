// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_MALLOC_POLICY_H_
#define BESS_RUNTIME_MALLOC_POLICY_H_

// glibc's heap in bessd (RT-1). By default glibc gives every thread its own
// arena, up to 8 per CPU, and an arena keeps what was freed in it: control
// threads that each held a transient copy left that much resident per arena,
// unaccounted, until the daemon ran out of its reserve. bessd caps the arenas
// before any thread starts and, after large releases or once glibc holds a
// bound's worth of new free bytes, hands freed pages back. Under THP "always"
// it also keeps transparent hugepages to the memory that asks for them (the
// tables' "bess-normal" heap, utils/dpdk_memory.cc), off glibc's arenas.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

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

// The THP mode in the text of /sys/kernel/mm/transparent_hugepage/enabled:
// the bracketed word ("always", "madvise", "never"), or empty.
std::string_view ThpModeOf(std::string_view enabled) noexcept;

enum class ThpConfinement {
  kNotNeeded,  // THP is not "always" here: glibc gets none unasked
  kConfined,   // only advised memory (MADV_HUGEPAGE) gets THP
  kRefused,    // the kernel predates the flag: THP left as it was
};

// Under THP "always", prctl(PR_SET_THP_DISABLE, 1,
// PR_THP_DISABLE_EXCEPT_ADVISED): the process gets THP only where it advises
// MADV_HUGEPAGE, as under "madvise". glibc's arenas then stay on 4 KiB pages:
// a trim that gives back part of a huge page left all of it charged to the
// cgroup until the kernel split it, while the reused part faulted new pages
// beside it, so trimming raised memory.current (live_strict_budget: 6 of 15
// runs over the unattributed bound). Never a full disable, which would take
// THP from the tables too; a kernel without the flag is left alone. Inherited
// by every thread, so bessd's main() calls it before any thread exists.
ThpConfinement ConfineThpToAdvised() noexcept;

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

// The free bytes MallocFreeCheck lets glibc gain before it trims: a quarter of
// `reserve_bytes`, the memory policy's reserve for what is not admitted
// (kMallocTrimAfterBytes when there is none: no limit known). The invariant:
// free bytes glibc retains <= reserve / 4 < reserve / 2, the unattributed
// bound (tests/live/strict_budget_test.cc); the other half of the reserve is
// the margin for a request in flight.
size_t MallocFreeBound(size_t reserve_bytes) noexcept;

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
