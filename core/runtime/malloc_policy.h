// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_MALLOC_POLICY_H_
#define BESS_RUNTIME_MALLOC_POLICY_H_

// glibc's heap in bessd (RT-1). By default glibc gives every thread its own
// arena, up to 8 per CPU, and an arena keeps what was freed in it: control
// threads that each held a transient copy left that much resident per arena,
// unaccounted, until the daemon ran out of its reserve. bessd caps the arenas
// before any thread starts and, after large releases, hands freed pages back.

#include <cstddef>
#include <cstdint>

namespace bess::runtime {

inline constexpr int kMallocArenas = 2;
// Released bytes that make the maintenance loop trim: a large teardown, not
// the churn of a single command.
inline constexpr size_t kMallocTrimAfterBytes = size_t{64} * 1024 * 1024;

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

// How many times TrimAfterRelease trimmed (tests and diagnostics).
uint64_t malloc_trims() noexcept;

}  // namespace bess::runtime

#endif  // BESS_RUNTIME_MALLOC_POLICY_H_
