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
```

Steps, in CI order: `bootstrap` (DPDK), `configure`, `build`, `verify-dpdk`,
`layers`, `test`, `verify-install`. Build trees go to `build/ci-<lane>`, DPDK to
`deps/dpdk-*/install-<lane>` (`tools/bootstrap_dpdk.py --variant`), so nothing
here touches the fast tree or its DPDK.

## What the profile fixes

| Setting | Value | Why |
|---|---|---|
| Meson `buildtype` | `debugoptimized` (`-O2 -g`) | optimisation-dependent warnings (`maybe-uninitialized`, `array-bounds`, `stringop-*`) only appear when the optimiser runs |
| `cpu` | `x86-64-v3` | the portable floor (D-016); `native` can hide a missing-intrinsic error |
| `af_xdp` | `required` | CI fails if the AF_XDP driver is absent |
| benchmarks | on (the default) | CI compiles them; the fast tree does not |
| sample plugin | on | the plugin path is part of the contract |
| DPDK | `bess` profile, `x86-64-v3`, built by the lane's compiler | a header-visible difference between a GCC- and a Clang-built DPDK is a real CI difference |
| compilers | gcc-14 / g++-14, clang-19 / clang++-19 | `check-pins` fails CI when the script and `ci.yml` disagree |

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
- **CPU.** The code is built for `x86-64-v3`, but only a CPU with it can run it.
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
   is in the script, so there is nothing to forget.
