# BESS Modernization — Progress & Roadmap

This document exists so any agent (or human) picking up this work — in this
session or a fresh one — can get oriented without re-deriving context. Update
it as you go: append to the log, move backlog items to "done", and add new
findings under "known issues" as you hit them. Keep entries factual and dated;
this is a working log, not marketing copy.

All modernization work happens on the **`develop`** branch of
`krsna1729/bess` (a fork of `NetSys/bess`), not `master`. Push commits there.
PRs/merging to `master` is a separate decision the user makes later — don't
propose it unprompted.

## Before you touch anything: sandbox constraints

- **Cap build parallelism at `-j4`.** A `-j20` build previously exhausted
  memory and crashed the whole WSL VM (~7.6GB RAM total). This is a hard
  constraint from the user, not a suggestion.
- **No real hugepages or NIC in this sandbox.** `/proc/meminfo` shows only
  ~256MB of hugepage capacity, often already exhausted. Run `bessd` with
  `-m 0` (no-hugepage mode — already a supported fallback path in
  `core/dpdk.cc`). `bessctl/run_module_tests.py` already does this
  (`daemon start -m 0`); do the same for any manual `bessd` invocation.
- **DPDK build lives in `deps/dpdk-<ver>/`**, gitignored, built via
  `./build.py dpdk` (Meson/Ninja, ~a few minutes). It survives disk restarts
  but not `git clean`. Check `deps/dpdk-*/install/lib/pkgconfig/libdpdk.pc`
  exists before assuming a rebuild is needed.
- **Python deps aren't installed by default** in a fresh shell in this
  sandbox. `pip3 install --break-system-packages --ignore-installed
  typing-extensions -r requirements.txt` gets scapy/flask/grpcio/protobuf;
  regenerate protobuf stubs with `./build.py protobuf` afterward.
- **Commit author identity**: the WSL sandbox auto-set a wrong identity
  (`root@PARAM.localdomain`). Environment-specific -- on the Arch machine this
  session moved to, commits are authored correctly (`Saikrishna Edupuganti
  <6157640+krsna1729@users.noreply.github.com>`, entries 32/33), and `git
  config user.*` is the thing to check rather than assume. Fixing history
  would still need the user's go-ahead, per standing git-safety rules.
- **`all_test` leaves SIGABRT entries in `coredumpctl` by design**: they
  are gtest death-test children -- 7 `EXPECT_DEATH` statements in
  `core/bessd_test.cc` (the `CheckRunningAsRoot.*`, `WritePidFile.*`,
  `ReadPidFile.*`, `TryAcquirePidfileLock.*`, `CheckUniqueInstance.*` cases)
  and 3 in `core/memory_test.cc`. There is no `BessdTest` suite; that wrong
  guess cost this session a phantom hunt. `coredumpctl info <pid>` settles
  it in one line: a death-test child's *command line* carries
  `--gtest_internal_run_death_test=<file>|<line>|...`. How many fire varies
  with which cases skip by environment (~6-7 observed of the 10). A green
  `all_test` summary plus such entries is *not* a crash -- and a deliberate
  abort looks identical to a notification system (this session aborted
  `python3 -c 'import os; os.abort()'` on purpose, as a control), so read
  the command line before concluding anything. `ulimit -c 0` does not
  suppress the *entry* (piped `core_pattern`) but does suppress the core
  itself: `Storage: none` with no size, versus a stored path plus
  `Size on Disk` when real.
- **Building/testing on a rolling-release host** (Arch, g++ 16, glibc 2.42+,
  glog 0.7, protobuf 36, grpc 1.83 — verified 2026-09-19, entry 32) needs
  build-flag workarounds only, no source changes:
  `-DGLOG_USE_GLOG_EXPORT` (glog >= 0.7's headers refuse to compile without
  it and Arch's `libglog.pc` does not supply the define);
  `-include cinttypes` (libstdc++ 16 no longer provides `PRIxPTR`
  transitively, which `core/memory.cc` relies on); `BESS_LINK_DYNAMIC=1`
  (Arch's `grpc++.pc` lists `-labsl_strerror`, absent from Arch's abseil
  package). g++ 16 additionally promotes three pre-existing warning classes
  to errors under the tree's `-Werror` (`Any::PackFrom`/`UnpackTo` are
  `[[nodiscard]]` since protobuf 4, `LOG(FATAL)` is no longer treated as
  noreturn, a few unused variables), so a local build needs `-Wno-error=...`
  for those or `-w`. CI's Ubuntu 24.04 toolchain is unaffected — these are
  local-build notes, not repo bugs. `all_test` also needs gtest *sources*
  (`/usr/src/gtest` ships them on Ubuntu; Arch does not): point `GTEST_DIR`
  at a googletest checkout whose headers match what the test TUs will
  include, or the link fails on `AssertHelper`'s `string_view` ctor.
  `bessctl/run_module_tests.py` needs root (the daemon refuses otherwise) —
  the CI runners have it, an unprivileged session does not.

## How to build and verify (the loop this session used)

```bash
./build.py dpdk              # once; rebuilds only if deps/dpdk-*/install missing
make -C core bessd all_test -j4
cd core && ./all_test --gtest_shuffle   # unit tests
cd .. && python3 bessctl/run_module_tests.py   # live integration tests (needs pip deps + protobuf stubs above)
```

A known-flaky test: `CodelTest.{DropTest,ChangeStateTest}` fail under full
suite load in this sandbox (timing-sensitive, pass cleanly in isolation via
`--gtest_filter=CodelTest.*`). Not a regression from any commit here; don't
chase it.

## Status snapshot

The completed-work log below is the authoritative record of landed
modernization work. For current branch-head build/CI status, consult
GitHub Actions -- this section deliberately records no SHAs, entry
numbers, or "not yet pushed" lists; all three went stale within a commit
or two every time they were written down here. Unpushed work, if any, is
visible in `git status` / `git log origin/develop..develop`, which is
where transient state belongs.

Both GCC and Clang CI jobs are expected to remain green. Each runs
`./build.py bess`, `all_test --gtest_shuffle`, the benchmark smoke loop,
`unittest discover`, and `run_module_tests.py`. Known timing-sensitive
test flakes (not regressions -- do not chase): `CodelTest.*` under full-
suite load, `timestamp.py::test_timestamped_and_measured`'s 1% histogram
self-consistency check under runner load (see entry 30), and
`TcpFlowReconstructTest.*` (seen once on 2026-09-19 under full-suite load:
3 failures, then 3/3 passing with `--gtest_filter` and 185/185 on a rerun,
with a working tree that touches neither the module nor its headers).

**Verified working:** `bessd` builds and links against DPDK 25.11.3 via the
new Meson/pkg-config build; a live `Source -> Sink` pipeline via `bessctl`
processed 7.2B packets with no crash or corruption; `core/all_test` is
185/185 (183 plus 2 new `PacketTest` cases from commit 16;
previously-noted `CodelTest` flakes did not reproduce on the latest
run — timing-sensitive, may still recur under load, not chased further);
`bessctl/run_module_tests.py` passes cleanly with **no known failures** —
the `url_filter.py` mismatch previously attributed to scapy version drift
was actually a real checksum bug (see commit 9 below), now fixed.

## Completed work (chronological, with commit hashes on `develop`)

1. **`a077810d`** — CI baseline: GitHub Actions replacing Travis (Bionic/DPDK
   19.11 container), Ubuntu 24.04 runner, Python 3 only. Added `!.github/`
   exception to `.gitignore` (its blanket `.*` rule was silently eating any
   workflow file — likely why this repo never had Actions before).
2. **`7feced65`** — Dropped remaining Python 2 `__future__` imports repo-wide;
   pinned `requirements.txt` floors (was fully unpinned); validated the
   pinned versions by actually generating C++/Python stubs from the real
   `.proto` files against them.
3. **`6caaee44`** — **The big one**: ported DPDK 19.11.4 → 25.11.3 LTS.
   - `build.py`/`core/Makefile`: DPDK now built via Meson/Ninja (the legacy
     Make build never produced a pkg-config file at all — this is *why*
     `core/Makefile` used to hardcode `RTE_SDK`/`RTE_TARGET` source-tree
     paths instead of using pkg-config like its other deps).
   - `core/packet.h`: **found and fixed a real, previously-undetected
     ABI-drift bug.** `Packet` overlays itself on `struct rte_mbuf` via a
     hand-written union mirroring DPDK's field layout (for zero-copy
     performance). Verified via a compiled `offsetof()` program (not just
     reading headers) that DPDK 25.11 moved `rte_mbuf::pool` from byte
     offset 72→56, `::next` from 80→64, and removed the old fixed
     `timestamp`/`userdata`/`seqn` fields entirely (replaced by a
     36-byte `dynfield1[]` reserved area). None of this tripped the
     existing `static_assert`s (they only checked total size), so it would
     have **silently corrupted `pool`/`next`** — exactly the failure class
     upstream `NetSys/bess#1050` described for DPDK 20.11+, now reproduced
     with concrete numbers for 25.11. Fixed the layout and added
     `offsetof`-based `static_assert`s (in a never-called member function,
     so they run in "complete-class context") pinning every field this
     union depends on, so a future DPDK bump can't silently break it again.
   - `core/drivers/pmd.cc`: `struct rte_bus`/`struct rte_pci_device` are
     opaque in the public API now — ported to `rte_bus_name()`/
     `rte_dev_name()` accessors. `rte_eth_stats`'s per-queue fields
     (`q_ipackets` etc.) were removed upstream; **not** reimplemented (see
     backlog below) — `queue_stats[][]` just stays zero now, same as it
     already did for the driver exclusion list this replaced.
   - `core/dpdk.cc`: three renamed EAL CLI flags
     (`--master-lcore`→`--main-lcore`, `--lcore`→`--lcores`,
     `--iova`→`--iova-mode`). This was the actual reason `all_test` was
     silently crashing before this fix.
   - Deleted two DPDK-19.11-specific patch files (`deps/*.patch`) whose
     target paths (`lib/librte_*`) don't exist post-DPDK-21.11's directory
     rename (`lib/librte_ethdev` → `lib/ethdev` etc).
4. **`9e8c4af1`** — Fixes from an **Opus 5 review** of commit 3 (see
   "Review process" below): a real regression in PCI-address port lookup
   (wrong sprintf format vs DPDK's actual `PCI_PRI_FMT`), a real build bug
   (`-no-pie` leaking into the shared-object/plugin link rule, breaking any
   plugin build), plus robustness fixes (pkg-config `-march` override,
   Debian-specific `-lcares` substitution made conditional, `make clean`
   requiring pkg-config unnecessarily, CI workflow never actually updated
   for the new build).
5. **`bd4ba860`** — Python 3.12 compatibility bugs, found by *actually
   running* the ported `bessd` live (not just unit tests): `google.protobuf`
   dropped `FieldDescriptor.label` (upb-backed implementation since 4.21) —
   every single `create_module` RPC crashed with `AttributeError` until
   fixed. `unittest.assertEquals` was removed in Python 3.12 — broke
   ~19 module test files.
6. **`49f1ec86`** — A **real, reproducible crash race** in `core/worker.cc`
   (pure C++ thread/TLS lifecycle code, zero DPDK involvement — found
   because running the full module-test suite live against a real daemon
   restarts workers rapidly, which stress-tests this path much harder than
   unit tests do). `launch_worker()` detached its OS thread; `destroy_worker()`
   only waited for a status flag that flips *before* the thread actually
   finishes and exits. Fixed by joining instead of detaching, plus made
   `run_worker()` defensively re-zero `current_worker` instead of
   `CHECK`-crashing the daemon if it ever isn't pristine. **This commit's
   own stated root cause (glibc TLS-block reuse) turned out to be wrong,
   and it introduced a critical regression — see commit 7, which an Opus
   review of this commit caught.** Left in the log as-is (don't rewrite
   history) since 7 supersedes/corrects it; read them together.
7. **`cfa82e3b`** — Opus review of commit 6 found: (a) **critical**: normal
   `bessctl daemon stop` now aborted `bessd` with `std::terminate()` — 6
   removed `.detach()` without accounting for `KillBess()` (bessctl.cc)
   resuming workers before an async shutdown, so `main()` returned with
   workers still running and their thread handles still joinable, and
   `worker_threads[]` (a namespace-scope global) calls `~std::thread()` on
   exit. **Confirmed by direct reproduction before and after the fix**
   (start `bessd`, build an active pipeline, `daemon stop` — crashed
   before, 3/3 clean after). Fixed with a new
   `detach_all_worker_threads()`, called once at shutdown. **(a)'s
   explanation of *why* workers were still alive turned out to be
   incomplete/wrong too — see 8.** (b) 6's root cause was wrong: the
   review traced glibc's actual TLS init and showed recycled TLS is
   zeroed synchronously inside `pthread_create()`, before the new thread
   runs — "stale TLS reuse" was never possible. The `join()` in 6 is
   still correct, but for a different, more serious reason: it
   serializes worker teardown (`~Scheduler` → `~TrafficClass` →
   `TrafficClassBuilder::Clear()`) against the **global, unsynchronized**
   `std::unordered_map all_tcs_` (`core/traffic_class.cc`, confirmed no
   locking exists around it anywhere) — without the join, concurrent
   mutation of that map from two teardown paths is a real heap-corruption
   race. Comments rewritten to reflect this. (c) Added a defensive `CHECK`
   in `launch_worker()` against reusing a still-joinable slot (the same
   `std::terminate()` failure mode as (a), on the move-assignment).
   (d) Upgraded the `run_worker()` recovery log from `WARNING` to `ERROR`
   with actual diagnostic values, since per (b) a non-pristine
   `current_worker` more likely indicates real corruption than benign
   timing. **Lesson for future work on this file**: verify claims about
   *why* a fix works independently of whether the fix itself is correct —
   6's fix direction was right, its explanation wasn't, and that
   explanation being wrong is exactly what let the shutdown regression
   through unnoticed.
8. **`ecbdd3bc`** — A *second* Opus review, of commit 7 specifically,
   verdict: **"correct and sufficient", no correctness defect found**
   (this review also independently re-audited every process-exit path —
   `exit()` call sites, `LOG(FATAL)`/`CHECK` → `GoPanic` → `_exit`/`abort`,
   signal handling via `SetTrapHandler` (only `SIGSEGV/BUS/ILL/FPE/
   ABRT/USR1`, not `SIGTERM`/`SIGINT`) — and confirmed `main()`'s
   `return 0` really is the only route to static destruction, so
   `detach_all_worker_threads()`'s placement is sufficient, not just
   lucky). Two things it did flag, both fixed in this commit: 7's own
   comments (and its commit message, and entry 7 above) claimed workers
   are alive at shutdown *because* `KillBess()`'s `WorkerPauser`
   destructor resumes them — true only for a bare `kill()` RPC. The
   actual `daemon stop` path (`bessctl/commands.py` `_do_stop`) calls
   `pause_all()` *before* `kill()`, so every worker is already
   `WORKER_PAUSED` by the time `KillBess()` runs, and `WorkerPauser`'s
   constructor only records workers it finds `WORKER_RUNNING` — nothing
   for its destructor to resume in this path. The real, simpler reason:
   nothing on the shutdown path ever joined or detached workers at all,
   regardless of paused/running state. Comments corrected; the fix
   itself was never affected by this (`detach_all_worker_threads()`
   handles both cases identically). Also added an explicit `<cstring>`
   include `worker.cc` was missing (used `memcmp`/`memset` via a
   transitive include only). **Flagged, not fixed** (see backlog): in the
   bare-`kill()`-without-pause-first case, `detach_all_worker_threads()`
   abandons workers that are still actively scheduling, which then keep
   running while the globals they touch are being torn down by static
   destruction — this exactly matches pre-`49f1ec86` behavior
   (detach-at-launch), so it's not a regression, but a stronger fix would
   `destroy_all_workers()` before the detach loop, trading "shutdown
   always completes promptly" for "a wedged worker can hang shutdown".
9. **`f79c20c8`** — Found and fixed the real root cause behind
   `url_filter.py`'s IP checksum test failure, previously (wrongly)
   written off in this doc as scapy-version drift. Every checksum inline
   x86 `asm()` block in `core/utils/checksum.h` (10 total) was missing
   `volatile` and a `"memory"` clobber. Without both, GCC is free to
   reorder the asm's memory reads across a caller's writes to the same
   buffer made through a *different* pointer type, once inlined.
   Confirmed by direct reproduction: at `-O3`,
   `CalculateIpv4NoOptChecksum()` inlined into `url_filter.cc`'s
   `Generate403Packet()` read `ip.length` with its stale template-default
   value (`0x0028`) instead of the just-assigned real value (`0x005a`),
   hoisted ahead of the `ip->length = ...` write — verified by hand-computing
   the checksum for that exact "stale length" hypothesis and matching it
   exactly against BESS's actual wrong output (`0xb023` vs. correct
   `0xaff1`). `-fno-strict-aliasing` was tried first and *appeared* not to
   fix it — **this test turned out to be invalid, see commit 10.** Fixed
   all 10 blocks with `asm volatile(... : "memory")`. Also found a subtler
   variant of the same bug specific to `Verify`/`CalculateIpv4NoOptChecksum`:
   both had a plain `uint32_t sum = buf32[0];` C statement *preceding* the
   asm block — a "memory" clobber only fences things around the asm
   statement itself, so it does nothing to stop an earlier plain load from
   being hoisted even further up. Fixed by folding that read into the
   asm's own memory operand list instead. Verified: `core/all_test`
   181/181, `run_module_tests.py` clean including `test_urlfilter`, live
   `bessd` rebuilt and re-run under `-m 0` with no regressions.
10. **`667c48a8`** — An **Opus review of commit 9** found it was
    incomplete: 4 of the 10 asm blocks use a `"g"` (general) operand for a
    masked/shifted read — e.g. `[u2] "g"(buf32[2] & 0xFFFF)` in
    `CalculateIpv4NoOptChecksum`/`CalculateIpv4Checksum`, the equivalent
    in `CalculateIpv4UdpChecksum`, and `[u4] "g"(buf32[4] >> 16)` in
    `CalculateIpv4TcpChecksum`. A `"g"` operand's expression is evaluated
    as an ordinary C load *before* the asm executes — it is not an asm
    memory reference at all, so 9's `"memory"` clobber does nothing to
    protect it. **The review reproduced a live miscompile from this**: a
    real copy-template → set-length → checksum shape (matching
    `flowgen.cc`/`l4_checksum.cc`'s UDP path) at `-O3` store-forwards the
    stale template's `udp->length` into the `"g"` operand instead of the
    just-written value. It doesn't reproduce in this repo's own
    `ChecksumTest.UdpChecksum` — coincidental code shape around it happens
    to prevent the reorder there — which is exactly why it went
    undetected: working by luck, not by a closed hazard. The review also
    determined commit 9's `-fno-strict-aliasing` claim was backwards:
    that flag **does** eliminate this whole class; the original in-session
    test of it was almost certainly invalid (Make doesn't recompile a
    `.o` on a Makefile-only flag change — the same gotcha noted earlier
    in this doc's "how to build" section). Fixed by introducing
    `may_alias`-attributed pointer typedefs (`aliasing_uint16_t/32_t/64_t`)
    and using them for every `reinterpret_cast` in the file that type-puns
    into a packed header or raw byte buffer — this closes the hazard at
    the pointer-type level for *every* read through these pointers
    (asm operand or plain C expression alike), rather than depending on
    each future edit picking the right asm-operand constraint. Same
    technique the file already used for the AVX2 path's 16-bit union.
    Makes 9's `volatile`/`"memory"` additions belt-and-suspenders rather
    than load-bearing. Verified: `core/all_test` 181/181,
    `run_module_tests.py` clean, live `bessd` rebuilt with no regressions.
11. **`ddae0181`** — A second Opus review, this time of commit 10
    specifically, verdict: **"correct and sufficient" for 10's own stated
    purpose** (independently reproduced the `may_alias` fix working via
    disassembly at `-O0` through `-O3`, and reproduced the pre-fix `"g"`-
    operand miscompile through the real header to confirm it was real).
    It also found one more **separate, pre-existing** defect in the same
    file, not caused by 9 or 10: 6 asm blocks declare their running sum as
    `[sum] "+r"(sum)` (no earlyclobber) while also reading other values
    into `"r"` input operands (src/dst/len for the UDP/TCP pseudo-header,
    or a masked/shifted header word) later in the same block. Without `&`,
    GCC may allocate an `"r"` input the same register as the `"+r"` output
    whenever it can prove they're equal at asm entry — e.g. a TCP checksum
    for a packet whose `CalculateSum()` result is 0 and `src == dst ==
    0.0.0.0` coalesces `sum` with `src`/`dst`, so `"adcl %[src], %[sum]"`
    ends up adding the register to itself instead of the real value.
    **Reproduced live**: g++ 13.3 `-O3` gave `0x0077` instead of the
    correct `0xac9d` (matched by `-O0` and by clang++ at any level) for
    exactly that packet shape. Predates both 9 and 10 — a register-
    allocation constraint gap, unrelated to the strict-aliasing issues
    those fixed. `f79c20c8` had already used the correct `"=&r"` for the
    two NoOpt functions, so the pattern was half-applied; this commit
    makes it consistent across `Verify`/`CalculateIpv4Checksum` and
    `Verify`/`CalculateIpv4UdpChecksum`/`TcpChecksum`. `CalculateSum`'s
    64-bit-loop blocks are unaffected (no other register-allocated input
    operands to coalesce with). Verified: `core/all_test` 181/181,
    `run_module_tests.py` clean, live `bessd` rebuilt with no regressions.
12. **`253b3832`** — A **third** Opus review, of commit 11 (`ddae0181`)
    specifically, verdict: **"correct and sufficient", no defects** —
    independently reproduced both the miscompile (`0x7e97`/verify-fails
    without the fix vs. `0xcc25`/verify-passes with it, for the exact
    src==dst==0/no-payload TCP packet) and confirmed `"+&r"` is GCC's
    documented idiom for this pattern (its own Extended Asm docs use this
    exact spelling), with byte-identical codegen against the alternative
    `"=&r"` + `"0"`-matching-input idiom. It flagged two low-risk hardening
    gaps, not live bugs: (a) `CalculateSum`'s two 64-bit-loop blocks still
    used plain `"+r"` for their `sum64` accumulator — never a live bug
    there (no other register-allocated input operand exists to coalesce
    with), but an inconsistency with the reasoning just applied to 6 other
    blocks in the same file; (b) no test in this file's history would
    have caught 11's bug class — the existing randomized TCP/UDP tests
    always use non-zero random src/dst, so they never construct the
    `src == dst == sum_in == 0` condition GCC needs. This commit closes
    both: made `"+&r"` uniform across every running-sum accumulator in the
    file (zero codegen change, confirmed), and added
    `ChecksumTest.TcpChecksumZeroAddressNoPayload`/
    `UdpChecksumZeroAddressNoPayload`, pinning exactly that packet shape
    against DPDK's independent `rte_ipv4_udptcp_cksum()` oracle. Verified
    the new tests actually catch the regression by reverting just the
    `"+&r"` change and rebuilding: the UDP test failed exactly as
    predicted (`cksum_dpdk=57087` vs `cksum_bess=50943`,
    `VerifyIpv4UdpChecksum()` returned `false`); the TCP variant didn't
    trigger in this specific build (register allocation for this exact
    coalescing is sensitive to surrounding code and compiler version, per
    the review) but is kept as still-valid coverage. This is hardening
    after a review found the underlying fix already correct, not a new
    defect — no further review round needed for this commit specifically.
    Verified: `core/all_test` 183/183 (181 + 2 new), `run_module_tests.py`
    clean, live `bessd` rebuilt with no regressions. **This concludes the
    checksum.h correction chain (commits 9-12) — ready to push.**

**Running theme across commits 9-12**: this file needed three correction
rounds before an Opus review returned a clean verdict, each round finding
something the previous round's fix (and its own test suite) missed —
reordering (9), then a strict-aliasing hazard the first fix didn't fully
close (10), then a register-allocation constraint gap unrelated to either
(11), closed with hardening + regression tests once nothing more turned
up (12). The lesson isn't "stop trusting reviews" — it's the opposite:
keep re-reviewing narrowly-scoped low-level fixes even after they look
done and tests pass, because the existing unit tests in this file weren't
written to catch any of these three bug classes (they don't trigger the
exact register/memory conditions needed, only by chance). All three now
have targeted regression coverage (9/10's `Ipv4NoOptChecksum`/`url_filter`
tests already covered the reordering case incidentally by exercising the
inlined path; 11/12 added the earlyclobber-specific tests above; 10's
"g"-operand hazard is covered structurally by the `may_alias` fix rather
than a specific test, since the fix closes the pointer-type-level hazard
rather than one call site).

13. **`49a6fdec`** — Pushing commits 1-12 finally triggered a real CI run
    of the `build (clang++)` job (the g++ job had been passing, but
    clang++ had apparently never actually been exercised end-to-end since
    it was added in the original CI baseline). It failed at multiple
    successive stages; fixed each by actually building locally with
    `CXX=clang++` rather than guessing from the error text alone — 9 real,
    independent portability bugs, none related to the checksum work:
    (a) `core/Makefile`'s g++/clang detection matched only the first word
    of `$(CXX) --version`, which is "Ubuntu" (not "clang") for Ubuntu's
    packaged clang — silently broke compiler detection, causing two
    `expr: syntax error` messages downstream; rewrote via `$(findstring)`
    across the whole version string. (b) glog's `logging_fail_func_t`
    spells noreturn via `__attribute__((noreturn))`; this repo's
    `exit_failure`/`abort_failure`/`GoPanic` use `[[noreturn]]` instead —
    GCC implicitly converts between the spellings for this function-
    pointer assignment, Clang does not (and rejects `static_cast` between
    them too, since they're "unrelated" types to it); fixed with
    `reinterpret_cast` at all 3 call sites (`debug.cc` x2, `main.cc`).
    (c) `Packet::Dump()`'s `dump_len` was written but never read — dead
    debug-output code, only Clang's `-Wunused-but-set-variable` caught
    it; removed. (d) `BigEndian<T>` explicitly defaults its copy
    constructor but never declares copy-assignment — deprecated since
    C++11 (rule of three), only Clang's `-Wdeprecated-copy` caught it;
    added the assignment operator. (e) A `std::move()` around an
    already-rvalue temporary in `histogram.h`, blocking elision
    (`-Wpessimizing-move`); removed. (f)/(g) 7 occurrences across
    `codel_test.cc`/`llqueue_test.cc` of `int* vals[n]` with non-const
    `n` — a GNU/Clang VLA extension, not standard C++
    (`-Wvla-cxx-extension`); `n` was always literal-initialized and never
    reassigned in every case, so `constexpr int n` fixes all 7 with zero
    behavior change. One genuine runtime-sized VLA in `fifo_test.cc`
    (`char buf[maxlen]`, `maxlen` a function parameter) needed an actual
    fix, not just `constexpr` — converted to `std::vector<char>`.
    (h) The static-link branch's `PKG_LIBS` pulls `-lm` inside the
    `-Wl,-non_shared ... -Wl,-call_shared` static bracket; glibc >= 2.34
    merged libm's real code into `libc.so`, leaving the standalone
    `libm.a` containing ifunc resolvers that reference glibc-internal
    symbols (`_dl_x86_cpu_features`) not exported for use outside
    glibc's own build — statically linking it fails at link time. Never
    reproduced with g++ (its codegen for this object set apparently never
    references `log`/`log2`/`pow` from libm at all), but clang++'s did,
    exposing the archive as genuinely unlinkable. Fixed the same way this
    file already handles `-lpthread`/`-ldl`: added `-lm` to
    `ALWAYS_DYN_LIBS`. (i) A second GCC-only version check (gating
    `-Wno-error=address-of-packed-member`, needed only because GCC >= 9
    added that warning to `-Wall` — Clang doesn't enable it there and
    doesn't error on the same casts without the flag) ran `test
    $(CXXVERSION) -ge 9` unconditionally; Clang's `-dumpversion` returns
    a dotted version ("18.1.3"), which `test -ge` rejects with "Illegal
    number" — harmless (the flag was already correctly skipped for
    Clang, just via a broken check) but noisy; gated on
    `$(CXXCOMPILER)` being `g++` using fix (a)'s now-reliable detection.
    Verified: clean `make clean && CXX=clang++ make bessd modules
    all_test -j4` AND the same with plain (g++) `make`, from the same
    tree, zero errors/warnings-as-noise in either; `core/all_test`
    183/183 under both binaries; `run_module_tests.py` clean under the
    g++ binary. **This is CI/build infrastructure work, not correctness-
    critical dataplane logic — no Opus review requested for this one**
    (each fix is either a mechanical portability shim with an obvious,
    narrow correct answer, or verified directly by a clean build +
    full test pass under both compilers from a clean tree, which is
    the strongest form of verification available for "does the code
    still do the same thing" questions like these).
14. **`e8545843`** — Built and ran the benchmark suite. Correcting this
    doc's own earlier, wrong claim ("no benchmark suite exists yet"):
    BESS already had one (`utils/checksum_bench.cc`, `copy_bench.cc`,
    `cuckoo_map_bench.cc`, `modules/url_filter_bench.cc`,
    `traffic_class_bench.cc`), and `core/Makefile` already fully supports
    it (`make benchmarks`) — none of it needed building from scratch.
    The actual gap: `build.py`'s `build_bess()` (what CI calls) never ran
    `make ... benchmarks`, so this whole pre-existing suite had never
    been built or run against the DPDK 25.11 port this entire session,
    despite the port touching code some of it depends on. Verified all 5
    by actually running each (not just compiling) — all pass;
    `cuckoo_map_bench` looked hung under a 30s timeout at first (false
    alarm — 22 cases up to 4M entries just need more wall time, confirmed
    fine with `--benchmark_min_time=0.001s`). Added `core/packet_bench.cc`
    (the one genuinely new file): `Packet`/`PacketPool`/`PacketBatch`
    benchmarks using `PlainPacketPool` (the no-hugepage-required backend,
    "for standalone benchmarks and unittests" per its own doc comment) —
    exactly the primitives the proposed Phase B refactor would touch.
    Fixed the actual gap: added `benchmarks` to `build.py`'s
    `build_bess()`, added a CI "Smoke-test benchmarks" step (crash/hang
    check via tiny `--benchmark_min_time`, not perf tracking — runner
    variance makes that unreliable in CI). See Phase B's "Benchmark
    suite" section below for the full writeup, including what's still
    not covered (PMD-level and Module/Gate/Task-dispatch benchmarks).
15. **`68d2b677`** — Pushing commit 14 (and the doc commit after it)
    exposed a real, independent CI reliability bug: two runs in a row
    (both doc-only commits — could not have been a real regression)
    crashed with "Illegal instruction (core dumped)" a fraction of a
    second into `all_test`, clang++ job only. Root cause: DPDK's meson
    build defaults to `-march=native`, and the "Cache DPDK build" step
    persists that compiled DPDK across CI runs — but different runs
    aren't guaranteed the same actual GitHub-hosted runner hardware, so
    a `native` build compiled for one run's CPU can contain instructions
    a later run's CPU lacks. Fixed with `CPU=corei7` (a workflow env var;
    already supported end-to-end with zero code changes — `build.py`
    already forwards `CPU` to DPDK's `-Dmachine=`, `core/Makefile`'s
    `CPU ?= native` already respects an inherited environment value).
    `corei7` is DPDK's own documented portable baseline (what
    `-Dmachine=generic` resolves to on x86). Also fixed the cache key
    (didn't depend on `CPU` at all, so today's fix would've been
    silently ineffective — the old `native`-built cache would just keep
    getting served under the same key). Verified locally end-to-end
    before pushing: built DPDK fresh with `-Dmachine=corei7` in a
    separate tree, built BESS against it, ran the full test suite
    (183/183) and every benchmark — no regressions, just the existing
    `#if __AVX2__` fallback paths taken at compile time.
16. **`dea288f9`** — **Phase B, Stage 1**. Ran two research
    passes into `PacketPool`/DPDK's `priv_size` mechanism and the codebase
    blast radius of Phase B's original "thin `rte_mbuf*` wrapper" text
    before writing code, per standing practice (plan mode + explicit
    sign-off for anything this size/risk). Found a concrete reason the full
    end-state can't land as one shot — see Phase B's own section below —
    and scoped a smaller, safe, real first slice instead: gave `Packet`'s
    private/metadata area an explicit `BessPacketPrivate` struct reached via
    `rte_mbuf_to_priv()` (zero layout/ABI change, pinned by a new
    `CheckPrivLayout()` static_assert), fixed `wildcard_match.cc`'s
    `mt_offset_to_databuf_offset()`-based raw-offset trick (the one latent
    hazard this surfaced — same "offset math assumed correct by
    construction" bug class as Phase A's two ABI-drift bugs), and deduped
    `CheckMbufLayout()` vs. the redundant (and dead — never called)
    `check_offset` macro in `Packet::CheckSanity()`. Added
    `core/packet_test.cc` (new — `Packet` had no gtest coverage before) and
    a `BM_PacketMetadataAccess` benchmark. Verified: `core/all_test`
    185/185, `run_module_tests.py` clean including
    `test_wildcardmatch_with_metadata`, live `bessd -m 0` smoke-started
    cleanly (262144-packet `PlainPacketPool` created via the new `priv()`
    path with no crash).
17. **`f22a69eb`** — Opus review of `dea288f9` verdict: **correct and
    sufficient for its stated purpose**, one real defect found plus three
    hardening gaps, all fixed here. Real defect: the new
    `PacketTest.MultiSegmentChaining` double-freed `seg1` --
    `rte_pktmbuf_free()` walks the `next_` chain, so a separate
    `Free(seg1)` before `Free(seg0)` (which still has `next_ == seg1`)
    returns `seg1` to the mempool twice (`avail_after` = 17 in a
    16-capacity pool, reproduced with a temporary probe test before
    fixing). Fixed by freeing `seg0` alone. Hardening: made `priv()`
    private (it was accidentally public, making `BessPacketPrivate`'s
    `vaddr_`/`paddr_`/`sid_`/`index_` directly writable from outside
    `Packet` in a way the old union members never were -- nothing outside
    `Packet` actually called it); added `CheckPrivLayout()` asserts pinning
    `BessPacketPrivate::metadata_`/`scratchpad_`'s offsets against
    `SNBUF_METADATA_OFF`/`SNBUF_SCRATCHPAD_OFF` (the size-only assert from
    `dea288f9` didn't catch an internal-offset drift that keeps `sizeof`
    correct -- matters because `core/kmod/sn_common.h`'s vport code
    addresses the scratchpad via a hardcoded `SNBUF_SCRATCHPAD_OFF`, a
    cross-language ABI contract nothing was checking either side of); and
    corrected `BessPacketPrivate`'s doc comment (`priv()`'s correctness
    comes from `rte_mbuf_to_priv()` being a compile-time constant offset,
    not from `PostPopulate()`'s runtime `mbuf_priv_size` configuration, as
    `dea288f9`'s comment wrongly implied). Verified: `core/all_test`
    185/185, `run_module_tests.py` clean. **Performance quantification**
    (done for both `dea288f9` and this commit together, since neither
    changes the accessor's compiled code): built the pre-Stage-1 tree
    (`3fcf8d4d`) in a throwaway git worktree sharing this session's
    existing DPDK install, so both binaries link against identical DPDK
    bits. `packet_bench`'s 5 pre-existing benchmarks showed no
    change beyond noise; the new `BM_PacketMetadataAccess` showed a
    within-noise 0.087ns-vs-0.108ns difference at 5 reps/1s min-time --
    too small to trust directly (cv ~12%), so settled it by diffing
    normalized objdump output for the benchmark's compiled function
    byte-for-byte identical apart from two unrelated relocation offsets
    elsewhere in the object file. Confirms `rte_mbuf_to_priv()`'s pointer
    arithmetic constant-folds to the exact same single fixed-offset
    computation the old union-member access already compiled to --
    **zero real cost**, not the noisy delta the timing numbers suggested.
    `wildcard_match.cc`'s `ProcessBatch()` likewise compiles to the same
    instruction count before/after (1828 vs. 1829 disassembly lines).
    Stage 2 (the actual thin wrapper) is explicitly deferred, not scoped,
    needs its own sign-off; see Phase B below.
18. **`640774cf`** — User asked to survey current C++ language/compiler-
    ecosystem work (the kind of thing Lemire/Sutter/Godbolt et al. publish
    about) for hardening/compile-time-bug-elimination ideas, fold anything
    worthwhile into this doc, and defer actual adoption since Phase H/I
    already exist for exactly that purpose. Research-only, no code: added
    a "Compiler/language-ecosystem hardening research" subsection to Phase
    H covering `-fhardened` (GCC's one-flag hardening bundle -- actionable
    for `bessctl`/control-plane now, but `_GLIBCXX_ASSERTIONS`'s reported
    ~6% overhead on some libstdc++ versions needs benchmarking against the
    dataplane specifically before enabling there), C++26 Standard Library
    Hardening (P3471, standardizes what `_GLIBCXX_ASSERTIONS` already
    does), GCC `-fanalyzer` (evaluated, not a good fit yet -- weak
    C++/template support), and confirmation that C++26 reflection/
    contracts/`std::simd` remain genuinely experimental (GCC's own docs:
    "not recommended for production use") -- validating this doc's
    existing caution rather than changing it. Also flagged the C++29
    "Profiles" effort (P3589/P3984, Stroustrup/Dos Reis --
    `[[profiles::enforce(...)]]`) as the real successor to the failed
    "Safe C++" proposal and directly relevant to Phase I's goals, though
    not shippable yet. Pushed right after commits 16-17; both pushes
    verified green on CI (runs 35255310803 and 35255689088 respectively).
19. **`f2f3bf84`** — **Phase H first concrete step**: bumped
    the whole build from `-std=c++17` to `-std=c++23` (`core/Makefile`) --
    the actual prerequisite for literally every item on Phase H's "adopt
    now" list, which this doc hadn't previously verified was even possible
    (the build had never been bumped past C++17 despite Phase H being
    written against a C++23 baseline). Confirmed GCC 13.3 and Clang 18.1.3
    (both installed here, matching CI's Ubuntu 24.04 packages) accept
    `-std=c++23`; **neither accepts `-std=c++26`** -- GCC 13.3 rejects the
    flag outright as unrecognized, so C++26 stays infeasible as a build-wide
    baseline in this environment/CI until the toolchain itself is upgraded
    (a separate, bigger undertaking than this step; C++26 features stay
    isolated/prototype-only per Phase H's existing text either way, since
    even newer compilers mark them experimental -- see commit 18 above).
    The bump itself **found two real, if minor/latent, pre-existing bugs**
    via hard compile errors (not just warnings) -- exactly the "move bug
    classes to compile time" value Phase H is for, discovered by the
    standard bump alone, before adopting any specific new feature:
    - `core/dpdk.cc`'s `GetNonWorkerCoreList()` had `return 0;` inside a
      function returning `std::string` -- `0` as a null-pointer-constant
      implicitly converted to `const char*` and then to
      `std::string(const char*)`, i.e. constructing a string from a null
      pointer (real UB, just never triggered in practice because
      `pthread_getaffinity_np(pthread_self(), ...)` essentially never
      fails). C++23 added `basic_string(nullptr_t) = delete` specifically
      to catch this pattern, turning it into a hard compile error. Fixed
      to `return "0";` (the actually-intended fallback string, per the
      function's own comment).
    - `core/kmod/llring.h`'s `llring_init()` had
      `r->prod.head = r->cons.head = 0;` / `r->prod.tail = r->cons.tail
      = 0;` -- chained assignment through `volatile uint32_t` fields
      (lock-free ring buffer indices). Using the *value* of an assignment
      to a volatile-qualified object has been deprecated since C++20
      (P1152) and is now a compiler error under this repo's `-Werror`.
      Split into 4 separate statements -- purely mechanical, no behavior
      change (this runs once at ring-buffer init, not per-packet).
    Verified: clean `make clean && make ... -j4` under both g++ and
    clang++ from a clean tree, `core/all_test` 185/185 under g++ (179/179
    excluding the pre-existing `CodelTest` flake) and 179/179 under
    clang++ (ran with `-CodelTest.*` filtered), `run_module_tests.py`
    clean. **Performance**: checked rigorously per standing instruction,
    not just assumed harmless from "it's just a language-standard flag" --
    built the identical source under `-std=c++17` and `-std=c++23`
    side by side and compared: `core/packet.o`'s `.text` size grew by
    ~1KB, but that delta is confined entirely to `Packet::Dump()` (a cold,
    debug-only diagnostic function never called in the packet-processing
    path -- likely different libstdc++ iostream/sstream template codegen
    under the newer `__cplusplus` value); `Packet::copy()` (the one
    actually-hot function in that file) is byte-identical in size between
    both builds. `packet_bench` under proper repeated measurement
    (5-7 reps, 1s min-time) shows no consistent difference beyond noise.
    `checksum_bench` initially looked alarming (one data point showed
    "777ns -> 62.7ns", i.e. ~12x) on a single-shot run -- re-ran with 7
    repetitions and found this benchmark has ~26-49% coefficient of
    variation in this sandbox regardless of compiler standard (visible in
    both the c++17 and c++23 builds equally), so the single-shot swing was
    just this benchmark's own noise floor, not a real per-standard effect;
    medians across both builds land within each other's stddev band. Not
    yet pushed or Opus-reviewed as of this writing -- this touches every
    file's compilation flags plus two real (if latent) bug fixes, so it
    gets the same review treatment as other milestone commits this
    session.
20. **C++26 toolchain experiment (no commit -- research only, not adopted)**
    — user asked to actually try getting ahead of the curve on C++26 rather
    than stop at "the stock toolchain can't do it." Installed `g++-14`
    and `clang++-20` (both natively available via Ubuntu 24.04's own
    `noble-updates/universe` repo, no PPA needed) and tested this exact
    codebase against `-std=c++26` in a throwaway worktree. Result:
    `clang++-20` builds the whole codebase clean at C++26 with zero code
    changes (179/179 tests pass) -- proves this codebase's actual C++
    usage has no C++26 incompatibility, the barrier really is toolchain
    availability, not this project's code. `g++-14` compiles every source
    file clean too, but fails to *link* any binary due to an unrelated
    distro-packaging issue (Ubuntu's system `libunwind.a` is LTO-tagged
    for GCC 13, which GCC 14's LTO reader rejects) -- not a C++26 or BESS
    problem, would need its own fix (newer `libunwind` or de-static-linking
    it) if ever pursued. See Phase H's "C++26 experiment" subsection for
    the full writeup. Not adopted: C++26's library features worth having
    (reflection/contracts/`std::simd`) remain experimental regardless of
    which compiler implements the language core, so this doesn't change
    Phase H's existing recommendation to stay on C++23 for now -- recorded
    purely so a future session doesn't have to re-derive this.
21. **`90d908f7`** — **Removed `core/kmod` and the `VPort` driver entirely**
    — user asked directly to get rid of `core/kmod`.
    Investigated first rather than deleting blindly: `core/kmod/` mixed
    genuinely-dead kernel-module source (`sn_host.c`, `sn_netdev.c`,
    `sn_ethtool.c`, `sndrv.c`, `sn_kernel.h` -- known-broken on modern
    kernels per upstream `#1056`, never built by this session's CI/build
    path) with two headers still load-bearing for other, unrelated,
    working code: `llring.h` (a general-purpose lock-free ring buffer used
    by `core/modules/queue.h`/`drr.h` and `core/utils/lock_less_queue.h`
    -- nothing to do with the kernel module) and `sn_common.h` (the
    vport/kmod shared-memory IPC protocol structs, whose only consumer was
    `core/drivers/vport.cc`/`.h`). Given the choice between removing just
    the kernel module (relocating the two shared headers) or also removing
    `VPort` (which only exists to talk to that kernel module via
    `/dev/bess`, so is dead-but-harmless without it), user chose the
    latter -- full removal. Did: relocated `core/kmod/llring.h` to
    `core/utils/llring.h` (`git mv`, preserves history) and updated its 3
    include sites; deleted `core/drivers/vport.{cc,h}`, the rest of
    `core/kmod/` (`sn_common.h` included, since nothing needed it once
    `vport.cc` was gone), and 5 VPort-only sample `.bess` configs under
    `bessctl/conf/` (not part of any automated test); removed `VPortArg`
    from `protobuf/ports/port_msg.proto`; removed a dead
    `friend class ZeroCopyVPortTest;` declaration in `core/port.h` (the
    class was never defined anywhere in the tree); removed
    `build.py`/`container_build.py`'s `build_kmod`/`build_kmod_buildtest`
    functions, their `kmod`/`kmod_buildtest` CLI actions, and the
    now-unused `kernel_release`/`is_kernel_header_installed()` helpers
    (verified both were only ever used by `build_kmod()`); updated stale
    comments in `core/snbuf_layout.h`, `core/packet.h` (the
    `CheckPrivLayout()` comment explaining why the scratchpad offset is
    pinned -- kept the assert, since it's still cheap general insurance,
    just fixed the now-wrong "cross-language ABI contract with
    core/kmod/sn_common.h" justification), and `.github/workflows/ci.yml`
    (dropped the "core/kmod build/build-test is not run here" known-gap
    comment, since there's no longer a kmod to not-build).
    `pybess/test_bess.py`'s `test_create_port` passes the string `'VPort'`
    to a **mock** gRPC servicer defined in the same test file -- confirmed
    by reading it that this doesn't touch the real C++ `PortBuilder`
    registry at all, so needed no change. `bin/dpdk-devbind.py`'s `kmod/`
    reference is upstream DPDK's own vendored script (Intel copyright
    header) with an unrelated meaning (DPDK's own kernel driver binding),
    not touched.
22. **Opus review of `f2f3bf84`** (the C++23 bump, commit 19) came back:
    **correct, both bug fixes right and complete** -- independently
    re-derived every claim (byte-identical `Packet::copy()` disassembly
    between standards; a tree-wide scan found no second occurrence of
    either bug class; confirmed clean builds under both compilers,
    185/185 tests each). It also **definitively settled the `CodelTest`
    flake question** commit 19 had only asserted: reproduced under g++
    (1/10 runs under CPU load) *and* under a temporary `-std=c++17`
    rebuild (2/12 under load, same two tests) -- decisive proof the flake
    predates this commit entirely; under clang++ it's nondeterministic
    (different assertion line fails each run), ruling out a fixed
    miscompile. Confirms this doc's longstanding "known pre-existing
    flake, don't chase it" note was right.

    **One real gap found (not a defect, an evidence gap)**: commit 19's
    "no dataplane performance regression" claim was based on `packet.o`
    alone, and doesn't generalize -- paired `-std=c++17`/`-std=c++23`
    object-file comparison across the whole tree found real `.text`
    growth in hot dataplane code (`core/modules/drr.o` +39%,
    `WildcardMatch::ProcessBatch` +18%, plus `traffic_class.o`, `nat.o`,
    `exact_match.o`). The review's own disassembly of the worst case
    found a likely-benign cause: `Task::AddToRun` got **inlined into**
    `WildcardMatch::ProcessBatch` under C++23 (one fewer call on the
    packet path, not added work) -- traced to the same root cause as
    `Packet::Dump()`'s growth, C++20's `constexpr`-ification of libstdc++
    `string`/`vector` changing what the inliner can see across every TU
    that touches them. But this was reasoned from static code, not
    measured. Followed up with real throughput numbers on
    `traffic_class_bench`'s `TCWeightedFair` suite (the scheduler
    hot-path the review flagged specifically, 3 reps/0.2s min-time,
    paired `-std=c++17`-worktree vs. current `-std=c++23` build): C++23
    is **consistently as fast or faster** across every batch size, with
    the larger cases (16384-65536 packets) **~10-14% faster**, beyond
    what stddev explains -- confirms the review's inlining hypothesis
    with actual measurement, not just architectural reasoning. No
    regression found anywhere; if anything, a modest real improvement in
    the scheduler path. `url_filter_bench`'s `BM_FlowHash` was flat
    (noise-level either way, and too simple a benchmark to be very
    informative here).

    Also flagged, not yet acted on: `std::is_pod` (13 sites across
    `core/utils/*.h`/`pktbatch.h`) is removed outright in C++26 (not just
    deprecated) -- confirmed to be the concrete reason commit 20's
    `clang++-20 -std=c++26` experiment only compiled clean because
    libstdc++ still ships it as an extension; mechanical fix is
    `is_standard_layout_v<T> && is_trivial_v<T>`, not done here, noted as
    backlog for whenever Phase H actually pursues C++26. `core/gate.h`'s
    `std::unary_function` base (removed from the standard in C++17,
    libstdc++-extension-only already, and dropped entirely by libc++
    >= 17) is pre-existing, unrelated to this commit, same backlog bucket.
    The review's cited "~26-49% coefficient of variation" for
    `checksum_bench` didn't reproduce on an idle re-run (0.9-2.6%) --
    likely a load artifact from whatever else this sandbox was doing at
    the time -- but the underlying conclusion (no real per-standard
    effect) holds for a stronger reason: every `checksum.h` kernel is
    hand-written `asm volatile` with a `"memory"` clobber, so its codegen
    physically cannot vary with the language standard.
23. **Opus review of `90d908f7`** (the `core/kmod`/`VPort` removal, commit
    21) verdict: **C++ side correct and complete, but one real,
    CI-breaking regression shipped, plus 2 more dead sample configs
    missed by the commit's own deletion criterion** -- both fixed here.

    **Real bug**: `pybess/test_bess.py::test_create_port` called
    `client.create_port('VPort', 'p0', {...})`. This test's assertion
    that removing the real C++ `VPort` driver "needed no change" (because
    the test talks to a mock gRPC servicer that doesn't consult the
    driver registry) was correct as far as it went, but missed that the
    breakage happens **client-side, before the RPC is even sent**:
    `pybess/bess.py`'s `create_port()` does
    `getattr(port_msg, driver + 'Arg', module_msg.EmptyArg)` --  with
    `VPortArg` removed from the regenerated stubs, this silently falls
    back to `EmptyArg`, and the strict-mode `dict_to_protobuf()` call
    right after it throws `KeyError: EmptyArg does not have a field
    called ifname`. Reproduced exactly as the review described
    (`python3 -m unittest pybess.test_bess -v` → `ERROR`), confirmed it
    would have failed the same way in CI (`.github/workflows/ci.yml`'s
    "Run bessctl/pybess unit tests" step runs `python3 -m unittest
    discover`, which discovers this file). Fixed by deleting the
    `VPort` block from `test_create_port` (the other three drivers in
    that test are still real, still meaningful coverage). Also worth
    knowing for later: `getattr(..., EmptyArg)`'s silent fallback is a
    general footgun -- removing any `*Arg` message degrades to a
    confusing `EmptyArg`-shaped error instead of a clear "no such
    driver" one, at any future call site, not just this test.

    **Missed cleanup**: `bessctl/conf/port/latency.bess` and
    `bessctl/conf/port/vxlan.bess` both instantiate `VPort` as their only
    port type (not fixable by editing around it -- both configs are
    built entirely around `VPort`, same as the 5 already deleted).
    Confirmed via the review neither is referenced by
    `bessctl/test_samples.py` (only walks `conf/samples/`, which is
    VPort-clean) or `run_module_tests.py` (only globs `module_tests/*.py`)
    -- dead-but-not-CI-breaking, unlike the bug above. Deleted both.

    **Also fixed**: `README.md`'s quickstart still told users to
    `make -C core/kmod # Build the kernel module (optional)` -- removed,
    since it's user-facing and now simply wrong.

    **Everything else the review checked came back clean**: the
    `kernel_release`/`is_kernel_header_installed()` removal (confirmed
    zero other uses, pre- and post-commit), `sn_common.h`'s single
    consumer, all 3 `llring.h` include-site rewrites, `git mv` history
    preservation (`git log --follow` walks back 12 commits to the
    original public-release commit), the protobuf removal (confirmed
    `VPortArg` was genuinely the last message, nothing else references it
    by name -- BESS packs port args via `google.protobuf.Any`, not a
    `oneof`), the `PortBuilder` registration mechanism (confirmed
    `ADD_DRIVER`'s static-initializer registration into
    `all_port_builders()` is the *only* path -- no separate driver
    enum/switch/hardcoded list anywhere needed updating), and the dead
    `ZeroCopyVPortTest` friend declaration.

    **Flagged, not fixed** (legacy packaging/provisioning scaffolding,
    not part of the current build/CI path at all -- same "already-dead,
    pre-2026-CI-rewrite" bucket as the Bionic/Travis container this
    session's `ci.yml` already documents replacing): `env/after_install.sh`
    still runs `make -C .../core/kmod && insmod .../bess.ko` as part of a
    package post-install hook; `env/ci.yml`, `env/kmod.yml`,
    `env/Dockerfile` are Ansible/Vagrant provisioning that installs
    kernel headers for the now-gone module. None of these are invoked by
    `.github/workflows/ci.yml` or `container_build.py`'s current code
    paths -- left as backlog rather than fixed blind, since this
    sandbox has no way to actually exercise/verify a packaging or
    Vagrant/Ansible flow.
24. **`705782b3`** — Fixed the real regression and the missed cleanup entry
    23's review found: dropped the `VPort` block from
    `pybess/test_bess.py::test_create_port` (the actual bug -- see entry
    23 for the `getattr(..., EmptyArg)` mechanism), deleted
    `bessctl/conf/port/{latency,vxlan}.bess` (two more VPort-only sample
    configs missed by commit 21's own deletion criterion), and fixed
    `README.md`'s now-wrong `make -C core/kmod` quickstart instruction.
    Verified: `python3 -m unittest pybess.test_bess -v` 5/5 (was 1
    `ERROR`); confirmed no other test file references `vport`/`VPort` or
    the deleted sample filenames.
25. **`5f23e318`** — **DPDK-proposal review folded into the roadmap.**
    User provided a long external write-up surveying DPDK's
    evolution since ~2017 against BESS's 25.11.3-pinned state (~30
    proposed items) and asked for an Opus review + fold-in, with an
    explicit filter: "we should not adopt just for the sake of it, only
    things which make BESS faster, less maintenance burden, without
    losing flexibility and developer ergonomics." The review verified the
    document's factual claims against the real tree at `705782b3` rather
    than trusting them, and found the document **unusually accurate on
    DPDK-side facts and unusually wrong about what BESS already has** --
    most importantly, several of its proposals described work already
    done (`PMDPort` already accepts arbitrary vdevs, including vhost-user
    -- `bessctl/conf/port/vhost/vhost.bess` already does this) or targeted
    a subsystem this session had already deleted (the document's proposed
    memory-model split assumed the `VPort`/`kmod` legacy path this
    session's commit 21 already removed). After filtering: **6 items
    landed as concrete backlog now** (Phase B Stage 2 scope notes on the
    now-dead `paddr`/`vaddr` plumbing; Phase C's `PMDPort` vdev-substrate
    scope correction, `rte_eth_dev_adjust_nb_rx_tx_desc()`, a narrow
    consumer-driven `PortCapabilities`, and the `WorkerId`-vs-lcore-ID
    decoupling; Phase F's `DumpMempool()` backend-independence fix), **4
    items became explicit benchmark/experiment backlog** (`llring` vs
    modern `rte_ring`, mempool backend/cache-size, DPDK's `rte_bpf` vs
    BESS's 1066-line hand-written FreeBSD-derived x86 JIT -- the
    strongest maintenance-reduction candidate found, and `rte_fib` vs
    `rte_lpm`), **one new cross-cutting phase was added** (Phase J,
    RCU/QSBR-based live table updates -- verified the actual current
    mechanism is worse than the source document described:
    `THREAD_UNSAFE` commands are *refused* with `EBUSY` while any worker
    runs, not merely routed through a pause, and `bessctl` works around
    that by unconditionally pausing every worker for *every* module
    command, thread-safe ones included), and **16 items were explicitly
    rejected** with reasoning recorded (`rte_hash` replacing `CuckooMap`,
    `rte_acl` replacing the toy `ACL` module, a NAT RSS-affinity feature
    whose premise — multi-worker NAT — doesn't exist in this codebase,
    `rte_flow`, DMAdev, `pdump` (blocked by `--no-shconf`), and others —
    see "Rejected from the 2026-09-18 DPDK-proposal review" below). Every
    library proposed across all ~30 items is already linked into `bessd`
    today (`libdpdk.pc`'s `--whole-archive` list includes
    `librte_{acl,fib,rcu,bpf,hash,stack,...}.a`), so none of the surviving
    experiment items carry a new-dependency cost. Not yet reviewed by a
    second Opus pass (this was itself the review of external material,
    not a diff of BESS's own code, so the usual "review the review"
    pattern doesn't apply the same way) or pushed as of this writing.
26. **`7ac9d660`** — **Replaced `PMDPort::Init()`'s hand-rolled descriptor
    clamping with `rte_eth_dev_adjust_nb_rx_tx_desc()`** (Phase C item
    below, done). The old code clamped `queue_size[]` against
    `nb_min`/`nb_max` by hand in two ~18-line blocks and ignored `nb_align`
    entirely; DPDK's helper (verified against
    `deps/dpdk-25.11.3/lib/ethdev/rte_ethdev.c`: `RTE_ALIGN_CEIL` to
    `nb_align` first, then min-with-`nb_max`, then max-with-`nb_min`)
    closes that one latent misconfiguration class. `queue_size` is `size_t`
    but the ethdev API is `uint16_t` throughout (including the downstream
    `queue_setup` calls), so values are clamped to `UINT16_MAX` before the
    narrowing conversion; the helper's before/after comparison preserves
    the old `LOG(WARNING)` observability (single "adjusting RX/TX queue
    size" message instead of separate "resizing"/"capping" ones), and a
    non-zero return becomes a `CommandFailure` (unreachable in practice —
    the port was just validated and configured a few lines above).
    Verified, full matrix, both compilers from clean trees (`-j4` throughout):
    g++ `./build.py bess` clean, `core/all_test --gtest_shuffle` 185/185
    (CodelTest included this time — no flake), all 6 `*_bench` smoke-run
    OK, `run_module_tests.py` all 22 files OK (including `iplookup.py`,
    0.271s), `pybess`/`test_sugar`/`test_utils` 84/84 OK; clang++
    `CXX=clang++ ./build.py bess` from `make clean` with zero errors,
    clang-built `all_test --gtest_shuffle` 185/185, all 6 clang-built
    benches smoke-run OK. Live `bessd -m 0` smoke test of the changed path:
    `PMDPort(vdev='net_null0')` created OK at default 1024 and at odd size
    1000, size 8192 still correctly rejected by the pre-existing
    `MAX_QUEUE_SIZE` guard in `bessctl.cc` (unrelated), daemon stopped
    cleanly via `pause_all`+`kill`. No throughput comparison run: the
    changed code executes once per port creation (config path), zero
    per-packet instructions touched — there is nothing hot to regress.
    No Opus review requested: config-path-only mechanical replacement,
    same rationale as commit 13's infra work.
    **Sandbox caveat found during verification (pre-existing, not caused
    by this change):** `python3 -m unittest discover`'s `test_samples`
    leg hangs at `iplookup.bess` here. Root-caused, not chased: a fresh
    `bin/bessctl daemon start` (no `-m 0`, unlike `run_module_tests.py`)
    consumes all 512 sandbox hugepages at startup
    (`HugePages_Free: 512→0` with zero pipeline running), so `IPLookup`'s
    `rte_lpm` allocation fails ENOMEM; repeated daemon cycling plus two
    `kill -9`ed orphans made it hang-or-fail erratically until a graceful
    `daemon stop` reclaimed everything (512/512 free). Same binary passes
    `iplookup.py` under `-m 0`, and the changed function
    (`PMDPort::Init`) never executes in that sample (no PMD ports) — the
    base tree fails identically by mechanism.    `test_samples` was never in
    this sandbox's verification loop (only in CI, where runners have
    real hugepage capacity); don't treat its sandbox hang as a gate.
27. **`cef92c50`** — **PMD forwarding benchmark (`core/pmd_bench.cc`, new
    file).** Benchmark-backlog item 1 ("do this first") and the review-
    recommended measurement substrate for Stage 2 / rings / mempools /
    burst-size / FIB work. `BM_PmdNullTx` (alloc + `rte_eth_tx_burst` +
    PMD-side free rate through `net_null0`) and `BM_PmdRingRoundTrip`
    (full TX→RX self-loopback through real `rte_eth_rx/tx_burst` on
    `net_ring0`, the closest standalone analogue of `PortInc → PortOut`),
    both with a 1-32 batch sweep (`RangeMultiplier(2)`), drop counters,
    and `SetItemsProcessed` throughput. Setup mirrors `bessd -m 0`:
    `InitDpdk(0)` (`--no-huge`, malloc-backed — no hugepages needed, so
    this runs in the sandbox and CI) plus `CreateDefaultPools()` with
    `FLAGS_m = 0` (`PlainPacketPool`; absolute numbers aren't production-
    comparable, relative before/after on one machine is the use case);
    one queue per port, 1024 descriptors (through the new
    `adjust_nb_rx_tx_desc()` path, incidentally). Deliberately PMD-level,
    not module-graph: `PortInc`/`PortOut` need a live daemon, and the
    cross-worker `Queue` shape belongs to the mempool experiment's own
    harness (gated on this file + the `DumpMempool()` fix) — see the
    file's header comment. No existing file touched, so no regression
    surface outside the new binary; still verified: g++ build clean,
    `pmd_bench` TU compiles clean under clang++ with the bench flags,
    smoke run (`--benchmark_min_time=0.001s`) OK, full 5-rep/0.5s run
    clean with tight CVs (mostly 1-4%) and zero `tx_drops` at every batch
    size. Baseline medians: NullTx 4.53/9.07/17.05/32.11/60.58/100.74M/s,
    RingRoundTrip 4.57/8.74/16.64/32.06/60.23/108.31M/s (batch
    1/2/4/8/16/32). No Makefile/`build.py`/`ci.yml` change needed: the
    `%_bench` pattern rule and the CI smoke loop pick up any
    `core/*_bench.cc` automatically. Phase B's benchmark-coverage note and
    backlog item 1 updated alongside.
28. **`0bda5a84`** — **`llring` experiment + removal** (benchmark-backlog item
    2, done). Built `core/ring_bench.cc`: `llring` MP/SC vs `rte_ring`
    MP/SC vs MP_RTS/SC vs MP_HTS/SC, N-producer burst traffic (32-wide,
    256K items/producer, per-item `(producer, sequence)` encoding so the
    consumer verifies exact delivery, not just counts), 1/2/4/8/16
    producers → 1 SC consumer, no EAL/hugepages needed (caller-owned
    memory both sides). **Two methodological findings before the verdict:**
    (a) the generic `rte_ring_enqueue/dequeue_burst` wrappers cost
    10-30% vs the explicit sync-mode entry points (runtime flag
    dispatch) — the first full run measured the wrappers and would have
    kept `llring` on false grounds; re-ran explicit-against-explicit
    (which is also what a migrated `Queue` calls); (b) DPDK 25.11 *does*
    ship a ring zero-copy API (`rte_ring_{en,de}queue_zc_burst_{start,
    finish}` in `rte_ring_peek_zc.h`, reached via `rte_ring_elem.h` — an
    earlier grep of `rte_ring.h` alone missed it and wrongly recorded it
    as absent, corrected 2026-09-18) but it was still dropped from the
    comparison: it wasn't needed to justify the removal, and its win
    applies to producers writing directly into reserved slots, not to
    this pointer-handoff shape. HTS+ZC stays an optional follow-up
    optimization experiment, not a migration question. Verdict (5-rep medians,
    M items/s, llring vs rte-MP/SC at 1/2/4/8/16P):
    253/243/196/132/100 vs 320/273/212/139/100 — rte faster-or-equal
    everywhere, RTS/HTS no better. Per the decision rule the header went:
    `Queue` → `rte_ring` MP/SC (`mp/sc_enqueue/dequeue_burst`), `DRR` →
    SP/SC (`sp/sc_enqueue/dequeue`, flags `RING_F_SP_ENQ|RING_F_SC_DEQ`),
    `LockLessQueue` (test-only) → runtime-flag `rte_ring` with the generic
    single/bulk calls; unique ring names per `rte_ring_init` via a
    per-TU atomic counter; `ENOBUFS`/`ENOENT` replacing
    `LLRING_ERR_NOBUF`. `git rm core/utils/llring.h` (1193 lines);
    `ring_bench.cc`'s llring variant removed with it (rte variants kept
    as the forward guard). **The migration's own tests caught a real
    convention bug**: `rte_ring_enqueue/dequeue_bulk` return the count
    moved (0-or-n), not llring's 0-on-success — `LLQueueTest.Resize` +
    `MultiPushPop` failed until fixed. Verified: g++ full
    `bessd`/`modules`/`all_test` build clean, `all_test --gtest_shuffle`
    185/185, `run_module_tests.py` all 22 files OK (incl. live `DRR`
    tests), python units 84/84, clang++ TU-clean on every touched file,
    `ring_bench` rte variants re-run clean post-edit. No throughput
    comparison beyond the decision benchmark itself (it *is* the perf
    evidence). No Opus review: measurement-led mechanical migration, and
    its own unit tests demonstrably covered the riskiest seam (return
    conventions).
29. **`e740b6b5`** — **Review follow-ups to entries 27-28** (external review of
    `cef92c50`/`0bda5a84`, verdict: keep the `llring` removal, fix listed
    items first). (a) **Real portability bug, fixed**: caller-owned
    `rte_ring` storage was hardcoded to 64-byte alignment in four places;
    the type is `alignas(RTE_CACHE_LINE_SIZE)` (64 here, larger on some
    ARM64 — silent UB there). New shared `core/utils/rte_ring_alloc.h`
    (`AllocRingMem` on `alignof(rte_ring)`, unique-name helper), used by
    `Queue`, `DRR`, `LockLessQueue`, and `ring_bench`. (b) **Factual
    correction**: DPDK 25.11 *does* ship a ring zero-copy API
    (`rte_ring_peek_zc.h`, included via `rte_ring_elem.h` — the earlier
    "absent" claim came from grepping `rte_ring.h` only). Decision
    unchanged (ZC wasn't needed to justify the removal); HTS+ZC stays an
    optional follow-up, backlog item 2 and entry 28 corrected. (c)
    `ring_bench` methodology: untimed exact-verification phase once per
    (variant, count), timed loop counts only (a verifying consumer becomes
    the ceiling at high producer counts); `--pin_threads` opt-in flag for
    quiet-machine scaling studies, default unpinned (pinning measured
    ~500x slower in this shared sandbox — scheduler migrates away from
    busy cores, pinning can't). Restructured-bench baselines (MP/SC
    medians, M/s at 1/2/4/8/16P): 274/232/183/126/95 — same band as the
    decision run; the verdict table in entry 28 stands (it was
    same-structure, same-session). (d) `pmd_bench`: `NullTx` reports
    actual sent with drops explicit, plus `EndToEnd` twins (alloc through
    free timed) for allocator-adjacent questions; comments now state which
    family measures what. Restructuring the bench caught its own lifetime
    bug first: threads spawned from a helper captured the ring pointer by
    reference to the helper's dead parameter (segfault) — fixed by
    by-value capture, with a comment at the site. Verified: full g++
    rebuild clean, `all_test` 184/185 (sole failure the known `CodelTest`
    flake, 6/6 in isolation), module tests 22 files OK, python units
    84/84, clang++ TU-clean on all six touched/new files, both benches
    re-run clean. Status snapshot above refreshed alongside (it still
    described commits 19-24 as unpushed).
30. **`23aacd29`** — **Fixed `ring_bench --pin_threads` affinity collapse +
    `net_ring` invariant hardening** (external review of `e740b6b5`).
    The review was right on all three points. (a) **Real bug**: `PinThread`
    derived placement from the calling thread's own affinity, but threads
    inherit their creator's mask -- the benchmark thread pinned itself in
    `VerifyRing`, so every later producer inherited a one-CPU mask and all
    N producers + consumer fought for a single core. That, not host noise,
    was the recorded "~500x pinning slowdown" (entry 29's interpretation
    is superseded here; 29 is left intact as the historical record).
    Fixed by snapshotting the process mask once in `main`
    (`g_allowed_cpus`, before any pinning) and deriving all placement
    from it. (b) The consumer stays on the benchmark thread deliberately:
    moving it to its own thread broke Google Benchmark's per-thread CPU
    accounting (items/sec computed against an idle main thread read
    21-51G/s); since the benchmark thread is now never pinned, producers
    inherit the full mask at spawn and the snapshot places them
    correctly. (c) `pmd_bench` ring benchmarks now `CHECK_EQ(recvd, sent)`
    -- the documented self-loopback invariant is a regression detector,
    so a future change that strands a packet fails loudly instead of
    measuring a shifted workload. Verified: pinned mode re-measured sane
    (MP/SC 326/302/190/117/95M at 1/2/4/8/16P, same band as unpinned),
    unpinned numbers unchanged in band, `pmd_bench` ring variants pass
    with the new check enforced. CI watch postscript: the g++ job first
    failed in `run_module_tests.py` on
    `timestamp.py::test_timestamped_and_measured` (`1.2366 not <= 1.0`)
    -- a stats self-consistency check (`|avg*count-total|/total` over
    1ns-quantized histogram buckets) in a `Source -> Rewrite ->
    Timestamp -> Bypass -> Measure -> Sink` pipeline containing zero
    touched code. Same commit went green on clang, green locally twice,
    and green on a g++ rerun: load-sensitive flake, same family as the
    `CodelTest` flakes -- don't chase it; treat `timestamp.py` the same
    way if it recurs.
31. **`97f4a065`** — **`DumpMempool()` backend-independent** (Phase F item,
    prerequisite for the mempool-backend experiment). `core/bessctl.cc`
    read `mempool->pool_data` as `struct rte_ring*`, valid only for the
    `ring_mp_mc` backend `PacketPool` happens to select -- same "correct
    by construction" coupling already removed twice this session. Now
    uses only `struct rte_mempool`'s own fields plus
    `rte_mempool_avail_count`/`in_use_count`; the `ring_*` response
    fields are unset (kept on the wire, marked DEPRECATED in
    `protobuf/bess_msg.proto` with field numbers reserved -- comment-only,
    no codegen/wire change), and `show system packets` no longer prints
    them. Verified live (`show system packets`: all `mp_*` populated, no
    crash) plus full matrix: g++ build, `all_test` 185/185 shuffled,
    python units 84/84, module tests 22 files OK, clang++ TU-clean.
    Sandbox notes: build with `MAKEFLAGS=-j2` after a swap scare (later
    runs stayed healthy, ~5-6G available -- the `-j4` cap in this doc's
    constraints still stands, `-j2` was session caution); mid-session
    `/proc/meminfo` showed `HugePages_Total: 0` (not merely exhausted --
    the pool itself was gone, cause unknown, possibly host reclaim),
    restored with `sysctl -w vm.nr_hugepages=512` (then 512/512 free);
    if a future session sees Total 0, that sysctl is the fix, not daemon
    archaeology.
32. **`5ee632f1`** — **mempool backend/cache/worker-topology benchmark**
    (benchmark-backlog item 3; the allocator half of the plan `pmd_bench.cc`
    documents for the PMD boundary, after the `DumpMempool()` fix in entry
    31). New `core/mempool_bench.cc`; no production file touched.

    Two families: **local** (one thread, alloc-burst + free-burst of the
    same packets) and **pipeline** (a producer thread allocates and
    enqueues on an `rte_ring` in `Queue`'s exact MP-enqueue/SC-dequeue burst
    mode; the benchmark thread dequeues and frees -- the minimal
    `Source -> Queue -> Sink` shape, ring inside the timed region). Sweeps
    DPDK's five registered backends (`ring_mp_mc`, `ring_mt_rts`,
    `ring_mt_hts`, `stack`, `lf_stack`) x cache size (0/32/128/512) x batch
    (1/8/32), plus the two axes the plan needs:

    - `--pin=none|smt|cores|numa`: placement from sysfs topology, restricted
      to the process mask, chosen CPUs logged and reported per case; an
      unsatisfiable request is a hard failure, never a silent fallback (a
      run must not report cross-node numbers it did not measure).
    - `--lcore_mode=bess|register|none`: BESS's manual
      `RTE_PER_LCORE(_lcore_id) = wid` (`core/worker.cc:311`) vs
      `rte_thread_register()` vs no lcore id at all. This axis is the
      regression test for the planned WorkerId/lcore decoupling.

    Metrics: wall-clock `pkt_per_s`, per-thread busy fractions, per-worker
    **default-cache occupancy** (read from the public `struct rte_mempool`'s
    `local_cache[].len`), and an epoch-boundary assertion that
    `rte_mempool_avail_count() == capacity` -- a packet stranded in use
    fails the run instead of shifting the next epoch's workload.

    Two things found while building it, both relevant beyond this
    experiment:

    - **`rte_mempool_avail_count()` counts the default per-lcore caches**
      (DPDK 25.11 `rte_mempool.c`: backend count + every
      `local_cache[lcore].len`, clamped to `size`). So the `show system
      packets` fields rewritten in entry 31 mean "free anywhere (backend +
      caches)", not "in the backend" -- accurate, just not the
      cache-versus-ring split, which is why this bench reads occupancy
      directly. The first metric design here (sampling availability at epoch
      boundaries) was blind for exactly that reason and was replaced.
    - **Any `rte_lcore_id()` other than `LCORE_ID_ANY` gets a default
      cache**, registered with EAL or not -- so the `none` control has to
      write `LCORE_ID_ANY` explicitly (the benchmark thread starts as EAL's
      main lcore, 127). `CheckCacheInPlay()` caught exactly that: the first
      `--lcore_mode=none` run aborted with `mempool cache present but not
      expected (cache_size=32, lcore_id=127)` instead of reporting
      meaningless numbers. The other aborts during this work were also
      self-inflicted and caught the same way: a `--pin=` off-by-one made the
      flag parser reject its own value (5 SIGABRTs in `coredumpctl`), fixed
      before any sweep.

    Results. Machine: a laptop (i7-13900H, single socket, 20 logical CPUs),
    g++ 16, DPDK 25.11.3 `machine=native`, malloc-backed `--no-huge`;
    `--pin=cores` = CPU0/CPU2 (distinct P-cores, same package),
    `--lcore_mode=bess`, batch 32, median of 3. Pipeline Mpps (CV in
    parentheses):

    | backend      | cache 0        | cache 32     | cache 128    | cache 512    |
    |--------------|----------------|--------------|--------------|--------------|
    | ring_mp_mc   | 59.4 (17%)     | 90.1 (0.8%)  | 94.6 (1.7%)  | 94.5 (0.6%)  |
    | ring_mt_rts  | 52.6 (23%)     | 91.6 (1.4%)  | 94.4 (0.6%)  | 94.1 (0.07%) |
    | ring_mt_hts  | 92.4 (7%)      | 91.7 (0.4%)  | 93.0 (0.7%)  | 94.8 (0.5%)  |
    | stack        | 34.5 (6%)      | 43.5 (5.5%)  | 94.9 (8%)    | 83.7 (11%)   |
    | lf_stack     | 43.7 (0.3%)    | 43.1 (0.3%)  | 45.3 (0.8%)  | 44.9 (0.3%)  |

    An earlier sweep of the same grid, hours before, measured the same
    shapes at roughly half the absolute rate (e.g. `ring_mp_mc` 32.3 / 41.4
    / 41.3 Mpps at cache 0/32/512, CV <= 4%), and a lone control run saw 91
    Mpps for the same configuration that sweep had just measured at 47. **The
    absolute rate on this host moves by more than 2x between and even within
    sessions (clock/thermal state), so only within-run comparisons are used
    below; every claim that needed two runs states both.** What survives
    both:

    - **Cache size stops mattering at the burst size.** No cache at all is
      clearly worse (`ring_mp_mc`: 32 vs 41 Mpps in the stable run, 59 vs 90
      in the fast one), and everything from cache 32 upward is identical
      within 2% CV at *every* batch and in both runs -- while cache 512
      parks 255 + 511 = 766 of 8191 objects (9.4%) in the two caches (256 +
      389 in the fast run): the consumer's cache is always the fuller one,
      and the producer's is always the emptier one, which is the
      cache-imbalance shape the plan predicted. The cache only has to cover
      the worker's burst; "as large as possible" is an assumption this
      benchmark retires, not a finding it confirms.
    - **`lf_stack` is 1.5-2x slower cross-worker** than every ring backend at
      cache >= 32 (27 vs 41 in the stable run, 44 vs 90-95 in the fast one),
      at CV < 2% for the ring side -- and it is also the worst cacheless
      single-threaded allocator (12.7 ns/pkt vs 4.9 for `ring_mp_mc`).
    - **`stack` is not measurable as a dataplane backend in this harness
      yet**: it is the *fastest* cacheless single-threaded allocator
      (2.9 ns/pkt) but under two-thread contention it swings between 28 and
      95 Mpps with CV up to 11% across cache sizes and runs -- consistent
      with `rte_stack`'s locked variant serialising both workers, but
      unproven here. Recorded as a follow-up, not a result.
    - **`ring_mp_mc` / `ring_mt_rts` / `ring_mt_hts` are equivalent at
      production settings** (91.6-94.8 Mpps, CV <= 1.7% in the fast run;
      41.3-45.5 in the stable one). One earlier run had RTS ahead by ~8% at
      batch 32 only; it did not reproduce at batch 1/8 or in any other run.
      No basis to change the shipped default.
    - **Single-threaded, the cache is worth ~2x** (4.93 -> 2.34 ns/pkt for
      `ring_mp_mc`; same shape in both runs) and the backend barely matters
      once a cache exists -- the exact inverse of the cross-worker ordering,
      which is why the "A alloc -> A free" and "A alloc -> B free" cases
      needed separate families.
    - **The lcore decoupling has no allocator-side cost**: `register` vs
      `bess` differ by <= 6% at cache >= 32 (95-100 vs 90-95 Mpps in the
      fast run; equal within 1% in the stable one), i.e. at drift scale.
      That work can proceed with this binary as its regression guard.
    - Topology, as expected: SMT siblings (`--pin=smt`) run at 30-35 Mpps
      against 41-95 on two physical cores (both runs agree on the
      direction; the ratio depends on host state), and unpinned placement
      beat the static pin in both runs (49.5 vs 41.3; 115 vs 94.5) --
      consistent with entry 29's "scheduler usually beats static pinning on
      a shared host". `--pin=numa` refuses on this single-node machine.
    - Controls: with no lcore id (`--lcore_mode=none`) cache size has no
      effect in the local family (0.4-1.3% spread over 0/32/128/512), i.e.
      the harness proves its own cache axis is real, and in the pipeline the
      cache-size trend disappears as well (4.8-19% spread with no monotone
      trend, against the 2x cache-0 penalty when caches are in play).

    Net: the shipped `ring_mp_mc` + cache 512 survives, but the reason is
    "nothing better reproduces", not "bigger is better" -- for the
    cross-worker pipeline the cache needs only to cover the burst size, and
    the backend family (ring vs stack/lf_stack) matters more than any other
    knob measured. No default changed. Follow-ups that need hardware this
    sandbox does not have: cross-socket `--pin=numa`; NIC-backed numbers
    (backlog item 6, `MBUF_FAST_FREE`, still gated on both); and, only if
    flush/refill *frequency* is ever needed, a separate DPDK build with
    `-Dc_args=-DRTE_LIBRTE_MEMPOOL_STATS` (it instruments the timed path, so
    it cannot be the same build these numbers came from).

    Verified: `g++ -fsyntax-only` and `clang++ -fsyntax-only` clean on the
    new TU under the tree's `-Werror -Wall -Wextra -Wcast-align -Wshadow`
    (only pre-existing `is_pod`/`unary_function` deprecations from other
    headers, demoted as in CI); CI-shape smoke run
    (`--benchmark_min_time=0.001s`) covers all 120 cases clean; five full
    sweeps (`--pin=cores`, `--pin=none`, `--pin=smt`, `--pin=cores
    --lcore_mode=register`, `--pin=cores --lcore_mode=none`) completed
    clean, then the whole grid re-run once more for the commit;
    `all_test --gtest_shuffle` 185/185 on this machine under the workarounds
    noted in the sandbox-constraints section (the new file is a translation
    unit of its own, not part of `bess.a` and not linked into any test --
    that run is the tree's state, not coverage of this file).
    `bessctl/run_module_tests.py` could **not** run here for an environmental
    reason, not a code one: it starts a real `bessd`, which refuses to run
    without root (`You need root privilege to run the BESS daemon`) and this
    session is an unprivileged user; CI covers it.

33. **`3dc30b0e`** — **worker pthreads register with DPDK instead of writing
    `_lcore_id`** (Phase C item). `core/worker.cc` used to do
    `RTE_PER_LCORE(_lcore_id) = arg->wid` and then assume
    `wid == rte_lcore_id()`; `Worker::Run()` now calls DPDK's public
    `rte_thread_register()` (after `rte_thread_set_affinity()`, so DPDK
    captures the pinned cpuset and derives the NUMA id from it) and
    `rte_thread_unregister()` after `delete scheduler_; delete rand_;` --
    deliberately last, because scheduler/TrafficClass teardown can still
    free packets and a free wants the worker's mempool-cache context.
    `rte_errno.h` is now included for `rte_strerror(rte_errno)`.

    The point of the item was that the old write was **load-bearing but
    invisible**: `rte_mempool_default_cache(mp, rte_lcore_id())` keys every
    worker's allocator cache on that value, so a missing or wrong lcore id
    silently degrades every `Packet` alloc/free to the cache-bypassing path
    with no symptom other than throughput. The three concepts are now
    explicitly separate -- BESS `WorkerId` (`arg->wid`), physical CPU
    (`arg->core`), DPDK lcore id (whatever `rte_thread_register()` returns)
    -- and the startup log line reports all three
    (`Worker 0(...) is running on core 0 (socket 0, DPDK lcore 0)`), so a
    run can no longer be mis-described. Nothing may assume
    `wid == rte_lcore_id()` any more; a repo-wide grep found no other reader
    of `rte_lcore_id()` in BESS outside this file, so nothing did.

    Registration cannot fail for a supported configuration: BESS's EAL
    config (`core/dpdk.cc`, `--lcores 127@<all cpus>`) leaves lcores 0..126
    free and `Worker::kMaxWorkers` is 64, so the `CHECK_EQ` guards a future
    EAL-config change rather than a live risk.

    Before/after regression, using the axis entry 32 built for exactly this
    (interleaved pairs in one session, so the host's >2x clock/thermal drift
    hits both sides -- `bess` = the old mechanism, `register` = the new one,
    pipeline family, `ring_mp_mc`, batch 32, median of 3, Mpps):

    | case                    | `bess` (before) | `register` (after) |
    |-------------------------|-----------------|--------------------|
    | `--pin=cores`, cache 512 | 93.9 / 94.4    | 99.4 / 98.9        |
    | `--pin=cores`, cache 128 | 94.1 / 93.9    | 90.9 / 99.6        |
    | `--pin=none`, cache 512  | 109.7 / 120.9  | 108.1 / 108.6      |
    | `--pin=none`, cache 128  | 105.7 / 116.3  | 96.9 / 106.6       |

    (Two interleaved pairs per row.) Indistinguishable: the signs disagree
    between pairs. The cache-occupancy counters were identical between
    mechanisms as well (the consumer's cache full at the configured size,
    the producer's at about half), i.e. the cache is still *in use* after
    the migration, not merely present -- the failure this item existed to
    prevent. `cache 0` stays noisy in both mechanisms (entry 32).

    Verified: `all_test --gtest_shuffle` 185/185 with the change; a live
    `bessd` run as an unprivileged user (`--skip_root_check -m 0`) with two
    workers -- the log shows `Worker 0 ... core 0 (socket 0, DPDK lcore 0)`
    and `Worker 1 ... core 2 (socket 0, DPDK lcore 1)`, no
    `rte_socket_id() returned -1` warning, a `Source -> Queue -> Sink`
    pipeline moved 26.3G packets, `show system packets` still reports the
    mempool (cache 512, 262144 objects), and `daemon stop` ended in
    `BESS daemon has been gracefully shut down` with exit code 0 -- so the
    unregister path is exercised, not just compiled. The eight regression
    runs all exited 0 with no coredumps.

34. **`e55fb8a2`** — **`rte_fib` vs `rte_lpm`: experiment done, FIB not adopted**
    (Phase D item; no module change). New `core/fib_bench.cc` compares the
    exact code shapes `IPLookup` uses or would use -- `lpm_vec` (the
    module's SSE byte-swap + `rte_lpm_lookupx4` path), `lpm_scalar` (its
    tail path), and `fib_bulk` (`rte_fib_lookup_bulk` over raw
    packet-order keys, FIB created with `RTE_FIB_F_LOOKUP_NETWORK_ORDER`,
    i.e. no swap anywhere) -- over deterministic 1K/64K/512K-route tables
    (mostly /24, mixed lengths, longer prefixes nested under earlier /24s),
    measuring lookup throughput, build cost, EAL-heap footprint and
    add/delete cost. Key conventions were settled against DPDK itself with
    a standalone probe rather than by reading docs: rules go in as
    `be32_t::value()` for both tables, and the network-order flag changes
    only how the *lookup key* is addressed (`lib/fib/rte_fib.c`:
    `dir24_8_get_lookup_fn(..., be_addr)`), so a FIB caller holding packet
    headers needs no swap at all. Before any timing, every case
    cross-checks the table against an independent longest-prefix match
    computed in the benchmark; a disagreement aborts and prints the rules
    that could explain both answers.

    Lookup results (median of 3, this benchmark's thread-CPU-time counter,
    CV <= 5%):

    | benchmark             | 1K routes      | 64K routes     | 512K routes    |
    |-----------------------|----------------|----------------|----------------|
    | `BM_LookupLpmVec`     | 3.95 ns (253M) | 5.42 ns (185M) | 4.47 ns (224M) |
    | `BM_LookupLpmScalar`  | 3.19 ns (314M) | 5.34 ns (187M) | 4.63 ns (216M) |
    | `BM_LookupFib`        | 3.70 ns (270M) | 5.20 ns (192M) | not registered |

    (Mpps in parentheses.) So FIB's lookup ties or very slightly beats the
    module's current vector path (0.94x at 1K, 0.96x at 64K), while its
    *other* numbers look much better: table build is 0.41/0.59 us per route at
    1K/64K versus 0.65/5.84/39.6 us for LPM (LPM's per-route build cost rises
    with table size -- ~21 s to build 512K routes -- while FIB's does not),
    and delete+add at 1K is 212 ns per operation versus 606 ns for LPM. **The 64K update numbers from the first
    run -- 497 vs 8299 ns, a 17x FIB win -- are withdrawn**: re-running with
    the post-churn verification below showed the FIB table those operations
    left behind no longer matches the reference (LPM's does, at every size),
    so that measurement is of a table that ended up wrong, not of usable
    update performance. Footprint goes LPM's way: 98 KB per route at 1K
    (its fixed 64MB tbl24 dominating) down to 1.5 KB at 64K and 200 B at
    512K; FIB 99 KB / 1.96 KB (about 28% larger at 64K).

    Decision: **not adopted**, on correctness rather than speed. At 512K
    routes, with rules inserted in the generator's arbitrary order -- the
    order `IPLookup CommandAdd` permits, since a control plane may add a
    /24 after a /28 under it -- `rte_fib` (DPDK 25.11.3, DIR24_8, 4-byte
    next hops) **produced an incorrect lookup result**: for a key that both
    `rte_lpm` and the independent reference resolve to a `/12` rule's next
    hop, it returned a next hop (7207) matching **no rule at all**. Observed
    behaviour, not a root cause: DPDK 25.11.3 already carries the upstream
    `fib: fix prefix addition handling` stable fix, so this is not that
    known defect, and no minimized reproducer exists yet (the two-rule and
    four-rule nested cases behave correctly in both orders, so the trigger
    needs something the larger mixed table has). What is established:
    deterministic (same key, same value on repeated runs), unchanged by a
    4x larger tbl8 pool, gone when the same rule set is inserted
    shortest-prefix-first, while `rte_lpm` passes the same gate in both
    orders. Re-verified after the harness-hardening pass below, with the
    full 64K-key stream checked instead of a 512-key sample and with
    add/delete/return values all validated -- same key, same wrong value.
    `BM_LookupFib` is therefore registered only up to 64K routes, with a
    comment at the registration saying why and what to re-enable after a
    DPDK fix, and `IPLookup` keeps `rte_lpm` -- which also settles the
    table representation Phase J's live-update pilot should build on.

    The same hardening pass then found a *second*, independent instance in
    the update path, which no amount of build-time checking would have caught:
    at 64K routes, timing the delete+add churn (the workload whose cost
    motivated this experiment) left the table resolving key `0xc8d35058` to
    3208 while the only matching rule -- a /24 -- has 7368, and only because
    the add/delete benchmark now re-verifies the whole table after the timed
    loop. `rte_lpm` returns exactly the reference after identical churn at
    every size. FIB's update comparison is therefore reported only at 1K;
    the 64K "17x cheaper" measurement from the first run is withdrawn, since
    the table it produced was wrong.

    Review-driven hardening of the gate itself (same commit series): the
    default-next-hop sentinel is now `DROP_GATE` (`8192`, `core/gate.h`),
    outside the generated route range, because a sentinel colliding with a
    real next hop would make a miss indistinguishable from a correct hit;
    the whole key stream is verified, not a sample; `rte_fib_lookup_bulk`'s
    return value is checked (it is `-EINVAL` or `0`, *not* the number of
    lookups -- an assumption this pass caught); and the add/delete
    benchmark now counts per-operation failures inside the timed loop and
    re-verifies the whole table after it, since a failed operation could be
    cheaper than a successful one and would otherwise be counted as work.
    `rte_lpm` passes the strengthened gate at every size, including 512K.

    Side finding, recorded but deliberately not acted on: the *scalar* LPM
    path beats the vector one by 19% at 1K, but the gap closes to 1% at 64K
    and reverses at 512K (vector 4.47 vs scalar 4.63 ns) -- so
    `VECTOR_OPTIMIZATION`'s SSE byte-swap plus `rte_lpm_lookupx4` is a cost at
    small tables and a win as tables grow, and the first run's
    "pessimization at every size" reading was a small-table artifact that the
    full-key-stream gate plus a re-measure corrected.

    Verified: `g++`/`clang++ -fsyntax-only` clean on the new file; a full
    default run of `fib_bench` exits 0 with all registered cases passing
    the correctness gate. The gate caught two real problems while this was
    built, which is why it exists: first a key-convention mistake in the
    harness itself (rules built in raw form, every lookup missing), then
    the FIB behaviour above.

35. **`4e7f420e`** — **Phase J pilot: `IPLookup` route updates without pausing
    workers**. `IPLookup` no longer keeps a single mutable `rte_lpm` that
    commands edit in place -- the coupling that forced
    `Command::THREAD_UNSAFE`, i.e. "pause every worker or refuse". It now
    holds one *immutable routing generation* (the route list it was built
    from, plus the `rte_lpm` built from it) behind
    `std::atomic<std::shared_ptr<const Generation>>`:

    - `ProcessBatch()` takes **one snapshot acquisition per batch** and holds
      the `shared_ptr` for that batch, so a concurrent command can neither
      free the table under an in-flight lookup nor swap it mid-batch;
    - `add`/`delete`/`clear` copy the current route list, apply the change,
      build a replacement off the data path, and publish it with a release
      store (a mutex serializes command-side mutation, so concurrent commands
      cannot lose an update);
    - retired generations are reclaimed by `shared_ptr` as soon as the last
      batch holding one returns -- deliberately no epoch/QSBR machinery: the
      first pilot is one module and one atomic pointer, per the review's
      scope (no BESS-wide QSBR, no graph RCU, no reusable framework).

    Deliberate first-cut costs, recorded rather than hidden: every command
    rebuilds the table from the route list (`O(routes)` off-path work; a
    control plane adding 500K routes one at a time would notice), and the
    command *semantics* are exactly as before -- `rte_lpm_add` overwrite (the
    route list replaces rather than duplicates a prefix), default gate via
    `prefix_len == 0`, `clear` drops rules and keeps the default gate,
    deleting a missing rule is still an error, and the
    `VECTOR_OPTIMIZATION` SSE path is untouched. Each generation gets a
    unique `rte_lpm` name (`<module>_g<N>`) because a rebuild happens while
    the previous generation may still be serving in-flight batches.

    Acceptance evidence (live: pybess against a real `bessd`, not the CLI --
    `bessctl`'s `command module` pauses workers unconditionally and would hide
    the answer): `Source -> Rewrite -> IPLookup -> two Sinks`, one worker,
    traffic flowing, six route commands issued with workers running. Packets
    counted on `ipl`'s output gates (task-less `Sink`s do not account for
    input, which cost one wrong reading before it was noticed):

    | phase | to gate 0 | to gate 1 |
    |-------|-----------|-----------|
    | no route (DROP) | 0 | 0 |
    | `add 10.7.7.0/24 -> 0` | +68.8M | 0 |
    | `add 10.7.7.7/32 -> 1` | +0.2M | +64.1M |
    | `delete 10.7.7.7/32` | +64.6M | +0.1M |
    | `add default -> 1` then `clear` | mixed phase | +65.2M |
    | `delete default` (-> DROP) | +0 | +0.09M in flight |

    The small deltas on the "wrong" gate at each switch are batches that had
    already snapshotted the previous generation -- per-batch atomicity working
    as designed, and visible here for the first time. Both daemon runs show
    exactly one `*** All workers have been paused ***` (the test's own setup,
    before traffic started) and none for any route command; both exited 0
    with no coredumps.

    Verified: full build clean; `all_test` 185/185; `g++`/`clang++`
    `-fsyntax-only` clean under the tree's `-Werror` set (modulo this host's
    pre-existing protobuf-36 `[[nodiscard]]` warnings inside `module.h`, which
    CI does not see); `bessctl/module_tests/iplookup.py` is unchanged in its
    expectations and runs in CI -- it exercises add/delete/prefix validation
    against a paused pipeline, and could not run locally because the harness
    starts its daemon through `sudo`.

    Not done, on purpose: generalizing the pattern to other modules, and any
    BESS-wide RCU/QSBR design. Those were the review's explicit "not yet";
    with the mechanism and its live evidence in place they can now be argued
    from a working example instead of a proposal.

    Reclamation follow-up (`9db37492`, review-driven): the first cut dropped
    the command's reference at publish time, which made the *last dataplane
    batch* to finish run `~Generation()` -> `rte_lpm_free()` -- a 64 MiB
    tbl24 free plus DPDK's global tailq write lock, on a packet worker.
    `Publish()` now stores the replacement and then waits for the batches that
    already held the retired generation to drain, so the final reference --
    and the free -- is released by the command thread; the wait is a
    `std::this_thread::yield()` loop on a `use_count()` that can only
    decrease, under `mutation_lock_`, which also keeps rapid commands from
    accumulating retired generations. Measured, not argued:

    - temporary glog instrumentation under 233 add/delete updates with
      traffic flowing: every free ran on the *same thread as its own
      `Publish()`*, and all four control-plane thread handles involved
      differed from the worker's (`0x7ce048ff9670`); the drain took 0-1
      yields, so the RPC-side cost is negligible.
    - update-time continuity (2 ms counter sampler on its own client, 50 ms
      buckets, one daemon, one pipeline): control (no updates) 0.933 min/p50,
      fail-fast commands 0.946, add/delete with rebuilds 0.907 -- no zero
      trough. The ~3pp delta is the rebuild's memory traffic at ~39
      updates/s, orders of magnitude above realistic route churn. A first run
      reported 0.088 and was a measurement artifact: its sampler shared one
      pybess client with the command thread.
    - daemon RSS flat (876.7 -> 876.9 MB) across those 233 generations:
      reclamation is prompt, not deferred.
    - the instrumentation was temporary; the committed code carries only the
      `Publish()` helper and its preconditions.

    Multi-reader validation (reviewer-recommended before copying the pattern):
    four workers, each with its own `Source -> Rewrite`, all feeding **one**
    shared `IPLookup`, 221 add/delete updates again with traffic flowing.
    No forwarding errors (0 drops, 0 command errors), daemon RSS flat
    (877.4 -> 877.5 MB across the retired generations), and with the
    instrumentation back in, zero `~Generation`/`Publish` lines ran on any
    of the four worker threads (checked by searching the log for each
    worker's `pthread_self()` handle as the glog thread field; the same
    query returns dozens of hits for a real control-plane thread, so it can
    fail). The drain still took 0-1 yields with four readers in flight.
    Update-time continuity at 4 workers: min/p50 0.898 vs 0.949 for the
    no-update control at ~212 Mpps aggregate -- same ~5pp signature as the
    single-worker run, no stop-the-world.

    Status and sequence: `bessctl`'s `command module` still pauses
    unconditionally (deliberate -- `Command::THREAD_UNSAFE` commands need
    that, and a "retry after EBUSY" hack could duplicate side effects); the
    fix is to expose command thread-safety through the module-class
    introspection API so `bessctl` pauses only when it must. That, and any
    reusable snapshot/publication abstraction, come **after** `ExactMatch` is
    done as the second Phase J module -- two proven modules first, per the
    scope discipline above.

36. **`03e1b6f8`** — **Phase J module #2: `ExactMatch` rule updates without
    pausing workers**. `ExactMatch`'s four mutating commands (`add`, `delete`,
    `clear`, `set_runtime_config`) edited one live table in place, which is why
    they were `Command::THREAD_UNSAFE`. They now build a replacement
    *generation* (the rule list it was built from, the default gate, and the
    table built from them) behind
    `std::atomic<std::shared_ptr<const Generation>>` and swap it in, so a batch
    sees the old table or the new one, never a half-applied change.
    `ProcessBatch()` takes one snapshot per batch; `Publish()` (same helper
    shape as the `IPLookup` pilot) publishes, then waits for the batches that
    held the retired generation to drain, so its destructor runs on the command
    thread.

    Where this module forced a *different* shape than `IPLookup` -- precisely
    what running a second module was meant to reveal:

    - `CuckooMap` deletes copy and defaults move, so a generation cannot be a
      copy of the live table; every rebuild replays the stored rule list, and
      the module's field configuration (fixed at `Init()`) is re-applied to
      each new table.
    - Rule identity is field-vector equality, which *is* key equality here
      because `gather_key()` validates every field's size against the
      configuration -- so "add with the same match values" overwrites the gate
      in the rule list, exactly what inserting the same key into the live table
      did.
    - `set_default_gate` was already `THREAD_SAFE` (via `ACCESS_ONCE`); the
      default gate now travels inside the generation, so rules and default
      change atomically.
    - `SetRuntimeConfig` is now all-or-nothing: the old code warned that "the
      state may be partially restored" on error (`TODO(torek)`); a failed
      rebuild leaves the running configuration untouched and that TODO is gone.

    Pre-existing quirk preserved rather than fixed: the "Invalid gate" EINVAL
    path is unreachable for `add`/`delete` because `gate_idx_t` truncates the
    protobuf's uint32 gate before `is_valid_gate()` sees it. The one
    behavioural change to old behavior is the insertion-failure report in the
    follow-up below (a rule that could not be inserted used to be reported as
    added).

    Acceptance evidence (live, pybess, one daemon, 4 workers, each
    `Source -> Rewrite -> the same ExactMatch`, matching IPv4 dst at offset 30,
    traffic running throughout; `bessctl/module_tests/exact_match.py` remains
    the CI-side check of the same semantics):

    - 13 semantics checks with workers running: no rules -> default DROP;
      `add` -> gate 1; `add` with the same fields -> gate 0 (overwrite, not a
      duplicate); `delete` -> back to DROP with the counters frozen (a strong
      observable); `set_default_gate` -> gate 0; a rule beats the default gate;
      `clear` -> rules gone, default gate kept; EINVAL for empty fields and for
      a wrong field count; ENOENT for a missing rule; `get_runtime_config` and
      `get_initial_arg` round-trip.
    - 251 add/delete updates: 0 command errors, 0 drops, daemon RSS
      745.5 -> 745.7 MB (retired generations reclaimed promptly), no
      `~Generation`/`Publish` line on any worker thread (searched the log for
      each worker's `pthread_self()` handle -- empty, against a query that
      returns hits for real control-plane threads), drain 0-1 yields.
    - Update-time continuity, 4 workers, the same three-way comparison the
      `IPLookup` review used: control (no updates) 0.952 min/p50, fail-fast
      commands 0.961, add/delete with rebuilds 0.816-0.860 across runs -- no
      zero trough, and the fail-fast control shows the command path itself is
      free, so the delta is rebuild/drain work.

    Verified: full build clean; `all_test` 185/185; `g++`/`clang++`
    `-fsyntax-only` clean under the tree's `-Werror` set.

    Follow-up from review (`a0688fcf`): three defects, all fixed before this
    module is used as an abstraction input.

    - **A metadata-field table could not be rebuilt at all.** `ApplyFields()`
      called `AddField(this, attr_name, ...)` for every generation, which calls
      `Module::AddMetadataAttr()` again, and re-registering a module attribute
      fails with `EEXIST` -- so the first
      `add`/`delete`/`clear`/`set_default_gate`/`set_runtime_config` after
      `Init` failed for any ExactMatch matching on metadata. Attribute ids are
      now resolved once during `Init()` into `FieldSpec::attr_id`, and
      `ExactMatchTable` gained a narrow `AddResolvedAttrField()` (plus an
      `attr_resolved` path in `DoAddField`) that configures a table from an
      already-resolved id without registering anything: generation
      construction never mutates module metadata. The offset-field live tests
      could not see this -- the pre-existing `test_exactmatch_with_metadata()`
      is what catches it, which is the argument for keeping the module-test
      suite in the gate.
    - **`SetRuntimeConfig()` could make the stored rule list disagree with the
      table.** It pushed every protobuf rule, while the table it built holds one
      entry per match value, so `get_runtime_config` emitted duplicates the old
      implementation never produced and `delete` removed them all at once. Both
      `add` and `set_runtime_config` now upsert through one helper (last gate
      wins), and `Build()` asserts `table.Size() == rules.size()` so the two
      cannot silently diverge again.
    - **Silent `CuckooMap` insertion failure is no longer swallowed.**
      `ExactMatchTable::AddRule()` ignored `Insert()`'s `nullptr` (documented
      after excessive hash collisions): a command used to report success for a
      rule absent from the table, and with a stored rule list that became a
      phantom rule -- reportable, deletable, forwarding nothing, possibly
      reappearing on a later rebuild. `AddRule()` now returns `ENOSPC` and the
      command fails instead. A deliberate fix to the old quirk, chosen over a
      source of truth that knowingly disagrees with the dataplane.

    Evidence the regressions have teeth: the same module tests against the
    pre-fix binary fail exactly as predicted -- `test_exactmatch_with_metadata`,
    `test_exactmatch_selfconfig` and the new metadata-rebuild test error with
    `EEXIST: add_metadata_attr() failed`, the new duplicate-rule test fails
    (`Ran 6 tests` / 1 failure + 3 errors); the fixed binary passes all six.
    Drivers re-run: 13 metadata/canonicalization checks, 13 semantics checks,
    4-worker storm at 0 drops with RSS +168 kB over 251 retired generations,
    `min/p50` 0.884. `iplookup.py` module tests still pass, build clean,
    `all_test` 185/185, both compilers clean under the tree's `-Werror` set.

    New CI-path coverage: `test_exactmatch_metadata_field_survives_updates`
    and `test_exactmatch_setconfig_dedups_duplicate_rules`.

    Not done, on purpose: nothing in `bessctl` (its `command module` still
    pauses unconditionally) and no shared abstraction yet. With both planned
    modules landed, reviewed, and (for ExactMatch) reviewed again after these
    fixes, extracting the common snapshot/publication mechanism -- and exposing
    command thread-safety so `bessctl` pauses only when it must -- is the next
    Phase J step; these two implementations are what that extraction should be
    designed from.

37. **`b14f431a`** — **Phase J: the snapshot/publication/reclamation mechanism,
    extracted**. `IPLookup` and `ExactMatch` had grown the same writer protocol
    and the same reader rule independently (entries 35 and 36); both now use
    `bess::utils::PublishedGeneration<Generation>` (`core/utils/`), and their
    own `Publish()`/members/duplicated drain loops are gone (241 insertions,
    176 deletions across the two modules and the new header).

    The class owns exactly three things, per the review's split: `Snapshot()`
    (one acquisition per batch, held for the batch's duration); `Update(build)`
    (serialize writers, ask the caller's builder for a replacement, publish it
    with a release store); and the drain that follows (wait for the readers of
    the retired generation to drop their snapshots, so the retired generation's
    destructor runs on the control-plane thread, not on whichever packet worker
    finishes its batch last). Construction stays in the modules -- replaying
    routes into an `rte_lpm` with unique per-generation names, replaying
    canonical rules plus resolved field specs into an `ExactMatchTable` -- so a
    later QSBR backend can replace the shared class without forcing the two
    modules into one table-building model.

    One hardening while extracting: `Update()` hands the builder a
    `const Generation &`, not a `shared_ptr`. A builder that kept a
    `shared_ptr` would inflate the use count the drain loop waits on and stall
    it forever; a reference makes that impossible rather than merely
    documented. Builders that fail return `nullptr`, the installed generation
    is left untouched, and each module keeps reporting its own error type.

    Behavior-preserving, verified by re-running everything that covered the two
    modules *before* the change (no new tests, no changed expectations):

    | suite (all pre-existing) | result after extraction | before |
    |---|---|---|
    | `module_tests/exact_match.py` (sugar runner, CI path) | 6/6 OK | 6/6 OK |
    | `module_tests/iplookup.py` (same) | 2/2 OK | 2/2 OK |
    | ExactMatch metadata/canonicalization driver | all pass | all pass |
    | ExactMatch semantics driver | all pass | all pass |
    | ExactMatch 4-worker storm `min/p50` | 0.815, 0 drops | 0.816-0.884 |
    | IPLookup single-worker control / updates | 0.950 / 0.887 | 0.933-0.949 / 0.898-0.907 |
    | IPLookup 4-worker control / updates | 0.968 / 0.911 | 0.949 / 0.898 |

    Daemon RSS stayed bounded in every run (tens of kB across hundreds of
    retired generations), `core/all_test` is 185/185, and both modules compile
    clean under the tree's `-Werror` set with g++ and clang++.

    Next, and last for this phase: the `bessctl` command-thread-safety
    metadata, so the capability these two modules now have is reachable through
    `command module ...` rather than only through pybess/direct RPC.

38. **`8b1f0e90`** + **`afc7860f`** — **Phase J's last item: the capability through
    the normal CLI**. `command module` / `command gatehook` paused every worker
    unconditionally, so live table updates were reachable only through
    pybess/direct RPC. Two commits:

    - `8b1f0e90`: module/gatehook class introspection now reports command
      descriptors -- one shared `CommandInfo { name, arg_type, thread_safe }`
      in `bess_msg.proto` replaces the parallel `cmds`/`cmd_args` string arrays
      at tag 4 (tag 5 reserved), which is the clean cutover the
      no-backwards-compatibility policy calls for and implements the FIXME
      those fields carried. `ModuleBuilder::cmds()`/`GateHookBuilder::cmds()`
      return the real `Commands`/`GateHookCommands` instead of rebuilding
      pairs; pybess's dynamic wrapper and `show mclass`/`show gatehookclass`
      (which now prints thread safety) were updated in the same commit.
      `thread_safe` defaults to false on purpose: skipping a pause requires an
      explicit true.
    - `afc7860f`: bessctl skips pausing only when the daemon reports the named
      command as explicitly thread-safe. Unknown command, `thread_safe=false`,
      empty/missing metadata, an unresolvable module, or an unknown/ambiguous
      gatehook instance all keep the old pause-run-resume behavior. The
      daemon's `RunCommand()` EBUSY enforcement is untouched -- the CLI
      metadata is control-plane UX, the daemon remains the authority. The
      user-supplied `ARG_TYPE` contract is unchanged.

    Live acceptance (one daemon, workers running, commands issued through
    `bessctl`, pause/resume counted in the daemon log):

    | CLI command | workers paused? | result |
    |---|---|---|
    | `command module ipl add …` (THREAD_SAFE) | **no pause at all** | succeeded |
    | `command module acl clear …` (THREAD_UNSAFE) | pause + resume | succeeded |
    | `command module ipl nosuchcmd …` (unknown) | pause + resume | daemon's normal error |
    | `command gatehook nosuchhook ipl out 0 reset …` (unresolvable) | pause + resume | daemon's normal error |

    Not demonstrable live: a THREAD_SAFE *gatehook* command. The tree has
    exactly one gatehook command (`Track::reset`) and it is THREAD_UNSAFE, so
    the gatehook skip path is covered by the unit test only; the fallback and
    the instance lookup are both exercised above.

    Writing the unit tests (`bessctl/test_commands.py`, picked up by the CI's
    unittest step) caught a real bug in this change: the helpers were first
    inserted *between* `@cmd('command module ...')` and its `def`, and
    `cmd_decorator()` registers into `cmdlist` and returns `None` -- so the
    helper became the registered handler and both `command module` and
    `command gatehook` lost their registrations. A guard test now asserts no
    `cmdlist` entry is `None` and that both handlers are registered under
    their syntaxes.

    Follow-up from review (`d2fe52f6`): three defects in the consumers of the
    new metadata, one of them CI-confirmed.

    - **pybess module construction was broken** by the cutover:
      `setattr(self, cmd, ...)` passed the `CommandInfo` object instead of
      `cmd.name`, so every pybess module construction raised
      `TypeError: attribute name must be string, not 'CommandInfo'`. The claim
      in this entry's first draft that ordinary pybess construction exercised
      the new metadata end-to-end was wrong -- CI's sample configurations were
      the first thing to run that path, and they caught it.
    - **Gatehook resolution ignored direction.** Matching on `ogate` alone is
      wrong for gate 0 specifically, because an unset oneof scalar reads as
      0 there: a hook on an *input* gate looked like an output hook at gate 0.
      Direction is now part of the identity (`in` -> `igate`, `out`/None ->
      `ogate`, anything else refuses to guess), the match must be unique after
      filtering on which field the oneof actually set, and the tests cover
      in/out at gate 0, the pitfall case, nonzero gates, and the same hook
      name on both directions.
    - **The new test polluted `sys.path`** with an unnormalized
      `bessctl/../pybess`, which `pybess/bess.py`'s plugin scanner then
      visited twice, reporting duplicate protobuf definitions. The test uses
      `SimpleNamespace` instead of protobufs and keeps only one normalized,
      deduplicated path entry (needed solely because `commands.py` imports
      its sibling `sugar` by name).

    Evidence: the new `PybessWrapperTest` fails on the pre-fix code with the
    exact CI error and passes after; `unittest discover` runs 98 tests with no
    protobuf collision (the one remaining error is `test_samples`' subprocess
    python lacking `grpc` -- environment, not code); module tests and the
    metadata driver still pass; the live CLI acceptance re-run on a clean
    daemon yields exactly four pauses -- one setup, none for the THREAD_SAFE
    command, one each for the THREAD_UNSAFE, unknown, and unresolvable-gatehook
    cases.

## Review process established this session

For anything touching correctness-critical code (DPDK ABI/layout, build
system, concurrency), spawn a fresh `Agent` (not a fork — needs independent
eyes) with `model: "opus"`, pointed at the specific commit hash, asking it to
verify claims independently (re-derive offsets from real headers, trace call
sites, don't just trust the commit message). This caught two real bugs
(PCI-address format, `-no-pie`/shared-object breakage) that would have
shipped otherwise. **Keep doing this at every significant milestone commit** —
it's a standing instruction from the user, not a one-time thing.

**Module tests *are* locally runnable** (found while fixing `a0688fcf`, and
worth not forgetting, since earlier entries in this log claim otherwise): the
harness only needs a daemon it can reach. Start one with `--skip_root_check`
(`hub`/`bessd`), then run

```
bessctl/bessctl daemon reset -- run file bessctl/module_tests/<name>.py
```

against it -- `daemon reset` goes over gRPC, so no sudo is involved. That is
the real CI path (the sugar runner that rewrites `->`), which is how the
pre-fix/post-fix module-test evidence for `a0688fcf` was produced.

## Known issues / explicit follow-ups (not yet fixed)

- [x] **Per-queue PMD stats** (`pmd.cc`) — investigated further; this was
      over-scoped in the original DPDK-port writeup. What DPDK actually
      removed is `rte_eth_stats::q_ipackets/q_ibytes/q_errors/...`, a
      *hardware-reported, per-queue* breakdown. Traced every consumer of
      `Port::queue_stats[dir][qid]` (`port.cc`'s `GetPortStats()`,
      `port_inc.cc`, `port_out.cc`, `queue_inc.cc`, `queue_out.cc`): the
      `.packets`/`.bytes`/histograms that actually get reported are
      populated generically at the *module* level (driver-independent,
      counting what the module itself processed), not from the driver's
      per-queue hardware stats at all -- PMDPort never touched
      `queue_stats[PACKET_DIR_INC]` even before this port, and its
      `SendPackets()`'s `queue_stats[PACKET_DIR_OUT][qid].dropped` is
      software-tracked (tx-burst requested-vs-sent), not hardware-sourced,
      unaffected by the removal. And `protobuf/service.proto`'s
      `GetPortStats` RPC has always explicitly documented "per-queue
      stats are not supported" at the API level -- there is no external
      consumer that would even receive hardware per-queue data if it were
      reimplemented. Net effect of the upstream removal, for BESS
      specifically: **none** -- aggregate port stats (what
      `GetPortStats()` actually returns) already come from
      `rte_eth_stats`'s whole-port fields (`ipackets`/`opackets`/etc,
      still present), which `PMDPort::CollectStats()` already uses
      correctly. Building an `rte_eth_xstats_get()` name→id lookup/cache
      for data nothing consumes would be scope creep, not a fix -- closing
      this without further action. If a real need for hardware-level
      per-queue visibility surfaces later (e.g. a new debugging RPC), the
      `rte_eth_xstats_get()` approach sketched in the old note is still
      the right shape for it. (Old upstream PR #1007 is still relevant
      prior art if that need arises.)
- [ ] `DPDK_VER` is duplicated (`build.py` and `core/Makefile` each hardcode
      it) and must be bumped in both places by hand — documented with a
      comment, not structurally fixed (see `9e8c4af1` commit message for why
      the obvious fix — `core/extra.mk` — doesn't work: it's `-include`d too
      late relative to where `DPDK_INSTALL_DIR` is first used).
- [x] `core/kmod` (legacy out-of-tree VPort kernel module) — resolved by
      full removal rather than the originally-proposed "make it optional"
      (see the completed-work log entry for this, and Phase C below).
- [x] `.github/workflows/ci.yml` has now actually run against real GitHub
      Actions repeatedly this session (see commits 5, 12-ish onward) — the
      g++ job passes; the clang++ job needed 9 real portability fixes
      (commit 13) before it did too. Matrix/caching/runner behavior
      confirmed working, not just the underlying `build.py`/`make`
      invocations. **Correction (2026-09-19, entry 33 session): the
      "Smoke-test benchmarks" step is not a fast check on hosted runners.**
      Completed runs measure it at 14.6-18.5 min (before the allocator
      benchmark existed) and 19.1 min with it; the same loop takes 36 s
      locally, 28 s of that `cuckoo_map_bench` alone. So the whole job's
      ~26 min is dominated by the smoke step, and a *hang* there would have
      looked exactly like ordinary slowness — `2a541ba7` added a 30-minute
      per-binary `timeout` so a real hang names itself instead of eating the
      6-hour job limit. If CI wall time ever matters, `cuckoo_map_bench` is
      the first thing to look at, not the new benchmarks.
- [x] Commit author identity — resolved by moving to a machine whose
      `git config user.*` is correct (the `root@PARAM.localdomain` commits
      were the WSL sandbox's; entries 32/33 are authored properly). Old
      commits left as they are: amending pushed history still needs the
      user's go-ahead, and the value is cosmetic.
- [ ] `detach_all_worker_threads()` (added in `cfa82e3b`) abandons still
      actively-scheduling workers in the bare-`kill()`-without-pause-first
      shutdown case (not the normal `daemon stop` path, which pauses
      first) — matches pre-`49f1ec86` behavior exactly, so not a
      regression, but a stronger fix would call `destroy_all_workers()`
      before the detach loop in `core/main.cc`. Trade-off: that makes a
      wedged worker able to hang shutdown, which is presumably why the
      minimal fix was chosen instead. See commit 8 in the log above.

---

# Roadmap / Backlog

Organized in phases. Phases A–F are the original DPDK-era modernization plan
(mostly still ahead of us — Phase A is done, Phase B's Stage 1 has landed,
Stage 2 and Phases C–F remain). Phases G–I are a newer, larger proposal — a from-first-principles
rethink of the control plane and language/tooling stack — added 2026-09-11.
**G and the A–F track are largely independent** (G touches `bessctl`/gRPC/the
client side; A–F touch the dataplane/DPDK/build side); either can proceed
first. Read the "how G relates to A–F" note at the start of Phase G before
picking one. Phase J (RCU/QSBR live table updates) was added 2026-09-18
from an external DPDK-modernization review and is cross-cutting — see its
own intro for why it doesn't fit under A–I.

## Phase A — DPDK/build modernization (complete as of 2026-09-12)

- [x] DPDK 19.11.4 → 25.11.3 LTS port (commits 3–4 above)
- [x] Verify the rewritten CI workflow actually passes on GitHub Actions
      (both g++ and clang++ jobs green as of commit 13 / run 34700531733)
- [x] Reimplement per-queue PMD stats via xstats — investigated instead of
      implemented; turned out to be unneeded (see known issues above for
      why: no consumer, zero externally-visible impact from the upstream
      removal).
- [x] Audit remaining drivers (`vport.cc`, `pcap.cc`) for latent DPDK 25.11
      API drift beyond what compiled cleanly. `pcap.cc` has zero DPDK API
      surface (libpcap + BESS's own `Packet` only) — no drift risk.
      `vport.cc` touches exactly 4 DPDK EAL calls (`rte_zmalloc`,
      `rte_free`, `rte_malloc_virt2iova`, `rte_prefetch0`); cross-checked
      every call site's argument types/count against the real installed
      `deps/dpdk-25.11.3/install/include/{rte_malloc,rte_prefetch}.h`
      signatures — all match exactly, no drift. Could not live-test
      `vport.cc`'s actual runtime path (it requires `open("/dev/bess")`,
      which needs `core/kmod` loaded — already documented above as
      known-broken/not built in this sandbox, tracked as Phase C, not a
      new finding). Header-level verification is the strongest check
      available here.

## Phase B — Packet/mbuf architecture

End state (unchanged from the original proposal): stop mirroring `rte_mbuf`
byte-for-byte in `Packet`; make `Packet` a thin wrapper (ideally
`sizeof(void*)`) around a real `rte_mbuf*`, with BESS's own metadata moved
into DPDK's supported mbuf private-data area instead of a hand-maintained
shadow struct. This is the fix that makes Phase-A-style ABI-drift bugs
structurally impossible instead of merely caught by `static_assert`.

**This is now explicitly staged, not a single commit sequence** — see
"Stage 1" and "Stage 2" below. Before writing any Stage-1 code, two research
passes (one on `PacketPool`/DPDK's `priv_size` mechanism, one on the
blast radius of `Packet`'s layout assumptions across `core/`) found a
concrete reason the full end-state can't land as one shot the way Phase A's
DPDK port did:

- `PacketPool`'s custom mempools are sized as `sizeof(Packet)` per element
  (`core/packet_pool.cc:72`, `rte_mempool_create_empty(..., sizeof(Packet),
  ...)`), and `Packet` embeds a fixed `char data_[SNBUF_DATA]` array. A
  pointer-thin `Packet` needs "the object stored in the mempool" decoupled
  from "the C++ type used to manipulate it" — a real allocator redesign.
- `PMDPort::RecvPackets()`/`SendPackets()` (`core/drivers/pmd.cc:504-511`)
  hand `Packet**` straight to `rte_eth_rx_burst()`/`rte_eth_tx_burst()` —
  **real DPDK PMD drivers write actual `struct rte_mbuf` bytes directly
  into that array.** This only works today because `Packet` *is*
  `rte_mbuf`-shaped. A thin wrapper needs an explicit wrap/unwrap step at
  this exact hot-path boundary, i.e. a real redesign of the single hottest
  code path in the system, not a mechanical rename.
- Together these mean there's no safe intermediate/bisectable state between
  "`Packet` overlays `rte_mbuf`" and "`Packet` is a thin wrapper" — it's an
  atomic swap across allocator + PMD I/O + every direct field user at once.
  That's exactly the kind of large, hard-to-reverse, hot-path change that
  needs a dedicated design spike and explicit sign-off, not something to
  start opportunistically just because the benchmark prerequisite is done.
- Dynamic per-pool data-room sizing (jumbo frames, upstream `#1024`) has the
  *same* blocker (`Packet::data_` is a fixed-size embedded array used as
  the mempool element) — it moves to Stage 2 with the rest, it can't be
  bundled into Stage 1 the way the original text here proposed.

Requires a benchmark suite *before* either stage — see "Benchmark suite"
below: this now exists and passes green, covering packet access, batch
operations, and scheduler throughput (the PMD-forwarding leg still needs a
real or simulated NIC and isn't covered — see that section). Do not merge
either stage if it regresses any benchmark checked in there.

### Stage 1 — explicit private-area accessor (done, commit 16)

Give `Packet`'s pool-bookkeeping/metadata/scratchpad area (today the
`reserve_` union) an explicit, named type (`bess::BessPacketPrivate`,
`core/packet.h`) reached via DPDK's own documented `rte_mbuf_to_priv()`
instead of a union member that merely happens to sit at the right offset by
construction — **zero layout/ABI change** (pinned by a new
`Packet::CheckPrivLayout()` static_assert alongside the existing
`CheckMbufLayout()`), since BESS's mempools already configure DPDK's
`mbuf_priv_size`/`mbuf_data_room_size` correctly
(`PacketPool::PostPopulate()`, `core/packet_pool.cc`) — this stage only
changes how that byte range is *reached*, not what's in it or where.
Surfaced and fixed one real latent hazard this replaces: `wildcard_match.cc`
computed a metadata field's address via `buf_addr + offset` pointer
arithmetic (`Packet::mt_offset_to_databuf_offset()`, now deleted — it was
the function's only caller), relying on `metadata_` and the packet's data
buffer being part of one contiguous allocation at a fixed relative offset —
the same class of "offset math assumed correct by construction" hazard as
the ABI-drift bugs Phase A found twice already. Fixed to address metadata
fields via `Packet::priv()`/`metadata<T>()` directly. Also deduped two
overlapping `rte_mbuf`-offset-check mechanisms found in the same pass
(`Packet::CheckMbufLayout()` vs. the `check_offset` macro inside
`Packet::CheckSanity()`, `core/packet.cc` — the latter was a strict subset
of the former and dead code besides, never called anywhere in the tree).
Added `core/packet_test.cc` (new file — no gtest coverage of `Packet`
existed before) covering the new `priv()` plumbing's round-trip and field
ordering, plus basic multi-segment (`next_`/`nb_segs_`) chaining, which had
no coverage anywhere either. Added `BM_PacketMetadataAccess` to
`packet_bench.cc` to catch a cost regression in the new accessor path
specifically (sub-nanosecond, no measurable change vs. the other
benchmarks in that file). Verified: `core/all_test` 185/185 (183 + 2 new),
`run_module_tests.py` clean including `test_wildcardmatch_with_metadata`
(the live functional check for the fixed code path), live `bessd -m 0`
smoke-started with no crash (exercises `PacketPool` creation for 262144
packets, i.e. `InitPacket()`'s `priv()`-based `set_vaddr()`/`set_paddr()`
calls, at real scale). Left untouched, deliberately: the `rte_mbuf`-mirroring
union (`buf_addr_`, `data_off_`, `pkt_len_`, `next_`, etc.) and
`CheckMbufLayout()` — that's Stage 2's problem.

### Stage 2 — thin `rte_mbuf*` wrapper (not started, needs its own sign-off)

The actual `PacketRef`-over-`rte_mbuf*` wrapper, the `PacketPool` allocator
redesign, the `PMDPort::RecvPackets`/`SendPackets` wrap/unwrap redesign at
the `rte_eth_{rx,tx}_burst` boundary, dynamic per-pool data-room sizing
(jumbo frames), and revisiting `core/drivers/pcap.cc`'s
`reinterpret_cast<Packet*>(snb->next())`-style multi-segment chain walking
(fine while the `rte_mbuf` overlay still exists; becomes an issue once
Stage 2 removes it) — `vport.cc` had the same pattern but was removed
entirely along with `core/kmod`, see Phase C. See the original
modernization-plan analysis of
`core/packet.h` earlier in this project's history for the detailed design
sketch (`PacketRef`, offset-resolved `MetadataRef<T>`) — `BessPacketPrivate`
from that same sketch is now already real, see Stage 1 above. High risk,
hot-path-touching — needs a dedicated design spike and explicit user
sign-off before any code, not started now.

### DPDK-proposal review notes (2026-09-18)

An Opus review of an external DPDK-modernization proposal (see this doc's
"Roadmap / Backlog" intro and the new Phase J below for the full context)
found three things specifically relevant to Stage 2's scope, all verified
against the tree at `705782b3`:

- **The `paddr`/`vaddr` layer is dead code, not a subsystem needing
  stronger types.** `PacketPool::from_paddr()` (`core/packet_pool.h:69`,
  `packet_pool.cc:261`) and `Packet::paddr()`/`vaddr()` (`core/packet.h`)
  had `core/drivers/vport.cc` as their *only* consumers (confirmed via
  `git grep` at `90d908f7^`, call sites at `vport.cc:95,113,133,147,
  617,652,682`). Since commit 21 removed VPort, nothing reads any of
  them; `InitPacket()` (`packet_pool.cc:24-25`) still *writes*
  `vaddr_`/`paddr_` per packet into fields with no reader, and
  `sid_`/`index_` are written nowhere outside `core/packet_test.cc`. An
  external proposal suggested introducing `Iova`/`PhysAddr`/
  `VirtualAddress` strong types to describe this field — reject that:
  there is no live consumer left to type. **Stage 2 should delete the
  24-byte `immutable_` union from `BessPacketPrivate` (`core/packet.h`)
  instead.** This shrinks `SNBUF_RESERVE` and moves
  `SNBUF_METADATA_OFF`/`SNBUF_SCRATCHPAD_OFF`, so it's genuinely Stage 2
  territory (a layout change, pinned by `CheckPrivLayout()`), not a
  Stage-1 follow-up to do now.
- **A third Stage 2 blocker, not previously listed here:**
  `PacketPool::AllocBulk()` (`core/packet_pool.cc:118-145`) writes
  `Packet`'s mirrored mbuf fields with raw `_mm_store_si128` into
  `rearm_data_`/`rx_descriptor_fields1_`, hand-reproducing
  `rte_pktmbuf_reset()`'s two 16-byte stores. This is simultaneously a
  Stage 2 blocker of the same kind as the `PMDPort::Recv/SendPackets`
  `reinterpret_cast` already listed above, *and* an x86-only SSE
  intrinsic block that Phase D has to deal with. Stage 2 and Phase D
  should agree on who owns it before either starts.
- **Verdict on "Stage 2 is more important than it first appeared"**
  (the external proposal's headline claim): agree with the conclusion,
  disagree with the reasoning. It framed Stage 2 as "the point BESS
  stops carrying the pre-2018 DPDK memory model forward" — but the
  `kmod`/`VPort` removal already deleted the one subsystem
  (`BessPacketPool`'s self-managed-hugepage/physical-contiguity path's
  only unique consumer) that framing depended on. `--legacy-mem`
  (`core/dpdk.cc:126`) remains, but whether it's droppable is a
  live-hardware question this sandbox cannot answer (no hugepages, no
  NIC) — it is blocked on a test rig, not on Stage 2. What Stage 2
  genuinely unlocks, and the honest argument for its priority: **external
  mbuf buffers** (the real enabler for AF_XDP/vhost zero-copy, see Phase
  C), **per-pool data-room sizing** (jumbo frames, already listed above),
  and **`MBUF_FAST_FREE` eligibility** (see Phase C). Keep Stage 2's
  priority high for those three, not for a memory-model-lineage argument
  the kmod removal already partly resolved.
- **Also verified, and correctly rejected by the proposal itself: don't
  replace BESS metadata with mbuf dynamic fields.** BESS's metadata
  system is a graph-liveness-aware offset allocator; dynfields are a flat
  registry with a shared, tiny (36-byte) budget across every DPDK
  subsystem on the process (already noted in Phase B's own header
  above). Use dynfields only for genuine DPDK-ecosystem contracts (flow
  marks, hardware timestamps) if a future feature needs interop with
  another DPDK subsystem that reads them — not for BESS's own logical
  attributes.

### Benchmark suite (added 2026-09-12, commit 14)

Correcting an error in this doc's own earlier text (both here and in Phase
H below both used to claim "this repo does not have one yet"): **BESS
already had a `*_bench.cc` Google Benchmark suite** (`utils/checksum_bench.cc`,
`utils/copy_bench.cc`, `utils/cuckoo_map_bench.cc`,
`modules/url_filter_bench.cc`, `traffic_class_bench.cc`) and the
`core/Makefile` already has full `%_bench.cc` build-rule support
(`make benchmarks`) — none of this needed to be built from scratch. What
was missing: `build.py`'s `build_bess()` (what CI actually calls) only ran
`make -C core bessd modules all_test`, never `make ... benchmarks`, so
**this entire pre-existing suite had never been built or run against the
DPDK 25.11 port this whole session**, despite the port touching code
several of these benchmarks depend on (`checksum_bench.cc` in particular,
given commits 9-12's checksum.h correction chain). Verified all 5 by
actually running each one (not just compiling): all pass, no crashes, sane
throughput numbers. `cuckoo_map_bench` looked hung at first under a 30s
timeout with default `--benchmark_min_time` — false alarm, it just has 22
cases up to 4M entries and needs more wall time, not a bug; confirmed by
rerunning with `--benchmark_min_time=0.001s` (all 22 cases complete in
seconds).

Added `core/packet_bench.cc` — the one genuinely new file — covering what
the existing suite didn't: `Packet`/`PacketPool`/`PacketBatch` themselves
(`BM_PacketAllocFree`, `BM_PacketAllocFreeBulk`, `BM_PacketHeadData`,
`BM_PacketAppendTrim`, `BM_BatchForward`), using `PlainPacketPool` (the
only pool backend that doesn't need real hugepages, which this sandbox
lacks — see packet_pool.h's own doc comment: "For standalone benchmarks
and unittests"). These are the primitives Phase B would actually
refactor, so they're the most direct regression check for it.

Fixed the actual gap: added `benchmarks` to `build.py`'s `build_bess()`
target list, and added a "Smoke-test benchmarks" CI step (runs every
`*_bench` binary with a tiny `--benchmark_min_time` — a crash/hang check,
not perf tracking; CI runner variance makes real perf regression
detection unreliable there, that's a local/dedicated-hardware job) so this
gap can't reopen silently.

**What Phase B's benchmark prerequisite still doesn't cover**: PMD-level
forwarding throughput is now covered for the single-worker loopback shape
by `core/pmd_bench.cc` (entry 27 below: `net_null` TX + `net_ring`
round-trip, batch sweep 1-32) — still open: the cross-worker
`PortInc → Queue → PortOut` shape (belongs to the mempool experiment's
own harness, gated on this file plus the `DumpMempool()` fix) and real-NIC
numbers generally. Full Module/Gate/Task dispatch overhead is still
uncovered (no C++-level harness exists for constructing a `Module` +
calling `ProcessBatch()` outside the live daemon — all existing module
testing goes through `bessctl/module_tests/*.py` against a running
`bessd`, not a standalone gtest/benchmark binary).
`traffic_class_bench.cc` already covers scheduler throughput specifically
(`TCWeightedFair`/`TCRoundRobin` scheduling), which covers the
"scheduler throughput" leg of Phase B's requirement in full.

## Phase C — Linux I/O modernization

- [x] `core/kmod` (the legacy out-of-tree VPort kernel module -- `sn_host.c`,
      `sn_netdev.c`, `sn_ethtool.c`, `sndrv.c`, `sn_kernel.h`, its own
      `Makefile`/`install` script) and the `VPort` driver that was its only
      consumer (`core/drivers/vport.cc`/`.h`) were **removed entirely**
      (see the completed-work log), not merely made optional as originally
      proposed here -- upstream `#1056` already documented it as
      known-broken on modern kernels, and it was never built or tested by
      anything in this session's CI/build (`build.py`'s `build_kmod()`
      wasn't in the default `build_all()`/CI path either). Two things it
      owned that other, still-working code actually needed were kept, just
      relocated: `llring.h` (a general-purpose lock-free ring buffer used
      by `Queue`/`DRR` modules and `core/utils/lock_less_queue.h` --
      nothing to do with the kernel module itself) moved to
      `core/utils/llring.h`; `sn_common.h` (the vport/kmod IPC protocol
      structs) had no other consumer once `vport.cc` was gone, so it went
      too. `VPortArg` removed from `protobuf/ports/port_msg.proto`
      (dead once nothing registers a `"vport"` driver); 5 VPort-only
      sample `.bess` configs under `bessctl/conf/` removed (not part of
      any automated test, would have silently stopped working).
      `pybess/test_bess.py`'s `test_create_port` passed `'VPort'` as a
      string to a **mock** gRPC servicer defined in that same test file, so
      the servicer itself needed no change -- but this reasoning missed
      that `pybess/bess.py`'s `create_port()` does
      `getattr(port_msg, driver + 'Arg', EmptyArg)` **before the RPC is
      even sent**, so removing `VPortArg` broke the test client-side. A
      later review caught and fixed this (see completed-work log entries
      23-24) -- left as a cautionary note here since it's a good example
      of "verified the obvious half of a claim, missed the other half."
- [x] **Scope-corrected and substantially descoped (2026-09-18, DPDK-proposal
      review): `PMDPort` already accepts arbitrary DPDK vdevs.**
      `protobuf/ports/port_msg.proto` has `oneof port { port_id | pci |
      vdev }` and `core/drivers/pmd.cc`'s `find_dpdk_vdev()` (via
      `rte_dev_probe()` + `RTE_ETH_FOREACH_MATCHING_DEV`) already
      implements it. `deps/dpdk-25.11.3/install/lib/pkgconfig/libdpdk.pc`
      already links `librte_net_{vhost,tap,af_packet,memif,null,ring}.a`
      under `--whole-archive` -- **zero additional dependency cost** to
      use any of them. Concretely:
      - **vhost-user is already done.**
        `bessctl/conf/port/vhost/vhost.bess` already runs
        `PMDPort(vdev='eth_vhost_...')`. The "keep vhost-user for VMs, but
        first-class it via a modern PMD" framing below described work
        BESS already shipped years ago -- no new driver needed. Unverified
        residual: whether the `eth_vhost` devargs alias still resolves
        under DPDK 25.11 vs. the current `net_vhost` driver name; worth a
        smoke test when a rig exists, not a design question.
      - **TAP / AF_PACKET / memif / null / ring, and switchdev
        representors** (via `...,representor=vf[0-3]` devargs on an
        existing `pci`/`vdev` port) are likewise reachable with **no new
        BESS code** -- this item is a documentation/config-ergonomics
        task, not an engineering project.
      - **AF_XDP is the one real gap, and it's a build gap, not a code
        gap.** This tree's DPDK build produces no `librte_net_af_xdp.*`
        (confirmed absent from `deps/dpdk-25.11.3/install/lib/`) because
        DPDK's AF_XDP PMD needs `libxdp`/`libbpf` present at *DPDK* build
        time. Work item: add `libxdp-dev`/`libbpf-dev` to the DPDK build
        prerequisites (`build.py`, `.github/workflows/ci.yml`, `env/`),
        confirm `librte_net_af_xdp` appears, then expose `net_af_xdp`
        through the existing `vdev` arg with BESS-side config sugar.
        **Do not write a new `AF_XDPPort : Port` driver** -- that's new
        code to own for something ethdev already covers.
      - Small ergonomic follow-up once AF_XDP works: `PMDPortArg`
        currently exposes only `loopback` and three VLAN-offload
        booleans; long vdev devargs strings are the whole configuration
        surface today. A few named fields (queues, zero-copy mode,
        busy-poll) would be worth adding as BESS-level semantics, not raw
        devargs passthrough.
      - VFIO for physical NICs was already the normal `PMDPort` path
        before this review (`pci=` args) -- nothing new needed there
        either. The net effect of this whole scope correction: this phase
        is mostly a config-ergonomics and one-build-flag-addition task,
        not the ground-up multi-driver effort it read as before.
- [x] **Replaced `PMDPort::Init()`'s hand-rolled descriptor clamping with
      `rte_eth_dev_adjust_nb_rx_tx_desc()`** (2026-09-18, see completed-work
      entry 26 for hash and full verification matrix).
      Original proposal text (DPDK-proposal review, 2026-09-18):
      `core/drivers/pmd.cc` clamps `queue_size[]` against
      `dev_info.rx_desc_lim.nb_min`/`nb_max` and `tx_desc_lim.nb_min`/
      `nb_max` by hand -- but **ignores `nb_align`** (`rte_ethdev.h`,
      "Number of descriptors should be aligned to"), which several PMDs
      enforce and which causes a later `rte_eth_{rx,tx}_queue_setup()`
      failure rather than a silent adjustment.
      `rte_eth_dev_adjust_nb_rx_tx_desc()` is DPDK's own answer and
      handles min/max/align in one call. Net effect: ~36 lines of
      hand-rolled clamping deleted, one latent misconfiguration class
      closed -- same "offset/limit math assumed correct by construction"
      bug family as the two Phase A ABI-drift bugs. Configuration path
      only, no performance implication. Small, low-risk, do this one
      without a design spike.
- [ ] **A minimal, consumer-driven `PortCapabilities` in `PMDPort`**
      (DPDK-proposal review, 2026-09-18) -- explicitly **not** a
      speculative capability struct built ahead of its consumers.
      Verified: `PMDPort` does essentially no offload negotiation today
      (`rxmode.offloads = 0`, `txmode` never touched); the one existing
      negotiation is RSS hash types masked against
      `dev_info.flow_type_rss_offloads`. Introduce a small **internal**
      struct populated once from `rte_eth_dev_info` at `Init()`, and only
      as consumers land -- not exposed in the protobuf API (that's Phase
      G2's `GetCapabilities`, which already has the right rule: expose
      BESS semantics, never raw `RTE_ETH_*` flags, same principle as
      Phase B's `PacketRef` work). First three consumers, in order: (1)
      the `adjust_nb_rx_tx_desc` item above; (2)
      `RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE` gating -- benchmark-gated, and
      must be re-evaluated after Phase B Stage 2, since clones/external
      buffers break its "direct packet, refcount 1, single known pool"
      precondition; (3) `RTE_ETH_DEV_CAPA_RXQ_SHARE`/MT-lockfree-Tx
      gating, to let `PortOut` skip its per-queue MCS lock
      (`core/modules/port_out.h`/`.cc`) when the PMD advertises lock-free
      multithreaded Tx. All three need a real NIC to evaluate; none
      should land unbenchmarked.
      **Design constraint to record now so it isn't re-litigated later**:
      do **not** globally enable checksum/TSO offload. BESS's
      `IPChecksum`/`L4Checksum` modules conflate two semantics -- *verify
      an incoming packet and route failures* (cannot be replaced by a Tx
      offload) and *produce a correct outgoing checksum* (can be). If Tx
      checksum offload is ever adopted it must be explicit graph
      semantics (a distinct module or mode that zeroes the field and
      populates mbuf offload metadata), so a later module that rewrites
      the header after the offload metadata was prepared is a visible
      graph error rather than silent corruption. Same for TSO/GSO.
- [ ] Make VFIO the primary physical-NIC path and AF_XDP the primary Linux
      host/container path (once the `libxdp`/`libbpf` build-prereq item
      above lands); keep vhost-user for VMs (already available, see
      above). With kmod gone, there's no "kmod optional" fallback-direction
      flip left to design.
- [x] **Decouple `WorkerId` from DPDK's lcore ID; migrate off direct
      `RTE_PER_LCORE(_lcore_id)` writes -- done 2026-09-19, entry 33**
      (`3dc30b0e`): `Worker::Run()` now calls `rte_thread_register()` /
      `rte_thread_unregister()`, the startup log reports WorkerId, CPU and
      DPDK lcore id separately, and the before/after regression this item
      asked for (via `mempool_bench --lcore_mode=bess|register`) found the
      two indistinguishable at production cache settings with identical
      cache occupancy -- i.e. the cache is still in use. Original scoping
      text: **carefully, this is load-bearing, not vestigial** (DPDK-proposal
      review, 2026-09-18). `core/worker.cc`
      writes `RTE_PER_LCORE(_lcore_id) = arg->wid` directly into DPDK's TLS
      and asserts `wid == rte_lcore_id()` immediately after -- i.e. BESS
      today hardcodes `WorkerId == DPDK lcore ID`. **This is not
      vestigial**: `rte_mempool_default_cache(mp, rte_lcore_id())` keys
      the per-core mempool allocator cache on exactly this value, so
      without the write every worker would be `LCORE_ID_ANY` and every
      `Packet` alloc/free would silently bypass the per-core cache -- a
      real, currently-invisible performance dependency. Modern DPDK has a
      public, intended API for this: `rte_thread_register()` lets a
      non-EAL pthread acquire a valid lcore ID (and
      `rte_thread_unregister()` release it) instead of writing the TLS
      variable directly, which is exactly the kind of "reaching into a
      library's internals instead of using its public API" pattern that's
      already bitten this project once (the Phase A `rte_mbuf` ABI-drift
      bugs were the same class of hazard, just for a different DPDK
      internal). If this migrates, decouple the types explicitly
      (`WorkerId`, DPDK lcore ID, and physical CPU ID as three distinct
      things -- `WorkerId` is already a Phase H "adopt now" strong-ID-type
      candidate) and **benchmark mempool cache-hit rate before and after**
      -- don't assume `rte_thread_register()`'s ID allocation behaves
      identically to the current scheme under BESS's actual worker
      topology. Low urgency (nothing is broken today), but worth doing
      before any new DPDK library that also cares about lcore identity
      (QSBR included, see Phase J -- though note QSBR itself doesn't
      depend on this: `rte_rcu_qsbr_thread_register()` takes an arbitrary
      caller-chosen `thread_id`, not an lcore ID, so the two migrations
      are independent and don't need to be sequenced).

## Phase D — ARM64 + portable SIMD

Add `core/arch/{generic,x86,arm64}/` with a correct scalar reference
implementation always available; x86 SSE/AVX and ARM NEON selected at
startup via one dispatch per batch (not per packet/byte). Harvest upstream
PR `#1041` (ARM support) as reference material, not a mergeable diff — it
predates this DPDK port and the packet-layout work.

- [ ] **Replace the BPF module's execution backend with `rte_bpf`
      (experiment, strong prior toward adoption)** — DPDK-proposal review,
      2026-09-18, the single best maintenance-reduction candidate it
      found. `core/utils/bpf.cc` is **1066 lines of hand-written x86-64
      machine-code emission**, adopted from FreeBSD 10, that `mmap()`s a
      writable buffer, emits opcodes into it, and `mprotect()`s it
      executable. The entire file is inside `#ifdef __x86_64`; on every
      other architecture `BPF::Match()` (`core/modules/bpf.cc`) falls back
      to libpcap's `bpf_filter()` **interpreter** -- i.e. ARM64 currently
      gets no JIT at all for this module. DPDK 25.11 ships
      `rte_bpf_convert(const struct bpf_program *)`, which takes exactly
      what `pcap_compile_nopcap()` already produces
      (`core/modules/bpf.cc`), plus `rte_bpf_load()`/`rte_bpf_get_jit()`/
      `rte_bpf_exec_burst()`. `librte_bpf.a` is **already linked into
      `bessd`** (`libdpdk.pc`'s `--whole-archive` list) -- zero new
      dependency. If throughput is comparable, this deletes a home-grown
      JIT (a real security/maintenance liability -- BESS writes executable
      memory at control-plane request) and gives ARM64 a real JIT instead
      of an interpreter, i.e. Phase D parity, for free. Caveats to settle
      in the experiment, not assume: DPDK BPF isn't full eBPF (no maps,
      limited tail calls -- irrelevant for cBPF filters; don't let this
      become "BESS gets eBPF"); BESS's current JIT returns `SNAPLEN` on
      match / `0` otherwise while converted cBPF returns the program's own
      return value, so `Match()`'s `!= 0` test needs re-checking against
      the new semantics; `rte_bpf_load()` may need an EAL-initialized
      process, which the module-command path already satisfies
      (`current_worker.SetNonWorker()` is called there for exactly this
      reason) but should be confirmed. 26.07 adds direct cBPF loading and
      a hardened validator, making this direction cleaner after a future
      DPDK bump -- not a reason to wait, since 25.11 already has everything
      needed.
- [ ] **Runtime maximum-SIMD-width policy, not just CPU detection**
      (DPDK-proposal review, 2026-09-18). DPDK deliberately doesn't always
      select AVX-512 even when available, letting the application cap the
      vector width, because AVX-512 can speed up one kernel while costing
      enough core-frequency throttling to slow the whole pipeline down.
      Today BESS has **no runtime dispatch at all** -- ISA selection is
      entirely compile-time (`#if __AVX2__` in `core/utils/{copy,checksum,
      simd,bits}.h`, `#if !__SSE4_2__` in `simd.h`, plus bare
      `<x86intrin.h>` includes in `core/modules/set_metadata.cc` and
      `core/modules/http_parser.cc`), under `-march=$(CPU)` with
      `CPU ?= native` (which is also why CI needed `CPU=corei7`, commit
      15 in the completed-work log). Phase D's dispatch layer should bind
      implementation pointers from `min(detected ISA, configured policy)`,
      with the policy settable at startup, not from detected ISA alone.
      **Two x86-only hot blocks Phase D must take ownership of**, both
      found by the same review: `PacketPool::AllocBulk()`'s
      `_mm_store_si128` mbuf-reset (`core/packet_pool.cc` -- also a Phase
      B Stage 2 blocker, coordinate with that phase) and
      `IPLookup::ProcessBatch()`'s `_mm_set_epi32`/`_mm_shuffle_epi8`
      address gather (`core/modules/ip_lookup.cc`, see the `rte_fib` item
      below for a way to delete it outright rather than port it).
- [x] **Benchmark `rte_fib` vs `rte_lpm` for `IPLookup` — done 2026-09-19,
      entry 34 (`e55fb8a2`): FIB not adopted, so the deletion is *not*
      available yet.** The SSE gather block stays and `IPLookup` keeps
      `rte_lpm` (which also settles the table representation Phase J's
      pilot should build on). Reason is correctness, not speed: at 512K
      routes inserted in arbitrary order `rte_fib` returns next hops
      matching no rule, order-dependently, while `rte_lpm` passes the same
      independent-LPM gate in both orders; FIB's *build* and *update* costs
      are dramatically better (17x cheaper delete+add at 64K), so if a DPDK
      fix lands, re-enable the benchmark's 512K FIB case and revisit —
      `core/fib_bench.cc` is the guard for that. Original scoping text
      (DPDK-proposal review, 2026-09-18) — the value here is a deletion, not
      necessarily a speedup. Correction to the source proposal's own framing: BESS does
      *not* hand-code the x4 lookup itself -- it already calls DPDK's own
      `rte_lpm_lookupx4()` (`core/modules/ip_lookup.cc`). What BESS *does*
      hand-code is the x86-only SSE **gather** feeding it (`_mm_set_epi32`
      + `_mm_shuffle_epi8`, same file, inside an ISA `#if`).
      `rte_fib_lookup_bulk()` takes a plain `uint32_t[]`, so migrating
      would let BESS **delete that intrinsic block outright** and get
      DPDK's runtime-dispatched (including AVX-512, and NEON where
      available) lookup instead -- directly serving Phase D's stated goal
      of replacing BESS-local intrinsics with runtime dispatch.
      `librte_fib.a` is already linked. Both `rte_fib` and `rte_lpm`
      implement DIR24_8 internally, so raw lookup speed is likely a wash
      on small tables -- benchmark with realistic route populations (small
      edge table, near-full IPv4, mostly-/24, mixed prefix lengths,
      update-heavy), not random prefixes, and keep `rte_lpm` if it wins;
      the deletion is the win, not a guaranteed speedup. `rte_fib` also
      has native RCU integration, making it a natural second pilot for
      Phase J below if the migration happens.

## Phase E — Build system: migrate BESS itself to Meson

Only after Phase A/B's DPDK-facing churn has fully settled — bisecting a
simultaneous build-system + API migration is much harder than doing them in
sequence. `core/Makefile` can serve as the bridge in the meantime (it already
does, post-Phase-A: DPDK is consumed via pkg-config exactly the way a Meson
build would too).

## Phase F — Ops / release automation

Pin DPDK download by version+checksum (not just URL); produce signed
OCI images (amd64+arm64), a `.deb`, a `pybess` wheel, and an SBOM per
release; add Renovate/Dependabot. Revive per-queue/pool/scheduler metrics
(old upstream PR `#1007` had the right idea) as a small Prometheus exporter
outside the dataplane hot path.

- [x] **Made `DumpMempool()` backend-independent** (2026-09-18, entry 31).
      Original proposal text (DPDK-proposal review, 2026-09-18):
      `core/bessctl.cc` does
      `reinterpret_cast<struct rte_ring*>(mempool->pool_data)`, which is
      only valid because `PacketPool::PacketPool()` hardcodes
      `rte_mempool_set_ops_byname(pool_, "ring_mp_mc", ...)`
      (`core/packet_pool.cc`). Not a live bug today -- correct by
      construction, same shape as Phase A's two ABI-drift bugs and Phase
      B Stage 1's `mt_offset_to_databuf_offset` -- but it's a hard blocker
      on ever evaluating another mempool backend (see the benchmark
      backlog below). Rewrite using `rte_mempool_avail_count()`/
      `rte_mempool_in_use_count()`/`rte_mempool_dump()`. **Do this before,
      not during, the mempool-backend benchmark.**
- [ ] **DPDK telemetry / `pdump` / CTF tracing** (DPDK-proposal review,
      2026-09-18) — mostly not worth pursuing, recorded so it isn't
      re-proposed cold:
      - Telemetry v2: don't build a second, competing monitoring API next
        to BESS's own control plane. If ever used, it's a *source* for
        this phase's Prometheus exporter (DPDK-native diagnostics, PMD
        xstats), not a user-facing surface. Unverified: whether it
        actually initializes given `bessd` passes `--no-shconf` (see
        `pdump` below for why that flag matters) -- needs a live check
        before relying on it for anything.
      - `pdump`/`dumpcap`: **blocked today, not just low-priority.**
        `dpdk-pdump`/`dpdk-dumpcap` are DPDK secondary processes;
        `bessd` passes `--no-shconf` (`core/dpdk.cc`), which makes
        `rte_eal_config_create()` return early without creating the
        shared-memory config a secondary process needs to attach to. Its
        own comment explains why: so BESS doesn't interfere with other
        DPDK applications. Adopting `pdump` means giving that up -- a real
        trade-off, not a free diagnostic win. Not worth it: BESS's gate
        hooks already capture at arbitrary internal graph locations,
        which `pdump` can't reach anyway (NIC-boundary only).
      - CTF tracing: the motivating pitch (that it would have helped find
        this session's worker-teardown concurrency bugs) doesn't hold up
        -- commit 7's `std::terminate()` bug was found by *reproducing a
        crash*, and the `all_tcs_` race was found by *reasoning about an
        unsynchronized global*, neither of which a timestamped event log
        surfaces. `rte_trace` instruments DPDK's own internals; adopting
        it for BESS-defined events would be a modest improvement over
        interleaved glog lines at best. Low priority, park it.
- [ ] **DPDK version discipline: stay on 25.11.3 LTS through Phase B–D**
      (DPDK-proposal review, 2026-09-18). 26.03/26.07 have relevant work
      (hash RCU deferred-free, richer BPF including direct cBPF loading
      and a hardened validator, ACL custom allocators), but none justifies
      leaving a stable baseline mid-refactor. **One forward hazard to
      pre-register**: DPDK 26.07 reportedly changes the mempool cache
      refill/flush algorithm and makes effective cache size match the
      requested size, with an upstream warning that pipelined applications
      allocating on one lcore and freeing on another may need retuning --
      which describes BESS's `PortInc(worker A) → Queue → PortOut(worker
      B)` shape exactly (unverified here against the actual 26.07 release
      notes; flagged from the source review, worth confirming before it
      matters). The mempool cache-size benchmark in the backlog below
      should exist *before* the next DPDK bump, so the bump has a
      baseline to compare against, not after.

---

## Phase G — C++-only control plane (proposed 2026-09-11, not started)

**Read this before starting:** `bessd` **already implements its control
plane in C++** — `core/bessctl.cc` is the gRPC `BESSControl::Service`
implementation (create ports/modules, connect gates, pause/resume workers,
stats, ~67KB of C++). Python's actual role today is: gRPC *client*
(`pybess/bess.py`), interactive CLI (`bessctl/cli.py`, `bin/bessctl`),
`.bess` DSL interpreter (executable-Python-flavored config language, see
`bessctl/sugar.py`), and test/tooling layer. This phase is about replacing
*that* layer, not touching the dataplane or (in the compatible variant) the
daemon's RPC surface. It is therefore independent of Phases A–F and can be
staffed/scheduled separately.

Two variants are on the table — **get explicit user sign-off on which one
before writing code**, since G2 is an intentional breaking change to the
wire API:

### G1 — Compatible variant (keep the existing gRPC API)

Reach C++ feature parity with the Python client *while Python still works*,
then make Python optional. Sequence (each step independently shippable):

1. Extract a `ControlPlane` C++ class from `core/bessctl.cc` so the gRPC
   handlers become thin adapters (`FromProto → ControlPlane call → ToProto`)
   instead of having business logic inline in RPC handlers. This benefits
   *everything* downstream (CLI, tests, a future REST/gNMI surface) and is
   good groundwork regardless of whether G1 or G2 proceeds further.
2. Build `libbessclient`: a typed C++ wrapper (`class BessClient`) around the
   generated gRPC stubs — `expected<PortInfo, Error> create_port(...)` etc.,
   not raw protobuf manipulation at call sites.
3. Build a minimal C++ `bessctl` (new `tools/bessctl/` or `cli/`) covering
   `show`/`list`/`create`/`destroy`/worker ops/module commands — enough for
   parity on the common path, not everything on day one.
4. Interactive completion/help parity (daemon-driven: `bessd` should expose
   `ListModuleClasses`/`DescribeModuleClass`/`ListPortDrivers`/etc.
   reflection RPCs so the CLI doesn't need to statically know every module's
   shape — this also kills `pybess`'s current dynamic-import-scanning trick
   for discovering `*Arg`/`*Response` message types, replacing it with
   protobuf descriptor reflection).
5. Define a `GraphSpec`/`Pipeline` protobuf message (ports+modules+
   connections+workers+traffic-classes as one struct) as a common IR that
   `.bess` files, a new C++ DSL, YAML/TOML, or hand-written C++ can all
   compile down to. Add an `ApplyGraph` RPC that validates/plans/commits as
   one atomic-ish operation instead of the current one-RPC-per-mutation
   sequence (`ResetAll; CreatePort; CreateModule; ...; ConnectModules; ...`).
6. New C++ config parser targeting `GraphSpec` (see "DSL note" below).
7. Automated compatibility corpus: every existing `.bess` file in the repo
   (and any others available) run through both the old Python path and the
   new C++ path, diffed.
8. Port module/integration tests from Python (`bessctl/module_tests/*.py`)
   to a C++ harness (GoogleTest-based `PipelineTest` fixture sketched in the
   proposal — packet construction via a small typed builder replacing most
   Scapy use, PCAP-fixture-based tests for the rest).
9. Make Python tooling opt-in (`BUILD_PYTHON_BINDINGS=OFF` default) once (7)
   and (8) give confidence.
10. Eventually: move the legacy Python `bessctl` to `legacy/` or split it
    into its own repo, if/when the user decides to.

Target dependency footprint for a normal install, once done: `bessd` needs
libstdc++/DPDK/protobuf/gRPC-C++/libnuma/system libs — no Python, no pip, no
virtualenv, no scapy/Flask, no protobuf-Python-vs-C++ version skew.

Explicitly **keep**: gRPC as the transport (process isolation, remote mgmt,
language-neutral schema — protobuf already generates C++/Go/Java/Python/Rust
bindings, so the schema is a better interop boundary than a C++ ABI), and
**keep `bessctl` and `bessd` as separate processes** even once both are C++
(unprivileged CLI vs. privileged/high-perf daemon — don't fold the CLI into
the daemon just because "it's all C++ now"). A Unix-domain-socket gRPC
transport (`unix:///run/bess/bessd.sock`) is worth adding alongside the
current TCP one, for local-management permissions/isolation.

### G2 — Breaking-change variant (redesign the wire API too)

Only pursue this if the user explicitly says wire/API compatibility doesn't
matter. Same end state as G1 (Python eliminated as an architectural
dependency) but *also* replaces the current procedural, fine-grained RPC
surface (`CreatePort`/`CreateModule`/`ConnectModules`/`AddWorker`/... as
~30+ separate imperative RPCs) with a smaller, desired-state-oriented v2 API
centered on one `Pipeline` message:

```protobuf
service Bess {
  rpc GetSystem(...) returns (System);
  rpc GetCapabilities(...) returns (Capabilities);
  rpc ValidatePipeline(Pipeline) returns (ValidationResult);
  rpc ApplyPipeline(ApplyPipelineRequest) returns (ApplyResult);
  rpc GetPipeline(...) returns (Pipeline);
  rpc DiffPipeline(...) returns (PipelineDiff);
  rpc ListModules(...) returns (ListModulesResponse);
  rpc GetStats(...) returns (Stats);
  rpc WatchStats(...) returns (stream Stats);      // streaming, not polling
  rpc WatchEvents(...) returns (stream Event);
  rpc Shutdown(...) returns (...);
}
```

Design rules for this API if pursued:
- Client submits desired state (`Pipeline`); `bessd` validates/plans/commits
  it — not "client orchestrates a sequence of mutations."
- `oneof` for every built-in module's config (typed, no client-side dynamic
  discovery needed); reserve `google.protobuf.Any` for genuine third-party
  plugin extension points only.
- Don't leak DPDK concepts into the schema (`PortCapabilities{rx_checksum,
  tso, rss, ...}`, not `rte_eth_dev_flags` verbatim) — same "expose BESS
  semantics, not backend implementation details" rule as the `PacketRef`
  work in Phase B.
- Keep the API coarse-grained (`ValidatePipeline`/`PlanPipeline`/
  `ApplyPipeline`/`PatchPipeline`, not one RPC per internal C++ setter) —
  this also simplifies synchronization/locking inside `bessd`.
- A custom `protoc-gen-bess` plugin (proto options like `option
  (bess.module) = { name: "Rewrite" input_gates: 1 ... }`) could generate
  the module registry entry, CLI help/completion metadata, SDK builder
  wrappers, and docs from one source of truth in the `.proto` file, deleting
  a lot of hand-written registration boilerplate. Worth prototyping early if
  G2 is chosen, since it changes how every subsequent module gets added.
- Plugin ABI: stop exposing C++ internals as the plugin contract. A tiny
  stable `extern "C" const bess_plugin_v1* bess_plugin_init_v1();` entry
  point returning a descriptor (ABI version, module/port descriptors,
  protobuf `FileDescriptorSet`, factory pointers) avoids C++
  name-mangling/STL-ABI coupling between `bessd` and plugins built with a
  possibly-different compiler/stdlib, at effectively zero runtime cost
  (plugin dispatch is already a dynamic boundary).
- Tooling: **Buf CLI** for proto formatting/linting/breaking-change
  detection/codegen (`buf format`, `buf lint`, `buf breaking --against ...`
  once v2 is declared stable); **Protovalidate** for structural/semantic
  field constraints (ranges, required fields, PCI-address shape) via CEL —
  but topology-level validation (gate compatibility, NUMA constraints,
  scheduler structure) stays hand-written C++, it's not a message-shape
  problem.
- SDKs: generated stubs plus a thin ergonomic wrapper per language (sketch
  in the proposal shows C++/Go/Rust builder APIs that all compile down to
  the same `Pipeline` proto) — don't ship only raw generated code.

### DSL note (applies to either G1 or G2)

Don't try to preserve `.bess`'s "config is executable Python" trick by
embedding a Python interpreter in the C++ daemon/CLI — that's exactly the
dependency this phase is trying to remove, just moved one layer down. If a
DSL is wanted at all (vs. `textproto`/JSON-protobuf/a typed C++ builder,
which can ship first and cover power users immediately), the proposal
suggests **lexy** (a compile-time-grammar C++ parsing library) as a
reasonable choice for parsing directly into `GraphSpec`/`Pipeline` — no
`eval()`, no embedded runtime. A config language needs variables, objects,
arrays, loops, conditionals, functions/templates, env-var lookup, and
includes; it does not need classes, threads, reflection, arbitrary syscalls,
or metaclasses — keep it deliberately smaller than a general-purpose
language so configs stay statically validate-able.

---

## Phase H — C++23/26 tooling adoption (proposed 2026-09-11; first step landed 2026-09-17)

Applies mainly to the control-plane/CLI/SDK code from Phase G — the
dataplane should stay on the more conservative C++20-or-newer baseline from
the earlier (Phase B-adjacent) modernization plan discussion unless a
specific feature is proven zero-cost there (this whole build is now
compiled as one target at one `-std=`, so in practice that means: don't
write new C++23-only idioms in dataplane files, not a hard compiler-enforced
split yet — see the still-open "compile targets should differ deliberately"
item below).

**Toolchain reality check** (this doc previously cited "GCC 16.2 is
current" as if that were installed; it isn't, here or in CI): this sandbox
and CI's `ubuntu-24.04` runners both have **GCC 13.3.0 and Clang 18.1.3**
(Ubuntu 24.04's repo packages). Both accept `-std=c++23`; **neither accepts
`-std=c++26`** (GCC 13.3 rejects it outright as an unrecognized flag) — so
until this project's build environment moves off Ubuntu 24.04's stock
toolchain, C++26 isn't adoptable as a build-wide baseline here regardless of
any individual feature's maturity. The build is now (commit 19) actually
compiled with `-std=c++23` — previously it was still pinned to `-std=c++17`
despite this section's text implicitly assuming C++23 was already the
baseline. Treat C++26 features as isolated experiments only (e.g. a
separately-`-std=c++26`-compiled `.a`, per the `std::simd` sketch below),
not production dependencies, until both the standard and this project's
toolchain mature together.

**C++26 experiment (2026-09-17, not adopted, informational)**: user asked
to actually try newer toolchains rather than stop at "the stock one can't do
it." Installed `gcc-14`/`g++-14` (14.2.0) and `clang-20`/`clang++-20`
(20.1.2) via `apt` from Ubuntu 24.04's own `noble-updates/universe`
repo (no PPA/third-party script needed for these two -- both are already
packaged there) into this sandbox, **without** touching `update-alternatives`
(so plain `gcc`/`g++`/`clang++` still resolve to the 13.3.0/18.1.3 CI-matching
versions; the newer ones are invoked explicitly as `g++-14`/`clang++-20`).
Tested both against this exact codebase in a throwaway git worktree
(`-std=c++26`, otherwise identical source to commit 19):
- **`clang++-20` at `-std=c++26`: builds this entire codebase clean, zero
  errors, zero code changes needed beyond what commit 19 already did.**
  `core/all_test` 179/179 (excluding the pre-existing `CodelTest` flake).
  This is a genuinely useful data point: it means nothing in this
  codebase's actual C++ *usage* is C++26-incompatible under a compiler
  that implements it -- the barrier is purely toolchain availability
  (CI/this sandbox's stock compiler), not this project's code.
- **`g++-14` at `-std=c++26`: compiles every source file with zero errors**
  (same result as clang++-20 for the actual language/library usage), but
  **fails at the link step for every single binary** (`bessd`, `all_test`,
  and all three `*_bench` targets that got that far) with `lto1: fatal
  error: bytecode stream in file '.../libunwind.a' generated with LTO
  version 13.1 instead of the expected 14.0`. This is Ubuntu's system
  `libunwind-dev` package shipping a "fat LTO" static archive built by
  the distro's default GCC (13), which GCC 14's LTO reader can't consume
  -- an orthogonal distro-packaging/LTO-version mismatch, **not** a C++26
  or BESS-code issue (every linked binary hits it identically, regardless
  of `-std=`). A newer GCC (15, 16 -- the user separately suggested the
  `ubuntu-toolchain-r/test` PPA for these) would very likely hit the exact
  same mismatch, since the gap versus GCC 13's bytecode only grows; fixing
  it needs either a matching newer `libunwind` build or changing how this
  Makefile links against it (not attempted here -- out of scope for an
  experiment, and this repo's static-linking choices elsewhere in the
  Makefile look deliberate, not accidental, so that would need its own
  investigation before touching it).

**Conclusion, not acted on further**: this is good news about the
codebase's own forward-compatibility, but doesn't change the recommendation
above -- C++26's *library* features that would actually be worth adopting
(reflection, contracts, `std::simd`) remain explicitly experimental even in
compilers that implement C++26's language core, per the research in the
subsection above. Installing a newer compiler didn't change that maturity
verdict, only confirmed this project's code isn't itself a blocker. Not
adopted as the project's toolchain or default `-std=`; recorded here so a
future session doesn't have to re-derive "does our code even work under
C++26" from scratch, and knows the GCC-side link failure is a known,
separate, orthogonal issue rather than something to debug as a C++26
problem.

### Compiler/language-ecosystem hardening research (added 2026-09-17)

User asked to survey what current C++ language-level and compiler-ecosystem
work (the kind of thing Lemire/Sutter/Godbolt et al. keep publishing about)
could plausibly move more of this codebase's bug classes to compile time or
harden it for free, and to fold anything worthwhile in here rather than
start implementing — this phase (and Phase I) already exists for exactly
that purpose. **Nothing below is adopted or scheduled; it's backlog input
for whoever picks up Phase H.** Every item's real-world performance cost
(where known) is stated explicitly, per standing instruction to always
check that before recommending a hardening feature — a feature that
"eliminates a bug class" by adding cost to the packet hot path is a
tradeoff decision for Phase H to make deliberately, not something to
enable blindly.

- **`-fhardened` (GCC ≥14, GNU/Linux only)** — one meta-flag bundling
  `-D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS -ftrivial-auto-var-init=zero
  -fPIE -pie -Wl,-z,relro,-z,now -fstack-protector-strong
  -fstack-clash-protection -fcf-protection=full`; it only fills in flags
  not already set on the command line, so it composes safely with this
  repo's existing `-Wall -Wextra -Werror` etc. **Performance**: most of
  these sub-flags are the same low-single-digit-percent-or-less overhead
  already standard in production Linux builds (stack protector, RELRO,
  CET) — but `-D_GLIBCXX_ASSERTIONS` specifically (libstdc++ container/
  smart-pointer precondition checks) has measured up to ~6% slowdown in
  some libstdc++ versions on containers-heavy code, and
  `-ftrivial-auto-var-init=zero` adds a real per-function stack-zeroing
  cost. **Recommendation for this repo**: safe to turn on for
  `bessctl`/control-plane C++ (Phase G) without much thought; for
  `bessd`'s dataplane build, benchmark `_GLIBCXX_ASSERTIONS` and
  `-ftrivial-auto-var-init` specifically against `core/*_bench.cc` before
  enabling — don't assume the "it's usually fine" number applies to a
  per-packet hot loop without checking, the same way Stage 1's
  `priv()`/`metadata()` change was checked by diffing actual generated
  assembly rather than trusting noisy sub-nanosecond timings alone (see
  commit 17 above — that objdump-diff technique, not just
  `--benchmark_min_time`, is the right verification method to reuse here).
  ([OpenSSF hardening guide](https://best.openssf.org/Compiler-Hardening-Guides/Compiler-Options-Hardening-Guide-for-C-and-C++.html))
- **C++26 Standard Library Hardening (P3471R4, P3697, P3878)** — turns
  many stdlib UB cases (`std::vector::operator[]` out of bounds, null
  smart-pointer deref, etc.) into a terminating contract violation instead
  of silent corruption, once compilers ship it as the standard's default
  rather than a vendor flag. GCC 15/16 and MSVC 19.44 already partially
  implement the underlying checks today via `_GLIBCXX_ASSERTIONS` (same
  mechanism `-fhardened` above enables) — so this is not a new thing to
  adopt, it's the standardization of what's already actionable today.
  Track it for when it becomes default-on rather than opt-in.
  ([P3471R4](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3471r4.html))
- **GCC `-fanalyzer`** — evaluated, not a good fit yet: GCC's own C++
  support for it remains weak, and it shows a higher false-positive rate
  than tools like Cppcheck specifically on template-heavy C++ (this
  codebase's `core/utils/*.h` templates, `Module`'s CRTP-adjacent patterns,
  etc.) — revisit once its C++ mode matures rather than adding it to CI
  now and fighting noise.
- **C++26 reflection (`<meta>`) and contracts** — confirms this doc's
  existing caution was already correctly calibrated, not outdated: GCC
  16.1 shipped both, but GCC's own release notes say explicitly **"not
  recommended for production use"** — incomplete, may still change before
  the final standard's implementations converge. No change to the
  "revisit once non-experimental" stance already in this phase.
- **C++26 `std::simd` (§29.10)** — likewise still incomplete in both GCC
  (`simd.loadstore`/`simd.permute.dynamic` missing, `simd.math` partial)
  and Clang (further behind) as of mid-2026. Confirms the existing
  "prototype only, compare against scalar/x86/ARM kernels before ever
  defaulting to it" stance in this phase needs no change yet.
- **C++29 "Profiles" (P3589 framework + P3984 type-safety profile,
  Dos Reis/Stroustrup)** — the actual successor to the "Safe C++"
  proposal that flamed out of C++26 consideration. Proposes
  `[[profiles::enforce(...)]]`/`[[profiles::suppress(...)]]` annotations
  turning on compiler-enforced Bounds/Lifetime/Type/Initialization safety
  subsets *as a first-class language mechanism*, opt-in per
  TU/block/statement. This is directly relevant to Phase I's whole
  premise (hand-rolled strong IDs, scoped enums, etc., to catch invalid
  states at compile time) — **not shippable now** (targeted C++29, still
  a draft), but worth tracking: if/when it lands, some of Phase I's
  hand-written patterns might become expressible as
  `[[profiles::enforce(bounds)]]` annotations instead of bespoke wrapper
  types, which would be less code to maintain for the same guarantee.
  Don't design Phase I around a draft proposal today; just don't be
  surprised if this changes the shape of "how" later.
  ([P3589R3](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p3589r3.pdf),
  [Stroustrup's type-safety profile](https://www.stroustrup.com/type-safety-profile.pdf))

**Adopt now (C++23, control-plane/CLI/SDK only):**
- `std::expected<T, Error>` as the standard fallible-call return type through
  `ControlPlane` and the C++ client SDK — replaces mixed
  integer-error/protobuf-error-field/exception conventions.
- Strong ID types (`enum class GateId : uint16_t {}` or a tagged `Id<Tag, T>`
  template) for `GateId`/`WorkerId`/`QueueId`/`PortId` — same technique the
  earlier dataplane-modernization plan already proposed for `gate_idx_t`
  etc., extended to the control-plane types too.
- Concepts (`BessModule`, `PortDriver`) to constrain module/driver interfaces
  at compile time instead of by convention/documentation.
- `consteval` parsers for network literals (`"10.0.0.1"_ipv4`,
  `"02:00:..."_mac`) — malformed literals become compile errors.
- `std::span`/`std::string_view` at API boundaries; `std::print`/`std::format`
  for CLI output; `std::filesystem` for plugin/config paths;
  `std::jthread`/`stop_token` for management-side background jobs;
  `std::pmr::monotonic_buffer_resource` for per-`Apply`-call arena allocation
  during graph validation/planning (freed as one block when the call
  completes — good fit for a transactional config-apply model).
- Compile targets should differ deliberately:
  `libbess-dataplane`: `-fno-exceptions` (verify nothing in
  dataplane code actually needs them first — the earlier plan noted this
  looks compatible with current practice, but didn't confirm it repo-wide);
  `bessd` control / `bessctl`: normal exception-enabled C++23. Don't let
  gRPC/protobuf's compilation requirements leak into the packet engine's
  build flags, or vice versa.

**Prototype only, not production-required yet (C++26, isolated):**
- `std::simd` for checksum/TTL/header-compare/rewrite kernels — build as a
  separate `libbess-simd26.a` compiled with `-std=c++26`, exposing
  C++23-compatible functions to the rest of the daemon, so this doesn't
  force the whole project onto an experimental standard. Compare against
  the existing scalar/x86/ARM kernels from Phase D before ever defaulting to
  it.
- RCU-style graph publishing (`active_graph.load/store` with
  acquire/release, `WaitForWorkersToQuiesce`, reclaim old graph) as the
  mechanism for live reconfiguration without a full pause/resume cycle.
  Design a `bess::RcuDomain` interface *now*, backed initially by DPDK's own
  QSBR/RCU primitives or a hand-rolled epoch scheme; swap in `std::rcu`
  underneath later once standard-library support exists, without changing
  callers. Do not block on C++26 `std::rcu` itself.
- Reflection (`<meta>`) and Contracts (`pre`/`post` assertions with
  ignore/observe/enforce build-mode semantics) are both explicitly
  interesting for killing registration-macro boilerplate and formalizing
  invariants respectively, but GCC 16 requires `-freflection` and calls both
  experimental — revisit once a compiler ships them non-experimentally.
- Do **not** adopt C++ Modules yet (build-time win only, not correctness/
  runtime, and still experimental in GCC 16). Use good header hygiene +
  PCH + ccache/sccache + include-what-you-use/clangd include-cleaner instead.

**Build/test/perf tooling to bring in alongside this phase** (most of this
is independent of G/H specifically and would benefit Phase A–F work too):
Meson+Ninja+pkg-config (DPDK itself is Meson-based and documents pkg-config
as the recommended consumption path — this repo already moved `core/`'s DPDK
consumption to pkg-config in Phase A, ahead of a full Meson migration);
CLI11 for `bessctl` argument parsing; replxx for the interactive shell
(BSD-licensed, not GPL-readline-coupled); GoogleTest/GoogleMock (already a
natural fit — `core/` already uses GoogleTest); Google Benchmark for a real
microbenchmark suite (already exists and is now actually built/run by CI —
`BM_PacketHeadData`/`BM_PacketAllocFree`/`BM_BatchForward` etc. in
`core/packet_bench.cc`, plus the pre-existing checksum/copy/cuckoo_map/
url_filter/traffic_class benchmarks; see Phase B's "Benchmark suite"
section for the full story and an `ExactMatch`-style module benchmark
gap that's still open); libFuzzer +
ASan/UBSan/TSan/MSan; clang-tidy/clang-format/clangd/include-what-you-use.
ThinLTO/PGO/BOLT are finishing-tools territory — only worth evaluating after
functional/performance parity is otherwise established, trained on a
representative corpus (PMD RX/TX, exact-match, ACL, NAT, rewrite, scheduler
traversal, vhost-user, AF_XDP, mixed packet sizes — not just `Source ->
Sink`, or the optimizer tunes for the wrong distribution).

**Explicitly avoid, per this proposal:** coroutines in the dataplane;
`shared_ptr`/`std::function` in per-packet paths; ranges in hot loops purely
for style (simple loops stay easier to verify in generated assembly);
exceptions for *expected* errors anywhere; hiding `native_mbuf()`-style
DPDK escape hatches from advanced modules (Phase B's `PacketRef` should
still expose the real `rte_mbuf*` for code that needs it).

## Phase I — Compile-time invalid-state prevention (proposed 2026-09-12, not started)

See Phase H's "Compiler/language-ecosystem hardening research" subsection
above (added 2026-09-17) for a survey of current standard-track/compiler
work in this exact space — most relevantly, the C++29 "Profiles"
(`[[profiles::enforce(...)]]`) effort, which could eventually let some of
this phase's hand-rolled patterns be expressed as annotations instead of
bespoke types. Not shippable yet; noted there so it isn't re-researched
from scratch later.

Cross-cutting principle proposed alongside G/H, prompted by a pattern in
this session's own bug log: the `rte_mbuf` ABI drift, the PCI-format bug,
ignored return values, and `DCHECK`-only invariants (disappears under
`-DNDEBUG`) were all bugs the type system could plausibly have caught at
compile time, while the `worker.cc`/`all_tcs_` races (commits 6-8) were
not — they're lifetime/synchronization bugs, found only by actually
running the daemon under load. Keep that distinction explicit rather than
over-claiming what stronger types buy: **use the compiler aggressively for
identity, ownership, units, layout, and interface shape; use runtime
synchronization and load-bearing tests for concurrency and lifetime
ordering.**

Several of the highest-value items are already captured in Phase H above
— `std::expected`, strong ID types (`GateId`/`WorkerId`/`QueueId`/`PortId`),
concepts (`BessModule`, `PortDriver`), `consteval` network literals,
`std::span`, `std::jthread` for management-side jobs, ABI `static_assert`s.
Don't duplicate those; this phase covers what isn't in Phase H yet:

- **Scoped enums** for status/policy values currently mixed with plain
  integers (`worker_status_t`, `resource_t`'s `RESOURCE_COUNT`/
  `RESOURCE_CYCLE`/etc., traffic-class policy identifiers) → `enum class`.
  Zero runtime cost; stops accidental cross-domain comparison/arithmetic.
- **Typed resource/accounting arrays**: replace
  `typedef uint64_t resource_arr_t[NUM_RESOURCES]` plus raw
  `usage[RESOURCE_COUNT]` indexing with a small wrapper
  (`operator[](Resource)`, `Resource` the scoped enum above) — identical
  codegen to the C array, but indexing by a bare integer or an unrelated ID
  no longer compiles.
- **A dedicated `PciAddress` value type** at the DPDK boundary, generalizing
  the one real bug this session already found and fixed (`9e8c4af1`:
  hand-rolled `%08x:%02x:%02x.%02x` vs. DPDK's actual `PCI_PRI_FMT`
  `%.4x:%.2x:%.2x.%x`). The shipped fix used `rte_pci_device_name()` at the
  one call site that needed it; this phase's version is the general form —
  one canonical `ToString()`/parser pair so no other call site can
  reintroduce the same format mismatch by hand-rolling it again.
- **Sentinel APIs → `std::optional`**: replace `kAnyWorker = -1`,
  `INVALID_GATE == UINT16_MAX`-style sentinels with
  `std::optional<WorkerId>` (or similar) at cold/control-plane call sites,
  while keeping a named `GateId::Invalid()` constant available for
  hot-path code that genuinely needs the fixed-width sentinel
  representation.
- **Ownership cleanup in the traffic-class tree specifically**
  (`core/traffic_class.cc`/`.h` — the same file whose unsynchronized
  global `all_tcs_` map was the real bug behind commits 6-7): migrate
  parent→child ownership edges to `std::unique_ptr<TrafficClass>`, keeping
  raw observer pointers for the scheduling hot path. This does **not** fix
  the `all_tcs_` registry race by itself (that's a synchronization
  problem, not an ownership-type one — see the non-goals note below), but
  it closes off an adjacent class of double-delete/ambiguous-ownership bug
  in the same destructor chain that caused this session's investigation.
- **`constinit thread_local` vs. the current `extern __thread Worker
  current_worker`** (`worker.h`): re-benchmark the GNU `__thread` vs.
  standard `thread_local` codegen difference that motivated the original
  choice (per its own comment). If `constinit thread_local Worker
  current_worker{}` disassembles identically for hot-path access like
  `current_worker.wid()`, switch to it — `constinit` additionally
  guarantees at compile time that initialization can't silently become
  dynamic, which is exactly the "uninitialized state only caught at
  runtime" class of bug this phase targets. If codegen differs, keep
  `__thread` and record the disassembly comparison in this doc so the
  decision doesn't get re-litigated from scratch later.
- **`std::jthread` for worker threads — do NOT do this mechanically.**
  Swapping `std::thread worker_threads[]` for `jthread` changes shutdown
  semantics (its destructor calls `request_stop()` + `join()`, so a wedged
  worker now blocks the destructor instead of being detached) and directly
  interacts with the exact shutdown-ordering bug already found and fixed
  twice this session (commits 6-8). If pursued, redesign worker ownership
  into an explicit `WorkerSlot { Worker worker_; std::jthread thread_; }`
  first, with stop/join behavior chosen deliberately, before changing the
  thread type.
- **Physical-quantity wrapper types** (`TscCycles`, a `std::chrono`-based
  nanoseconds alias) at scheduler/rate-limiter APIs that currently pass
  raw `uint64_t` and mix cycles/ns/packets/bytes by convention only. Lower
  priority than the items above — evaluate call-site churn against benefit
  before committing.
- **A compile-time negative-test file** (e.g.
  `core/utils/typesafety_test.cc`) asserting the properties the rest of
  this phase is for: `static_assert(!std::is_convertible_v<CpuId,
  WorkerId>)`, `static_assert(sizeof(WorkerId) == sizeof(uint16_t))`,
  concept-satisfaction checks (`static_assert(BessModule<NoOp>)`), etc., so
  a future refactor that accidentally reintroduces an implicit conversion
  or breaks a concept fails CI immediately instead of silently.

**Explicit non-goal, stated plainly so it isn't re-litigated:** the
`worker.cc`/`all_tcs_` concurrency and shutdown-ordering bugs (commits
6-8) are lifetime/synchronization problems, not type-safety problems — no
amount of `enum class`/strong-ID/ownership-type work would have caught
them on its own; they needed runtime synchronization (a join, ultimately)
and were found only by running the daemon live under load, not by a
stronger type system. The worker-lifecycle redesign (`jthread`,
`WorkerSlot`, or otherwise) is tracked as its own concurrency-focused
effort, not folded into this phase's compile-time-provable scope.

C++26 contracts (`pre`/`post`) are the natural long-term home for some of
the invariants this phase encodes as constructors/factories instead —
already deferred in Phase H pending non-experimental compiler support;
revisit there rather than re-deciding it here.

---

## Phase J — Live table updates without stopping the world (done 2026-09-19: entries 35-38)

Cross-cutting phase, not a natural fit under A–I: touches the control
plane (Phase G), the module command API, and the scheduler loop. Emerged
from an Opus review of an external DPDK-modernization proposal (see the
"Roadmap / Backlog" phases above for where its other findings landed, and
"Rejected" / "Benchmark backlog" below for the rest).

**The problem, as actually measured in this tree** (the external proposal
described this mechanism, and got it wrong in a way that *understated* the
real cost): BESS today cannot update a rule table on a running pipeline at
all. `ModuleBuilder::RunCommand()` (`core/module.cc`) **refuses** any
command not marked `Command::THREAD_SAFE` with `EBUSY` whenever
`Module::HasRunningWorker()` (`core/module.h`) is true; `bessctl`'s
`command_module` (`bessctl/commands.py`) works around that by calling
`pause_all()` before **every** module command -- thread-safe ones
included -- and `resume_all()` after. `pause_all` blocks every worker on
an `eventfd` read (`core/worker.cc`). There are **45 `THREAD_UNSAFE`
commands across 22 module files**, including `ACL::CommandAdd`,
`IPLookup::CommandAdd`/`Delete`, `ExactMatch::CommandAdd`/`Delete`/
`SetRuntimeConfig`, `BPF::CommandAdd`/`Delete`, and `Queue::CommandSetSize`.
Net effect: **adding one ACL rule or one route stops packet processing on
every worker in the process.**

**The mechanism**: replace pause-mutate-resume with build-publish-reclaim
-- construct a replacement table off the dataplane, atomically publish the
pointer (release store), let workers keep running, wait for a grace
period, then destroy the old table.

**Which `RcuDomain` backend to actually build first: C++-native, not
DPDK** (design note added 2026-09-18, answering a direct question).
Real options, in increasing order of "how much you're building yourself":
`std::shared_mutex` (not RCU at all, a reader-writer lock -- every read
pays a real lock even with zero writer activity, strictly worse than any
RCU-style approach for this access pattern, mentioned only as the naive
baseline); `std::atomic<std::shared_ptr<T>>` ("RCU via refcounting" --
`std::atomic_load`/`_store` on a `shared_ptr`, reclamation is automatic
because the refcount *is* the grace-period tracking, at the cost of a
real atomic op per access); a hand-rolled epoch-based scheme with
`std::atomic` (reimplementing QSBR's algorithm yourself: readers store a
relaxed/release epoch counter once per loop iteration, writers defer
freeing until all readers have advanced past the retirement epoch --
same performance ceiling as DPDK's QSBR, but every memory-ordering detail
is now this project's problem); `rte_rcu_qsbr` (the same algorithm as
the hand-rolled version, already written, already tested in a networking
context, and `librte_rcu.a` is **already linked into `bessd`** -- no new
dependency, reader cost is one `rte_rcu_qsbr_quiescent()` store per
`Scheduler::ScheduleLoop()` iteration); and C++26 `std::rcu`/hazard
pointers (not production-ready anywhere yet, per Phase H -- the eventual
target, not a near-term option).

**Recommendation: prototype the first backend as
`std::atomic<std::shared_ptr<T>>`, not the hand-rolled epoch scheme and
not `rte_rcu_qsbr` yet.** Two reasons, both concrete rather than
theoretical: (1) this session already found a real, hard-to-reproduce
concurrency bug in this exact codebase's hand-rolled synchronization
(`worker.cc`/`all_tcs_`, commits 6-8) -- a live argument against adding a
*second* bespoke concurrency primitive when a well-tested one exists, and
`shared_ptr`'s refcounting needs no separate "prove the grace period
elapsed" logic to get right at all, unlike either epoch-based option; (2)
BESS processes packets in batches of `PacketBatch::kMaxBurst = 32`, not
one at a time -- if the RCU-protected read happens once per
`ProcessBatch()` call (grab a stable table reference for the whole
batch, not once per packet), the atomic refcount cost amortizes over 32
packets, meaningfully weakening the usual "shared_ptr is too slow for
RCU" argument for BESS's specific access pattern. Phase J's own
acceptance criterion is explicitly capability, not a `*_bench.cc` number
-- so the honest performance bar to clear here is low. Swap the backend
to `rte_rcu_qsbr` behind the same `RcuDomain` interface later only if
profiling actually shows the refcount cost matters, as a measurement,
not an assumption. **Caveat if that swap ever happens**: `rte_rcu_qsbr`'s
writer side would need the control-plane thread (handling gRPC commands,
not currently EAL-registered) to either call `rte_thread_register()`
once at startup or have a worker perform the actual reclaim on the
control plane's behalf via a deferred/queued mechanism -- a real design
detail to work out then, not automatic.

**Independent of the `WorkerId`/lcore-ID migration** (Phase C, above):
`rte_rcu_qsbr_thread_register()` takes a caller-chosen `thread_id`, not an
lcore ID -- BESS can pass `wid` directly, today, with no prerequisite.

**Scope discipline -- do exactly one pilot first.** `IPLookup` is the best
candidate: its table is already an opaque `rte_lpm*` behind a single
pointer, its three mutating commands are all `THREAD_UNSAFE`, the
rebuild-and-swap cost is bounded, and `rte_fib` (Phase D, above) has
native RCU support if the table migrates there anyway. `ExactMatch` is the
natural second. Only after two modules work should this generalize into a
`Module`-level contract. *(Status: complete. `IPLookup` (entry 35) and
`ExactMatch` (entry 36, plus the review-follow-up fixes `a0688fcf`) publish
generations instead of mutating live tables; the snapshot, publication and
writer-side reclamation mechanism they share lives in
`bess::utils::PublishedGeneration` (entry 37); and the capability is reachable
through the normal CLI, which now pauses only for commands the daemon reports
as not thread-safe (entry 38). Deliberately out of scope, unchanged: QSBR/RCU
machinery, the `ModuleGraph`/traffic-class tree, and removing the
user-supplied `ARG_TYPE` from `command module`.)* Explicitly **not** in scope for the first pass:
RCU-swapping the `ModuleGraph`, gate adjacency, or the traffic-class tree
-- those are Phase G/H territory (live reconfiguration), not this phase's
narrower table-update goal.

**Stated non-goal, so it isn't over-claimed later:** this would **not**
have prevented any bug in this session's log. The `worker.cc`/`all_tcs_`
defects (commits 6-8, see completed-work log above) are a writer-writer
race on an unsynchronized global map during concurrent teardown plus a
thread-lifetime bug; QSBR protects readers from writers and needs its own
writer serialization. Phase I's existing non-goal note on those bugs
applies here unchanged -- this is a capability improvement for
control-plane table updates, not a concurrency-bug-prevention mechanism.

**Judge this by the right metric.** This is a *capability* change (update
rules on a live pipeline), not a throughput change -- it will not move any
number in `core/*_bench.cc`. The acceptance test is a live one: drive a
`Source → IPLookup → Sink` pipeline at a steady rate while adding/removing
routes, and show zero packet loss and zero throughput dip across the
update, versus today's full stop.

---

## Rejected from the 2026-09-18 DPDK-proposal review

Listed so these don't get re-litigated from scratch later. Each was
considered and independently assessed against real code, not dismissed on
the reviewing document's word alone.

- **Replace `rte_hash` for `CuckooMap`** — reject. `CuckooMap<K, V, H, E>`
  (`core/utils/cuckoo_map.h`) is a typed C++ template; `NAT` stores
  `CuckooMap<Endpoint, NatEntry, ...>` as a real value type, and
  `ExactMatchTable`/`WildcardMatch` pass runtime-constructed
  hasher/comparator objects carrying a key length. `rte_hash` is a C API:
  fixed-size byte keys declared at create time, `void*` data, no
  iterators, no per-call comparator. Swapping means hand-rolling
  serialization for every `V` and losing type safety at every call site --
  squarely against "without losing flexibility and developer ergonomics"
  for a speculative lookup delta on a container that already has a
  benchmark (`core/utils/cuckoo_map_bench.cc`) showing it performs fine.
  The one genuinely useful thing in `rte_hash`'s modernization -- its QSBR
  integration pattern -- is achievable by making `CuckooMap`
  snapshot-publishable under Phase J, without replacing the container.
- **Replace the `ACL` module's engine with `rte_acl`** — reject as framed.
  `core/modules/acl.cc`/`.h` (~170 lines total) is a minimal demo module:
  it matches only IPv4 src/dst prefix plus src/dst port with `0` meaning
  wildcard, **ignores the IP protocol field entirely**, and reads the L4
  header assuming UDP-shaped layout regardless of actual protocol. Its
  maintenance burden is approximately zero. Adopting `rte_acl` means a
  rule-model translation layer, a context-rebuild control path, and *more*
  code -- to accelerate a module whose current semantics don't support a
  real ACL workload anyway. If a real ACL is ever needed: **write a new
  `rte_acl`-backed module and leave `ACL` alone**, don't swap the engine
  under a toy. Kept in the benchmark backlog below at low priority, under
  that framing only.
- **NAT RSS-affinity subsystem** (predictable RSS via `rte_thash` so
  NAT's translated-port choice makes the return flow land on the same
  worker) — reject the feature as scoped, but the underlying question
  ("is NAT single-worker *because* predictable RSS doesn't exist?") is
  worth answering precisely rather than dismissing (follow-up
  investigation, 2026-09-18): **no, not primarily.** `NAT::DoProcessBatch`
  calls `map_.Insert()` (`core/modules/nat.cc`) -- i.e. NAT mutates its
  `CuckooMap` of translation state **from the data plane itself**, on
  every newly-seen flow, not just via control-plane commands. That's
  structurally different from `ACL`/`IPLookup`/`ExactMatch`/
  `WildcardMatch`, which all *do* override `max_allowed_workers_` upward
  (verified: all four appear in the set of modules under `core/modules/`
  that override it) because their tables are only ever mutated by
  control-plane commands, never by `ProcessBatch` itself -- concurrent
  *reads* across workers are fine, only writes need serializing, and
  those already go through the pause-all path. NAT has no such
  separation: two workers processing two different flows could call
  `map_.Insert()` on the shared table concurrently, and `CuckooMap` isn't
  documented or verified safe for concurrent multi-writer access. **So
  the real blocker is table write-safety, not RSS.** That said, RSS
  affinity is still the *architectural key* to fixing it, just not on its
  own: a viable multi-worker NAT would most naturally shard its
  translation table one-per-worker (eliminating the shared-writer problem
  entirely), and that sharding only stays correct if forward *and*
  reverse packets of the same flow are guaranteed to land on the same
  worker -- which is exactly what predictable/symmetric RSS provides. So:
  predictable RSS is necessary but not sufficient; the table-sharding
  design is the other, undesigned half. Still a substantial new feature,
  still rejected as out of scope for modernization -- but noted precisely
  so a future NAT-scaling effort starts from the right diagnosis.

  **Does Phase J's RCU/QSBR help here? Not directly, but it's a natural
  third piece if this is ever built** (follow-up, 2026-09-18). RCU is
  built for one infrequent writer plus many frequent readers -- Phase J's
  actual target (control-plane rule changes, dataplane reads). NAT's
  problem is the opposite shape: **multiple frequent writers** (every
  worker inserting into one shared table on every new flow), and RCU says
  nothing about writer-writer races. It only becomes relevant *after* the
  per-worker sharding above: once each shard has exactly one writer (its
  owning worker -- confirmed via `core/modules/nat.cc`'s "lazy reclaim of
  expired" comment that expiry (`map_.Remove()`) already happens inline
  in the same per-packet path as insertion, not from a separate aging
  thread, so there's no second writer to worry about even within one
  shard), RCU is the right tool for **cross-shard reads**: a stats RPC,
  `GetRuntimeConfig`, or a future connection-tracking/debug API reading a
  shard from outside its owning worker, consistently, without locking the
  hot insert/expire path or pausing that worker. So the complete picture,
  if ever pursued: **symmetric RSS** (routes flows correctly) +
  **per-worker sharding** (eliminates writer contention structurally) +
  **RCU/QSBR** (makes cross-shard/control-plane reads safe) -- three
  pieces, not one.

  **Independent of NAT, one thing genuinely worth leveraging: symmetric
  RSS as a general `PMDPort` capability.** Verified `core/drivers/pmd.cc`
  configures `rss_key = nullptr` (PMD default key), `rss_key_len = 0`,
  and never calls RETA query/update -- the *only* RSS control BESS
  exposes today is which hash types to request. A symmetric hash key (a
  well-known technique: designing the Toeplitz key, or using `rte_thash`'s
  adjustment helpers, so a 5-tuple and its reverse hash identically) makes
  forward/reverse flows land on the same RX queue for **any** future
  stateful per-flow module wanting worker affinity -- not NAT-specific,
  and not blocked on redesigning NAT at all. This is small, general,
  non-speculative infrastructure. Add to Phase C: expose RSS key
  (including a symmetric-key helper/preset) and hash-type selection on
  `PMDPortArg`; RETA control as a follow-up once a real consumer wants
  non-default queue mapping.
- **`rte_flow` as an optional hardware-offload backend** — reclassified as
  **exploratory research, not a rejection** (per explicit direction,
  2026-09-18): don't schedule it, but don't file it as "no" either. The
  design constraint from the original assessment still stands and is the
  useful output of this pass: compile BESS logical rules down to
  `rte_flow`, never let a module secretly *become* `rte_flow` under the
  hood (same "expose BESS semantics, not backend implementation details"
  principle as the narrow `PortCapabilities` in Phase C and `PacketRef`
  in Phase B). The blocker is real -- the work presupposes a rule IR BESS
  doesn't have yet, so it can't start before Phase G's `GraphSpec`/
  `Capabilities` work exists -- but "blocked on a prerequisite" is a
  different status than "not worth doing," and this doc should say which
  one it means. Revisit as a real research spike once `GraphSpec` lands.
- **DMAdev** — reject/defer. Submission/completion overhead dominates for
  BESS's typical per-packet work (TTL decrement, MAC swap, small header
  rewrites); DPDK's own DMA docs use packet-copy-heavy workloads as the
  motivating case, which BESS mostly doesn't have. Revisit only if a
  genuinely copy-heavy path (e.g. async vhost) appears. (No change from
  the original assessment as of 2026-09-18 -- flagged for a second look
  but nothing new found to shift the verdict; revisit if a concrete
  copy-heavy use case shows up rather than re-litigating abstractly.)
- **`rte_eth_recycle_mbufs()`** — reject as a global default, but **the
  graph-aware angle raised 2026-09-18 is worth keeping as a research
  note, not dropping**: BESS's control plane already has full topology
  knowledge (today via the imperative connect-time graph; more
  explicitly once Phase G's `GraphSpec` exists), which is exactly the
  information needed to determine *per RX/TX queue pair* whether a
  simple, eligible 1:1-forwarding sub-path exists (no buffering, cloning,
  dropping, cross-worker handoff, or packet generation between a specific
  `PortInc` and `PortOut`) -- and to re-evaluate that eligibility whenever
  the graph changes, enabling/disabling the DPDK optimization dynamically
  per queue pair rather than as an all-or-nothing global switch. This is
  a genuinely different (and more interesting) proposal than the source
  document's static "detect a simple topology, use it" framing: it makes
  the optimization *follow* live topology changes instead of requiring a
  static deployment shape. It is **not buildable yet** -- it needs (a) a
  graph-analysis pass that can answer "is this specific path eligible"
  and (b) a way to toggle the recycle optimization per queue pair at
  runtime, neither of which exist -- and the API itself remains
  experimental in DPDK's ethdev layer regardless. Right home: revisit
  alongside Phase G's graph/topology work once `GraphSpec` gives BESS a
  real data structure to run this analysis over, not as a standalone
  `PMDPort` feature today.
- **`rte_bitset`** — reject. `kMaxWorkers = 64` (`core/worker.h`), so the
  existing `std::vector<bool>` active-worker set already fits one
  `uint64_t`; nothing to gain.
- **`rte_lcore_var`** — reject for `current_worker`. Per-worker state is
  already a per-thread `Worker` object with real lifetime semantics that
  `thread_local`/`constinit` (already tracked in Phase I) fits better than
  a lcore-indexed array abstraction.
- **`pdump`/`dumpcap`, DPDK CTF tracing as a fix for past concurrency
  bugs, replacing the graph runtime with `rte_graph`, replacing the
  scheduler with eventdev, DPDK multi-process for control-plane
  isolation** — reject, each with a concrete reason recorded in the phase
  section it's most related to above (`pdump`: Phase F, blocked by
  `--no-shconf`; tracing: Phase F, wrong fix for the wrong bug class;
  `rte_graph`/eventdev: not new rejections, this doc's Phase H/backlog
  language already treated both as reference-only; multi-process: doubly
  moot since `bessd` already passes `--no-shconf`, reinforcing Phase G's
  gRPC/UDS direction rather than changing it).

## Benchmark / experiment backlog (from the 2026-09-18 DPDK-proposal review)

Ordered by "what must exist before other answers here are trustworthy."
None of these are decided adoptions -- see the phase sections above and
the rejected list for what's already been decided either way.

1. **[x] PMD loopback forwarding benchmark — done 2026-09-18
    (`core/pmd_bench.cc`, entry 27 below).** Was "do this first; it unblocks
    most of the rest of this list", closing the gap Phase B's own benchmark
    section flagged ("needs a real NIC or a simulated one ... not attempted
    here"). Needed no DPDK rebuild: `librte_net_null.a`/`librte_net_ring.a`
    were already linked and `PMDPort` already accepted `vdev=`.
    `BM_PmdNullTx` (TX + alloc/free rate) and `BM_PmdRingRoundTrip`
    (self-loopback through real `rte_eth_rx/tx_burst`), batch sweep 1-32,
    zero drops throughout. A later review pass corrected two weaknesses
    without changing the numbers' meaning: `NullTx` now reports actual
    sent (not requested) items with drops as an explicit counter, and two
    `EndToEnd` twins (alloc + TX + RX + free all timed) were added for
    allocator-adjacent questions, since the originals time only the
    PMD/interface boundary by design. Baseline medians (this sandbox, g++, malloc-
    backed `--no-huge` EAL — relative comparisons only, not production
    numbers): NullTx 4.5→100.7M/s, RingRoundTrip 4.6→108.3M/s at batch
    1→32. Still open on top of it: the cross-worker
    `PortInc → Queue → PortOut` harness (mempool experiment's own work,
    gated on the `DumpMempool()` fix) and real-NIC numbers.
2. **[x] `llring` vs modern `rte_ring` — done 2026-09-18, `llring.h`
    deleted** (entry 28 below). Compared in `Queue`'s exact mode (MP
    enqueue-burst, SC dequeue-burst) at 1/2/4/8/16 producers → 1 consumer
    (`core/ring_bench.cc`, new file, kept — the `rte_ring` variants stay
    as the ring perf guard). **First measurement was misleading and
    would have decided wrong**: the generic `rte_ring_enqueue_burst` /
    `dequeue_burst` wrappers dispatch on creation flags at runtime and
    measured 10-30% slower than `llring`'s explicit calls across the
    board. Re-ran explicit-against-explicit and the deficit inverted:
    rte MP/SC medians 320/273/212/139/100M vs llring 253/243/196/132/100M
    items/s (1/2/4/8/16 producers) — faster-or-equal everywhere, so per
    the decision rule `core/utils/llring.h` (1193 lines) is gone and
    `Queue` (MP/SC), `DRR` (SP/SC), `LockLessQueue` (runtime flags) moved
    to `rte_ring`. RTS/HTS measured no better than classic MP/SC here;
    zero-copy has no API in DPDK 25.11 (verified absent), both dropped.
    `DRR`'s SP/SC rings were migrated alongside (same mechanical change,
    not benchmarked separately per the original scoping note).
3. **[x] Mempool backend × cache size × worker topology — done 2026-09-19,
   entry 32** (`core/mempool_bench.cc`, kept; the doc's item text below is
   the original scoping). Cross-worker results (`--pin=cores`, two P-cores,
   batch 32, median of 3, CV <= 2% for everything at cache >= 32): the
   per-lcore cache is worth ~30-70% over cache 0 but **saturates at the
   worker's burst size** -- cache 32 == cache 128 == cache 512 within CV,
   while 512 parks 766 of 8191 objects in the two caches (consumer 511,
   producer 255); no reproducible difference between `ring_mp_mc` /
   `ring_mt_rts` / `ring_mt_hts`; `lf_stack` is 1.5-2× slower than the ring
   backends and `stack` is not measurable in this harness yet (its locked
   variant serialises both workers: CV up to 11%, 28-95 Mpps across runs).
   Single-threaded the ordering inverts (cache ~2×; cacheless `stack`
   fastest at 2.9 ns/pkt vs 4.9 for `ring_mp_mc`). Shipped default
   unchanged. `--lcore_mode=register` matches BESS's manual `_lcore_id`
   write within drift, so the WorkerId/lcore decoupling is unblocked and
   this binary is its regression guard. Still open on top of it:
   cross-socket `--pin=numa` and NIC-backed numbers on real hardware (item
   6's `MBUF_FAST_FREE` remains gated on both), plus a stats-instrumented
   DPDK (`-Dc_args=-DRTE_LIBRTE_MEMPOOL_STATS`) if flush/refill *frequency*
   is ever needed. Original scoping text: Backends: `ring_mp_mc`
   (current), `ring_mt_rts`, `ring_mt_hts`, `stack`, `lf_stack`
   (`librte_mempool_stack.a` already linked). Cache sizes: 0/32/64/128/
   256/512 (current `kMaxCacheSize = 512`, `core/packet_pool.h`, applied
   only above 1024-capacity pools). **The workload that matters is
   cross-worker**: `PortInc`(worker A) → `Queue` → `PortOut`(worker B),
   since pools are shared per-NUMA-socket across workers while the
   per-core cache is keyed on `rte_lcore_id()` -- single-core Source→Sink
   won't show the effect. Establish this before any DPDK bump past 25.11
   (see Phase F's version-discipline note above).
4. **DPDK BPF vs the FreeBSD JIT** — see Phase D above for full detail;
   listed here for backlog completeness.
5. **[x] `rte_fib` vs `rte_lpm` — done 2026-09-19, entry 34: FIB not
   adopted** (order-dependent wrong answers at 512K routes; `rte_lpm`
   stays, and `core/fib_bench.cc` is the guard for any future DPDK fix).
   FIB remains tempting for update-heavy workloads: 0.75 vs 5.56 us per
   route build at 64K, 497 vs 8299 ns per delete+add.
6. **`MBUF_FAST_FREE` on/off** — needs a real NIC and (1); re-run after
   Phase B Stage 2 lands, since clones/external buffers can invalidate its
   preconditions.
7. **`PortOut` MCS lock vs PMD MT-lockfree Tx** — 2/4/8 workers on one Tx
   queue, on a PMD advertising the capability. Low priority; few PMDs
   support it.
8. **Burst size sweep** — 32 (current `PacketBatch::kMaxBurst`) / 64 /
   128 / 256, through `traffic_class_bench` and the new PMD loopback
   bench from (1), to check DPDK's own claim that 256 is a common sweet
   spot on server x86/ARM64. Informational only -- `kMaxBurst` is deeply
   wired into the codebase; this is not a proposal to change it, just to
   measure before anyone does.
9. **`rte_acl` as a *new* module** (low priority, only under the framing
   in "Rejected" above) — if a real ACL workload ever materializes,
   compare a new `rte_acl`-backed module against the existing toy `ACL`
   at 16/64/256/1K/10K/100K rules with varying match position.
10. **Power-aware idle** — legitimate but mostly a **scheduler** problem,
    not primarily a DPDK-adoption one: `core/scheduler.h` already has a
    `TODO` stating BESS "currently ha[s] no functionality to support such
    whole-scheduler blocking/unblocking." The scheduler would need to
    compute an earliest-wake condition across queue work, rate-limiter
    wakeups, timers, and multiple `PortInc` tasks before any DPDK
    monitor/pause primitive is useful. Judge by idle watts, p50/p99 wake
    latency, low-rate CPU consumption, and full-load throughput -- not
    packet rate. Not evaluable in this sandbox (no real NIC/power
    control). Track as a scheduler-redesign item first.
