[![Build Status](https://travis-ci.org/NetSys/bess.svg?branch=master)](https://travis-ci.org/NetSys/bess)
[![codecov](https://codecov.io/gh/NetSys/bess/graph/badge.svg)](https://codecov.io/gh/NetSys/bess)

## BESS (Berkeley Extensible Software Switch)

BESS (formerly known as [SoftNIC](https://www2.eecs.berkeley.edu/Pubs/TechRpts/2015/EECS-2015-155.html)) is a modular framework for software switches. BESS itself is *not* a virtual switch; it is neither pre-configured nor hardcoded to provide particular functionality, such as Ethernet bridging or OpenFlow-driven switching. Instead, you (or an external controller) can *configure* your own packet processing datapath by composing small "modules". While the basic concept is similar to [Click](http://read.cs.ucla.edu/click/click), BESS does not sacrifice performance for programmability.

BESS was created by Sangjin Han and is developed at the University of California, Berkeley and at Nefeli Networks. [Contributors to BESS](https://github.com/NetSys/bess/blob/master/CONTRIBUTING.md) include students, researchers, and developers who care about networking with high performance and high customizability. BESS is open-source under a BSD license.

If you are new to BESS, we recommend you start here:

1. [BESS Overview](https://github.com/NetSys/bess/wiki/BESS-Overview)
2. [Build and Install BESS](https://github.com/NetSys/bess/wiki/Build-and-Install-BESS)
3. [Write a BESS Configuration Script](https://github.com/NetSys/bess/wiki/Writing-a-BESS-Configuration-Script)
4. [Connect BESS to a Network Interface, VM, or Container](https://github.com/NetSys/bess/wiki/Hooking-up-BESS-Ports)

To install BESS on Linux quickly, download a release from this repository's
Releases: the `bess` .deb (`sudo apt install ./bess_<version>_amd64.deb`; the
optional `bess-tools`, `bess-python`, `bess-dev` and `bess-sdk` packages carry
dpdk-devbind.py, the legacy Python client, the plugin headers and the C++
control SDK) or the tarball. Neither needs Python; the tarball needs the
libraries `env/install-deps.sh runtime` installs. With the tarball:

    sudo sysctl vm.nr_hugepages=1024  # For single NUMA node systems
    tar -xf bess-<version>-linux-x86_64.tar.gz
    cd bess-<version>-linux-x86_64
    sudo bin/bessd -f    # in another terminal:
    bin/bessctl apply share/bess/conf/samples/exactmatch.yaml && bin/bessctl resume

Documentation can be found [here](https://github.com/NetSys/bess/wiki/). Please consider [contributing](https://github.com/NetSys/bess/wiki/How-to-Contribute) to the project!

## Build with Meson

The supported developer build uses Meson and Ninja.  DPDK is pinned in
`deps/dpdk.json`; the bootstrap helper verifies its SHA256 before extracting
or building it, then applies the local patches in `deps/dpdk-patches/`, if any
(D-131; see the README there).

```bash
tools/bootstrap_dpdk.py --af-xdp auto --cpu x86-64-v3   # aarch64: --cpu armv8.2-a
export PKG_CONFIG_PATH="$(tools/bootstrap_dpdk.py --print-pkg-config-path):${PKG_CONFIG_PATH}"
meson setup build-meson -Dcpu=x86-64-v3 -Daf_xdp=auto   # aarch64: -Dcpu=armv8.2-a
meson compile -C build-meson -j4
meson test -C build-meson --no-rebuild --print-errorlogs
```
Routine compilation is incremental. Meson's `-j4` caps each invocation at four
workers. Locally, run GCC and Clang builds sequentially because Meson does not
coordinate separate build directories. GitHub Actions schedules its compiler
matrix according to hosted-worker capacity. Use a fresh build directory for
release verification instead of cleaning the day-to-day build.

CI uses `--af-xdp required`; this checks the libxdp/libbpf development
packages, headers, and DPDK `net_af_xdp` shared and static artifacts.  Use
`-Daf_xdp=required` locally for the same check.  `-Dcpu` is passed as
`-march=<cpu>` on x86_64 and aarch64. `native` is the default; for
distributable builds use the CI floor of the architecture: `x86-64-v3` (AVX2,
BMI1/2) or `armv8.2-a` (LSE atomics, CRC32; Neoverse N1 and newer).
`-Dcpu=` (empty) passes no `-march`. BESS's ISA comes from this option alone:
the `-march`/`-mcpu`/`-m*` flags in DPDK's pkg-config cflags are dropped, and
`bess-dev.pc` gives plugins bessd's `-march`. `tools/bootstrap_dpdk.py --cpu`
builds DPDK for the same floor (on aarch64 DPDK's generic arm64 target).
`-Dtune=<name>` passes `-mtune=<name>` (the scheduling model, not the ISA:
plugins do not inherit it). The default, empty, passes none: GCC tunes
`-march=x86-64-v3` for `generic`, Clang for Haswell, and `-march=native`
tunes for the build machine. Releases keep the default; D-127 has the
measurements behind that.
`-Darch_generic=true` makes `core/arch/` use its portable code paths instead
of x86/arm64 intrinsics and asm, to build and test on x86 what other
architectures run; architecture-specific code belongs only in `core/arch/`
(`tools/check_arch.py`). For benchmarking, see
[docs/benchmarking.md](docs/benchmarking.md). Every fixed limit (gates,
workers, fields, masks, table sizes) and what happens past it is in
[docs/limits.md](docs/limits.md); `tools/check_limits.py` keeps it complete.
DPDK is consumed through `pkg-config` and is dynamically linked by default.
Use `-Ddpdk_link=static` only when a static DPDK link is intentional.

Useful Meson-native checks:

```bash
meson test -C build-meson --no-rebuild --suite python
meson test -C build-meson --no-rebuild --suite integration
meson test -C build-meson --no-rebuild --suite benchmarks
meson setup build-asan -Db_sanitize=address,undefined -Db_coverage=true
meson compile -C build-asan -j4
meson install -C build-meson --destdir "$PWD/stage"
```

Generated C++ and Python protobuf files, plus the version header, live only
under the build directory.  The sample plugin is enabled by default and is
built as `sample_plugin/libsequential_update.so`.
