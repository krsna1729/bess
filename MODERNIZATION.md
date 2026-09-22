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
- **DPDK build lives in `deps/dpdk-<ver>/`**, gitignored, and is bootstrapped
  with `tools/bootstrap_dpdk.py`.  It reads the single pinned source record
  in `deps/dpdk.json`, verifies the SHA256 before extraction, and installs the
  pkg-config file under `deps/dpdk-<ver>/install/lib/pkgconfig/`.
- **Python dependencies are still external** in a fresh shell.  Install
  `requirements.txt` with the environment's package manager before running
  Python tests.  Meson generates all protobuf stubs in its build tree.
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

## How to build and verify

```bash
tools/bootstrap_dpdk.py --af-xdp auto
export PKG_CONFIG_PATH="$(tools/bootstrap_dpdk.py --print-pkg-config-path):${PKG_CONFIG_PATH}"
meson setup build-meson -Dcpu=corei7 -Daf_xdp=auto
meson compile -C build-meson -j4
meson test -C build-meson --print-errorlogs
meson test -C build-meson --suite python --print-errorlogs
meson test -C build-meson --suite integration --print-errorlogs
meson test -C build-meson --suite benchmarks --print-errorlogs
```

CI configures `-Daf_xdp=required` and runs the same Meson graph with both GCC
and Clang.  `-Db_sanitize=address,undefined` and `-Db_coverage=true` are
Meson's native sanitizer and coverage controls.  The default DPDK linkage is
shared; `-Ddpdk_link=static` is an explicit opt-in.

The old `core/Makefile`, `core/extra.mk`, and top-level `build.py` are not part
of the supported build.  Generated protobuf code is never written into the
source tree.

A live module integration run through `run_module_tests.py` still starts its
daemon through sudo.  Local verification used the same gRPC reset/run path
against a foreground `bessd -skip_root_check -m 0`, avoiding sudo while
exercising all 22 module test files.

### Build invariants

- Meson/Ninja is the sole BESS build system.
- Do not restore `core/Makefile`.
- Do not restore top-level `build.py` as a BESS build orchestrator.
- DPDK is external and consumed through `pkg-config`.
- DPDK source/version/checksum has one tracked source of truth: `deps/dpdk.json`.
- Generated C++ and Python protobuf artifacts live in the build tree.
- The source tree should remain clean after a normal build/test.
- Cap local build parallelism at `-j4` on memory-constrained development systems.
- C++23 is the production baseline.
- C++26 remains experimental and must not become a project-wide production requirement yet.

---


## Phase E: Meson cutover — COMPLETE

Phase E is closed as of `e8c8e176` on `develop`. Meson is the sole BESS build
entrypoint; DPDK bootstrap is separate and checksum-pinned; C++ and Python
protobuf generation is build-tree-only; native unit tests, Python tests,
module integration, benchmarks, sample plugin, install layout, and AF_XDP
artifact checks are first-class Meson targets. No dataplane ownership,
`MBUF_FAST_FREE`, `PortOut`, plugin ABI, or DPDK-version behavior changes are
part of this phase.

CI (`35615217363` at `e8c8e176`) is green for both `Meson (gcc)` and
`Meson (clang)`: bootstrap, configure, build, AF_XDP artifact checks, all
Meson tests, install staging, and the installed Python client smoke all pass.

Phase E intentionally does not publish an external plugin-development package:
installed BESS headers and pkg-config/Meson dependency metadata remain part of
the later plugin-ABI work. The in-tree sample plugin is the only supported
plugin build surface for this phase.

The installed tree also does not provide a `bin/bessctl` entrypoint. The
control-plane CLI remains source-tree tooling while its redesign is pending
(see §14.4 in the roadmap); Phase E verifies the installed Python API
separately.

## Status snapshot

Phase E is closed, and **Phase G0 is complete** (seven commits, entries 41-49):
the control plane is a real C++ subsystem -- `RuntimeState` owns every mutable
instance, `PipelineSpec`/`PipelineSnapshot` describe desired and active state,
validation is side-effect-free, diff and planner are deterministic, and
`ApplyPipeline()` runs multi-object transactions with rollback, a generation
counter, optimistic concurrency and engine-decided quiescence. Verified on GCC
and Clang: 41 native test binaries, 22/22 module integration files against a
foreground daemon, and the wire-parity script.

The modern-glog daemon-mode recursion is fixed (§8, entry 54), **K1 is
complete** (entries 55-57), and **K2, K2.6, K3.1, K3.2, and K3.3 are
complete** (entries 58-64):
`RuntimeState` owns one dataplane `RcuDomain`, workers register/online/offline/
unregister around the pause boundary, the scheduler reports quiescence at a safe
task boundary, and `RcuPtr<T>` publishes and retires immutable state with an
acquire-load read path. K2 adds `StrongId<Tag, Rep>`/`ActionId`, the immutable
`ObjectTable<Id, T>` with a mutable builder, and a benchmarked flat
inline generation-owned representation. K2.6 constrains the id/hash substrate,
makes batch-size mismatch a caller precondition, corrects the benchmark's exact
object sizes and hot-field layout, and measures sparse high-water occupancy
against a flat validity bitmap and indirect storage. K3.1 adds the reusable
`bess::classifier` substrate without touching `ExactMatch`, `WildcardMatch`, or
`ACL`: resolved runtime schemas compile into exact-width extraction and result
placement plans, typed users get a metadata-free constrained API, runtime
backend selection is generation-level type erasure, and one immutable generation
owns extraction, backend, and placement state.
Generation ownership remains flat; `optional<T>` is kept for its general
move-only semantics, while sparse bitmap/indirect policies remain benchmark
candidates rather than public types. K3.2 delivers the exact-backend laboratory
and result-transport pressure fixes (`RuntimeExactBackend<Result>`, hit masks,
`PackedValueStore`, `CuckooExactBackend`, `RteHashPositionBackend`,
`RteHashDataBackend`, `SmallExactBackend`, `DirectExactBackend`, and normalization
 masks), benchmarked across multiple batch sizes and rule counts, while leaving
`ExactMatch` untouched until K3.3. K3.3 migrates `ExactMatch` onto the runtime
classifier (forced Cuckoo backend, dense packed keys, per-packet extraction
validity, `PreResume` metadata-offset refresh with fail-closed generations),
proven by a legacy-vs-new differential test and before/after benchmarks.
K3.3.1 and K3.3.2 are committed fast-path follow-ups recorded in entries
65-66. K3.4 adds the typed-author surface for the same exact-match family: the
typed backends accept an author's own `Key`/`Hash`/`Equal` without a
`KeyTraits` registration or `ByteKey`, `ExactTable` is decoupled from the key
concept, `BatchExactBackend` names the result type explicitly, and a
three-rung benchmark (author loop / `ExactTable` / runtime-generic) records the
comparison without declaring a winner. K3.5 adds the generic masked/tuple-space
substrate (`classifier/masked_exact.h`) as a library. K3.6 migrates
`WildcardMatch` onto it: immutable RCU generations, dense `ExtractPlan`
extraction under `kCheck`, last-write rule canonicalization, the module's own
field/tuple ceilings, and all seven documented legacy defects plus the missing
`MAX_FIELDS` check fixed by the new ownership model. The next milestone is
K3.7, K4-K8, and G1. The active build graph is
Meson/Ninja only. GCC and Clang full Meson compiles succeed with pinned DPDK
25.11.3. The registered suite is now 73 tests: 53 native C++ binaries, 17
benchmark smoke tests (including the PMD null/ring smoke), the sample-plugin
registry load, the Python target, and the module integration run. All 56
non-benchmark targets pass in one full run; K3.4-K3.6's own targets (the typed-
and masked-backend unit binaries, the WildcardMatch module test, and
`classifier_typed_bench`, `classifier_masked_bench`, and
`modules_wildcard_match_bench`) pass under GCC and Clang, with the sanitizer
coverage noted below.
The classifier extract-plan, Cuckoo, masked, and migration tests pass under
ASan+UBSan. Two targets remain environment-incompatible under sanitizer for
pre-existing reasons unrelated to this work: the Rte hash classifier test (DPDK
EAL cannot allocate its required memory) and `modules_wildcard_match_test`,
which hits the same `Any::PackFrom` null-descriptor SEGV as the pre-existing
`module_test` under this ASan build. `-Daf_xdp=required`
configuration, install staging, generated build-tree protobuf imports, and
source-tree hygiene checks also pass.

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

39. **`c02f41ef`** — **Phase E: Meson is now the sole BESS build entrypoint.**
    DPDK bootstrap is checksum-pinned and separate; DPDK is consumed through
    `pkg-config`; C++/Python protobuf and the version header are generated
    only under the build tree; and the obsolete top-level `build.py` and
    `core/Makefile` paths are removed.  Native unit tests, benchmarks, Python
    tests, module integration, the sample plugin, install staging, sanitizer
    and coverage controls, and required AF_XDP artifact checks are first-class
    Meson targets.  CI now runs the GCC/Clang Meson matrix with `-j4`.

    Verification on the pinned DPDK 25.11.3 build:

    | check | result |
    |---|---|
    | GCC and Clang full Meson compiles | pass |
    | GCC native C++ tests | 28/28 |
    | Python protobuf tests | 2/2 |
    | module integration | 22/22 files |
    | benchmark smoke | 10/10 |
    | PMD null/ring smoke | pass |
    | sample plugin registry load | pass |
    | AF_XDP required configure/artifacts | pass |
    | install staging and source hygiene | pass |

    The only compiler-specific source adjustment is removing unnecessary
    `virtual` specifiers from destructors in `shared_obj_test.cc`; DPDK's
    intentional unaligned header casts remain diagnostics but are demoted from
    errors with `-Wno-error=cast-align`.

40. **`5ef2a971`** + **`23fdc84a`** — **Roadmap re-based on the reordered
    future-work phases, and Phase G0 fully specified** (docs only, no code).

    The completed record is untouched — chronological log, completed-phase
    detail, known issues and the benchmark/experiment backlog all keep their
    numbering, so source comments that cite entry or backlog-item numbers stay
    valid — and the updated future-work ordering is spliced in above it:
    modern-glog daemon mode → G0 → K1-K8 → G1 → D → F → H/I, with
    hardware-gated work parked under Phase C-HW and unable to block the
    sequence. A naming map resolves older section names (the old
    `Phase G`/"G1 compatible"/"G2 breaking" variants, old D/F/H/I) to their
    current homes, Phase E is marked complete with CI run `35615217363` at
    `e8c8e176` as evidence, the Meson build invariants are recorded in the
    build section, and the AF_XDP/Linux-I/O end-state policies plus the
    cross-cutting rules (static graph/dynamic state, transaction classes,
    G0↔K1 boundary, hardware-offload direction, performance and classifier
    acceptance discipline, OMEC relationship, rejected directions, execution
    plan, end-state definitions, current handoff) are folded in.

    §9 (Phase G0) is then replaced by the operative control-plane spec:
    ControlPlane extraction with its internal API and one error model, RPC
    handlers as pure adapters, RuntimeState ownership (type registries stay
    global, mutable instance registries get one owner, destructors stop
    mutating registries, WorkerManager, module init context, ModuleGraph split,
    task/TC and port-queue ownership), PipelineSpec with explicit
    desired-state names, deterministic PipelineSnapshot, side-effect-free
    validation including a stageable metadata layout, deterministic diff and
    typed dependency-ordered plan operations, the transaction state machine
    with engine-decided quiescence and a prepare/commit split, the atomicity
    caveat made explicit via a `Reversibility` classification, generation and
    optimistic-concurrency semantics, the failure-injection/test matrix with
    its 19-item acceptance checklist, hot-path and pause-time discipline,
    Meson source layout, the 7-commit landing structure with the final
    verification gate, and the G0 non-goals. Baseline: `e8c8e176`.

41. **`8449ed78`** — **G0 commit 1/7: `ControlPlane` extracted from the gRPC
    service.** `core/bessctl.cc` shrinks 1957 → 1143 lines and is now a
    protocol adapter: protobuf request → plain C++ spec → `ControlPlane` call
    → protobuf response/error. New `core/control/`:
    `control_error.{h,cc}` (one internal error model: semantic
    `ControlErrorCode`, errno-compatible `err`, legacy message text;
    `ControlResult<T> = std::expected<T, ControlError>`) and
    `control_plane.{h,cc}` (ports, modules, connections, workers, traffic
    classes incl. the `AttachTc`/`FindTc` placement logic, gate hooks, resume
    hooks, plugins, and `Reset()`).

    The service-level `std::recursive_mutex` is gone: handlers no longer call
    each other (`ResetAll` used to invoke four reset handlers), composition
    moved into `ControlPlane::Reset()`, and the control plane owns the single
    non-recursive writer lock. Delegating handlers take no lock; the read-only
    handlers that still read runtime state directly take it through
    `AcquireLock()`, the documented temporary seam until reads move behind
    snapshot accessors.

    Two legacy quirks are preserved verbatim and marked `NOTE(G0)` for the
    registry-ownership commit rather than hidden here: `CreatePort`'s
    silent-failure path when an existing name is reused (it leaves a stale
    registry entry), and the code-0 failure reports that `ModuleGraph`
    produces.

    Verification on this sandbox (GCC + Clang, DPDK 25.11.3, `-m 0`):

    | check | result |
    |---|---|
    | GCC Meson build | clean |
    | Clang Meson build | clean |
    | native C++ tests + benchmarks + sample-plugin load | 39/39 |
    | module integration (`bessctl daemon reset -- run file`, foreground daemon) | 22/22 files |
    | wire-error parity (12 negative cases + positive path) | identical codes/messages |
    | `git diff --check` | clean |

    The integration and Python suites' own `bessctl daemon start` needs root,
    so they still fail in this sandbox for that reason alone; the module tests
    were therefore run through the same gRPC reset/run path against a
    foreground `bessd -skip_root_check -m 0`.

    Remaining G0 commits: 2 RuntimeState ownership, 3 PipelineSpec/Snapshot/
    validation, 4 diff/planner, 5 transaction engine, 6 internal
    `ApplyPipeline`, 7 failure injection/integration/performance/docs.

42. **`00e961a0`** — **G0 commit 2a/7: `RuntimeState` owns ports and modules.**
    Mutable instance state now has an owner: `PortBuilder` and `ModuleGraph`
    keep only what they are (a type registry and graph topology), while live
    objects live in `bess::control::RuntimeState` as `unique_ptr`s in
    `PortRegistry` / `ModuleRegistry` (`core/control/runtime_state.{h,cc}`).
    The registry API is `Find`/`Contains`/`Size`/`All` (non-owning view),
    `Add` (refuses duplicates without consuming the argument), `Remove`
    (hands ownership back), `Destroy` (busy check → `DeInit` → destroy),
    `GenerateDefaultName`, `Clear`; `ModuleRegistry` also owns the
    task-membership set that used to be `ModuleGraph::tasks_`.

    Call sites migrated: `ModuleGraph` create/destroy/destroy-all, task graph,
    gate numbering, active-worker propagation and name generation;
    `metadata.cc`; the four port-resolving modules (`PortInc`, `PortOut`,
    `QueueInc`, `QueueOut` resolve through the runtime instead of a global
    map); the read-only RPC handlers; `port_test`/`module_test`.

    Two legacy bugs died with the ownership change instead of being carried
    forward: `CreatePort` with an existing name used to free the old port,
    leave a dangling registry entry and report success with an empty name (it
    now refuses with `EEXIST` and touches nothing), and a failed module `Init`
    can no longer leave a registered half-module because the registry takes
    ownership only after `Init` succeeds.

    Verification: GCC + Clang builds clean, native tests + benchmarks +
    sample-plugin load 39/39, module integration 22/22 files against a
    foreground daemon, wire-parity script passes (extended with the new port
    lifecycle: create → duplicate refused → destroy), `git diff --check`
    clean. `port_test`'s queue test needed a fix: it used the port after
    moving it into the registry.

    Still on the old globals (rest of commit 2): traffic classes
    (`TrafficClassBuilder::all_tcs_`, whose destructors mutate the registry)
    and workers (`workers[]`, `worker_threads[]`, `num_workers`,
    `orphan_tcs`).

43. **`28761cd3`** — **G0 commit 2b/7: `RuntimeState` owns traffic classes.**
    `TrafficClassRegistry` joins the runtime state: it owns `unique_ptr`s keyed
    by name (`Register`/`Find`/`All`) and exposes the ownership handoff that
    teardown needs — `Release`/`ReleaseTree`/`ReleaseAll` erase entries
    *without* destroying objects, so the code that actually deletes a tree
    (scheduler teardown, `AdjustDefault`, `Module::DestroyAllTasks`) stays in
    charge of destruction and the registry is not a second owner.
    `TrafficClassBuilder` keeps only the factory and forwards `all_tcs()`/
    `Find()`; `all_tcs_` is gone; the five destructors that called
    `TrafficClassBuilder::Clear(this)` no longer touch the registry; `~Scheduler`
    releases its tree before `delete root_`; `ClearAll()` keeps its legacy
    "forget, do not delete" meaning.

    Bug caught by verification: `Release`/`ReleaseAll` initially used
    `erase`/`clear` on the owning map, which destroys the object — the exact
    double free the model is meant to prevent. It appeared as SIGSEGV in
    `traffic_class_test`/`traffic_class_bench` and as a daemon abort ("double
    free or corruption") during the module tests; both paths now `release()`
    before erasing.

    Verification: GCC + Clang builds clean, native tests + benchmarks +
    sample-plugin load 39/39 (including the two that caught the bug), module
    integration 22/22 files, wire-parity script passes, `git diff --check`
    clean.

    Remaining from commit 2: the worker globals behind an explicit
    `WorkerManager`.

44. **`fb1259b8`** — **G0 commit 2c/7: `WorkerManager` owns worker slots,
    threads and orphans — commit 2 complete.** The last mutable-instance
    globals move into the runtime: `core/control/worker_manager.{h,cc}` owns
    the worker slots, the OS threads and the orphan-TC list, while `worker.cc`
    keeps `current_worker` (the thread's TLS `Worker`), pause/resume signalling
    and `WorkerPauser`, and reduces its free functions to forwarders.

    The worker slots became `std::atomic<Worker *>` instead of the legacy
    `Worker *volatile workers[]` — a correctness fix, not cosmetics: the old
    array relied on `volatile` to stop the compiler caching the pointer between
    the "slot filled?" test and the dereference in the launch spin. Without it
    the daemon segfaulted (null deref at address 0) in
    `WorkerManager::Launch` the first time a worker was launched from
    `AttachOrphans`; reproduced and diagnosed from the daemon's own crashlog
    before the fix. `num_workers`/`orphan_tcs` globals are gone, the thread
    entry point and its argument struct moved next to the manager, and
    `worker_threads[]` left the public header.

    Verification: GCC + Clang builds clean, native tests + benchmarks +
    sample-plugin load 39/39, module integration 22/22 files, wire-parity
    script passes, `git diff --check` clean.

    Commit 2's acceptance condition now holds end to end: ports, modules,
    traffic classes and workers each have one owner in `RuntimeState`, and no
    destructor mutates a global registry. Next: commit 3
    (PipelineSpec / PipelineSnapshot / side-effect-free validation).

45. **`7b62da94`** — **G0 commit 3/7: `PipelineSpec`, `PipelineSnapshot` and
    pure validation.** The desired-state half of the control plane exists and is
    read-only: nothing in this commit mutates the runtime.

    - `core/control/pipeline_spec.{h,cc}` — the desired-state IR
      (`PortSpec`/`ModuleSpec`/`ConnectionSpec`/`WorkerSpec`/`TrafficClassSpec`
      + `PipelineSpec`) with value equality (protobuf arguments compared by
      serialized bytes) and `Normalize()` (stable ordering; "0 queues" resolves
      to one queue) so equal descriptions compare equal. The structural types
      moved out of `control_plane.h`.
    - `core/control/pipeline_snapshot.{h,cc}` — deterministic structural
      snapshot: ports, modules, connections (derived from module gates in
      `(upstream, ogate)` order), workers (with the scheduler each was launched
      with — `WorkerManager` now records it) and traffic classes (parent,
      policy, worker, leaf task identity). Ordered maps, no pointer addresses,
      no transient statistics.
    - `core/control/pipeline_validator.{h,cc}` — `ValidatePipeline()`,
      side-effect free by construction (type registries and CPU topology only,
      never the instance registries): names, types, references, gates, workers,
      port shape, traffic classes (policy, resource, reserved names, wid range,
      task-id range, parent cycles), returning the canonical spec.
    - `ControlPlane::ValidatePipeline()` / `ControlPlane::GetPipeline()` expose
      both under the control-plane lock (now `mutable`, since the snapshot path
      is const).

    Deliberately recorded rather than faked: module metadata attributes exist
    only once a module instance exists (`Module::AddMetadataAttr` runs in the
    module's own `Init`), so metadata *layout* validation cannot be
    side-effect-free today. It belongs to `Prepare()` — where candidate modules
    exist — and the runtime's metadata pipeline must become stageable for it,
    exactly as section 9.5 already requires for driver-specific checks.

    Tests: `core/control/control_plane_test.cc`, 14 cases wired into Meson like
    every other native test — normalization, every rejection class, parent
    cycles, purity (rejected *and* accepted specs leave port/module/TC counts
    unchanged) and snapshot determinism/reflectivity.

    Verification: GCC + Clang builds clean, native tests + benchmarks +
    sample-plugin load **40/40**, module integration 22/22 files, wire-parity
    script passes, `git diff --check` clean.

46. **`8f528f52`** — **G0 commit 4/7: deterministic diff and dependency-ordered
    planner.** `Diff(current, desired)` and `Plan(diff)`, both pure.

    `core/control/pipeline_diff.{h,cc}` classifies every change as
    create/remove/replace/update per resource class, comparing by name and
    normalized value, never by pointer: "0 means driver default" in a desired
    port matches what the runtime resolved (so re-applying an equivalent
    description is an empty diff, not churn); a different driver or module
    argument is a replace; connections are diffed from the graph itself;
    internal traffic classes (`!leaf_*`, `!default_rr_*`) are skipped on both
    sides because they belong to their modules and workers, not to desired
    state; unchanged entries are dropped and everything is sorted.

    `core/control/pipeline_plan.{h,cc}` turns that into typed operations
    (`std::variant` of twelve op structs, not closures) in three dependency
    phases: prepare (workers → ports → modules, since module Init resolves
    ports), commit (disconnect → connect → create/reparent TCs) and retire (TCs
    detach before their modules, modules release port queues before ports,
    workers last). `SpecFromSnapshot()` reconstructs the desired-state
    description of the running pipeline, which is what makes "apply what is
    already running" a no-op; `ControlPlane::DiffPipeline()`/`PlanPipeline()`
    expose both under the control-plane lock.

    Tests: 7 new cases (21 total in `control_plane_test.cc`) — empty diff/plan
    for identical state, create/remove classification, module-argument change
    as replace with prepare+retire, connection add/remove → ConnectOp/
    DisconnectOp, internal TCs invisible to desired state, phase ordering, and
    diff stability across runs.

    Verification: GCC + Clang builds clean, native tests + benchmarks +
    sample-plugin load 40/40, module integration 22/22 files, wire-parity
    script passes, `git diff --check` clean.

47. **`8be5cb8f`** — **G0 commit 5/7: transaction engine, generation,
    optimistic concurrency.** `ControlPlane::ApplyPipeline(desired, options)`
    is the transactional path — validate → expected-generation check → diff →
    plan → prepare → commit → retire, all under the control-plane lock so
    concurrent writers cannot interleave.

    `core/control/transaction.{h,cc}` holds the state machine and an explicit
    undo log: Prepare runs reversible setup while workers run; Commit takes the
    smallest quiesced window that `RequiredQuiescence()` decides (setup-only
    plans never pause anything); Retire destroys what the new state replaced
    *after* the transition succeeded and reports failures rather than pretending
    the transaction failed; Abort walks the undo log in reverse and logs loudly
    if an undo step fails instead of claiming the rollback worked. Generation
    lives in `RuntimeState` and is bumped exactly once per successful
    state-changing transaction — never for reads, validation, planning,
    failures, or a no-op apply (which returns zero operations and no pause).
    `expected_generation` is checked before any side effect and reports a
    conflict (`ESTALE`). `CheckReversibility()` refuses what cannot be staged
    reversibly — port reconfiguration, module replacement, traffic-class policy
    changes — with `kUnsupportedTransaction`, naming the object.

    `ControlPlane`'s mutating primitives were split into `*Locked` bodies so the
    engine can compose them under one lock; `RemoveTcLocked` is new. Planner
    fix: traffic classes are created parent-before-child and removed
    child-before-parent.

    Bug found by verification: `AttachTc` deleted an already-registered traffic
    class on failure without releasing its registry entry, leaving a dangling
    pointer that `ListTcs` followed — the daemon segfaulted. The entry is now
    released on every failure path, and the parity script covers the case (a
    priority child without a priority fails, the parent survives, the daemon
    stays up).

    Tests: 6 new cases (27 total) — generation bumps once and not on a no-op;
    validation failure leaves it untouched; stale generation is a conflict with
    no side effects; a mid-transaction failure rolls back what it created;
    replacement is refused with the right code; TC hierarchies plan
    parent-first.

    Verification: GCC + Clang builds clean, native tests + benchmarks +
    sample-plugin load 40/40, module integration 22/22 files, extended
    wire-parity script passes, `git diff --check` clean.

    Scope note: unit binaries have no DPDK EAL and therefore cannot launch a
    worker, so commit-phase failures (which need a scheduler root) are exercised
    through the daemon runs; commit 7's failure-injection matrix covers them
    systematically.

48. **`76cab6a8`** — **G0 commit 6/7: a complete pipeline applied from C++ end
    to end.** `core/control/apply_pipeline_test.cc` is a native test binary that
    brings up a runtime the way the daemon does (DPDK EAL with `--no-huge`,
    packet pools, port drivers — launching a worker needs a live EAL) and drives
    `ApplyPipeline()` directly, so the engine is exercised as a real
    multi-object transaction rather than through RPC adapters.

    Cases: a complete pipeline (worker, two modules, a connection, a TC
    hierarchy) applies in one transaction with exactly one generation bump and a
    worker-pausing commit; the snapshot of the active runtime reconstructs the
    desired state and diffs clean (idempotency, end to end); **a transaction that
    fails while committing leaves the previous pipeline active** — generation,
    module count, TC count and the structural snapshot all unchanged, and the
    still-active pipeline still diffs clean; and removal goes through the retire
    phase leaving a consistent runtime.

    Bug found by the third case and fixed here: undoing the operations was not
    enough, because leaving the quiesced window attaches orphan traffic classes
    — a scheduler that briefly held two roots keeps a `!default_rr_*` wrapper
    behind, and the failed transaction left it in the snapshot. `Abort()` now
    collapses scheduler defaults after replaying the undo log, so a failed
    transaction leaves no trace, internal traffic classes included.
    `SpecFromSnapshot()` also treats an internal parent as "no parent", which is
    what makes re-applying the running pipeline a no-op.

    Verification: GCC + Clang builds clean, native tests + benchmarks +
    sample-plugin load **41/41**, module integration 22/22 files, wire-parity
    script passes, `git diff --check` clean.

49. **`f48e8dc0`** — **G0 commit 7/7: failure injection, leak assertions, phase
    timings — Phase G0 complete.** `SetFailureInjector()` /
    `ClearFailureInjector()` in `transaction.h` is a test-only hook the engine
    consults before every operation (phase + operation), empty by default, with
    deliberately no environment-variable switch. `apply_pipeline_test.cc` gains
    a failure-injection matrix that walks a *real* plan — a module to create, a
    connection to make, a traffic class to attach — and fails each operation in
    turn, comparing a full runtime fingerprint after every one: generation,
    port/module/TC/worker counts, orphan-TC count, per-queue `users` occupancy
    (no acquired queue left behind) and the structural snapshot, all of which
    must be identical, with the still-active pipeline diffing clean.

    Retirement is not undoable by construction, so an injected retire failure is
    logged and the transaction still counts as applied — the test pins that
    semantic down. `ApplyResult::timing` records validation / prepare /
    paused-commit / retire microseconds, so the quiesced window is measurable
    from the start (section 9.10).

    Docs: section 9 is marked COMPLETE with the seven-commit table and an
    explicit list of what G0 left out and where each item lands — metadata layout
    validation in `Prepare()`, transactional replacement refused by design, RCU
    in K1, the public desired-state API in G1. The order of work shows G0 done
    and K1 next; the status snapshot reflects the finished state.

    Verification: GCC + Clang builds clean, native tests + benchmarks +
    sample-plugin load 41/41, module integration 22/22 files, wire-parity script
    passes, `git diff --check` clean.

50. **`5fc0ee2a`** — **G0 closure follow-up: generation coherence,
    traffic-class fidelity, retire contract.** Three gaps found in a review of
    the finished G0 code.

    **Generation coherence.** The counter was only advanced by
    `ApplyPipeline()`, so a legacy structural RPC could mutate the runtime
    without moving it and a stale writer would then be accepted. Every public
    *structural* mutation now bumps exactly once on success (ports, modules,
    connections, workers, traffic classes, and `Reset()` once for the whole
    composition); the `*Locked()` primitives never bump, so `ApplyPipeline()`
    still bumps once per multi-operation transaction. Reads, validation,
    planning, diffing and pause/resume do not bump.

    **Traffic-class fidelity.** `TrafficClassSnapshot` carried only identity,
    placement and leaf ownership, and `Diff()` compared only parent and policy,
    so a changed weighted-fair resource/share, a child's priority, or a rate
    limit/burst was reported as *unchanged*. The snapshot now reconstructs the
    full spec semantics — own `resource`/`limit`/`max_burst`, plus the
    attachment parameters the parent holds (priority, share) — and the diff
    classifies: policy → `kReplace` (refused), parent/priority/share →
    `kUpdate` (detach and reattach), resource/limit/burst → `kUpdateParams`
    (applied in place, undone by restoring the old values). New
    `UpdateTcParamsOp` and `ReparentTcLocked()`; the latter purges the subtree
    from the scheduler wakeup queues before moving it, so a class attached to a
    worker can be moved safely (the legacy `UpdateTcParent` keeps its stricter
    orphan-only rule).

    **Retire contract.** `Retire()` only logged failures, so a successful
    `ApplyPipeline()` could leave an object the desired state says should not
    exist. Retirement preconditions are now proven before the commit: a plan
    removing a port still in use by a surviving module, or a worker still
    running a surviving module's tasks, is refused with
    `kUnsupportedTransaction`. If a retire step still fails, the generation is
    bumped (the commit happened) and the caller gets `kResourceFailure` saying
    the pipeline was committed but retirement failed — never an ordinary
    success. The new invariant test asserts
    `Normalize(SpecFromSnapshot(GetPipeline())) == Normalize(desired)` after
    creation, change and removal.

    Tests: 10 new cases (29 in `control_plane_test.cc`, 12 in
    `apply_pipeline_test.cc`). Verification: GCC + Clang builds clean, native
    tests + benchmarks + sample-plugin load 41/41, module integration 22/22
    files, wire-parity script passes, `git diff --check` clean.

51. **`9affd519`** — **Unit-test death tests stop reporting crashes.**
    `bessd_test` (8 death tests) and `memory_test` (3) assert on fatal paths by
    forking a child that really aborts; each child reached systemd-coredump, so
    a `bessd_test` run left six cores, a `memory_test` run one, every one logged
    as a crash and firing a desktop notification — noise that buried the real
    daemon cores during G0 development.

    The unit-test binaries now link our own `core/gtest_main.cc` (dead code
    since the Meson cutover, which is also why its death-test-style setting was
    not in effect) instead of the packaged `gtest_main`. It makes the test
    process non-dumpable and zeroes its core limit, both inherited by death-test
    children. `RLIMIT_CORE=0` alone is insufficient — with a piped
    `core_pattern` the kernel skips the limit, and systemd-coredump still logged
    "terminated abnormally without generating a coredump" (measured) —
    `prctl(PR_SET_DUMPABLE, 0)` is refused a core before that path, so nothing
    is reported at all.

    Measured: one run produced 7 entries before, none after; a full native-suite
    run goes 458 → 458 entries while the suite stays 41/41 and the death tests
    still execute their fatal paths. `BESS_TEST_CORE_DUMPS=1` opts out for
    debugging. Scope is the unit-test binaries only: the daemon keeps its core
    dumps, and no system configuration was touched.

    Follow-up (not done, deliberate): the asserted fatal paths are environment
    preconditions (no root, no hugepages, bad arguments) rather than programming
    errors; returning errors instead of aborting is a product decision with
    startup-semantics ripples, unlike the client-reachable paths where G0
    already returns errors.

52. **`20d9fe2a`** — **G0.1: reparent must not destroy the class it moves.**
    `ReparentTcLocked()` detached an existing class and called `AttachTc()`,
    which owns cleanup for a *newly created* class — on failure it released the
    registry entry and let its `unique_ptr` destroy the object. A refused move
    therefore destroyed the class being moved, and since the transaction records
    the reparent undo only *after* success, `Abort()` could not restore it; for
    a non-leaf class it could recursively destroy descendants while their
    registry entries still existed. Reachable through an ordinary desired-state
    edit (priority collision, missing priority, `share == 0`, any parent-specific
    `AddChild()` refusal).

    Attachment ownership is split: `AttachExistingTcLocked()` attaches a class
    that already exists and never unregisters or destroys it; creation keeps its
    semantics (release the entry, destroy the new class); reparent snapshots the
    current attachment (`AttachmentSpecOf`), detaches, tries the new attachment
    and *reattaches the old one* when it is refused; the legacy
    `UpdateTcParent` gets the same failure semantics while keeping its stricter
    orphan-only rule for non-leaf classes.

    `ValidatePipeline()` now refuses impossible attachment up front: a priority
    parent's child must carry a non-reserved, sibling-unique priority, a
    weighted-fair parent's child must carry a share greater than zero,
    `rate_limit` takes a single child, and policies that take no attachment
    parameter reject children carrying one.

    Bug found while fixing this: `Diff()` compared the raw snapshot parent, so a
    root class the scheduler had placed under its internal `!default_rr_*`
    wrapper looked like an attachment change on every apply — hand-written
    desired state could never reach an empty diff. The diff now treats an
    internal parent as "no parent", matching `SpecFromSnapshot`.

    Tests: 4 new (15 in `apply_pipeline_test.cc`) — the review's acceptance case
    (refused priority move: generation, snapshot, both children and their
    priorities unchanged, no dangling entries), an attach failure validation
    cannot see (rate limiter's current child retired later in the same
    transaction) rolling back cleanly including the class created for it,
    `share == 0` refused before any change, and the policy-change refusal against
    a validation-clean setup.

    Verification: GCC + Clang builds clean, native tests + benchmarks +
    sample-plugin load 41/41, module integration 22/22 files, wire-parity script
    passes, `git diff --check` clean.

53. **`3f49a18a`** — **G0.1 follow-up: snapshot the old attachment before the
    legacy detach.** `UpdateTcParentLocked()` captured `AttachmentSpecOf(c)`
    *after* detaching the class, so the description it kept was no longer where
    the class had been: a leaf that had been a child of a priority class with
    priority 10 was recorded as a root/orphan, and a refused reattach
    "restored" it to the orphan list instead of back under its parent. The class
    was no longer destroyed (that part of `20d9fe2a` was right) but it was still
    moved. The snapshot now happens immediately after `FindTc()`, the same
    ordering the transactional `ReparentTcLocked()` already used.

    Tests: 2 new (17 in `apply_pipeline_test.cc`) — a real leaf class created by
    a `QueueInc` module is homed under a round-robin parent through the legacy
    path, then a move colliding with a sibling's priority is refused: failure,
    generation unchanged, class still registered *and still under its original
    parent*, pipeline still diffing clean. Verified to fail against the pre-fix
    ordering and pass after. The second pins the stricter legacy rule for
    non-leaf classes (they may only move as orphans) leaving the snapshot
    byte-identical.

    Verification: GCC + Clang builds clean, native tests + benchmarks +
    sample-plugin load 41/41, module integration 22/22 files, wire-parity script
    passes, `git diff --check` clean.

54. **`05413f53`** — **Daemon-mode glog recursion fixed by deletion.**
    `CloseStdStreams()` redirected the C `FILE*` streams through
    `fopencookie()` callbacks that called `LOG()`. On any glog that writes
    through libc's stderr — absl-based glog 0.7+ does — that is a feedback loop:
    `LOG → glog → fwrite(stderr) → cookie callback → LOG → …` until the stack
    was exhausted.

    Reproduced before the fix on this machine (google-glog 0.7.1-2, absl LTS
    20260817): daemon mode died with SIGSEGV during `Daemonize()`, the launcher
    reported "Failed to launch a daemon process", and the symbolized core shows
    the exact loop — `LogMessage::Flush → SendToLog → fwrite → _IO_file_xsputn →
    bessd.cc:303 (stderr cookie) → LOG(WARNING) → LogMessage::Flush → …`, with
    the recursive message literally containing the previous line's text.

    The fix deletes the machinery (production file: 41 lines added, 55 removed):
    `stdout`/`stderr` are no longer replaced as `FILE*`, C stdio output in daemon
    mode goes to `/dev/null` by design, buffers are flushed before the `dup2()`
    calls so pre-fork output is not carried across, and the C++ streams keep
    their one-way bridge into glog (`std::cout → LOG(INFO)`,
    `std::cerr → LOG(WARNING)`), which is safe because glog writes through fd 2
    and never through those streambufs. `FLAGS_stderrthreshold = FATAL + 1`
    stays, but correctness no longer depends on it. The contract comment at the
    top of `bessd.cc` describes the real behaviour instead of the deleted one.

    New regression test `Daemonize.LoggingAfterDaemonizationDoesNotRecurse`
    exercises `LOG(INFO)`, `fprintf(stdout)`, `fprintf(stderr)`, `std::cout`,
    `std::cerr`, `LOG(WARNING)` and `LOG(ERROR)` after daemonizing and then
    reports readiness — process survival is the assertion. Verified to fail
    against the pre-fix code (10s timeout: the child died) and pass after.

    Verification: GCC + Clang builds clean; native tests + benchmarks +
    sample-plugin load 41/41; module integration 22/22 files; wire-parity script
    passes; real daemonized daemon smoke (launcher exit 0, child alive, gRPC
    request served, clean SIGTERM shutdown, no cores) and real foreground smoke
    (unchanged: normal stderr logging, gRPC served); `git diff --check` clean.

55. **`45e5723a`** + **`81f62180`** — **K1.1/K1.2: the RCU domain, and workers as
    its readers.** `core/rcu/rcu_domain.{h,cc}` is the BESS semantic interface
    over DPDK's `rte_rcu_qsbr` backend: reader identity is the BESS `WorkerId`,
    readers register/unregister and go online/offline, grace periods are started
    and checked (or synchronised), and retired objects of any type are destroyed
    by `ReclaimReady()`/`Drain()` on the calling control thread — never on a
    packet worker. One grace period can retire several objects (the shape K2/K3
    need), the retirement queue is bounded with control-side back-pressure, and
    the QSBR memory is ordinary cache-line-aligned memory so a domain needs no
    EAL (14 unit tests, including the teardown assertion as a death test).

    `RuntimeState` then owns exactly one domain, and the worker lifecycle drives
    it: the thread registers its reader on startup and stays *offline*;
    `BlockWorker()` goes **offline before blocking** and **online before
    dataplane work resumes** (reporting quiescence on the way); the thread
    unregisters before teardown, so a destroyed worker stops blocking grace
    periods and its id can be reused. 3 worker-level tests cover launch/resume/
    pause/destroy/recreate and the pause-during-grace case.

    Verification: GCC + Clang clean; 43/43 native tests + benchmarks + plugin
    load; module integration 22/22; wire-parity passes.

56. **`0a635e2f`** — **K1.3: quiescence is reported at the scheduler boundary.**
    An online worker now advances grace periods while it runs: the report
    happens at the scheduler's existing periodic boundary (every 256 rounds,
    where the pause request is already checked), which is exactly "the previous
    task invocation returned and the next has not started" — the definition of a
    BESS quiescent state, and a worker execution-state property rather than
    something a module reports. Both schedulers report, and because the boundary
    is reached whether or not there is work, an *idle* worker still advances
    grace periods: reclamation does not stall when traffic stops.

    Tests: `core/rcu/rcu_scheduler_test.cc` — a task that holds a published
    pointer keeps the old object alive until it returns and the worker reaches
    the boundary; an idle worker completes a grace period promptly.

    Two bugs found while writing them, both in the test: the release flag was
    set outside the mutex guarding the wait predicate (lost wakeup — the task
    slept past its release), and teardown destroyed the worker before the module,
    leaving `Module::tasks_` dangling (only visible under meson, which sets
    `MALLOC_PERTURB_`; the test now follows the daemon's order — modules under a
    pause, then workers).

    Verification: GCC + Clang clean, 44/44 native tests + benchmarks + plugin
    load, module integration 22/22, wire-parity passes.

57. **`45e5723a` … `8207d913`** — **K1: the generic RCU/QSBR substrate, and the
    two modules that used to do this by hand.** Landed as seven bisectable
    commits (entries 55-57 cover the first six; the benchmarks and this closure
    are the rest).

    - **`RcuDomain`** (`core/rcu/`): one dataplane reader domain per runtime,
      reader identity is the BESS `WorkerId`, DPDK's `rte_rcu_qsbr` behind a
      BESS interface. Readers register/unregister and go online/offline; grace
      periods are started/checked/synchronised; retired objects of any type are
      destroyed by `ReclaimReady()`/`Drain()` on the calling control thread --
      never on a packet worker; one grace period can retire several objects; the
      queue is bounded with control-side back-pressure; the QSBR memory is
      ordinary cache-line-aligned memory, so no EAL is needed.
    - **Worker lifecycle**: the thread registers its reader on startup and stays
      *offline*; `BlockWorker()` goes offline **before blocking** and online
      **before dataplane work resumes**; the thread unregisters before teardown,
      so a destroyed worker stops blocking grace periods and its id is reusable.
    - **Scheduler**: quiescence is reported at the existing periodic boundary
      (every 256 rounds) -- previous task returned, next not started -- so an
      online worker advances grace periods whether or not it has work.
    - **`RcuPtr<T>`**: `Read()` is one acquire load (measured equal to a raw
      atomic pointer load); `Publish()` encodes build → release-store → start
      grace period → retire old; `Exchange()` supports retiring several objects
      against one grace period; `ResetQuiesced()` is the named teardown path;
      writers are serialized; destroying it with online readers is a debug
      failure.
    - **Migrations**: `IPLookup` and `ExactMatch` publish and retire through the
      domain, each with one private `Publish()` helper that owns the writer
      protocol; `core/utils/published_generation.h` is deleted (no consumers
      left). Rule semantics, gates, keys and update APIs unchanged.
    - **Benchmarks** (`core/rcu/rcu_bench.cc`): read path 66 ns vs 68 ns for the
      raw-atomic baseline (256 loads per iteration), publish with an idle reader
      39 ns, publish while a reader holds 123 ns (bounded), grace-period latency
      16 ns. A 2000-generation publication storm with a live reader reclaims
      everything and never touches the active generation.

    Tests: 34 RCU cases across `rcu_test`, `rcu_ptr_test`, `rcu_worker_test` and
    `rcu_scheduler_test` (unit, publication, worker lifecycle and scheduler
    boundary), plus the existing module suites for the two migrated modules.

    Bugs found while building it, all in the tests and all the same family --
    boundary ordering: a wait predicate changed outside its mutex (lost wakeup),
    a worker destroyed before the module that owns its task, and a blocking
    `Synchronize()` while the reader was still online. Each is now pinned by the
    test that found it.

58. **`35fdbbb1`** — **K2.1/K2.2: strongly typed ids and immutable object
    tables.** The generic mechanism for when a lookup result is better carried as
    a compact id than as an inline value: `Id -> const T *`.

    - **`core/dataplane/strong_id.h`**: `StrongId<Tag, Rep>` — zero-overhead,
      trivially copyable, standard layout, no implicit conversion from an integer,
      none to one, and none between different id types, so an action id cannot
      silently become a gate index or a next-hop id.
    - **`core/dataplane/action_id.h`**: `ActionId = StrongId<ActionIdTag,
      uint32_t>`, `ActionId{0}` reserved as invalid. It is a *continuation token*,
      not a dispatch result: an output gate stays `gate_idx_t`, nothing in K2
      requires classification to produce action ids, and no drop/pass ids exist
      (§38 — invalid means "no object", the caller decides what that means).
    - **`core/dataplane/object_table.h`**: `ObjectTable<Id, T>` (immutable) and
      `ObjectTableBuilder<Id, T>` (mutable). One-based ids with slot 0 reserved,
      so there is no subtract-one arithmetic and invalid-id handling is obvious.
      `Lookup()` is an index, a bounds check and a validity check — no hashing, no
      allocation, no lock, no refcount — plus a batch form over spans. Capacity is
      runtime-configured; the id width never sizes the allocation. Storage is flat
      and inline, so `T` need only be movable.

    Two rules are deliberate and enforced where documented: published tables never
    mutate (every update builds a replacement generation), and erasing an id leaves
    a hole that is *never* automatically reused — RCU protects object memory
    lifetime, not semantic id reuse, and handing a stale id to a different object
    is an ABA problem the table cannot reason about. Stable ids across generations
    are trivial, because nothing is compacted or renumbered. `ObjectTable` owns no
    RCU domain and contains no RCU code: publication is `RcuPtr<ObjectTable<...>>`
    plus the runtime's domain (K1).

    Tests: `core/dataplane/object_table_test.cc`, 13 cases — the compile-time
    contract (§29, including the uint64 `StrongId` escape hatch), lookup,
    invalid/out-of-range ids, holes, no automatic reuse, replacement keeping the
    id, capacity enforcement, move-only objects, exactly-once destruction, batch
    lookup matching scalar lookup for valid/invalid/hole/out-of-range mixes at
    batch sizes 1/8/32, and the introspection counters.

    Two bugs found while writing the tests, both in the tests: the batch result
    span was sized for one batch while the comparison indexed the whole id list,
    and a replacement case used an id above the builder's capacity (correctly
    rejected by the implementation under test).

59. **`1be7deb1`** — **K2.3: the ObjectTable is proven through K1's publication
    path.** Three integration tests, no production module involved — K2 has no
    natural consumer yet, and inventing one so the phase looks used would be worse
    than saying so (§35). The harness is a test-only task module in the test
    binary.

    - **A worker holding an old generation keeps it alive**: the task reads the
      published table once, blocks, the control plane publishes a replacement for
      the same `ActionId`, and only then does the task resolve the id — through the
      old generation, still seeing the old object while a fresh read of the
      published pointer sees the new one. The retired generation is not destroyed
      while the worker holds it, becomes reclaimable once the worker reaches its
      next quiescent state, and its destructor runs on the control thread, not on
      the packet worker.
    - **One grace period covers several tables** (§33): an action table and a
      next-hop table are exchanged and retired against the same token, neither is
      reclaimed while the reader is online, both are reclaimed after it reports
      quiescence, and both replacements are active.
    - **Publication storm** (§34): 10000 generations of the same table with a live
      reader publishing and reclaiming continuously; exactly the active generation
      remains, every retired generation is destroyed exactly once, the retirement
      queue returns to zero.

    The file is split by EAL dependence: the worker-holding test needs a real
    worker and lives in `object_table_rcu_test.cc`; the shared-grace-period and
    storm tests need nothing but the domain, so they live in
    `object_table_publication_test.cc` and run under ASan/UBSan. That split is
    forced by DPDK, not by taste: `rte_eal_init()` cannot succeed under ASan here
    ("IOVA exceeding limits of current DMA mask"), so anything that launches a
    worker is out of the sanitizer lane's reach, which is what §34's "non-EAL
    parts" means in practice.

    One bug found, again in the test: an assertion that expected one live action
    where the retired generation plus the active one are both alive.

60. **`38467dbb` plus `9049ba11`** — **K2.4/K2.5: the representation
    question, measured and decided — flat inline remains the generation
    ownership model.** `core/dataplane/object_table_bench.cc` contains the
    dense cross-product baseline: raw indexed storage, bare flat storage,
    `ObjectTable`, and benchmark-only indirect slots across payload sizes,
    table capacities, access distributions, batches, full builds and
    one-object replacements. Uniform and mixed access walk the table rather
    than sampling a fixed 4096 ids, and every lookup consumes the payload tag,
    so the benchmark measures cache footprint and the loaded object rather than
    an optimized-away pointer calculation.

    The original K2.4 numbers were recorded before K2.6 corrected the benchmark
    payload layout. They are retained in the earlier history for provenance,
    but are not treated as final numbers: `Payload<4>` was eight bytes because
    its tag followed a zero-length `std::array`, and the consumed field was at
    the end of the object. K2.6 reruns the dense rows with exact sizes and a
    front-loaded hot field.

    The indirect replacement row is intentionally not a production comparison.
    Its pooled object replacement destroys the old object immediately; a
    published pointer table could still reference that object. An RCU-correct
    indirect representation would defer object retirement or version ownership,
    so the replacement result is a lower bound for the unsafe/simple lifetime
    model. The flat generation remains the boring correct baseline.

61. **`f6624799`** — **K2.6 hardening** — the narrow follow-up requested
    before K3:

    - **`core/dataplane/strong_id.h`** now constrains `Rep` with
      `std::unsigned_integral` and provides `StrongIdHash<Id>` without adding
      implicit representation conversions. `object_table_test` proves the
      typed id works as an `unordered_set` key.
    - **`core/dataplane/object_table.h`** now treats unequal `LookupBatch`
      spans as a caller precondition (`promise`); it no longer silently
      truncates to the shorter span. The old shorter-result test was removed
      because it tested the rejected contract.
    - The benchmark payload has exact sizes (`sizeof(Payload<4>) == 4`,
      `sizeof(Payload<16>) == 16`) and places the consumed tag first.
    - The benchmark adds the requested sparse high-water matrix: 100/75/50/
      10/1% occupancy, compact-low/uniform-high-water/churn-style hole
      distributions, 64K and 1M high-water capacities, 16/64/256-byte
      payloads, and three benchmark-only representations:
      `ObjectTable`, flat raw storage plus a validity bitmap, and indirect
      pointer slots. Uniform sparse lookup walks every high-water id, so
      holes are part of the measured working set. The bitmap candidate reduces
      validity metadata to one bit per slot but still reserves the full flat
      object range; indirect storage reduces sparse memory but needs deferred
      object retirement under RCU. Neither alternative becomes a public
      representation in K2.6.

    The sparse matrix completed as 270 registered rows in the focused
    benchmark run. It is a workload characterization, not a claim that one
    layout wins every occupancy/payload regime: K3 must keep generation
    ownership, result semantics, and any storage policy independently
    selectable.

    Verification for this follow-up: `core/object_table_bench` builds with
    GCC; `core/dataplane_object_table_test` passes all 13 cases; and the
    sparse benchmark executes all 270 rows. The full Meson graph remains the
    required closeout check after the final K2.6 edit.

62. **`9420ba12`** — **K3.1 classifier substrate** — the reusable runtime and
    typed frontend contracts now exist under `core/classifier/`, without
    changing `ExactMatch`, `WildcardMatch`, or `ACL`:

    - **`classifier.h` / `runtime_schema.h`** define `bess::classifier`,
      `ClassifierError`/`std::expected` construction results, explicit packet
      versus metadata sources, checked versus caller-guaranteed bounds, result
      modes, backend vocabulary, `BackendInfo`, and `ResultSlot` as a strong
      type distinct from `ActionId`.
    - **`ExtractPlan`** compiles resolved arbitrary-width runtime fields into
      coalesced operations, exact-size `memcpy` kernels, explicit batch stride,
      and single-packet/single-metadata/generic batch kernels. **`ResultPlan`**
      independently compiles and coalesces value-to-metadata placement.
      Neither plan carries protobuf objects, metadata names, strings, or
      `Module` pointers into packet execution.
    - **`byte_key.h` / `typed_exact.h`** provide canonical `ByteKey<N>`,
      explicit `KeyTraits`, safe endian/bit-cast helpers, constrained backend
      concepts, and a metadata-free `ExactTable<Key, Result, Backend>`.
      Typed lookup remains ordinary template code with no mandatory virtual
      dispatch; empty and move-only backends are covered by static/runtime
      tests.
    - **`backend.h` / `generation.h`** provide generation-level runtime
      function-pointer type erasure (one batch dispatch), immutable ownership of
      extraction/backend/result state, and the `RuntimeClassifierGeneration`
      seam for K1 `RcuPtr` publication. The fake-generation test proves an old
      generation remains coherent until its grace period completes.
    - Meson registers eight classifier unit-test binaries and
      `core/classifier_bench`. The benchmark compares direct versus compiled
      extraction and placement independently, and direct versus typed-wrapper
      lookup. These are smoke/abstraction-cost measurements, not backend
      winner claims. K3.2 remains responsible for Cuckoo/rte_hash implementations
      and selection measurements.

63. **`e41f2044`** — **K3.2 exact-backend laboratory and contract pressure fixes** —
    the exact classifier backends, runtime result transport, and normalization
    contracts now exist and are measured across multiple representation and
    workload axes, leaving `ExactMatch` untouched until K3.3:

    - **Result transport and hit masks (`backend.h`)**: `RuntimeExactBackend<Result>`
      is parameterized on the semantic result type (`gate_idx_t`, `ResultSlot`,
      or `ActionId`) rather than hard-wiring `ResultSlot`. Batch lookup
      returns a 64-bit hit mask (`uint64_t`) where bit $i$ is set iff `results[i]`
      is valid, avoiding per-packet optional allocations and sentinel values.
    - **Orthogonal concepts and native batch dispatch (`typed_exact.h`)**:
      replaced monolithic `ExactBackend` with `ScalarExactBackend<B, Key>`,
      `BatchExactBackend<B, Key>`, and `MeasurableBackend<B>`.
      `ExactTable::lookup_batch` dispatches natively via `if constexpr`
      when `BatchExactBackend` is satisfied, and gracefully falls back to a
      scalar loop setting one hit bit per valid result otherwise.
    - **Packed value store (`packed_value_store.h`)**: contiguous, generation-owned
      store indexed by 1-based `ResultSlot` (slot 0 reserved for invalid/miss).
      Allows arbitrary runtime byte payloads to be resolved from internal position
      tokens without leaking semantic IDs.
    - **Per-field normalization mask (`runtime_schema.h`, `extract_plan.h`)**:
      `RuntimeKeyField` carries a byte-width `Normalization` mask (empty = plain exact
      all-ones). `ExtractOp` applies the mask in-place after copying, preserving
      odd-size generic key boundaries while supporting field masking without
      regressing to 8-byte integers.
    - **CuckooMap exact adapter (`cuckoo_exact.h`)**: typed `CuckooExactBackend<Key, Result>`
      wrapping `bess::utils::CuckooMap`, satisfying `ScalarExactBackend` and
      `MeasurableBackend`. Internal storage classes (`8`, `16`, `32`, `64`, `128`,
      `256` bytes) under `detail::` adapt CuckooMap to runtime keys while hashing
      and comparing only logical key bytes.
    - **DPDK hash backend (`rte_hash_exact.h`)**: `RteHashPositionBackend` and
      `RteHashDataBackend<Result>` provide position-mode and direct-data lookups.
      `lookup_batch` utilizes DPDK's native `rte_hash_lookup_bulk` and
      `rte_hash_lookup_bulk_data` with hit masks. DPDK concurrency features
      (`RW_CONCURRENCY`, `RW_CONCURRENCY_LF`, internal QSBR) are kept off;
      whole-generation RCU handles updates.
    - **Small and Direct baselines (`small_exact.h`, `direct_exact.h`)**:
      `SmallExactBackend` provides a linear-scan baseline for $\le 64$ rules;
      `SortedFlatBackend` provides binary search over packed arrays;
      `DirectExactBackend` provides bounded direct array indexing for 1-byte
      and 2-byte key domains.
    - **Measurements (`classifier_bench.cc`)**: added smoke-baseline coverage for
      DirectExact (1.08 ns / 1.51 Glookups/s), SmallExact (~4.1 ns/lookup),
      CuckooExact (~5.8 ns/lookup), and rte_hash position/data bulk (~10 ns/lookup)
      across batches of 1, 8, 16, and 32. These are **not a backend-selection
      experiment**: fixed 8-byte keys, all-hit traffic, fixed 32-rule Small and
      64-rule Cuckoo/rte_hash cases, no rule-count sweep, miss mix, key-width
      sweep, runtime-erased Cuckoo, SortedFlat measurement, build/rebuild cost,
      or memory comparison. Cuckoo uses `ByteKeyHash`/FNV while rte_hash uses
      its default hash, so the numbers measure backend + hash choice, not just
      the table implementation. Do not derive `Auto` thresholds from them.

 64. **K3.3 `ExactMatch` cutover to the runtime classifier** — the module no
     longer owns `ExactMatchTable`/`ExactMatchKey`/`MakeKeys()`; each immutable
     generation owns a compiled `ExtractPlan` (kCheck), a forced-Cuckoo
     `RuntimeExactBackend<gate_idx_t>` over densely packed keys, the default
     gate, and the control-plane rule vector, published through the existing
     `RcuPtr`:
     - **Matching semantics preserved**: rule bytes pack verbatim (legacy
       `gather_key` applied no mask) while packet/metadata bytes are masked on
       extraction with the legacy BE/LE converted mask bytes, so rules with
       nonzero masked-off bits stay unmatchable exactly as before. Module
       limits (8 fields, 1-8 bytes, packet offset 0-1024) are unchanged;
       protobuf API, commands, gates, duplicate-overwrite, delete-not-found,
       clear-keeps-default, runtime-config replacement, and introspection
       output are unchanged. `GetInitialArg` emits the same converted mask
       bytes. `utils/exact_match_table.h` stays for `hash_lb` (key
       construction only).
     - **Metadata offsets at the right lifecycle point**: attribute IDs are
       still registered once at `Init()`; physical offsets are baked into the
       plan at every build and refreshed at `PreResume` (which runs after
       `SetupMetadata` recomputes offsets on the global-resume path and for
       every module attached to paused workers on the `WorkerPauser` path).
       `ControlPlane::ResumeWorker()` runs no hooks and recomputes no
       offsets, so it cannot stale the plan (audited, not changed). Refresh
       failure cannot leave a stale plan: the dispatcher ignores ordinary
       `OnEvent` errors, so failure publishes a fail-closed generation that
       routes everything to the default gate and logs the cause.
     - **No over-reads**: the packet span is the first segment only
       (`head_data`/`data_len`); metadata is the 128-byte region. Short
       packets fail extraction per packet under kCheck and take the default
       gate. Batch key scratch is zeroed over the used region only, so every
       hashed byte is initialized and `valid & hits` selects the gate.
       `ProcessBatch()` does one RCU read, one extraction dispatch, one
       backend dispatch, and no allocation, name resolution, or config
       parsing (verified by inspection).
     - **Proof**: `modules/exact_match_migration_test` differentials the
       legacy table against the new path over thousands of random
       packet/metadata inputs across 5 field/mask/rule configurations plus
       planted hits (including unmatchable masked-off-bit rules), short
       packets, and multisegment tails; `extract_plan_test` pins gap-byte
       behavior; `exact_match.py` gains a masked-off-bits integration test.
       `modules/exact_match_bench` measures complete old
       (`MakeKeys`+`CuckooMap`) vs new (`ExtractPlan`+runtime Cuckoo) paths:
       at 2 fields/batch 8 the new path costs ~2.5x per packet (~14ns vs
       ~6ns; new also pays bounds checks while legacy loads are unchecked),
       scaling similarly at 1/8/16/32 and 1/2/4/8 fields across hit/miss/mixed
       traffic. Rebuild costs 117us/1K, 968us/10K, 9.1ms/100K rules.
       Follow-ups: compact-scatter for invalid keys, CRC-family hashing for
       the runtime Cuckoo backend, and richer miss/key-width sweeps before any
       `Auto` policy work.
     - **Deliberate deviations from legacy** (all previously UB or crash):
       value_bin masks longer than 8 bytes are rejected instead of
       overflowing the stack mask word; zero-field modules fail `Init`
       instead of OOB-crashing on the first batch; commands fail fast when a
       metadata attribute has no valid offset instead of serving crashes.

65. **`6e074b86` — K3.3.1 runtime exact fast-path recovery (2026-09-22)** —
     review follow-up to K3.3, intentionally before K3.4:
     - `RuntimeCuckooKey<StorageBytes>` now stores only fixed-width key bytes;
       logical length is stateful hash/equality configuration, entries are
       naturally aligned without per-entry size metadata, and duplicate
       generic rule keys are rejected while `BackendInfo::rule_count` reports
       the actual map count.
     - Runtime Cuckoo hashing uses DPDK CRC32C instead of the retired FNV-1a
       byte loop. `exact_match_bench` now measures FNV, runtime CRC, legacy
       CRC, and the key-materialization cost over identical 8-byte values.
     - `ExtractPlan` precomputes required packet/metadata extents, executes
       exact-width masked copies for 1/2/4/8-byte fields, returns a per-packet
       validity mask, and exposes dense key coverage. `ExactMatch` skips dense
       scratch zeroing on the all-valid path and zeroes only invalid rows
       before the backend lookup.
     - `modules/exact_match_bench` splits extraction, prebuilt lookup,
       end-to-end, hashing, zeroing, and rebuild measurements. Its matrix now
       covers default-mask contiguous, masked, non-contiguous packet, and
       packet-plus-metadata schemas. The mixed-source legacy comparison uses a
       safe semantic mirror because `MakeKeys(const void **)` has no metadata
       input.
     - Focused GCC and Clang tests, the ExactMatch differential suite, and the
       complete K3.3.1 benchmark matrix pass in the current tree. GCC, Clang,
       and ASan+UBSan full builds complete. ASan+UBSan focused coverage passes
       ExtractPlan, Cuckoo, and migration tests; the Rte hash classifier test
       cannot initialize DPDK EAL under sanitizer because its memory allocation
       fails in this environment. The prior full GCC suite run recorded 66
       passes; after temporary 1440-minute passwordless sudo was enabled, the
       Python and module-integration targets each pass individually, covering
       all 68 registered targets.

66. **`0e1d00fd` — K3.3.2 borrowed/prehashed runtime Cuckoo lookup recovery
    (2026-09-22)** —
    - `CuckooMap` now exposes heterogeneous `FindAs`, raw-hash
      `FindPrehashedAs`, and diagnostic `FindPrehashedAsWithStats` APIs. The
      probe type is distinct from the stored fixed-width key, so the runtime
      batch loop can compare directly against packed key bytes without
      constructing a `RuntimeCuckooKey` per packet.
    - `cuckoo_exact.h` uses a short-lived borrowed `RuntimeCuckooProbe`,
      computes each raw hash once, and binds exact-width 1/2/4/8/16-byte
      probe hash/equality kernels at backend construction. The 1/2/4/8-byte
      kernels use DPDK's scalar CRC entry points; other widths retain the
      generic byte-range path.
    - The lookup ladder is now explicit in `modules_exact_match_bench`:
      legacy lookup, direct prebuilt runtime keys, materialized runtime keys,
      borrowed lookup, prehashed borrowed lookup, fixed batch lookup, and the
      type-erased production path. At variant 0 / batch 8 / alternating
      hit-miss traffic, one final five-repetition run gave mean times of 33.3,
      51.7, 41.4, 39.4, 22.9, 28.5, and 29.0 ns/batch respectively (CPU
      scaling was enabled; absolute values are noisy).
    - The same run measured 40.6 ns/batch for legacy extraction plus lookup
      and 41.8 ns/batch for ExtractPlan plus the production runtime backend.
      The complete schema/batch/mix matrix exits
      successfully. Probe instrumentation observed 100% primary hits on
      all-hit traffic, 100% misses on all-miss traffic, and
      46.875% primary / 3.125% secondary / 50% miss at mixed batch 32.
    - Fixed probe hashes for widths 1/2/4/8/16 are tested against
      `rte_hash_crc`; the benchmark fixture also checks stored-key CRC and
      the legacy 8-byte CRC helper on identical values. Focused Cuckoo,
      runtime-backend, and migration tests pass, and the full GCC Meson build
      passes.
    - No batch pipelining or separate `rte_hash` comparison was justified:
      the production type-erased lookup is within the legacy lookup rung and
      the end-to-end result is within measurement drift. K3.4 remains
      untouched.

67. **K3.4 typed exact-classifier author surface** — the second first-class
    consumer model, proven without touching any module:
    - **Typed keys need no registration.** `CuckooExactBackend` and
      `SmallExactBackend` no longer require `ClassifierKey`. They accept an
      author's own `Key`/`Hash`/`Equal`, so a natural `struct FlowKey` with
      `operator==` and a hand-written hash works with no `KeyTraits`
      specialization and no `ByteKey`. `DefaultTypedEqualT<Key>` uses a
      registered `KeyTraits<Key>::equal_type` when present and otherwise falls
      back to the author's `operator==`; hashing has no fallback, because
      object representation is never hashed implicitly. The
      `ClassifierKey`/`CanonicalByteKey`/`TypedClassifierKey` concepts stay for
      the generic paths that do want representation-level hashing.
    - **`ExactTable` is decoupled from the key concept** and now requires only
      `std::is_object_v<Key>` plus `ScalarExactBackend<Backend, Key>`. It owns
      the backend with `[[no_unique_address]]`, has no virtuals, and adds no
      storage (`static_assert(sizeof(ExactTable) == sizeof(Backend))`).
    - **`BatchExactBackend` names the result type explicitly**
      (`BatchExactBackend<Backend, Key, Result>`). This replaced a
      `detail::backend_result_type_t` trait that defaulted to `void`; a backend
      with a native `lookup_batch` but no `result_type` alias previously
      hard-errored while forming `std::span<void>` inside the concept. Scalar
      lookup stays mandatory, native batch stays an orthogonal capability, and
      `ExactTable` selects it with `if constexpr`.
    - **Typed results are unrestricted.** `gate_idx_t`, `ActionId`, 2/4/8/16/32
      byte structs, `ResultSlot`, and pointer-returning backends all work
      directly; a 32-byte decision needs no `ResultSlot` and no indirection.
      `Small`/`Direct`/`Cuckoo` are asserted to agree on hits and results for
      identical rules and keys, and a backend whose native batch path is
      reachable is asserted to have that path reach the caller unchanged.
    - **Assembly equivalence is exact, not approximate.** Two translation
      units, one exercising the author-written `backend.lookup()` loop and one
      calling `ExactTable::lookup_batch`, compiled with the production flags:
      the Cuckoo configuration used by the benchmark is byte-identical (122
      instructions, zero `call`s) and Small is identical modulo label
      numbering (34 instructions, zero `call`s). Neither unit contains
      `operator new`/`malloc`, and neither references `ExtractPlan`,
      `RuntimeExactBackend`, `RuntimeClassifierSchema`, `ConstBytes`, or
      protobuf — the typed path pulls in no runtime-generic machinery.
    - **`classifier/typed_exact_bench.cc`** measures three rungs over one
      logical workload: the author-written loop, the same backend behind
      `ExactTable`, and the runtime-generic equivalent. Both sides hash with
      DPDK CRC32C so a rung difference is framework cost rather than a hash
      artifact. Workloads are generic (4/8/16/32-byte byte keys, a `uint16_t`
      domain for `Direct`/`Small`, a 13-byte natural flow struct parsed from
      packet bytes, 2/4/8/16/32-byte results, 4/16/64/1K/10K/100K rules, batch
      1/8/16/32, hit/miss/alternating/hot-four distributions); 2112
      registrations complete with zero errors. One packet-parser benchmark
      compares `Packet → authored parser → typed Key → typed backend` against
      `Packet → ExtractPlan → packed bytes → runtime backend`, so neither side
      imitates the other's representation.
    - **Representative means** (5 repetitions, batch 8, alternating hit/miss,
      CPU scaling enabled — absolute values are noisy): Cuckoo K8/R4 at 64
      rules 33.8 / 39.6 / 24.8 ns, Cuckoo K32/R32 at 10K rules 67.6 / 71.8 /
      76.4 ns, Small K4/R2 at 64 rules 99.5 / 108 / 31.3 ns, scalar `Direct`
      R4 at 64 rules 6.29 / 5.39 / 29.6 ns, and the packet-parser flow
      workload 38.8 / 46.8 / 110 ns for Direct / Table / Runtime. The rungs
      differ by backend, hash-free framework cost, and representation; the
      Cuckoo K8 gap is code layout, since that exact configuration is
      byte-identical in assembly. **No universal backend winner is declared.**
    - **Deliberate non-changes**: no `Auto` policy, no batch-prefetch
      machinery, no migration of another module, and no runtime-generic
      machinery in the typed path. `FindPrehashedAsWithStats` stays diagnostic
      with a caller-owned `LookupStats&`; probe counters are not production
      `CuckooMap` state.
    - **Verification**: GCC and Clang full builds; `classifier_typed_exact_test`,
      `classifier_cuckoo_exact_test`, `classifier_small_exact_test`,
      `classifier_direct_exact_test`, `classifier_rte_hash_exact_test`, and
      `modules_exact_match_migration_test` pass under GCC, Clang, and
      ASan+UBSan; the full 2112-registration typed benchmark matrix exits 0; the
      whole registered suite passes (54 non-benchmark targets in one run, 15
      benchmark smoke targets in another).

68. **K3.5 generic masked/tuple-space substrate** — the mechanism behind
    `WildcardMatch`, extracted as a library with no module changes:
    - **Contract** (`classifier/masked_exact.h`): `RuntimeMaskedRule<Result,
      Priority>` is `{value, mask, priority, result}`; `RankedResult<Result,
      Priority>` is the per-tuple candidate `{priority, ordinal, result}`;
      `RuntimeMaskedBackend<Result, Priority>` owns one tuple per distinct mask,
      each holding a `RuntimeExactBackend<RankedResult<…>>` keyed by the masked
      value. Lookup is tuple-major, batch-minor: mask the batch once per tuple,
      exact-look it up, merge by rank. No second hash table — the packed keys,
      borrowed probes, hit masks, and immutable generations are the K3.3.2
      machinery reused as-is.
    - **Semantics fixed in the substrate, not inherited from the module**:
      `value & ~mask == 0` is required and a violation is rejected at build time
      rather than normalized; tuple count is unbounded (the module's
      `MAX_TUPLES = 8` stays a wire-compatibility concern); priority is
      `int64_t` by default with no narrowing; equal priorities resolve by
      `(priority, ordinal)` with the later rule winning, so the outcome does not
      depend on tuple iteration order — proven by a test that swaps the rule
      order and asserts the winner swaps with it. Duplicate `(mask, value)`
      entries collapse deterministically to the better rank instead of
      depending on insertion order. Key width is capped at 64 bytes at build
      time, which bounds the packet path's stack scratch.
    - **`MaskedBackendInfo`** (new, in `classifier.h`) reports tuple-space
      metrics through `WildcardBackendKind::kTupleSpace` rather than being
      forced through the exact-matching `BackendInfo`.
    - **`classifier_masked_bench.cc`** compares three rungs — naive linear rule
      scan, legacy-style tuple-space (per-tuple `CuckooMap` over 8-byte-word
      keys with the module's CRC32C-chained hash, reimplemented so no module
      code is involved), and the substrate — plus two isolations, mask-only and
      a single tuple's exact lookup. Masks are arbitrary bit patterns, never
      prefixes; each configuration reports its measured average tuple-match
      count so the distribution labels are checked against the data. The sweep
      varies key width 4/8/16/32/64, tuple count 1/2/4/8/16/32, rules per tuple
      4/16/64, mask density 50/75/100, identical-vs-diverse masks, batch
      1/8/16/32, and one-tuple/multi-tuple/all-miss traffic; 162 registrations
      exit 0.
    - **Representative means** (5 repetitions, batch 8, 8-byte keys, 16
      rules/tuple, density 75, diverse masks, one-tuple traffic; CPU scaling
      enabled): tuple count 1/2/4/8/16/32 gives substrate 66.6/101/167/349/621/
      1187 ns against legacy 63.2/127/247/496/818/1364 ns and naive
      174/271/485/900/1723/3432 ns. The substrate and the legacy mechanism are
      equal within noise at one tuple (where the substrate still runs its merge)
      and separate from two tuples upward. At four tuples the isolation
      measures mask-only ≈ 31 ns and one tuple's exact lookup ≈ 22 ns, so
      31 + 4×22 ≈ 119 ns of the measured 167 ns is masking plus exact lookup and
      the remainder is the merge — the decomposition the isolation exists to
      provide.
    - **Deliberate non-changes**: `WildcardMatch` is untouched. Its defects
      (`ProcessBatch()`'s unchecked fixed 8-byte loads, the
      `total_key_size_ == 0` underflow, `DelEntry()`'s inverted
      `CuckooMap::Remove()` handling, `Clear()` leaving tuple objects behind,
      partially-applied `SetRuntimeConfig()`, `int64` priority narrowed to
      `int`, and unvalidated value/mask lengths) are K3.6 work with regression
      tests written first; none of them is encoded in the substrate.
    - **Verification**: `classifier_masked_exact_test` (14 tests) passes under
      GCC, Clang, and ASan+UBSan. ASan caught a real defect while writing it —
      rule spans pointing into temporary byte vectors — which is why the tests
      now build rules through an owning `RuleSet` instead of a helper that
      accepts temporaries. GCC and Clang full builds pass; the 55
      non-benchmark targets pass in one full run.

69. **K3.6 `WildcardMatch` cutover to the masked substrate** — the module no
    longer owns `wm_hkey_t`, `wm_hash`, `wm_eq`, per-tuple `CuckooMap`s, or a
    mutable rule set; it converges on the K3.3 `ExactMatch` structure:
    - **Generation ownership**: `Generation { rules, default_gate, extract,
      backend, key_size, extraction_valid, baked_source_offsets }` published
      through `bess::rcu::RcuPtr` on the runtime `RcuDomain`. One acquire load
      per batch, no in-place mutation a worker can observe, retired generations
      destroyed on the control thread. `add`, `delete`, `clear`, and
      `set_runtime_config` are now `THREAD_SAFE` — they only build and publish
      immutable generations.
    - **Extraction**: dense `field0 || field1 || ...` key from a compiled
      `ExtractPlan` under `BoundsPolicy::kCheck` and **no normalization masks**
      (wildcard masks belong to rules, not to extraction). The legacy
      `align_ceil(size_acc, 8)` padding is gone; it never affected matching
      because the legacy mask zeroed those bytes, and the differential tests
      confirm identical gates. Fields reaching past the packet or into a later
      segment fail extraction and take the default gate instead of being
      over-read.
    - **Matching**: `RuntimeMaskedBackend<gate_idx_t, int64_t>`, one tuple per
      distinct mask, highest `(priority, ordinal)` wins. No second hash table
      and no module-owned tuple machinery.
    - **Rule canonicalization**: identity is `(mask, value)`. `UpsertRule`
      erases the previous occurrence and appends the new one, so a later
      command overwrites an earlier one **regardless of priority** — what
      inserting the same key into the live tuple table did — and the substrate's
      ordinal then represents command order. Both `add` and `set_runtime_config`
      use it. This is pinned by a module test (priority 100 then priority 1 on
      the same `(mask, value)` leaves priority 1) and by the pre-existing Python
      `test_wildcardmatch`.
    - **Limits**: `kMaxFields = 8`, `kMaxFieldSize = 8`, `kMaxTuples = 8` are
      module constants. Zero fields and more than eight fields fail `Init()`
      (the legacy code had no `MAX_FIELDS` check at all). The tuple ceiling is
      enforced on the *active distinct masks of the candidate generation*, after
      canonicalization, so a rejected candidate leaves the published one
      serving and `clear()` genuinely restores capacity.
    - **Metadata lifecycle**: attribute IDs registered once at `Init()`; physical
      offsets baked per generation and compared at `PreResume`; a moved offset
      rebuilds, and a rebuild failure publishes a fail-closed generation that
      routes everything to the default gate rather than serving a plan with
      stale offsets.
    - **Legacy defects fixed by the new model** (not emulated): unchecked fixed
      8-byte packet/metadata loads; the `(total_key_size_ - 1) / 8` underflow at
      zero fields; missing `MAX_FIELDS` enforcement; `DelEntry()`'s inverted
      `CuckooMap::Remove()` handling (a successful removal returned early, so
      empty tuples were never erased, and a missing key in an existing tuple
      reported success); `Clear()` leaving tuple objects behind; partially
      applied `SetRuntimeConfig()`; `int64` priority narrowed to `int`; and
      unvalidated binary value/mask lengths.
    - **Binary length compatibility**: `value_bin`/`mask_bin` shorter than the
      field are accepted and zero-padded (the legacy stack-local decode started
      zeroed); longer input is rejected instead of copied past the field word.
      `value_int` keeps its legacy big-endian field encoding.
    - **Proof**: `modules_wildcard_match_test` (11 tests) covers the module
      limits, duplicate overwrite, the tuple ceiling and clear-restores-capacity,
      delete existing/missing, binary length rules, non-canonical rejection, full
      `int64` priority round trip, atomic failed `set_runtime_config`,
      `get_runtime_config` byte-identical round trip, `get_initial_arg`, and
      integer-encoded rules. `bessctl/module_tests/wildcard_match.py` grew from
      4 to 9 packet-level tests: non-prefix masks, mixed 1/3/7-byte fields,
      multi-mask priority plus equal-priority tie order (asserted both ways),
      delete/clear with capacity reuse, and a short packet taking the default
      gate. The pre-existing tests (priority override, metadata matching,
      `get_initial_arg`/`set_runtime_config` parity) still pass unchanged.
    - **Integration tax** (`modules_wildcard_match_bench.cc`, 5 repetitions,
      mean ns/batch, packet-only, single-hit traffic): the migrated path is
      slower at one field/small batch — 55.6 vs 48.6 at 1 tuple/batch 8, 174 vs
      199 at 1 tuple/batch 32 — and faster from two tuples upward: 4 tuples/batch
      32 539 vs 655, 8 tuples/batch 32 986 vs 1480. The one-tuple deficit is the
      price of `kCheck` extraction and the merge; the ≥2-tuple win is the
      substrate's masking/merge against per-tuple `CuckooMap` lookups.
    - **Measured finding, not fixed here**: the integration cost is dominated by
      **field count, not by metadata**. At 4 tuples/batch 32 the migrated path
      costs 492 ns for one 4-byte packet field and 1235 ns for two fields, and
      the two-field case is 1238 ns with the second field read from metadata
      versus 1235 ns from the packet — identical. `ExtractPlan` has
      single-source fast kernels (`kSinglePacket`/`kSingleMetadata`) and a
      generic per-op kernel, so a second field leaves the fast path. That is
      K3.7 material (a multi-field extraction kernel), recorded here rather than
      papered over.
    - **Rebuild cost**: plan compile + masked-backend build is 0.7 us (1 tuple,
      8 rules), 3.05 us (4 tuples, 8 rules), 6.8 us (8 tuples, 8 rules), 112 us
      (8 tuples, 64 rules), and 500 us (8 tuples, 256 rules).
    - **Two real defects found while doing this**: `detail::MaskBatchVariable`
      was a non-inline function defined in a header, which only linked because a
      single TU had included it — `wildcard_match.cc` made it a multiple
      definition. And `RuntimeMaskedBackend::lookup_batch` value-initialized its
      ~7 KiB of stack scratch and rank buffers on every call, which dominated
      the useful work at small batches; the buffers are now left
      uninitialized with the write-before-read argument recorded in the code
      (masking writes every byte of the used rows; `candidates[i]` is read only
      on a per-tuple hit bit; `best[i]` only once `matched` says a previous
      tuple wrote it). The K3.5 numbers in entry 68 were taken before that
      second fix, so the substrate is faster there than those figures show.
    - **Verification**: GCC and Clang full builds; the 56 non-benchmark targets
      pass in one full run, including the 9-test WildcardMatch integration
      suite; `classifier_masked_exact_test` and the module test pass under
      Clang; the 162-registration WildcardMatch benchmark matrix exits 0. Under
      ASan+UBSan the module test cannot run: it hits the same `Any::PackFrom`
      null-descriptor SEGV that the pre-existing `module_test` hits in this ASan
      build (verified by running both), so the module's packet path has no
      sanitizer coverage and the substrate's does.

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

- [x] **Daemon mode (`bessd` without `-f`) recursed in logging on glog >= 0.7** —
      fixed by `05413f53` (entry 54): the `fopencookie()` feedback path is gone,
      C stdio in daemon mode is deliberately discarded, and a regression test
      exercises logging after `Daemonize()`. Details in the subsection below.

### 8. Daemon-mode glog recursion — FIXED (`05413f53`, entry 54)

**Status: fixed.** `CloseStdStreams()` no longer installs `FILE*` callbacks that
call `LOG()`; C stdio in daemon mode goes to `/dev/null` by design and the C++
streams are bridged into glog one-way. Reproduced before the fix on glog 0.7.1
(absl-based) as a SIGSEGV from stack exhaustion, and covered by a regression
test that exercises `LOG(INFO)`, `fprintf`, `std::cout` and `std::cerr` after
`Daemonize()`.

A pre-existing daemon-mode bug was exposed during Meson verification on systems with glog >= 0.7.

Observed shape:

```text
bessd daemon mode
  ↓
CloseStdStreams()
  ↓
stdout/stderr replaced by cookie-backed FILE*
  ↓
cookie callback calls LOG(...)
  ↓
newer glog writes log output back to that FILE*
  ↓
callback calls LOG(...)
  ↓
recursive logging until stack overflow / segfault
```

The observed backtrace alternates through approximately:

```text
LogMessage::Flush
SendToLog
fwrite
BESS cookie callback
LOG
...
```

with BESS frames around `core/bessd.cc:303/305`.

Ubuntu CI currently uses an older glog where this does not reproduce because stderr logging behavior differs, so green CI does not prove daemon mode is safe on modern glog.

#### Required follow-up

Fix this independently before relying on daemonized operation on glog >= 0.7.

The fix should ensure the replacement stdio sink cannot recursively enter glog.

Do not mix this bug fix into G0 transaction semantics.

Add a regression test or process-level smoke that launches actual daemon mode under a modern glog implementation if practical.

---


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

## Order of work

The active order is deliberately **not** phase-number order (consolidated roadmap
§1 and §26):

```text
fix modern-glog daemon mode            DONE (§8, entry 54)
  |
  v
G0   C++ transactional control-plane core   DONE (§9, entries 41-53)
  |
  v
K1   generic RCU/QSBR publication and reclamation   DONE (entries 55-57)
K2   ActionId + immutable action/object tables
K3   unified runtime-schema classifier framework
K4   packet parsing/mutation primitives
K5   generic software metering
K6   worker-local statistics/snapshots
K7   route and next-hop abstraction
K8   generic fragmentation/reassembly                     §10
  |
  v
G1   desired-state API + Go/C++ SDKs + C++ bessctl        §14
  |
  v
D    ARM64 / runtime SIMD / rte_bpf                       §16
  |
  v
F    release, packaging, observability, CI                §17
  |
  v
H/I  language / tooling / type-safety hardening           §18, §19
```

Hardware-gated work is parked under **Phase C-HW** (§5) and must not block this
sequence.

### Naming map: older roadmap text -> current sections

| older heading | subject | current home |
|---|---|---|
| `Phase G — C++-only control plane` (including its older `G1` "compatible" / `G2` "breaking" variants) | control-plane redesign | §9 **G0** (transactional core) + §14 **G1** (desired-state API and SDKs); the compatible-vs-breaking wire decision now lives in §14.2 |
| `Phase D — ARM64 + portable SIMD` | ARM64 / SIMD / `rte_bpf` | §16 |
| `Phase F — Ops / release automation` | releases / observability | §17 |
| `Phase H — C++23/26 tooling adoption` | language / tooling | §18, plus the retained 2026-09-17 research detail |
| `Phase I — Compile-time invalid-state prevention` | type safety | §19 |
| new `Phase K` | generic dataplane substrate | §10; no older counterpart |
| older `Phase C` items | Linux I/O | software items are complete (worker/lcore decoupling: entry 33); hardware items are parked in §5 |

The benchmark/experiment backlog is retained verbatim at the end of this
document; source comments cite its item numbers.

## Completed phases

### Phase A — DPDK/build modernization (complete as of 2026-09-12)

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


### Phase B — Packet/mbuf architecture

End state: stop mirroring `rte_mbuf` byte-for-byte in a C++ packet object.
`PacketHandle` is the transport/ownership representation (`rte_mbuf *`);
`PacketRef` is the only C++ packet-processing wrapper and remains one pointer
wide. BESS's own metadata lives in DPDK's supported mbuf private-data area
instead of a hand-maintained shadow layout. This makes Phase-A-style ABI-drift
bugs structurally impossible rather than merely caught by `static_assert`.

**This is now explicitly staged, not a single commit sequence** — Stage 1
introduced the private-area accessor, Stage 2A introduced the safe
`PacketHandle`/`PacketRef` intermediate and migrated consumers, and Stage 2B
replaced the backing representation. The Stage 2A seam provided the safe,
bisectable state that the original proposal lacked.

Stage 2B completed the two backend boundaries:

- `PacketPool` now sizes and populates native pktmbuf objects with one
  centralized element layout rather than `sizeof(Packet)` elements.
- `PacketRef` now uses native mbuf fields/helpers while PMD RX/TX receives the
  native handle array directly; no per-packet or per-burst wrap/unwrap
  conversion exists at the PMD boundary.

Stage 2C is staged rather than one cutover. Its first substage makes the
payload data-room size a `PacketPool` property; the following sections record
the jumbo/multisegment policy, external-buffer plumbing, and clone semantics.
The default remains the existing 2048-byte BESS payload limit plus
`RTE_PKTMBUF_HEADROOM`.

The benchmark comparison and observed native-API costs are recorded in the
completed Stage 2B section below. Follow-up performance work must preserve
the native representation rather than reintroduce an overlay.

#### Stage 1 — explicit private-area accessor (done, commit 16)

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
packets at real scale; this was the historical Stage 1 `priv()` path).
The `rte_mbuf`-mirroring union and `CheckMbufLayout()` were intentionally
left in that Stage 1 commit and were removed by the completed Stage 2B
cutover.

#### Stage 2A — `PacketHandle`/`PacketRef` seam and consumer migration (done)

Stage 2A landed the storage/processing boundary without changing packet
layout or allocator representation. `PacketHandle` names the stored packet
handle used by batches, rings, queues, ports, and ownership helpers;
`PacketRef` is the pointer-sized, trivially copyable, non-owning view used by
packet-processing code. `PacketBatch::packet()` supplies that view, while
`handles()` keeps native arrays at transport boundaries. Core drivers,
allocators, queues/rings, built-in modules, gate hooks, the sample plugin,
benchmarks, and packet utilities now use the seam. CI run
`35464737427` passed for both g++ and clang++.

The post-Stage-2A baseline below was captured on the development machine with
five repetitions and `--benchmark_min_time=0.5s`; PMD values are mean
throughput in Mpps at burst sizes 1/2/4/8/16/32.

| `packet_bench` benchmark | Mean |
| --- | ---: |
| `BM_PacketAllocFree` | 6.00 ns |
| `BM_PacketAllocFreeBulk` | 34.8 ns |
| `BM_PacketHeadData` | 0.258 ns |
| `BM_PacketAppendTrim` | 2.54 ns |
| `BM_PacketMetadataAccess` | 0.188 ns |
| `BM_BatchForward` | 2.12 ns |

| `pmd_bench` benchmark | 1 | 2 | 4 | 8 | 16 | 32 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `BM_PmdNullTx` | 6.79 | 12.87 | 25.07 | 47.83 | 78.65 | 149.01 |
| `BM_PmdNullTxEndToEnd` | 58.40 | 89.88 | 156.55 | 253.75 | 289.30 | 385.26 |
| `BM_PmdRingRoundTrip` | 6.50 | 12.50 | 25.13 | 48.58 | 89.02 | 159.25 |
| `BM_PmdRingRoundTripEndToEnd` | 55.05 | 89.32 | 123.28 | 218.26 | 331.13 | 466.06 |

#### Stage 2B — native `rte_mbuf *` handle and `PacketRef` backend (done)

Stage 2B replaced the legacy overlay with native DPDK pktmbuf storage.
`PacketHandle` is `struct rte_mbuf *`; `PacketBatch` arrays therefore cross
the `rte_eth_{rx,tx}_burst` boundary directly with no wrap/unwrap conversion.
`PacketRef` provides native mbuf processing operations,
`BessPacketPrivate` contains only metadata and scratchpad, and every
PacketPool backend uses one centralized native element-size calculation. The
existing 2048-byte BESS payload limit remains deliberate; jumbo/data-room
changes, external-buffer pool plumbing, clone semantics, and offload work
remain later phases.

The PCAP receive path now drops the already-built chain when a later segment
allocation fails, before dereferencing that segment. The obsolete
overlay-specific chain cast is gone.

Correctness evidence: the native packet layout/ownership tests in
`core/packet_test.cc` pass, and the migrated TCP reconstruction test compiles
against `PacketRef`. The full daemon/module object graph compiles with g++ on
the local DPDK 25.11.3 install; the local static link remains unavailable
because this Arch environment does not ship the transitive static gRPC/Abseil
archives. CI is the authoritative full-link gate.

The packet benchmark comparison below uses median values from the same
five-repetition command as the Stage 2A baseline above. PMD values are mean
throughput in Mpps at burst sizes 1/2/4/8/16/32; every run reported zero drops.

| `packet_bench` benchmark | Stage 2A | Stage 2B | Delta |
| --- | ---: | ---: | ---: |
| `BM_PacketAllocFree` | 6.01 ns | 6.24 ns | +4% |
| `BM_PacketAllocFreeBulk` | 34.7 ns | 107 ns | +208% |
| `BM_PacketHeadData` | 0.257 ns | 0.286 ns | +11% |
| `BM_PacketAppendTrim` | 2.54 ns | 2.74 ns | +8% |
| `BM_PacketMetadataAccess` | 0.188 ns | 0.223 ns | +19% |
| `BM_BatchForward` | 2.12 ns | 2.41 ns | +14% |

| `pmd_bench` benchmark | 1 | 2 | 4 | 8 | 16 | 32 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `BM_PmdNullTx` | 6.74 | 13.09 | 25.37 | 48.38 | 84.21 | 147.24 |
| `BM_PmdNullTxEndToEnd` | 54.76 | 83.17 | 134.92 | 178.79 | 211.21 | 259.14 |
| `BM_PmdRingRoundTrip` | 6.36 | 11.90 | 23.75 | 45.01 | 79.71 | 134.41 |
| `BM_PmdRingRoundTripEndToEnd` | 46.10 | 69.32 | 99.96 | 149.24 | 190.22 | 234.01 |

The native packet representation and the Candidate-C performance recovery are
complete. The measured allocation regressions came from replacing BESS's
specialized simple-packet bulk lifecycle—the old hand-optimized raw-mempool
allocation/reset/free paths, including layout-specific x86 SIMD fast paths—
with DPDK's fully general
`rte_pktmbuf_alloc_bulk()`/`rte_pktmbuf_free_bulk()` lifecycle, not from an
intrinsic cost of native `rte_mbuf` storage. Candidate C restores that
specialized lifecycle through native DPDK raw bulk APIs while retaining checked
generic fallback for non-simple ownership cases. No overlay restoration is
planned.

##### Stage 2B final Candidate-C acceptance signoff

The final standardized comparison used A = final Stage 2A (`0d4da18a`), B =
the original native Stage 2B (`493dc520`), and C = the final Candidate-C
implementation in this change.
Each endpoint received ten interleaved observations on pinned CPU 2 with its
SMT sibling offline, `--benchmark_min_time=1.0s`, the same compiler (`g++`,
`-march=native`), and DPDK 25.11.3. The raw captures are archived as
`raw-captures.tar.gz`; `acceptance-summary.json`, `metadata.json`, and
`sha256sums.txt` preserve the compact summary, protocol, and integrity record
under `scratch/phaseb-study/acceptance-final/` on the validation host.

Packet benchmark medians are ns/op:

| Endpoint | A | B | C | A→B | B→C | A→C |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `BM_BatchForward` | 2.242 | 2.253 | 2.256 | +0.5% | +0.1% | +0.6% |
| `BM_PacketAllocFree` | 6.534 | 6.427 | 6.490 | -1.6% | +1.0% | -0.7% |
| `BM_PacketAllocFreeBulk` | 38.639 | 105.988 | 70.806 | +174.3% | -33.2% | +83.2% |
| `BM_PacketAppendTrim` | 2.676 | 2.718 | 2.693 | +1.6% | -0.9% | +0.6% |
| `BM_PacketHeadData` | 0.275 | 0.321 | 0.274 | +16.5% | -14.8% | -0.7% |
| `BM_PacketMetadataAccess` | 0.200 | 0.200 | 0.200 | -0.1% | +0.0% | -0.1% |

PMD benchmark medians are ns/burst. EndToEnd rows are synthetic local
allocator/lifecycle paths, not real-NIC throughput:

| Endpoint | Burst | A | B | C | A→B | B→C | A→C |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `BM_PmdNullTx` | 1 | 150.428 | 150.942 | 150.059 | +0.3% | -0.6% | -0.2% |
| `BM_PmdNullTx` | 2 | 153.723 | 155.171 | 154.091 | +0.9% | -0.7% | +0.2% |
| `BM_PmdNullTx` | 4 | 157.281 | 158.638 | 157.463 | +0.9% | -0.7% | +0.1% |
| `BM_PmdNullTx` | 8 | 165.842 | 167.800 | 166.942 | +1.2% | -0.5% | +0.7% |
| `BM_PmdNullTx` | 16 | 190.030 | 191.983 | 189.593 | +1.0% | -1.2% | -0.2% |
| `BM_PmdNullTx` | 32 | 213.639 | 217.075 | 210.785 | +1.6% | -2.9% | -1.3% |
| `BM_PmdNullTxEndToEnd` | 1 | 16.843 | 18.367 | 17.556 | +9.1% | -4.4% | +4.2% |
| `BM_PmdNullTxEndToEnd` | 2 | 22.004 | 24.073 | 23.320 | +9.4% | -3.1% | +6.0% |
| `BM_PmdNullTxEndToEnd` | 4 | 25.761 | 29.249 | 27.675 | +13.5% | -5.4% | +7.4% |
| `BM_PmdNullTxEndToEnd` | 8 | 32.281 | 41.995 | 39.980 | +30.1% | -4.8% | +23.8% |
| `BM_PmdNullTxEndToEnd` | 16 | 55.224 | 73.396 | 77.720 | +32.9% | +5.9% | +40.7% |
| `BM_PmdNullTxEndToEnd` | 32 | 84.665 | 117.453 | 138.863 | +38.7% | +18.2% | +64.0% |
| `BM_PmdRingRoundTrip` | 1 | 150.950 | 155.145 | 150.641 | +2.8% | -2.9% | -0.2% |
| `BM_PmdRingRoundTrip` | 2 | 154.348 | 159.741 | 154.640 | +3.5% | -3.2% | +0.2% |
| `BM_PmdRingRoundTrip` | 4 | 155.374 | 163.956 | 156.787 | +5.5% | -4.4% | +0.9% |
| `BM_PmdRingRoundTrip` | 8 | 162.526 | 178.671 | 163.346 | +9.9% | -8.6% | +0.5% |
| `BM_PmdRingRoundTrip` | 16 | 174.130 | 199.674 | 181.465 | +14.7% | -9.1% | +4.2% |
| `BM_PmdRingRoundTrip` | 32 | 195.215 | 230.095 | 204.927 | +17.9% | -10.9% | +5.0% |
| `BM_PmdRingRoundTripEndToEnd` | 1 | 17.740 | 22.457 | 19.058 | +26.6% | -15.1% | +7.4% |
| `BM_PmdRingRoundTripEndToEnd` | 2 | 21.438 | 29.402 | 23.529 | +37.1% | -20.0% | +9.8% |
| `BM_PmdRingRoundTripEndToEnd` | 4 | 30.373 | 38.171 | 34.177 | +25.7% | -10.5% | +12.5% |
| `BM_PmdRingRoundTripEndToEnd` | 8 | 34.736 | 50.293 | 41.743 | +44.8% | -17.0% | +20.2% |
| `BM_PmdRingRoundTripEndToEnd` | 16 | 47.130 | 83.937 | 73.189 | +78.1% | -12.8% | +55.3% |
| `BM_PmdRingRoundTripEndToEnd` | 32 | 74.644 | 140.722 | 127.380 | +88.5% | -9.5% | +70.7% |

Lifecycle decomposition at burst 32, using Candidate-C's checked/trusted
endpoints, was also run with all three pool modes:

| Pool mode | `A_CurrentChecked` | `B_CurrentTrusted` | `C_NormalChecked` | `D_NormalGeneric` |
| --- | ---: | ---: | ---: | ---: |
| `PlainPacketPool` | 68.970 | 47.977 | 73.362 | 99.323 |
| DPDK hugepages | 66.563 | 43.820 | 66.552 | 91.126 |
| BESS-managed hugepages | 131.761 | 145.568 | 164.310 | 179.515 |

The packet suite and PMD fixtures use `PlainPacketPool` for source-comparable
interleaved A/B/C measurements. The decomposition above supplies the
backend-specific pool comparison; DPDK and BESS hugepage modes were run with
1024 MiB hugepage backing where available.

The result separates cleanly. Native `rte_mbuf *` storage and the transport
boundary are complete. The generic native lifecycle caused the major
`BM_PacketAllocFreeBulk` regression; the checked raw native path recovered
substantial cost, from 105.988 ns to 70.806 ns, but remains 83.2% above A.
Native access and direct PMD transport remain effectively flat. `BM_PmdNullTx`
is within measurement spread across the burst sweep. `BM_PmdRingRoundTrip`
retains +4.2% and +5.0% A→C cost at bursts 16 and 32 because its timed region
also includes post-RX `PacketFreeBulk()`; the earlier split experiments with
free outside timing isolated TX/RX itself as effectively flat. The residual is
a measured fresh-state initialization, ownership-validation, and lifecycle
tradeoff, not “expected DPDK overhead.” Further recovery
would require weakening ownership validation, pruning DPDK fresh-state
semantics, or adding layout-sensitive/vectorized initialization; those choices
were rejected.

Correctness gates passed: g++ and clang++ `-Werror` builds with the repository's
documented local warning exceptions, `core/all_test` (196/196), the focused
packet ownership tests, Python/pybess discovery (129 tests), the complete
module-test runner, all ten benchmark smoke binaries, and
`./build.py --plugin sample_plugin`. Packet ownership coverage retains fresh
and reused packets, mixed pools, multisegment chains, shared references,
indirect clones, external buffers, zero-count calls, and oversized-count
handling. Candidate C is accepted as the final Stage 2B implementation.

The legacy SIMD reset also zeroed the mbuf hash/RSS union as an incidental
consequence of its packed stores. DPDK's raw-reset contract does not reset that
field; BESS has no fresh-packet consumer requiring a zero hash, so Stage 2B
follows DPDK's validity-state semantics rather than preserving that incidental
initialization.

At the Stage 2B boundary, residual Stage 2 scope was explicit: jumbo/
multisegment policy, AF_XDP/vhost external-buffer pool plumbing, clone
semantics, hardware offloads, `MBUF_FAST_FREE`, `PacketBatch::kMaxBurst`
changes, and real-NIC/cross-worker measurements remained future work.
Stage 2C below records the portions now implemented, including the internal
PMD capability and RX MTU/scatter foundation.

#### Stage 2C.1 — variable packet data-room sizing (first substage)

`PacketPool` now accepts a payload data-room size independently of pool
capacity. The value is retained as a pool property, exposed through
`data_room_size()`, and also used by `CreateDefaultPools()`. The centralized
native element-size helper computes:

```
sizeof(struct rte_mbuf) + sizeof(BessPacketPrivate) +
RTE_PKTMBUF_HEADROOM + payload_data_room
```

`PostPopulate()` passes the same room, including
`RTE_PKTMBUF_HEADROOM`, to DPDK's pool-private initializer. The default remains
`SNBUF_DATA`; it is a compatibility default, not a restriction on explicitly
configured pools. Plain and BESS pool population use the actual system page
shift, so small pools still populate their requested capacity.

The `SNBUF_DATA` audit is intentional:

| Use | Classification |
| --- | --- |
| `core/drivers/pmd.cc` MTU validation | Capacity-dependent; consumes usable RX bytes (`rte_pktmbuf_data_room_size(pool->pool()) - RTE_PKTMBUF_HEADROOM`) plus PMD-reported Ethernet frame overhead |
| `core/modules/source.cc` packet-size validation | Capacity-dependent; remains fixed until the jumbo policy substage |
| `core/modules/random_update.cc`, `core/modules/update.cc`, `sample_plugin/modules/sequential_update.cc` | Fixed application-level packet-field offset contract |
| `core/modules/set_metadata.cc` | Fixed application-level packet-field offset contract |

The offset-contract uses are not allocator sizing. They must not be replaced
with a larger pool room without separately changing the module's packet-field
contract. The PMD check now consumes the actual pool room and PMD capabilities.
The source checks are the remaining capacity consumers and are deliberately
left for the jumbo/multisegment decision.

The variable-room before/after gate was recaptured after the initial unpinned
attempt: both `8ab929fd` and the working tree were built with clang++ against
DPDK 25.11.3 and run with `taskset -c 2`, where CPU 2 is a P-core and its SMT
sibling CPU 3 was offline for the runs. Both packet and PMD suites used
`--benchmark_min_time=1.0s`; raw captures are under
`scratch/phaseb-study/stage2c-pinned-baseline/` and
`scratch/phaseb-study/stage2c-pinned-variable-room/`. This gate did not show a
repeatable regression requiring investigation; CPU scaling remained enabled,
so the numbers are protocol-consistent but not a fixed-frequency claim.

#### Stage 2C.2 — jumbo and multisegment policy

Stage 2C supports both jumbo representations:

- A pool configured with `data_room_size` (or the
  `--packet_data_room` default-pool flag) carries a jumbo frame in one native
  mbuf segment when its payload capacity is sufficient.
- `PacketPool::AllocCopy()` falls back to a native mbuf chain when the source
  exceeds one segment. PCAP receive uses this path, so captured packets do not
  need a fixed `SNBUF_DATA` limit.

PMD initialization derives an internal capability object from
`rte_eth_dev_info`. It enables `RTE_ETH_RX_OFFLOAD_SCATTER` only when the
configured MTU plus the effective RX frame overhead exceeds the usable
single-mbuf RX capacity (`rte_pktmbuf_data_room_size(pool->pool()) -
RTE_PKTMBUF_HEADROOM`) and the PMD advertises scatter; an unsupported
geometry, a device minimum/maximum MTU violation, or a PMD without scatter
support is rejected explicitly.
Native `PacketRef` operations remain segment-aware:
prepend operates on the first segment, `tailroom()` reports room in the
segment referenced by the `PacketRef`, and `append()`/`trim()` follow DPDK's
chain-tail behavior when called on a chain head. No overlay-specific jumbo
path was added.

PCAP TX gathers multisegment packets into dynamically sized scratch storage;
`PCAP_SNAPLEN` remains a capture snapshot-size constant, not a TX boundary.
Transmission is limited only by the `int` length accepted by
`PcapHandle::SendPacket()`, and failed or unrepresentable packets are returned
as drops rather than counted as successful sends.

The packet suite covers large single-segment operations, PCAP-sized chains,
partial-chain allocation cleanup, and deep copies of chained bytes. The
`net_ring` PMD benchmark covers chained RX/TX and a large external-backed
packet round trip.

#### Stage 2C.3 — external-buffer plumbing

`PacketPool::AllocExternal()` accepts a caller-managed buffer, IOVA, length,
and `rte_mbuf_ext_shared_info`, validates the DPDK external-buffer contract,
and attaches it with `rte_pktmbuf_attach_extbuf()`. The caller retains
ownership when validation or mbuf allocation fails; after attachment, DPDK's
external-buffer reference count and free callback own the lifetime.

Generic `PacketFree()` and `PacketFreeBulk()` continue to use native DPDK
mbuf release, so the same free paths handle ordinary, indirect, and external
mbufs without a BESS-specific ownership branch. The packet suite exercises
the production allocator and callback behavior, and the `net_ring` PMD
benchmark exercises an external-buffer receive/transmit round trip. AF_XDP
and vhost-specific adapters still require their device integrations; this
substage supplies the common native mbuf plumbing they can consume.

#### Stage 2C.4 — clone semantics

`PacketClone()` is explicitly shallow: it creates independent mbuf headers
that share every payload segment, including external-buffer storage and its
DPDK reference count. BESS private metadata is not copied. Destruction in
either order releases shared storage only after the last owner is freed.

`PacketCopy()` is explicitly deep: it copies all bytes across linear or
chained source segments into independent native mbufs and does not copy BESS
private metadata. Tests cover destruction ordering, chained clones, and
external-buffer callbacks waiting for the final clone.

The combined Stage 2C.2–2C.4 gate was recaptured with clang++ and DPDK
25.11.3 on `taskset -c 2`; CPU 3 was offline during each one-second run and
restored afterward. Packet and PMD benchmark captures are under
`scratch/phaseb-study/stage2c-pinned-jumbo-external-clone/`, with a repeat PMD
capture under
`scratch/phaseb-study/stage2c-pinned-jumbo-external-clone-repeat/`. The packet
ownership suite passed all 21 tests, and the PMD suite passed the existing
null/ring cases plus the multisegment and external-buffer round trips.

Against the pinned variable-room capture, packet benchmark CPU times improved
by 2.6%–18.2%. One PMD `BM_PmdNullTxEndToEnd/16` run exceeded the requested
2% investigation threshold (+2.1%, then +16.0%); a focused pinned rerun
measured 64.0 ns versus the 65.6 ns pinned reference, so the signal was not
repeatable. No regression investigation was triggered. CPU scaling remained
enabled, so these measurements are not a fixed-frequency claim.

#### DPDK-proposal review notes (2026-09-18)

An Opus review of an external DPDK-modernization proposal (see this doc's
"Roadmap / Backlog" intro and the new Phase J below for the full context)
found three things specifically relevant to Stage 2's scope, all verified
against the tree at `705782b3`:

- **The `paddr`/`vaddr` layer was dead code.** The review confirmed that
  `PacketPool::from_paddr()` and `Packet::paddr()`/`vaddr()` had VPort as
  their only consumers. Stage 2B deleted those writers and accessors with
  the overlay, along with the unused `sid`/`index` fields; no strong address
  type was needed.
- **The bulk-reset and PMD casts were genuine blockers.** Stage 2B now uses
  `rte_pktmbuf_alloc_bulk()`/native field stores and passes `PacketHandle`
  arrays directly to PMD RX/TX. The `_mm_store_si128` overlay fast path and
  `reinterpret_cast` conversion boundary are gone.
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

#### Benchmark suite (added 2026-09-12, commit 14)

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
the existing suite didn't: `PacketHandle`/`PacketRef`/`PacketPool`/
`PacketBatch` themselves (`BM_PacketAllocFree`, `BM_PacketAllocFreeBulk`,
`BM_PacketHeadData`, `BM_PacketAppendTrim`, `BM_BatchForward`), using
`PlainPacketPool` (the only pool backend that doesn't need real hugepages,
which this sandbox lacks — see packet_pool.h's own doc comment: "For
standalone benchmarks and unittests"). These are the primitives Phase B
refactored, so they remain the direct regression check.

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


### Phase C — Linux I/O modernization

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
      - **AF_XDP is the one real gap, and the baseline is a DPDK build gap
        rather than a BESS port gap.** At the start of this slice, `develop`
        was at `9016d973ec223430b148987ae5b1166a9d9d1452` with DPDK `25.11.3`
        installed. `PMDPortArg.vdev` already reached `rte_dev_probe()` in
        `core/drivers/pmd.cc`, followed by `RTE_ETH_FOREACH_MATCHING_DEV` and
        `rte_eth_dev_info_get()`; no new BESS port class was needed. The
        installed DPDK had no `librte_net_af_xdp.so`,
        `librte_net_af_xdp.a`, or AF_XDP PMD plugin. DPDK's Meson log recorded
        `libxdp` missing from pkg-config, `libbpf` found at 1.7.0,
        `linux/if_xdp.h` present, and `bpf/xsk.h` absent; the driver summary
        was `missing dependency, "libxdp >=1.2.2" and "libbpf"`. This is the
        exact reason the `net_af_xdp` driver was disabled, not an inference
        from the missing installed library.
        This slice adds `libbpf-dev`/`libxdp-dev` to the supported Ubuntu
        24.04 CI/container definitions and makes AF_XDP policy explicit:
        `AF_XDP=auto` is the local default and only reports a disabled
        capability when prerequisites are absent; `AF_XDP=required` is set by
        official CI and the Noble build container and fails if prerequisites
        or the DPDK PMD artifact are missing.
        With Arch `libxdp` 1.6.3 and `libbpf` 1.7.0 installed, the rebuilt
        DPDK produced `install/lib/librte_net_af_xdp.so`,
        `install/lib/librte_net_af_xdp.a`, and the
        `install/lib/dpdk/pmds-26.0/librte_net_af_xdp.so` plugin. Its
        `libdpdk.pc --static --libs` output contains
        `-l:librte_net_af_xdp.a -lxdp -lbpf`, while `ldd` on the shared PMD
        reports `libxdp.so.1` and `libbpf.so.1`.
        The follow-up permission experiment separated the kernel boundary
        from the interface type. This user has `CapEff=0` and
        `kernel.unprivileged_bpf_disabled=2`; the same temporary veth pair
        (`veth-afx0`/`veth-afx1`) failed for an unprivileged BESS daemon at
        `xdp_umem_configure()` with `Operation not permitted`, while a root
        daemon created the same `net_af_xdp` vdev on `veth-afx0` with
        `force_copy=1,mode=skb` and PMDPort creation succeeded. The temporary
        veth pair and both daemons were removed after the experiment. AF_XDP
        therefore needs a privileged daemon or an explicitly granted
        capability/device policy; veth is not a permission-free workaround.
        The existing unprivileged PMDPort path was also exercised through
        pybess with `vdev='net_af_xdp,iface=wlo1'`; the daemon logged the
        same `xdp_umem_configure()` `Operation not permitted` failure,
        surfaced as `rte_eth_rx_queue_setup() failed`. A privileged
        loopback probe attached generic XDP but did not complete port
        creation before the smoke timeout; the test daemon was stopped and
        interfaces were confirmed free of residual XDP programs. This host
        therefore proves PMD discovery, dependency linkage, the permission
        boundary, and the BESS error path, but not packet I/O.
        The static dependency question is distro packaging, not an AF_XDP
        source limitation. Arch ships shared `libxdp.so*` and `libbpf.so*`
        but no `/usr/lib/libxdp.a` or `/usr/lib/libbpf.a`; it also lacks
        several unrelated static system archives needed by a complete BESS
        static link. A disposable Ubuntu 24.04 container with
        `libxdp-dev` 1.4.2-1ubuntu4 and `libbpf-dev` 1:1.3.0-2build2
        shipped `/usr/lib/x86_64-linux-gnu/libxdp.a` and
        `libbpf.a`. Its `pkg-config --static --libs libxdp libbpf` output
        included the transitive `libelf`, `zstd`, `zlib`, and pthread
        flags, and a tiny `gcc -static` program referencing both libraries
        linked successfully (`ldd` reported `not a dynamic executable`).
        Thus the AF_XDP PMD dependencies can be static like the rest of
        DPDK when the distro's development packages provide archives; the
        current Arch host still cannot produce a fully static BESS binary
        without separately sourcing all missing system archives.
        Ownership is compatible with BESS's native packet contract: DPDK's
        zero-copy path overlays UMEM on the RX queue's configured mempool and
        returns ordinary direct `rte_mbuf` objects; copy mode allocates
        ordinary mbufs from that same pool and copies into them. The driver
        has no external-mbuf attachment path. Its TX path accepts same-pool
        direct mbufs in zero-copy mode, copies/frees other mbufs, and always
        follows DPDK's mbuf ownership rules. BESS `PacketRef` is non-owning,
        `PacketFree` delegates to `rte_pktmbuf_free`, and the bulk-free
        helper has a native direct-mbuf fast path with a safe fallback.
        No AF_XDP-specific BESS ownership shim is required. Permanent
        regression coverage is the required-mode DPDK build plus shared/static
        PMD artifact check; a privileged interface smoke is intentionally not
        in CI because XDP attach rights and NIC/kernel support are host-specific.
        A follow-up attempt to run traffic through two temporary veth pairs
        (`veth -> AF_XDP -> BESS -> AF_XDP -> veth`) was stopped before any
        packets were sent. Loading the kernel `pktgen` module succeeded at
        15:01:08, but the host rebooted at 15:04:18 while the two AF_XDP
        ports were being attached/configured. The previous-boot kernel
        journal ends with `afx-out-wire: entered promiscuous mode` at
        15:03:09; it contains no panic/oops trace, `/sys/fs/pstore` is empty,
        and `kdump` is inactive. This is temporal correlation only, not proof
        of root cause; no packet or performance result was collected. Do not
        repeat live AF_XDP/veth traffic on this host until kernel crash
        capture and an isolated test host are available.
        The userspace follow-up used DPDK `testpmd` instead of kernel
        `pktgen`: a `txonly` testpmd AF_XDP port on `tp-in-tx`, a BESS
        `QueueInc -> QueueOut` path between `net_af_xdp0` on `tp-in-bess`
        and `net_af_xdp1` on `tp-out-bess`, and an `rxonly` testpmd AF_XDP
        port on `tp-out-rx`. The testpmd processes and BESS daemon were
        root-owned userspace processes; the only kernel objects were the two
        temporary veth pairs and AF_XDP sockets. Testpmd's default
        155456-mbuf pool did not fit with no hugepages, so the experiment
        used `--total-num-mbufs=8192`.
        The path forwarded real 64-byte packets. Three consecutive five-second
        BESS samples measured 1.945/1.922/1.920 Mpps in and
        1.945/1.922/1.920 Mpps out, or 0.996/0.984/0.983 Gbps, with zero
        BESS-reported drops. The latest testpmd live samples showed
        2.335 Mpps / 1.195 Gbps transmitted and 1.854 Mpps / 0.949 Gbps
        received; testpmd was generating above the steady BESS forwarding
        rate, so those samples are not a synchronized loss measurement.
        The userspace run completed without another reboot; all processes and
        veths were stopped and removed afterward.
      - **No AF_XDP-specific BESS configuration sugar is planned.** AF_XDP
        uses the existing generic `PMDPort.vdev` devargs path; physical
        devices continue to use `pci=...`, while DPDK virtual devices use
        `vdev='net_af_xdp,...'`. PMD-specific options (`iface`, `force_copy`,
        `mode`, busy-poll, UMEM, and future PMD parameters) remain DPDK
        devargs. BESS adds fields only for portable cross-PMD semantics such
        as queue counts, generic MTU, VLAN behavior, loopback, and capability
        reporting.
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
- [x] **Internal `PmdCapabilities` in `PMDPort`** (completed 2026-09-19,
      DPDK-proposal review). `PMDPort::Init()` now populates this semantic
      object once from `rte_eth_dev_info`; it is not exposed through protobuf.
      The fields are `rx_scatter`, effective `min_mtu`/`max_mtu`,
      `rx_frame_overhead`, `rx_offload_capa`, `tx_offload_capa`, and
      `dev_capa`.
      `PMDPort` validates the configured MTU as an Ethernet frame length
      (`mtu + rx_frame_overhead`) against the default pool's usable
      single-mbuf RX capacity (`rte_pktmbuf_data_room_size(pool->pool()) -
      RTE_PKTMBUF_HEADROOM`): it leaves scatter disabled when the frame fits,
      enables scatter only when needed and advertised, rejects unsupported
      geometry clearly, and rejects device minimum/maximum MTU violations.
      Deterministic coverage lives in `core/drivers/pmd_test.cc`.
      `RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE` remains a hardware/performance gate;
      do not enable it until the direct-packet/refcount/pool preconditions are
      benchmarked on a real NIC after the relevant Phase B work.
      `RTE_ETH_DEV_CAPA_RXQ_SHARE`/MT-lockfree-Tx likewise remains a
      hardware/performance gate; `PortOut` locking is unchanged until a PMD
      and real workload prove the contract.
      No raw capability flags or AF_XDP-specific configuration sugar belongs
      in the protobuf API.
      **Design constraint to record now so it isn't re-litigated later**:
      do **not** globally enable checksum/TSO offload. BESS's
      `IPChecksum`/`L4Checksum` modules conflate two semantics -- *verify
      an incoming packet and route failures* (cannot be replaced by a Tx
      offload) and *produce a correct outgoing checksum* (can be). If Tx
      checksum offload is ever adopted it must be explicit graph
      semantics (a distinct module or mode that zeroes the field and
      populates mbuf offload metadata), so a later module that rewrites the
      header after the offload metadata was prepared is a visible
      graph error rather than silent corruption. Same for TSO/GSO.
- [ ] **Real-NIC AF_XDP zero-copy validation and primary-path policy remain
      hardware-gated.** The PMD build/runtime path and generic `pci=`/`vdev=`
      wiring are complete; the BESS daemon does not add AF_XDP-specific
      protobuf fields. Validate zero-copy and any physical-NIC/container
      default policy only on a suitable NIC/kernel/privilege setup.
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


### Phase E — Meson cutover — COMPLETE

See the Phase E summary and CI evidence at the top of this document.

### Phase J — Live table updates without stopping the world (done 2026-09-19: entries 35-38)

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


## 9. Phase G0 — C++ transactional control-plane core — COMPLETE

**Status: done.** Landed as seven reviewed commits (entries 41-49):

| commit | what |
|---|---|
| 1 | `ControlPlane` extracted from the gRPC service; handlers are adapters; one error model; service lock gone |
| 2a/2b/2c | `RuntimeState` owns ports, modules, traffic classes and workers; no destructor mutates a registry |
| 3 | `PipelineSpec` (desired state), deterministic `PipelineSnapshot`, side-effect-free `ValidatePipeline()` |
| 4 | deterministic `Diff()` and the dependency-ordered planner (typed operations, three phases) |
| 5 | transaction engine (prepare/commit/retire/abort), generation, optimistic concurrency, reversibility gate |
| 6 | a complete pipeline applied from C++ end to end, including "a failed commit keeps the active pipeline" |
| 7 | failure injection over every operation of a real plan, leak/queue/orphan assertions, phase timings |

**Closure follow-up (`5fc0ee2a`, entry 50; `20d9fe2a`, entry 52)** — four
correctness gaps found in review of the finished code, all about semantics
rather than structure (the fourth: a refused reparent destroyed the class it
was moving — see the last bullet):

- **generation coherence**: legacy structural mutations advance the same
  generation as `ApplyPipeline()`, so a stale writer is detected no matter which
  path changed the runtime;
- **traffic-class fidelity**: the snapshot carries the full spec semantics
  (resource, limit, burst, priority, share) and the diff classifies parameter
  changes as `kUpdateParams` (applied in place, undoable) instead of calling
  them unchanged;
- **retire contract**: retirement preconditions are proven before the commit,
  and a retire failure is reported as a failure — an ordinary successful apply
  never claims a pipeline that does not exist;
- **reparent ownership**: `AttachExistingTcLocked()` never destroys the class it
  attaches, so a refused move reattaches it where it was; attachment
  constraints are validated up front, and `Diff()` no longer mistakes a
  scheduler's internal `!default_rr_*` wrapper for a desired parent.

What G0 deliberately did **not** do, and where it lands instead:

- **metadata layout validation** stays in `Prepare()`: a module's attributes only
  exist once the module instance exists (`Module::AddMetadataAttr` runs in its
  `Init`), so it needs the stageable metadata layout described above;
- **transactional replacement** (port reconfiguration, module rebuild,
  traffic-class policy change) is refused with `kUnsupportedTransaction` rather
  than attempted -- correct refusal beats false atomicity;
- **RCU/QSBR** is K1's job: `RuntimeState` is unique and quiesced during
  mutation, and `IPLookup`/`ExactMatch` keep their existing immutable-generation
  mechanism untouched;
- **the public desired-state API** is G1's job: `ApplyPipeline()` is C++-internal,
  and the RPC surface still exposes the legacy imperative calls (now thin
  adapters over one-operation transactions).

This was the next major architecture surgery, and the first item in the active
order of work.

The goal is **not** "make every client C++".

The goal is:

> BESS-specific validation, planning, resource ordering, pause/RCU decisions,
> rollback, generation management, and commit semantics live in C++ inside
> `bessd`. Go/C++/other SDKs are thin typed bindings.

Current BESS exposes many imperative RPCs:

```text
CreatePort
CreateModule
ConnectModules
AddWorker
AddTc
...
```

and explicit pause/resume operations, so the client is an orchestration engine
and can reach partial states like:

```text
CreatePort A       ✓
CreatePort B       ✓
CreateModule X     ✓
CreateModule Y     ✗
---------------------
client must somehow repair partial state
```

The invariant G0 establishes:

> The daemon owns resource dependency ordering and transaction semantics. A
> client describes the requested state or transaction; it never performs BESS
> rollback itself.

Target shape:

```text
gRPC / future Go SDK / future C++ SDK
                 │
                 ▼
          protocol adapters
                 │
                 ▼
        C++ ControlPlane
                 │
      ┌──────────┼───────────┐
      │          │           │
   Validate     Diff        Plan
                             │
                             ▼
                    Transaction engine
                             │
                   Prepare → Commit
                             │
                         Rollback
                             │
                             ▼
                       RuntimeState
                             │
                             ▼
                         dataplane
```

Baseline for the work: `e8c8e17684115e71ff9727134b5eb6e346db175b`.

This is an architectural rewrite. Do not preserve the current RPC-handler-centric
control architecture merely for compatibility: at the end of G0, `core/bessctl.cc`
primarily translates protobuf requests/responses and no longer owns BESS
resource lifecycle semantics.

### 9.1 Extract a real ControlPlane subsystem

`core/bessctl.cc` should stop being both:

- gRPC adapter; and
- BESS control business logic.

Suggested tree (logical boundaries matter more than exact filenames; do not
over-fragment tiny files to match it):

```text
core/control/
    control_plane.{h,cc}
    runtime_state.{h,cc}
    pipeline_spec.{h,cc}
    pipeline_snapshot.{h,cc}
    pipeline_validator.{h,cc}
    pipeline_diff.{h,cc}
    pipeline_plan.{h,cc}
    transaction.{h,cc}
    control_error.h
```

Use C++23 where it improves the control plane — in particular
`std::expected`. The internal C++ layer must not require callers to inspect
protobuf `Error` messages:

```cpp
struct ControlError {
  int code;
  std::string message;
  std::string object;
  std::string field;
};

template <typename T>
using ControlResult = std::expected<T, ControlError>;
```

One internal error model, with codes such as:

```cpp
enum class ControlErrorCode {
  InvalidArgument,
  NotFound,
  AlreadyExists,
  Conflict,
  ResourceBusy,
  UnsupportedTransaction,
  ResourceFailure,
  Internal,
};
```

It may preserve an `errno` where useful; the RPC adapters translate it to the
existing protobuf error fields for now. Do not propagate random combinations of
`errno`, protobuf errors, `CommandResponse`, `LOG(ERROR)` and `nullptr` through
the new control layer.

Consequences for the RPC layer:

- handlers become thin adapters: protobuf request → C++ spec/request →
  `ControlPlane` call → protobuf response;
- handlers never call other handlers — composition moves into `ControlPlane`
  (this is what lets the service drop its `std::recursive_mutex` for a plain
  single-writer lock owned by the control plane);
- `ResetAll()` becomes one C++ operation (`ControlPlane::Reset()`, or
  `ApplyPipeline(empty_pipeline)` if the semantics match), not an internal
  fan-out over other RPCs;
- legacy imperative RPCs stay, but each becomes a **one-operation transaction**
  (validate → apply → generation bump), so old clients inherit the new internal
  correctness;
- `ApplyPipeline()` and `GetPipeline()` exist in C++ first and do not need to be
  public gRPC yet — tests call them directly.

```cpp
class ControlPlane {
 public:
  ControlResult<ValidatedPipeline> Validate(const PipelineSpec &desired) const;
  PipelineDiff Diff(const PipelineSnapshot &current, const ValidatedPipeline &desired);
  PipelinePlan Plan(...);

  ControlResult<ApplyResult> ApplyPipeline(const PipelineSpec &desired,
                                           const ApplyOptions &options);
  PipelineSnapshot GetPipeline() const;
};
```

For G0, one control-plane writer at a time is entirely acceptable; do not try to
make multiple concurrent writers lock-free.

Transaction logging carries a transaction ID for diagnostics (never per packet):

```text
tx=128 generation=42 desired_objects=17
tx=128 validated / prepared / pause_workers / commit
tx=128 generation=43
tx=128 retire complete
tx=129 prepare failed object=port:p0 ...
tx=129 aborted active_generation=43
```

Add introspection sufficient to inspect current generation, last successful
transaction, last failed transaction, and current resource counts. This can
start as C++/log/test-visible; the external telemetry API is not G0.

### 9.2 RuntimeState: explicit ownership of mutable instance state

Two fundamentally different kinds of global state must be separated.

**Keep process-global type registries** — these are registration metadata
describing available types, not per-pipeline mutable runtime state:

```text
ModuleBuilder::all_module_builders()
PortBuilder::all_port_builders()
GateHookBuilder::all_gate_hook_builders()
ResumeHook registrations
```

**Remove mutable instance registries from builder/global ownership.** Today
runtime state is spread across statics/globals:

```text
ModuleGraph::all_modules_
ModuleGraph::tasks_
PortBuilder::all_ports_
TrafficClassBuilder::all_tcs_
workers[]
worker_threads[]
num_workers
orphan_tcs
```

That shape makes staging, snapshotting, testing and transactions unnecessarily
difficult. Introduce an explicit runtime-state owner:

```text
RuntimeState
  ├── ModuleRegistry          (module instances, graph connectivity, task membership)
  ├── PortRegistry            (port instances)
  ├── TrafficClassRegistry    (TC instances / hierarchy)
  ├── WorkerManager           (workers, worker threads)
  └── generation
```

The important property is: **mutable instance state has an owner**. Do not create
a new collection of unrelated singleton managers that merely rename today's
globals.

Ownership rules:

- use RAII and `std::unique_ptr<T>` for control-plane ownership;
- do not introduce `shared_ptr` into the packet path;
- raw pointers remain reasonable as non-owning dataplane references whose
  lifetime is guaranteed by the active runtime generation;
- the owner removes an object from its registry; **a destructor must not mutate
  a global registry** (`TrafficClass` destruction currently mutates
  `TrafficClassBuilder::all_tcs_` via destructor callbacks — that pattern is
  hostile to transactions and has already contributed to concurrency problems,
  and it must go with the introduction of `TrafficClassRegistry`);
- `WorkerManager` owns worker lifecycle behind one explicit manager
  (`Add`/`Remove`/`Get`/`PauseAll`/`ResumeAll`/`AnyRunning`), internally free to
  keep fixed-size `std::array` storage; the public global `workers[]` must not
  remain the architectural API. Packet/runtime code that needs its current
  worker keeps using the existing worker/TLS mechanisms — this is about
  management ownership, not new packet-path lookups;
- modules resolve mutable runtime objects during **initialization** through a
  narrow context (`ModuleInitContext` with `ports()`, `traffic_classes()`, …, or
  `Module::runtime()`), not through `PortBuilder::all_ports()`-style globals.
  Never pass `RuntimeState` through packet-processing calls — this is
  control/initialization context, not packet-path context;
- `ModuleGraph` splits its responsibilities (runtime module registry, topology,
  task membership, gate-ID computation, task-graph propagation) as needed, but
  the requirement is only that **one active `RuntimeState` clearly owns module
  lifetime and graph state**;
- graph recomputation (`UpdateTaskGraph`, `SetUniqueGateIdx`, `ConfigureTasks`,
  `changes_made_`) operates on the candidate/active runtime object explicitly
  (`runtime.graph().Recompute()`), never through process-global dirty flags, and
  any recomputation failure happens **before** publication;
- `Module::RegisterTask()` must stop being a hidden global side effect: module
  task creation, TC creation, orphan registration and worker attachment get
  explicit ownership and rollback behavior (ideal direction: module owns the
  `Task`, the runtime/TC registry owns the scheduling node, the transaction
  attaches scheduling state);
- `Port::AcquireQueues()` / `Port::ReleaseQueues()` get explicit rollback
  correctness: a failed module init or transaction rollback must never leave
  `users[dir][qid]` pointing at a dead module.

### 9.3 PipelineSpec / desired-state IR

Introduce an internal desired-state representation for structural BESS
configuration. Protobuf is not the in-process domain model.

```cpp
struct PortSpec {
  std::string name;
  std::string driver;
  uint16_t num_rx_queues;
  uint16_t num_tx_queues;
  size_t rx_queue_size;
  size_t tx_queue_size;
  google::protobuf::Any driver_arg;
  Port::Conf conf;
};

struct ModuleSpec {
  std::string name;
  std::string mclass;
  google::protobuf::Any arg;
};

struct ConnectionSpec {
  std::string upstream;
  gate_idx_t ogate;
  std::string downstream;
  gate_idx_t igate;
  bool skip_default_hooks;
};

struct WorkerSpec {
  int wid;
  int core;
  std::string scheduler;
};

struct PipelineSpec {
  std::vector<PortSpec> ports;
  std::vector<ModuleSpec> modules;
  std::vector<ConnectionSpec> connections;
  std::vector<WorkerSpec> workers;
  std::vector<TrafficClassSpec> traffic_classes;
};
```

The TC representation must capture hierarchy cleanly — prefer explicit parent
references or a tree over duplicating today's awkward mutation RPC model.

Naming rule: `PipelineSpec` requires **explicit stable names**. Legacy
`CreateModule(name="")` may still generate a name; desired state may not, because
diff, reapply, idempotency, diagnostics and generation comparisons all depend on
desired-state identity.

G0 does not invent APIs for K-subsystem resources that do not exist yet.

### 9.4 PipelineSnapshot

The active runtime must serialize into a deterministic structural snapshot
describing ports, modules, connections, workers, traffic classes and generation:

```cpp
PipelineSnapshot ControlPlane::Snapshot() const;
```

Rules:

- no transient statistics;
- no worker paused/running transitions as desired-state config unless
  deliberately justified;
- stable ordering; no pointer addresses; no unordered-map iteration order
  leaking into output; normalized defaults.

It is the basis for `GetPipeline`, diffing, tests and future SDK introspection.
A successful apply must normalize back to the requested desired state:

```text
Apply(A)
GetPipeline()
normalize(result) == normalize(A)
```

### 9.5 Validation must be side-effect free

```cpp
ControlResult<ValidatedPipeline> Validate(const PipelineSpec &desired) const;
```

Validation must not create a PMD, allocate a live module, attach a worker,
connect gates, acquire port queues, mutate metadata allocation, or modify
scheduler trees. At minimum it checks:

- **Names** — non-empty where required; unique ports, modules and traffic
  classes; valid worker IDs.
- **Types** — port driver exists; module class exists; scheduler name supported.
- **References** — module connection endpoints exist; TC parent exists; leaf
  module exists; worker reference exists; explicit port/module relationships
  resolve.
- **Gates** — ogate exists for the upstream class; igate exists for the
  downstream class; no conflicting output-gate connection; indices fit BESS
  limits.
- **Workers** — `wid < Worker::kMaxWorkers`; CPU exists; duplicate cores handled
  per BESS policy; scheduler value valid.
- **Traffic classes** — policy, required policy arguments, parent compatibility,
  hierarchy cycles, leaf task references, worker/root constraints.
- **Port shape** — generic queue counts/sizes validated before driver creation.

Driver-specific checks that genuinely require invoking a driver happen in
`Prepare()`; do not pretend those are pure validation.

**Metadata layout is part of validation.** Module metadata constraints are a
pipeline-wide dependency, and a transaction must not discover metadata
incompatibility after half the graph is committed: build a candidate metadata
layout from the desired graph, validate it, and never mutate
`bess::metadata::default_pipeline` during validation. Candidate metadata state
must be stageable for side-effect-free validation to be possible at all.

> **Status (G0 commit 3):** the structural validator landed and is pure, but
> metadata layout validation is *not* in it yet, and that is a real boundary
> rather than an omission: a module's attributes only exist once a module
> instance exists, because `Module::AddMetadataAttr()` runs inside the module's
> own `Init()`. Deciding metadata compatibility without instantiating anything
> therefore requires the candidate/staged metadata layout that section 9.2
> calls for — so metadata validation lands with `Prepare()` (commit 5), where
> candidate modules exist, and the same rule as driver-specific checks applies:
> never claim a check is pure when it is not.

### 9.6 Diff and planner

```cpp
PipelineDiff Diff(const PipelineSnapshot &current,
                  const ValidatedPipeline &desired);
```

Classify changes deterministically: ports (create/remove/replace/update/unchanged),
modules (create/remove/replace/unchanged), connections
(connect/disconnect/unchanged), workers (add/remove/replace-move/unchanged),
traffic classes (create/remove/update/reparent/unchanged). No pointer equality;
normalize config before diffing so default-equivalent states do not churn. A
re-apply of identical desired state produces an empty diff, does not stop
workers and does not increment the generation.

The planner turns a validated diff into an explicit dependency-ordered plan — not
`for (...) CreateWhatever()`:

```text
PrepareCreatePort(p0)
PrepareCreateModule(src)
Disconnect old connections
Connect src → out
Attach tasks / traffic classes
Retire removed modules
Retire removed ports
```

Ordering constraints to encode intentionally:

```text
port before PortInc/PortOut Init
module before connection
connection before graph propagation
module task before leaf TC attachment
worker before attaching scheduler root
module releases port queues before port destruction
TC/task references detached before module destruction
```

The plan is a real, inspectable data structure:

```cpp
using PlanOperation = std::variant<
    CreatePortOp, RemovePortOp, UpdatePortOp,
    CreateModuleOp, RemoveModuleOp,
    ConnectOp, DisconnectOp,
    AddWorkerOp, RemoveWorkerOp,
    CreateTcOp, RemoveTcOp, ReparentTcOp>;

struct PipelinePlan {
  uint64_t based_on_generation;
  std::vector<PlanOperation> prepare_ops;
  std::vector<PlanOperation> commit_ops;
  std::vector<PlanOperation> retire_ops;
};
```

Typed operations (or virtual operation objects) are required over
`std::vector<std::function<void()>>`: they can be inspected, tested, logged,
serialized later, and reasoned about during rollback.

### 9.7 Apply semantics and the transaction engine

```text
PipelineSpec
    │
    ▼
Normalize → Validate → Diff(current, desired) → Plan ordered operations
    │
    ▼
Prepare   (allocate/create resources, reversible setup)
    │
    ▼
Commit    (minimal quiesced window) → publish new generation/state
    │
    ▼
Retire old state
```

State machine:

```text
Created → Validated → Prepared → Committing → Committed → Retired
   failure at any stage  → Abort
   failure while committing → Rollback
```

```cpp
class Transaction {
 public:
  ControlResult<void> Prepare();
  ControlResult<uint64_t> Commit();
  void Abort() noexcept;
};
```

**Quiescence is the engine's decision, not the client's.** Structural graph
mutation currently requires pausing workers; that stays, but the contract is not
"client calls `PauseAll`". Prepare as much as possible while workers run, then:

```text
pause required workers / all workers
apply structural transition
recompute graph/scheduler metadata
publish
resume
```

Model the requirement explicitly (`enum class Quiescence { None, Workers }`) and
do not pause blindly for every future transaction — K1/K2/K3 will publish many
dataplane-state transactions without structural pause.

**Phase J must not regress:** a module/gate-hook command marked
`CommandInfo.thread_safe` may still run without a global pause. Structural
pipeline transactions are a different thing from thread-safe module table
updates.

**Preparation versus commit.** Expensive reversible setup (parse arguments,
allocate ordinary memory, build candidate metadata plan, construct pure graph
description, instantiate stageable resources) happens before the pause; the
commit window installs resources, connects the graph, switches scheduler
relationships, publishes the generation and resumes. Record
`validation_us` / `prepare_us` / `paused_commit_us` / `retire_us` at least in
debug/test instrumentation — this will matter later.

**Retirement is a contract, not a best effort.** A successful apply must leave
exactly the desired state, so retire preconditions are proven *before* the
commit — a plan that would remove a port still in use by a surviving module, or
a worker still running a surviving module's tasks, is refused with
`kUnsupportedTransaction` and the offending object named. If a retire step still
fails, the commit has already happened: the generation is bumped (the state did
change) and the caller gets `kResourceFailure` saying the pipeline was committed
but retirement failed. Returning ordinary success there would claim a pipeline
that does not exist.

**Atomicity is not a lie to be told.** For accepted transactions, the target is:
if the transaction returns failure, the previous logical runtime remains active.
External resources can defeat this, so the planner must classify reversibility:

```cpp
enum class Reversibility {
  Reversible,
  RequiresDestructiveCommit,
  UnsupportedTransactionally,
};
```

If an operation cannot currently be made safely reversible (e.g. replacing a
physical device that cannot be opened twice), **reject the transactional plan**
rather than attempting it and hoping rollback works. Correct refusal is better
than false atomicity; §9.9 records which operations stay non-transactional.

**K1 seam, not K1.** G0 leaves a clean seam for RCU but implements no half-baked
RCU: structural transactions may pause workers, `RuntimeState` stays unique and
quiesced during mutation, and no attempt is made to RCU-swap the whole
`ModuleGraph`, `WorkerManager` or scheduler tree. `IPLookup` and `ExactMatch`
keep their existing immutable `PublishedGeneration` mechanism unchanged.

### 9.8 Generations, optimistic concurrency, and the transaction contract

```cpp
using Generation = uint64_t;
```

Generation starts at a deterministic initial value and increments exactly once
per successful state-changing operation. It does not increment for reads,
validation, planning, failed transactions, worker pause/resume or a no-op
apply.

**Both control paths share the sequence.** While the legacy RPC surface is still
live, every public structural mutation (ports, modules, connections, workers,
traffic classes, and `Reset()` as one operation) bumps once on success, and the
`*Locked()` primitives never bump — so `ApplyPipeline()` still bumps exactly
once for a whole multi-operation transaction. Without this, a legacy
`CreateModule` would change the runtime while the generation stood still, and a
stale `ApplyPipeline(expected_generation=...)` would be accepted.

```text
generation 12 --Apply success--> 13
generation 12 --Apply failure--> 12
```

Requests carry an optional expected generation, internally now and publicly
later:

```cpp
struct ApplyOptions {
  std::optional<uint64_t> expected_generation;
};
```

`current = 42, expected = 41` returns a **conflict before any side effect** — a
distinct error, not hidden as generic `EINVAL`.

Transaction contract, publicly visible eventually:

```text
SUCCESS
    requested generation/state is active

VALIDATION_FAILED
    active state unchanged

PREPARE_FAILED
    active state unchanged
    staged resources released

COMMIT_FAILED
    previous active state retained/restored where contractually possible
```

Irreversible external-resource operations are modeled explicitly and kept few
and late in the plan.

### 9.9 Tests, failure injection and acceptance gates

Add substantial control-plane tests (`core/control/control_plane_test.cc` or
several focused tests) covering:

- empty → pipeline (worker, ports, modules, connections, TCs) and the final
  snapshot;
- pipeline → empty (all resources removed cleanly);
- pipeline A → pipeline B (module add/remove, connection change, worker change,
  TC change);
- no-op apply `A → A`: no mutation, no pause, no generation change;
- validation failure and prepare failure: zero runtime change;
- commit failure: rollback to the exact previous snapshot;
- stale expected generation: fail before mutation;
- concurrent callers: writer serialization, generation ordering, no mixed state.

Failure injection is mandatory — correctness cannot be established from
successful applies. Provide test-only hooks or fake resources (no
environment-variable behavior in production) and inject failure after port
creation, module creation, second module creation, connection, worker creation,
TC creation, task attachment, and at a commit midpoint. After each injection
assert: generation unchanged, snapshot equals the original, and **no** leaked
module, port, TC, worker thread, acquired queue or orphan task.

Test-only resource types (`TestPort`, `TestModule`) with configurable `Init`
success/failure and recorded `DeInit` allow transaction tests without DPDK PMDs;
real null/ring PMDs appear in integration smoke tests only after unit semantics
are proven.

Add at least one process/integration test proving the active pipeline survives a
failed transaction: `Source → Measure/Sink` running on a worker, apply a new or
invalid pipeline while traffic runs, and require the old graph to remain usable
(bounded traffic pause is acceptable; a partially replaced or disconnected graph
is not).

Run the new control tests under ASan/UBSan and specifically look for leaked
modules/tasks, double destruction, stale port-queue users, TC double deletes,
detached worker threads and use-after-free after rollback.

The work is not complete until every one of these holds:

```text
[ ] invalid PipelineSpec produces no side effects
[ ] prepare failure produces no active-state change
[ ] failure at every tested commit operation rolls back
[ ] generation changes once per successful transaction
[ ] generation does not change on failure
[ ] generation does not change on no-op Apply
[ ] stale expected generation returns conflict
[ ] concurrent writers cannot interleave state
[ ] old pipeline remains valid after failed replacement
[ ] successful Apply snapshot equals desired normalized state
[ ] Apply(desired) twice is idempotent
[ ] ResetAll is implemented through ControlPlane, not RPC-to-RPC calls
[ ] RPC service no longer needs recursive locking
[ ] mutable module/port/TC ownership is no longer hidden in builder globals
[ ] failed module creation releases tasks/TCs/port queues
[ ] failed port creation does not enter active registry
[ ] worker creation/destruction rollback leaves no joinable/leaked thread
[ ] structural commit pauses workers internally when required
[ ] packet hot path contains no new transaction overhead
```

### 9.10 Performance and pause-time discipline

A committed transaction must cost nothing per packet: no transaction locks,
protobuf, planner state, `std::expected`, registry maps, `shared_ptr` or
generation bookkeeping inside `ProcessBatch()`. The packet path keeps
dereferencing ordinary stable runtime pointers. Run at minimum `packet_bench`,
`pmd_bench`, `ring_bench` smoke and `traffic_class_bench` smoke; a repeatable
>2% hot-path regression requires investigation.

Structural transactions may pause workers, so make pause duration observable now
(no premature optimization): a harness measuring no-op apply, a small graph
update and a larger synthetic graph update, keeping validation/planning/prepare
before the pause and retirement after resume where safe.

### 9.11 Source organization and commit structure

Update Meson cleanly: add the new control sources explicitly (no globbing, no
compatibility make files, no `build.py` revival), and make all new tests
first-class Meson tests.

The work lands as several reviewed commits under one roadmap item:

```text
1. Extract ControlPlane         — move mutation/read business logic out of
                                  BESSControlImpl; RPCs delegate; no behavior change
2. Explicit RuntimeState        — runtime-owned module/port/worker/TC managers;
                                  remove instance registries from static builders;
                                  update module init lookup paths
3. PipelineSpec/Snapshot/Validate — deterministic desired state + pure validator
4. Diff / Planner               — deterministic plans, dependency ordering
5. Transaction/generation/rollback — prepare/commit/abort, conflict semantics
6. Internal ApplyPipeline       — complete multi-object transactions from C++
7. Failure injection / integration / performance / docs
```

Do not wait until the final commit to run the full suite: every intermediate
commit that materially changes ownership runs focused unit tests. Final gate:

```text
Meson GCC build
Meson Clang build
all native C++ tests
Python unittest discovery
module integration
sample plugin load
installed pybess smoke
packet benchmarks smoke
PMD null/ring smoke
traffic-class benchmark smoke
ASan/UBSan control-plane tests
git diff --check
source-tree hygiene
```

CI stays green on both compiler lanes.

### 9.12 G0 scope discipline

G0 establishes the internal transaction architecture. It does **not** include:

```text
glog daemon recursion fix            (separate small fix, §8 / Milestone 1)
K1 RCU/QSBR implementation
ActionId, new classifier, meters, routing abstraction
Go SDK, C++ bessctl, final v2 public protobuf API
Python removal, plugin ABI redesign
real NIC work, AF_XDP changes
MBUF_FAST_FREE, PortOut lock elimination, DPDK version upgrade, ARM/SIMD
```

Do not design the final G1 transactional protobuf API here (minor additions
strictly needed to keep tests/introspection working are fine; no classifier,
action, meter, route or SDK-facing messages — K does not exist yet). Do not write
the Go SDK, and do not add transaction logic to `pybess` or `bessctl` Python:
the existing Python tools must get correctness from routing through the C++
control plane, not from new Python-side rollback.

Design the engine so future Phase K resources can register transactional
operations (a `PreparedResource`-style `Commit()`/`Abort()` seam is a useful
direction, but do not over-generalize before concrete K resources exist).

Ideal end state:

```text
current generation = N

desired state → Validate → Plan → Prepare → Commit

success:  generation = N + 1, complete new state active
failure:  generation = N, previous state remains active
```

That is the foundation required before adding the generic mutable dataplane
substrate and before exposing robust Go/C++ transactional SDKs. This is the point
where a large invasive refactor is justified: `ModuleGraph::all_modules_`,
`PortBuilder::all_ports_`, `TrafficClassBuilder::all_tcs_` and the worker globals
are exactly what make real transactions difficult today, so G0 attacks that
ownership model rather than building a transaction veneer over it.

`MODERNIZATION.md` must record, as the work lands: the G0 architecture,
RuntimeState ownership, transaction guarantees, generation semantics, the
unsupported/non-reversible resource policy, the G0↔K1 boundary and the G0↔G1
boundary — including which operations remain non-transactional and why. Never
claim full atomicity for an operation the resource lifecycle cannot support.


## 10. Phase K — generic high-performance dataplane substrate

This phase exists because modern BESS needs reusable mutable dataplane machinery, especially for applications such as OMEC UPF.

The split is strict:

### BESS owns generic mechanisms

Examples:

- RCU/QSBR publication;
- classifier engines;
- action/object tables;
- meter engines;
- worker-local counters;
- route/next-hop mechanics;
- packet parsing/mutation helpers;
- fragmentation/reassembly;
- semantic hardware-offload abstraction.

### Applications own protocol/domain semantics

For OMEC UPF, BESS must **not** own:

- PFCP;
- PDR semantics;
- FAR semantics;
- QER semantics/hierarchy;
- URR semantics;
- F-SEID;
- TEID interpretation;
- QFI;
- PSC;
- N3/N6/N9 meaning;
- session precedence/policy semantics;
- GTP control messages;
- buffering/NOCP policy.

OMEC should compile these semantics into generic BESS dataplane resources.

---

### K1 — generic RCU/QSBR lifetime and publication

Phase J proved immutable generation swapping in selected modules, but it intentionally did not add general read-side reclamation.

Add a generic BESS RCU domain.

Conceptual surface:

```cpp
class RcuDomain {
 public:
  ReaderToken RegisterReader(...);
  void Quiescent(ReaderToken);
  void Synchronize();
  void Retire(...);
};

template <typename T>
class RcuPtr;

template <typename K, typename V>
class RcuTable;
```

Exact types may differ.

#### Requirements

- Packet path must not perform `shared_ptr` refcount operations.
- Reader-side overhead must be extremely small and batch-friendly.
- Worker registration/unregistration must be explicit.
- Worker quiescent-state reporting must integrate with BESS worker scheduling.
- Writers must have explicit serialization; RCU does not solve writer/writer races.
- Safe reclamation must survive workers pausing, stopping, or being removed.
- Unit tests must cover delayed readers and object retirement.
- Add microbenchmarks for read-side cost and update/reclaim cost.

DPDK QSBR is an obvious candidate implementation, but callers should depend on a BESS semantic interface rather than raw DPDK QSBR APIs everywhere.

---

### K2 — ActionId and immutable object/action tables

Introduce a generic stable dataplane object reference:

```text
ActionId
```

or more generally a strongly typed object ID where useful.

A classifier should be able to return an ID that directly resolves to immutable action state.

Conceptual flow:

```text
packet
  │
  ▼
classifier
  │
  ▼
ActionId
  │
  ▼
ActionTable[ActionId]
  │
  ▼
immutable action state
```

This eliminates architectures where classification repeatedly materializes fields into metadata only for later modules to re-read and re-interpret them.

#### Generic action table requirements

- immutable published objects;
- stable IDs within a generation;
- RCU-safe replacement/deletion;
- batch-friendly lookup;
- no packet-path heap allocation;
- no packet-path atomic reference counting;
- explicit invalid/deleted behavior;
- transaction integration from G0.

For OMEC, `PdrAction`, `FarAction`, etc. remain OMEC-defined types layered on this generic mechanism.

---

### K3 — unified runtime-schema classifier framework

This is one of the highest-value missing generic BESS facilities.

The generic classifier must support a schema known at **module initialization/configuration time**, not require compile-time key size.

The framework should compile the runtime schema into a fixed hot-path extractor/backend configuration.

Conceptually:

```text
runtime configuration
      │
      ▼
ClassifierSchema
      │
      ▼
compile at init/update
      │
      ├── fixed field extraction plan
      ├── fixed key layout/size
      ├── fixed backend
      └── fixed result representation
      │
      ▼
batch classify
      │
      ▼
ActionId
```

#### Critical design rule

Do **not** fix generic classifier key size at C++ compile time.

A UPF-specific or other domain-specific module may know its chosen schema and therefore its key size before activation, but the generic classifier framework must remain runtime configurable.

Internally it may specialize common sizes after initialization:

```text
8 bytes
16 bytes
24/32 bytes
48 bytes
64 bytes
generic fallback
```

That is an implementation optimization, not a public type restriction.

#### Backend strategy

One logical classifier API may compile to different software backends:

```text
exact match
    → current typed CuckooMap or another proven exact backend

few masks / tuple structure
    → tuple-space style hash

many masks / prefixes / arbitrary ranges
    → rte_acl

future hardware
    → rte_flow / MARK(ActionId)
```

Do not replace the existing toy `ACL` module underneath its historical semantics merely to say BESS uses `rte_acl`.

Build the new classifier framework with explicit semantics.

#### Arbitrary port ranges

Fast non-power-of-two port ranges are a first-class requirement.

Do not explode arbitrary ranges into large sets of prefix-like masks unless a measured backend proves that preferable.

`rte_acl` RANGE fields are a natural backend for:

```text
src port range
dst port range
```

combined with prefix/mask/exact fields.

Benchmark representative mixes, not only exact rules.

#### Result model

Classifier result should be an opaque generic ID such as `ActionId`, not only a graph gate.

A gate may be part of an action, but classification and graph topology should not be artificially coupled.

---

### K4 — packet parsing and mutation primitives

Stage 2 established correct packet storage/ownership. The next layer is ergonomic and safe packet manipulation.

Desired generic primitives include concepts like:

```text
PacketCursor
HeaderView<T>
EnsureContiguous(n)
EnsureLinear()
PushHeader<T>()
RemoveHeader()
ChecksumPlan
TxOffloadPlan
```

Goals:

- centralize mbuf-chain boundary handling;
- avoid repeated ad-hoc pointer arithmetic in modules;
- make head/tail/segment semantics explicit;
- make packet mutation failure explicit;
- preserve zero-copy/multisegment operation where possible;
- allow optimized contiguous fast paths;
- keep protocol semantics out of BESS generic code.

OMEC may use these primitives to implement GTP-U operations, but BESS should not interpret PFCP/GTP policy.

---

### K5 — generic software metering

Add a generic meter abstraction with a software backend built on DPDK metering primitives.

Conceptually:

```text
MeterProfile
MeterState
MeterHandle
MeterColor
```

Backend:

```text
SoftwareMeterBackend
    → rte_meter
```

Future hardware backend:

```text
HardwareMeterBackend
    → rte_mtr / rte_flow
```

The hardware backend is C-HW gated.

BESS owns:

- token-bucket/trTCM mechanics;
- state placement/lifetime;
- batch-friendly execution;
- worker-safe ownership model.

OMEC owns:

- what a QER means;
- application/session/slice hierarchy;
- what green/yellow/red means to UPF policy;
- PFCP encoding.

---

### K6 — worker-local statistics and snapshots

Build the data mechanism before building more exporters.

Desired primitives:

```text
WorkerLocal<T>
CounterSet
EpochCounter
Histogram
SnapshotGeneration
```

Rule:

```text
worker writes local state
controller aggregates snapshots
```

Packet path must not take a shared statistics mutex.

Support:

- monotonic counters;
- deltas;
- histograms;
- reset/epoch semantics;
- per-worker aggregation;
- consistent-enough snapshot generation;
- low-cost batch updates.

The future Prometheus exporter in Phase F should consume this mechanism rather than invent another counter ownership model.

For OMEC, subscriber/PDR/URR identities remain application-level semantics.

---

### K7 — route and next-hop abstraction

Introduce a generic routing layer:

```text
RouteTable
NextHopId
NextHop
neighbor state
egress port
L2 rewrite
bulk lookup
route generation
```

Do not couple application logic directly to a specific DPDK table implementation.

Current trusted backend:

```text
rte_lpm
```

`rte_fib` was benchmarked and showed order-dependent correctness failures at large arbitrary-order route sets. It must **not** become the default until that issue is resolved and independently revalidated.

Desired shape:

```text
RouteTable API
   │
   ├── rte_lpm backend       trusted/default
   └── rte_fib backend       disabled/experimental until correctness fixed
```

---

### K8 — generic IPv4 fragmentation/reassembly

Add generic packet mechanisms around DPDK fragmentation/reassembly facilities.

Requirements:

- native `PacketHandle`;
- chained packet correctness;
- worker-local or intentionally sharded state;
- NUMA-safe ownership;
- explicit timeout/resource bounds;
- correct cleanup on partial failures.

These may be libraries rather than permanent graph modules.

Lower priority than K1–K3 and K5/K6.

---


## 14. Phase G1 — desired-state API and thin language SDKs

After G0 and enough K resources exist to design against real semantics, expose the new public API.

The old imperative API should not constrain the ideal end state.

A v2-style service should be coarse-grained and desired-state oriented.

Conceptually:

```protobuf
service Bess {
  rpc GetSystem(...) returns (...);
  rpc GetCapabilities(...) returns (...);

  rpc ValidatePipeline(Pipeline) returns (ValidationResult);
  rpc PlanPipeline(Pipeline) returns (PlanResult);
  rpc ApplyPipeline(ApplyPipelineRequest) returns (ApplyResult);
  rpc GetPipeline(...) returns (Pipeline);
  rpc DiffPipeline(...) returns (PipelineDiff);

  rpc ApplyDataplaneTransaction(DataplaneTransaction)
      returns (DataplaneTransactionResult);

  rpc GetStats(...) returns (...);
  rpc WatchStats(...) returns (stream ...);
  rpc WatchEvents(...) returns (stream ...);

  rpc Shutdown(...) returns (...);
}
```

Exact API should be designed only after G0/K primitives exist.

### 14.1 C++ remains authoritative

Business logic lives in `bessd` C++.

The gRPC layer performs:

```text
FromProto
  ↓
ControlPlane call
  ↓
ToProto
```

No client is responsible for rollback or ordering BESS internals.

### 14.2 SDK philosophy

SDKs are intentionally thin.

Go SDK responsibilities:

- ergonomic typed builders;
- obvious local type validation;
- serialization;
- RPC;
- typed errors;
- generation/conflict handling.

C++ SDK responsibilities are equivalent.

Python may remain temporarily while migration is in progress.

SDK transaction builders must **not** implement transactions as ten sequential RPCs plus client-side rollback.

For example:

```go
tx := client.NewDataplaneTransaction()

action := tx.AddAction(...)
tx.AddMeter(...)
tx.AddClassifierEntry(..., action)

generation, err := tx.Commit(ctx)
```

`Commit()` serializes one transaction request.

### 14.3 OMEC relationship

OMEC can remain Go.

That does not reduce the value of a robust C++ control plane; it increases it.

Responsibility split:

```text
OMEC Go:
    PFCP
    PDR/FAR/QER interpretation
    session lifecycle
    precedence/policy
    compile domain semantics to generic BESS resources

BESS C++:
    graph/resource validation
    dependency ordering
    object lifetime
    generations
    transactions
    rollback/abort
    worker coordination
    pause vs live-publication decisions
    generic resource publication

Go SDK:
    thin typed binding
```

This keeps all language clients consistent and prevents each SDK from becoming a partial reimplementation of BESS.

### 14.4 C++ bessctl

A future C++ CLI should use the same public transactional API.

Keep `bessctl` and `bessd` separate processes.

A Unix-domain-socket gRPC transport remains attractive for local management and permissions.

Do not fold the CLI into the privileged daemon merely because both are C++.

---


## 16. Phase D — ARM64 + portable architecture

Phase D remains important but no longer blocks G0/K.

### D1 — architecture layer

Target:

```text
core/arch/
    generic/
    x86/
    arm64/
```

Keep scalar reference implementations.

Architecture-specific implementations should have explicit interfaces and batch-level runtime dispatch.

Do not scatter ISA checks through per-packet code.

### D2 — runtime SIMD policy

Detect what the CPU supports, but also allow a configured maximum implementation.

Conceptually:

```text
available ISA
    ∩
configured max ISA
    ↓
selected kernel
```

Supporting AVX-512 does not mean every workload should always use it.

Selection should occur outside the inner packet loop.

### D3 — `rte_bpf` experiment

BESS currently carries a large architecture-specific BPF execution/JIT path.

Evaluate DPDK BPF as a replacement.

Acceptance criteria:

- semantic equivalence;
- verifier behavior understood;
- x86 performance acceptable;
- ARM path materially improved over interpretation;
- maintenance/security benefit justifies any performance tradeoff.

Do not adopt solely because it removes code.

### D4 — ARM validation

Without local ARM hardware:

- cross-build;
- CI compile;
- unit tests under practical emulation where useful;
- maintain scalar correctness.

Real ARM performance remains a later hardware gate and must not block software architecture.

---


## 17. Phase F — operations, releases, and observability

Some Phase F prerequisites are now complete because Meson landed.

Remaining work:

- signed amd64 OCI image;
- signed arm64 OCI image;
- `.deb`;
- SBOM;
- dependency update automation;
- reproducible release metadata;
- sanitizer lanes;
- static-analysis/format tooling;
- metrics exporter built on K6;
- explicit supported dependency/toolchain matrix.

DPDK source pinning by version + SHA256 is already complete.

### DPDK version discipline

Stay on DPDK 25.11.3 until the current software architecture settles.

Do not combine:

```text
new DPDK
+
new transaction engine
+
new classifier
+
new RCU layer
```

in one debugging window.

Before the next DPDK bump, preserve benchmark coverage for:

- packet lifecycle;
- PMD null/ring;
- ring implementation;
- mempool topology/cache;
- routing;
- cross-worker allocation/free.

Future DPDK mempool cache changes are especially relevant because BESS commonly allocates on one worker and frees on another.

### Observability

Do not create a competing DPDK telemetry control plane.

A future Prometheus exporter should consume BESS-native snapshots, including K6 worker-local counters.

DPDK telemetry may be an input for PMD-specific diagnostics, not the primary user-facing API.

`pdump` remains unattractive because BESS uses `--no-shconf` and gate hooks provide more useful internal capture points anyway.

---


## 18. Phase H — modern C++ and toolchain hardening

C++23 is already the project baseline.

Current Ubuntu CI GCC 13.3 / Clang 18.1 can compile the tree at C++23.

Local newer compiler verification has also succeeded.

Do not raise the project to C++26 yet.

### Useful C++23 adoption areas

Primarily control-plane and management-side code:

- `std::expected<T, Error>`;
- strong IDs;
- concepts for interfaces where useful;
- `std::span`;
- `std::string_view`;
- `std::filesystem`;
- `std::jthread` / stop tokens for management jobs where lifetime semantics are deliberately designed;
- `std::pmr::monotonic_buffer_resource` for short-lived validation/planning arenas.

Do not use modern syntax merely for stylistic churn in packet hot loops.

### Hardening

Evaluate, do not blindly enable in dataplane:

- `_GLIBCXX_ASSERTIONS`;
- trivial auto-variable initialization;
- GCC `-fhardened` subfeatures;
- stronger linker hardening;
- ASan/UBSan;
- targeted TSan;
- libFuzzer;
- clang-tidy;
- include-what-you-use.

Every dataplane-hardening option with potential runtime cost requires benchmarks.

---


### Retained research detail (2026-09-17)


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


## 19. Phase I — compile-time invalid-state prevention

Use the type system aggressively for things it can actually guarantee.

High-value items:

- strongly distinct `WorkerId`, `GateId`, `PortId`, `QueueId`;
- scoped enums;
- typed resource/accounting arrays;
- canonical `PciAddress`;
- `std::optional` instead of cold-path integer sentinels;
- physical-quantity types for cycles/time/rates where call-site clarity justifies them;
- compile-time negative tests.

Example:

```cpp
static_assert(!std::is_convertible_v<WorkerId, QueueId>);
```

Do not claim type safety solves concurrency.

The historical worker teardown and `all_tcs_` issues were synchronization/lifetime bugs and require runtime concurrency design.

---


## 5. Hardware-gated Phase C-HW

No suitable real NIC is currently available.

These items are parked deliberately and **do not gate software-only phases**:

```text
[ ] Physical NIC VFIO validation
[ ] Real-NIC MTU/jumbo validation
[ ] Real-NIC scatter RX
[ ] Multi-queue/RSS validation
[ ] Real-NIC throughput baseline
[ ] AF_XDP zero-copy on suitable real hardware
[ ] RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE correctness
[ ] RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE performance
[ ] MT-lockfree Tx capability validation
[ ] PortOut lock-elision benchmark
[ ] checksum-offload validation if later justified
[ ] TSO/GSO evaluation if later justified
[ ] hardware meter backend
[ ] rte_flow acceleration backend
```

#### Important rules

Do not enable `MBUF_FAST_FREE` merely because a PMD advertises it.

Stage 2C deliberately supports:

- clones;
- chains;
- external buffers;
- nontrivial refcounts.

Any fast-free optimization must prove that the packets sent through the relevant path satisfy DPDK's ownership preconditions.

Likewise, do not remove the current `PortOut` synchronization until a PMD actually advertises the relevant thread-safety capability and a real workload validates the result.

---


## Cross-cutting design and acceptance rules

### 4. Linux I/O end state

#### 4.1 PMDPort is the generic DPDK port abstraction

Physical device:

```text
PMDPort(pci=...)
```

Virtual DPDK device:

```text
PMDPort(vdev=...)
```

This is sufficient for:

- vhost-user;
- TAP;
- AF_PACKET;
- memif;
- null;
- ring;
- AF_XDP;
- other DPDK ethdev vdevs;
- representor-style device arguments where supported by the PMD.

Do not add BESS port subclasses merely to wrap DPDK PMDs.

#### 4.2 AF_XDP policy

AF_XDP is configured through ordinary DPDK devargs:

```text
PMDPort(vdev="net_af_xdp,...")
```

There is **no planned AF_XDP-specific BESS protobuf/configuration layer**.

PMD-specific knobs remain DPDK devargs, including things such as:

- interface;
- copy/zero-copy mode;
- busy-poll related knobs;
- UMEM-specific settings;
- future AF_XDP PMD options.

BESS should only expose named fields for portable BESS semantics shared across backends.

#### Build dependency policy

Local:

```text
AF_XDP=auto
```

Official CI/release:

```text
AF_XDP=required
```

AF_XDP build dependencies are optional capabilities for local BESS development, but official supported builds must exercise them so the feature does not silently rot.

#### Runtime result already established

DPDK AF_XDP uses ordinary direct mbufs from the configured mempool:

- copy mode copies into normal mbufs;
- zero-copy overlays UMEM on the configured mempool;
- no BESS-specific external-buffer ownership adapter is required.

A root-owned userspace AF_XDP test through veths successfully forwarded real 64-byte traffic through BESS at roughly 1.92 Mpps / 0.984 Gbps with zero BESS-reported drops.

That number is a functional host/veth result, not a NIC performance claim.

---


### 11. Static graph, dynamic state

This is a core architecture rule for modern BESS applications.

High-frequency control changes should not require graph topology mutation.

For a UPF-like appliance:

```text
PFCP session change
    ✗ create/destroy arbitrary BESS modules per rule
    ✗ reconnect gates per session
```

Instead:

```text
mostly static packet graph
        +
dynamic classifier/action/meter/route generations
```

Structural pipeline mutation is comparatively rare.

Dynamic dataplane state mutation is frequent.

This motivates two transaction classes.

---


### 12. Two transaction classes

#### 12.1 Structural pipeline transaction

Rare.

Owns:

```text
ports
modules
connections
workers
traffic classes
```

Public operation later:

```text
ApplyPipeline
```

#### 12.2 Dataplane-state transaction

Frequent.

Owns generic mutable resources:

```text
classifier entries
actions/objects
meter profiles/states
routes/next-hops
```

Example:

```text
OMEC PFCP Session Establishment
        │
        ▼
OMEC Go PFCP compiler
        │
        ▼
BESS dataplane transaction
        │
        ├── create action
        ├── create meter
        ├── add classifier entry
        └── reference route/next-hop
        │
        ▼
validate all references
        │
        ▼
commit generation N+1
```

A failure before commit should not leave a half-programmed BESS dataplane.

---


### 13. G0 and K1 relationship

These are complementary.

```text
G0 ControlPlane
    decides WHAT state becomes active
          │
          ▼
transaction plan / generation
          │
          ▼
K1 RCU/QSBR
    decides HOW old dataplane state
    remains valid for active readers
          │
          ▼
workers report quiescent state
          │
          ▼
old objects reclaimed safely
```

Do not make the RCU library understand PFCP, pipelines, or transactions.

Do not make the transaction engine implement ad-hoc packet-path reader tracking.

---


### 15. Future hardware offload architecture

Do not allow application modules to construct PMD-specific `rte_flow` structures.

Future generic shape:

```text
software logical rule
        │
        ▼
semantic FlowProgram
        │
        ├── Match(...)
        ├── Mark(ActionId)
        ├── Queue(...)
        └── Count(...)
        │
        ▼
port.TryOffload(...)
```

Critical unification:

Software:

```text
classifier
  ↓
ActionId 4711
  ↓
ActionTable[4711]
```

Hardware:

```text
NIC rte_flow
  ↓
MARK 4711
  ↓
ActionTable[4711]
```

The continuation is identical.

Only classification placement changes.

Implementation/validation remains C-HW gated until suitable hardware exists.

---


### 20. Performance acceptance discipline

Modernization is allowed to change implementation, not silently sacrifice the dataplane.

Use the existing benchmark suite as a regression guard.

For software-only hot-path changes:

> Any repeatable regression larger than roughly 2% in a relevant benchmark must be investigated before signoff.

This is an investigation threshold, not a claim that every run must fall inside ±2%.

Use:

- pinned CPU;
- SMT sibling isolation where practical;
- same compiler/flags;
- same DPDK build;
- interleaved before/after observations;
- sufficient benchmark duration;
- median/variance rather than one sample.

Do not optimize based on noisy one-off CI timings.

CI benchmark runs are smoke tests, not performance measurements.

#### Important Stage 2B conclusion

Native `rte_mbuf *` transport itself was effectively flat.

The major lifecycle regression came from replacing BESS's specialized simple-packet lifecycle with fully generic DPDK allocation/free semantics.

The accepted design uses native raw APIs for safe/simple cases and generic fallback otherwise.

Do not reopen the packet representation to recover allocator microbenchmarks.

---


### 21. Generic classifier performance requirements

K3 must have a dedicated benchmark suite before it becomes a foundation for OMEC.

Measure at least:

```text
rule counts:
    16
    64
    256
    1K
    10K
    100K
    larger where memory allows

traffic:
    all hits
    all misses
    mixed
    hot-rule skew
    random-rule distribution

fields:
    exact only
    prefixes
    arbitrary port ranges
    mixed exact/prefix/range
```

Compare backend choices.

Benchmark:

- lookup throughput;
- cycles/packet;
- batch scaling;
- build/update cost;
- memory;
- worst-case rule position/distribution;
- generation swap latency.

Do not choose `rte_acl`, tuple space, or another backend based on a single synthetic rule shape.

---


### 22. OMEC UPF target relationship

The long-term desired division is:

```text
+------------------------------------------------------+
|                    OMEC UPF                          |
|                                                      |
| PFCP / PDR / FAR / QER / URR / GTP semantics        |
| session compilation                                  |
| Go control-plane logic                               |
+---------------------------+--------------------------+
                            |
                            | thin transactional Go SDK
                            v
+------------------------------------------------------+
|                 BESS C++ Control Plane               |
|                                                      |
| Validate / Diff / Plan / Prepare / Commit            |
| generations / conflicts / rollback                   |
+---------------------------+--------------------------+
                            |
                            v
+------------------------------------------------------+
|          Generic modern BESS dataplane substrate     |
|                                                      |
| RCU | ActionId | classifier | meter | stats | route  |
| packet mutation | fragmentation | PMD capabilities   |
+---------------------------+--------------------------+
                            |
                            v
+------------------------------------------------------+
|                  DPDK / NIC / Linux                  |
+------------------------------------------------------+
```

The goal is to make OMEC-specific code a consumer of BESS, not a permanent fork of generic dataplane infrastructure.

---


### 23. Things that should remain application-specific

Do not move these into generic BESS merely because OMEC needs them:

```text
PFCP message/session semantics
PDR precedence semantics
FAR behavior semantics
QER hierarchy and QFI meaning
URR accounting semantics
F-SEID/session identity
N3/N6/N9 interface semantics
GTP-U TEID interpretation
PSC semantics
Echo / End Marker policy
UPF buffering / NOCP behavior
UPF-specific SessionProgram
UPF-specific PdrAction/FarAction/QerState
```

BESS provides the mechanisms underneath.

---


### 24. Rejected or constrained directions

These decisions should not be repeatedly reopened without new evidence.

#### Do not restore a custom VPort/kernel module path

The legacy kmod/VPort architecture is gone.

Modern host/network integration is PMD-based.

#### Do not create an AF_XDP-specific BESS port

Use `PMDPort.vdev`.

#### Do not mirror `rte_eth_dev_info` into public protobufs

Expose semantic BESS capabilities, not raw backend implementation flags.

#### Do not globally enable checksum/TSO offload

BESS checksum modules may verify packets or produce checksums; those are different semantics.

Future TX offload must be explicit graph/action semantics with correct mbuf metadata preparation.

#### Do not replace `CuckooMap` globally with `rte_hash` without a concrete workload/result

The typed C++ map semantics are useful and the generic replacement cost is nontrivial.

#### Do not silently replace the existing toy ACL module with `rte_acl`

Build the new classifier framework with explicit modern semantics.

#### Do not adopt `rte_fib` as routing default yet

The existing benchmark found correctness failures.

#### Do not treat RCU as a multi-writer synchronization primitive

RCU solves read-side lifetime/reclamation.

Writers still require correct serialization/transaction semantics.

#### Do not expose raw `rte_flow` to applications

Compile semantic BESS rules to a backend.

#### Do not use client-side RPC sequences as transactions

Transactions belong in `bessd`.

---


#### Rejected from the 2026-09-18 DPDK-proposal review

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


## 26. Immediate execution plan

### Milestone 1 — fix modern glog daemon mode

Small independent correctness fix.

Do not let it grow into G0.

Acceptance:

- foreground mode still works;
- daemon mode works on glog >= 0.7;
- Ubuntu CI behavior remains correct;
- no recursive logging through replaced stdio streams.

### Milestone 2 — G0 transactional C++ control core

First major project.

Deliver:

1. extract `ControlPlane`;
2. define internal `PipelineSpec`;
3. implement validation;
4. implement current-vs-desired diff;
5. define explicit operation plan;
6. separate prepare/commit/abort lifecycle;
7. introduce generation IDs;
8. introduce optimistic expected-generation conflict checks;
9. route existing RPC mutations through the new C++ core where practical;
10. prove failure does not leave half-applied structural state.

Do not build Go SDK yet.

### Milestone 3 — K1 RCU/QSBR

Implement generic read-side lifetime mechanism integrated with workers.

### Milestone 4 — K2 ActionId/object tables

Make immutable generic actions a first-class continuation target.

### Milestone 5 — K3 classifier framework

Build runtime-schema classifier and benchmark backends.

Once these three are in place, modern OMEC UPF work can begin depending on stable generic BESS primitives rather than inventing them inside the UPF tree.

---


## 25. Known lower-priority research/backlog

These remain useful but should not distract from G0/K.

- symmetric RSS configuration as a generic PMD capability;
- RETA control if a real consumer appears;
- graph-aware mbuf recycling research after a desired-state graph representation exists;
- power-aware scheduler idle/wakeup design;
- C++26 `std::simd` prototype when toolchains/library maturity justify it;
- stable external plugin ABI;
- stable plugin development package/export metadata;
- optional Unix-domain-socket gRPC transport;
- streaming stats/events API;
- PGO/BOLT/ThinLTO only after representative workloads and architecture settle.

---


## End state

### 27. Definition of the software-only modernization end state

Ignoring hardware-gated validation, the modernization effort is software-complete when:

```text
Build
    Meson-only, reproducible, pinned dependencies, strong CI

Packets
    native rte_mbuf, multisegment/extbuf, explicit safe ownership

Ports
    PMD-centric, capability-aware, generic vdev path

Control
    C++ desired-state/transaction core inside bessd
    generation/conflict/rollback semantics

Mutable dataplane state
    RCU/QSBR-managed generations
    ActionId/object tables
    unified classifier
    generic meters
    worker-local stats
    route/next-hop abstraction

SDKs
    thin Go and C++ transactional bindings
    no client-side BESS orchestration/rollback

Portability
    scalar + x86 + ARM architecture boundaries
    measured runtime ISA selection
    BPF backend decision complete

Operations
    install/package/release/SBOM/observability paths established

Safety
    strong IDs and compile-time invalid-state prevention where useful
    sanitizer/static-analysis coverage appropriate to the codebase
```

Hardware validation can then be performed as a dedicated lab phase without blocking the software architecture.

---


### 28. Hardware validation end state

When suitable hardware becomes available, validate as one focused program:

```text
VFIO physical NIC
    ↓
single/multi-queue RX/TX
    ↓
MTU / jumbo / scatter
    ↓
RSS / affinity
    ↓
cross-worker forwarding
    ↓
AF_XDP zero-copy where relevant
    ↓
MBUF_FAST_FREE
    ↓
MT-lockfree Tx / lock elision
    ↓
semantic rte_flow MARK(ActionId)
    ↓
hardware meters if useful
```

Every hardware acceleration must retain a software fallback with identical BESS semantics.

---


### 29. Final architectural principles

1. **BESS owns mechanisms; applications own domain semantics.**
2. **`bessd` owns transaction semantics; clients submit desired state.**
3. **Language SDKs are bindings, not orchestration engines.**
4. **Static-ish graph, dynamic state.**
5. **Publish immutable state; avoid mutating structures read by workers.**
6. **Use RCU for lifetime, not as a substitute for writer synchronization.**
7. **Expose semantic capabilities, not raw DPDK internals.**
8. **Keep DPDK PMD-specific configuration in DPDK devargs unless BESS has a portable semantic reason to abstract it.**
9. **No packet-path shared ownership/refcounting abstractions.**
10. **Optimize only after correctness and representative measurement.**
11. **Hardware absence must not stall software architecture.**
12. **Do not preserve compatibility layers merely because they existed historically when a clean cutover is feasible.**

---


### 30. Current handoff

Current reviewed `develop` baseline:

```text
e8c8e17684115e71ff9727134b5eb6e346db175b
Fix Meson packaging and CI coverage
```

Phase E is closed.

The next architecture work is:

```text
1. glog >= 0.7 daemon-mode recursion fix
2. G0 C++ transactional control-plane core
3. K1 RCU/QSBR
4. K2 ActionId/object tables
5. K3 unified classifier
```

Do not block this sequence on real-NIC work.


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




