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
- **Commit author identity is wrong** (`root@PARAM.localdomain`, auto-set by
  the harness). Not yet fixed — ask the user before amending anything, per
  standing git-safety rules.

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

Last updated: 2026-09-11, at commit `ecbdd3bc` on `develop`.

**Verified working:** `bessd` builds and links against DPDK 25.11.3 via the
new Meson/pkg-config build; a live `Source -> Sink` pipeline via `bessctl`
processed 7.2B packets with no crash or corruption; `core/all_test` is
175/181 (6 pre-existing `CodelTest` flakes under load, see above);
`bessctl/run_module_tests.py` passes cleanly except one pre-existing,
unrelated scapy-version checksum mismatch in `url_filter.py`.

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

## Review process established this session

For anything touching correctness-critical code (DPDK ABI/layout, build
system, concurrency), spawn a fresh `Agent` (not a fork — needs independent
eyes) with `model: "opus"`, pointed at the specific commit hash, asking it to
verify claims independently (re-derive offsets from real headers, trace call
sites, don't just trust the commit message). This caught two real bugs
(PCI-address format, `-no-pie`/shared-object breakage) that would have
shipped otherwise. **Keep doing this at every significant milestone commit** —
it's a standing instruction from the user, not a one-time thing.

## Known issues / explicit follow-ups (not yet fixed)

- [ ] **Per-queue PMD stats** (`pmd.cc`) were removed, not reimplemented.
      Proper fix needs `rte_eth_xstats_get()` with driver-specific named
      counters (e.g. `rx_q0_packets`) and a name→id lookup/cache — real
      work, not a mechanical fix. (Relates to backlog Phase F below and old
      upstream PR #1007.)
- [ ] `DPDK_VER` is duplicated (`build.py` and `core/Makefile` each hardcode
      it) and must be bumped in both places by hand — documented with a
      comment, not structurally fixed (see `9e8c4af1` commit message for why
      the obvious fix — `core/extra.mk` — doesn't work: it's `-include`d too
      late relative to where `DPDK_INSTALL_DIR` is first used).
- [ ] `core/kmod` (legacy out-of-tree VPort kernel module) is not built or
      tested by anything currently — known-broken on modern kernels per
      upstream `#1056`. Making it optional/legacy-by-default is backlog
      Phase C.
- [ ] `.github/workflows/ci.yml` was rewritten for the new build but **never
      executed against real GitHub Actions** — verify it actually works
      before trusting it (matrix, caching, runner behavior are all
      unverified; the underlying `build.py`/`make` invocations are the same
      ones verified locally).
- [ ] `url_filter.py` module test has one pre-existing `FAIL` (not crash) —
      an IP checksum byte mismatch almost certainly caused by scapy version
      drift between whenever this test's expected-packet data was last
      generated and the now-installed scapy 2.7.0. Not investigated further;
      unrelated to this session's diff.
- [ ] Commit author on all commits this session is `root@PARAM.localdomain`
      — cosmetic, but ask the user before fixing (would require amending
      already-pushed commits).
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
(mostly still ahead of us — only the DPDK version bump itself, Phase A, is
underway). Phases G–I are a newer, larger proposal — a from-first-principles
rethink of the control plane and language/tooling stack — added 2026-09-11.
**G and the A–F track are largely independent** (G touches `bessctl`/gRPC/the
client side; A–F touch the dataplane/DPDK/build side); either can proceed
first. Read the "how G relates to A–F" note at the start of Phase G before
picking one.

## Phase A — DPDK/build modernization (in progress)

- [x] DPDK 19.11.4 → 25.11.3 LTS port (commits 3–4 above)
- [ ] Verify the rewritten CI workflow actually passes on GitHub Actions
- [ ] Reimplement per-queue PMD stats via xstats (see known issues)
- [ ] Audit remaining drivers (`vport.cc`, `pcap.cc`) for latent DPDK 25.11
      API drift beyond what compiled cleanly — they compiled without error,
      but "compiles" isn't "verified correct" the way `pmd.cc` now is after
      the live-pipeline test

## Phase B — Packet/mbuf architecture (deferred, large, needs benchmarking)

Stop mirroring `rte_mbuf` byte-for-byte in `Packet`; make `Packet` a thin
wrapper (ideally `sizeof(void*)`) around a real `rte_mbuf*`, with BESS's own
metadata moved into DPDK's supported mbuf private-data area instead of a
hand-maintained shadow struct. This is the fix that makes Phase-A-style
ABI-drift bugs structurally impossible instead of merely caught by
`static_assert`. Requires a benchmark suite *before* starting (this repo does
not have one yet) — packet access, batch operations, PMD forwarding,
scheduler throughput, at minimum. Do not merge if it regresses any of those.
See the original modernization-plan analysis of `core/packet.h` earlier in
this project's history for the detailed design sketch (`PacketRef`,
`BessPacketPrivate`, offset-resolved `MetadataRef<T>`).

Also bundle when doing this: dynamic packet-pool data-room sizing (today
`SNBUF_DATA` is a fixed 2048-byte compile-time constant — this is why jumbo
frames don't work, see upstream `#1024`), and real multi-segment mbuf test
coverage.

## Phase C — Linux I/O modernization

Make VFIO the primary physical-NIC path and AF_XDP the primary Linux
host/container path; keep vhost-user for VMs. Move `core/kmod` behind an
explicit "legacy, unsupported by default" build flag rather than something
`bessd` tries to auto-load on startup (see current behavior:
`vport.cc:318` logs a warning and moves on when it's missing — that's
already graceful, but the fallback direction should flip: VFIO/AF_XDP should
be first-class, kmod optional).

## Phase D — ARM64 + portable SIMD

Add `core/arch/{generic,x86,arm64}/` with a correct scalar reference
implementation always available; x86 SSE/AVX and ARM NEON selected at
startup via one dispatch per batch (not per packet/byte). Harvest upstream
PR `#1041` (ARM support) as reference material, not a mergeable diff — it
predates this DPDK port and the packet-layout work.

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

## Phase H — C++23/26 tooling adoption (proposed 2026-09-11, not started)

Applies mainly to the control-plane/CLI/SDK code from Phase G — the
dataplane should stay on the more conservative C++20 baseline from the
earlier (Phase B-adjacent) modernization plan discussion unless a specific
feature is proven zero-cost there. As of this writing GCC 16.2 is current;
GCC 16 marks C++20 Modules, reflection, contracts, and `std::simd` as
*experimental* — so require C++23 for production code, treat C++26 features
as isolated experiments only, not production dependencies, until compiler
support matures.

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
microbenchmark suite (`BM_PacketHeadData`, `BM_PacketAllocFree`,
`BM_BatchForward`, `BM_ExactMatch32`, etc. — this repo does not have one yet
and Phase B explicitly should not start without one); libFuzzer +
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
