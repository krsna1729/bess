# Running what CI runs

The fast tree (`tools/profiles/fast.ini`) is for the edit loop. It is not CI:
it builds at `-O1`, for `cpu=native`, without benchmarks, and with one
compiler. A change can pass it and still fail in CI. That has happened: an
unused private field in a benchmark compiled with GCC 16 in the fast tree and
failed under Clang 19 in CI, because the fast tree builds no benchmarks.

`tools/ci_profile.py` is the one place that defines what the gating CI lanes
do. CI calls it; a developer calls the same script. Neither side can drift,
because there is no second copy.

```
tools/ci_profile.py info --compiler gcc      # what would run, and how it differs from CI
tools/ci_profile.py all  --compiler gcc      # the whole gcc lane
tools/ci_profile.py all  --compiler clang    # the whole clang lane
tools/ci_profile.py build --compiler clang   # one step; steps can be re-run
tools/ci_profile.py all  --compiler gcc --arch-generic     # the gcc-generic lane
tools/ci_profile.py all  --compiler gcc --arch aarch64 --dry-run  # the arm64 lane's commands
tools/ci_profile.py check-cpu                # does this CPU have the floor?
```

Steps, in CI order: `bootstrap` (DPDK), `configure`, `build`, `verify-dpdk`,
`layers`, `test`, `verify-install`, `clean-tree`. `clean-tree` fails if the
build, the tests or the install left the source tree changed (a modified or
untracked, non-ignored file): a build writes only to its build directory.
Under `all` it compares with the tree as `all` found it, so uncommitted work
of your own is not blamed on the build; run alone it needs a clean tree.
Build trees go to `build/ci-<lane>`, DPDK to
`deps/dpdk-*/install-<lane>` (`tools/bootstrap_dpdk.py --variant`), so nothing
here touches the fast tree or its DPDK.

## What the profile fixes

| Setting | Value | Why |
|---|---|---|
| Meson `buildtype` | `debugoptimized` (`-O2 -g`) | optimisation-dependent warnings (`maybe-uninitialized`, `array-bounds`, `stringop-*`) only appear when the optimiser runs |
| `cpu` | `x86-64-v3` on x86_64, `armv8.2-a` on aarch64 | the portable floor per architecture (D-016; M21); `native` can hide a missing-intrinsic error |
| `af_xdp` | `required` | CI fails if the AF_XDP driver is absent |
| benchmarks | on (the default) | CI compiles them; the fast tree does not |
| sample plugin | on | the plugin path is part of the contract |
| DPDK | `bess` profile, built by the lane's compiler; x86_64 `cpu_instruction_set=x86-64-v3`, aarch64 `platform=generic` (`-march=armv8-a+crc`, 128-byte cache lines) | a header-visible difference between a GCC- and a Clang-built DPDK is a real CI difference |
| compilers | gcc-14 / g++-14, clang-19 / clang++-19 | `check-pins` fails CI when a lane in `LANES` disagrees with its family's pin |

## Architectures

The architecture is the machine's (`uname -m`); the script picks the floor
for it (`CPU_FLOOR`). `--arch` only shows another architecture's commands
(`--dry-run`, `info`); nothing cross-builds.

- **aarch64 floor `armv8.2-a`** (LSE atomics, CRC32, RDM): Neoverse N1
  (Graviton2, Ampere Altra) and newer, and GitHub's `ubuntu-24.04-arm` runners
  (Azure Cobalt 100, Neoverse N2). Not Cortex-A72 (Graviton1). `check-cpu`
  reads `/proc/cpuinfo` for `fp asimd crc32 atomics asimdrdm`; on x86_64 it
  asks glibc's loader whether `x86-64-v3` is supported. CI runs it first.
- **Lanes.** `gcc-arm64` and `clang-arm64` run every step on
  `ubuntu-24.04-arm` with the gating compilers, AF_XDP required (Ubuntu 24.04
  ships libxdp/libbpf for arm64). `gcc-generic` is x86 with
  `-Darch_generic=true`: `core/arch/` takes its portable paths, the ones a
  third architecture runs, and everything (benchmarks too) builds and tests
  against them. All three are experimental (cannot block a merge) until they
  have been green; `check-pins` already holds them to the gating pins.
- **Which lanes run when.** The lane table is `LANES` in `tools/ci_profile.py`;
  the workflow's `Lanes` job turns it into the matrix
  (`ci_profile.py lanes --event <event>`). Pull requests run the gating pair
  (`gcc`, `clang`) and the sanitizer lanes; pushes to `develop`/`master` and
  tags run every lane, adding arm64, generic and ubuntu-26.04 (`push_only`).
  A break those lanes catch shows up on the push and is fixed forward. A new
  push to a pull request cancels its older run.
- **What a pull request skips.** Benchmarks are built in every lane but run
  only on pushes and the nightly run (at 0.001 s per case they measure
  nothing; their setup was 80% of a gating lane's test time). clang-tidy
  checks only the changed C++ sources, or every source when a header, a
  `meson.build` or the tidy configuration changes; pushes and the nightly run
  check all. A nightly scheduled run (03:17 UTC) runs every lane and the
  benchmarks on `develop`.
- **Compiler cache.** CI restores a ccache per lane (keyed by commit, restored
  from the newest; pull requests can read `develop`'s) and uses it when
  `CCACHE_DIR` is set. Local runs use ccache when installed.
- **DPDK's flags do not choose BESS's ISA.** `meson.build` drops every `-m`
  flag from libdpdk's cflags (prints them at configure) and stops on any flag
  it does not recognise. `bess-dev.pc` carries bessd's `-march` and the
  filtered DPDK flags instead of requiring libdpdk, so plugins build for the
  same ISA; in `verify-install`, `check_installed_headers.py --march <cpu>`
  fails unless bess-dev's only machine flag is exactly that `-march`.
- **Architecture code stays in `core/arch/`.** `tools/check_arch.py` (in
  `layers` and the meson `architecture` suite) counts, outside `core/arch/`,
  intrinsic-header includes, preprocessor conditions on architecture/ISA
  macros, asm instructions and intrinsic uses, tests and benchmarks included.
  `tools/arch_allowlist.json` grandfathers the existing ones per file with a
  reason; any count above or below its entry fails, so the list only shrinks.
  It is edited by hand; `check_arch.py --report` prints the current counts in
  its format.

## Compiler versions

The script uses `gcc-14` and `clang-19` when they are installed. When they are
not, it uses the nearest compiler (`g++`, `clang++`) and says so; `info` lists
this under "divergences from CI". A newer compiler finds most of what an older
one finds, but not all of it: a diagnostic that exists only in one version, or
a feature the older compiler lacks, can still pass locally and fail in CI. To
run the exact versions, use the container:

```
tools/ci_container.sh gcc            # all steps, in Ubuntu 24.04 with gcc-14
tools/ci_container.sh clang build    # one step, with clang-19
```

`env/ci.Dockerfile` installs the same packages the CI jobs install, with the
same script (`env/install-deps.sh`). The container is limited to 8 CPUs and
10 GB. Its trees are `build/ctr-<lane>`, kept apart from host trees.

## What still differs from a CI run

- **Hugepages.** The runner reserves 512; a developer machine usually has none,
  and the tests use the no-hugepage mode.
- **CPU.** The code is built for the floor (`x86-64-v3` or `armv8.2-a`), but
  only a CPU with it can run it; `check-cpu` tells.
- **Cache and runner image.** CI restores DPDK from a cache and starts from a
  fresh runner image; a local tree is incremental.
- **Ubuntu 26.04 lanes.** `gcc-u26` and `clang-u26` (GCC 15, Clang 22) run in CI
  as experiments that cannot block a merge. They are not part of this profile.
- **The release job** (`publish-release`: static standalone binaries, `NDEBUG`,
  the `full` DPDK profile) is a separate configuration. This script does not
  build it. It uses gcc-14, the same compiler as the `gcc` lane.

## Two mistakes this prevents

1. Passing the fast tree and failing CI on a compiler, a flag or a target the
   fast tree does not build. Run `tools/ci_profile.py all` before a push that
   touches headers, benchmarks or the build.
2. Changing the CI configuration in `ci.yml` and not locally. The configuration
   and the lane table are in the script, so there is nothing to forget.

## Include paths

`tools/check_includes.py` rejects every `..` path component in quoted and angle
includes, with no allowlist. D-058's 359 legacy `..` includes in 124 files were
respelled root-relative (`"../utils/x.h"` -> `"utils/x.h"`), each to the same
header; `core/resume_hooks/metadata.cc` uses `<metadata.h>` because the quoted
form would find its sibling `resume_hooks/metadata.h`. No installed header path
changed.
