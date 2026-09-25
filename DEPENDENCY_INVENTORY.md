# Dependency inventory

This is a source-level inventory of explicit dependency declarations in tracked repository files, checked at the current `develop` tree after CI action refs were upgraded. It records declarations and historical references; it does **not** report versions installed on a particular machine. “Unpinned” means the repository names a package or major/tag but does not lock its resolved version.

## Explicit version constraints and pins

| Dependency / tool | Explicit version in the repository | Source and interpretation |
|---|---|---|
| DPDK | **25.11.3**, exact archive and SHA-256 `3719acc586b310c4f60ba230683bf4f1e12c6f2f5bee11f7c01b1bbd0ded7490` | `deps/dpdk.json`; bootstrap downloads this archive and verifies the digest. |
| Meson | `>=1.2.0` | `meson.build`; minimum, not a lock. |
| libxdp | `>=1.2.2` when AF_XDP is enabled/required | `meson.build`, `tools/bootstrap_dpdk.py`; minimum only. |
| Python `scapy` | `>=2.7.0` | `requirements.txt`; stable release minimum as of 2026-09-25. |
| Python `flask` | `>=3.1.3` | `requirements.txt`; stable release minimum as of 2026-09-25. |
| Python `grpcio` | `>=1.84.0` | `requirements.txt`; stable release minimum as of 2026-09-25; requires Python >=3.10. |
| Python `protobuf` | `>=7.36.2,<8` | `requirements.txt`; bounded major range; stable release minimum as of 2026-09-25; requires Python >=3.10. |
| GitHub Actions checkout | `actions/checkout@v7` | `.github/workflows/ci.yml`; major tag, not an immutable commit SHA. |
| GitHub Actions cache | `actions/cache@v6` | `.github/workflows/ci.yml`; major tag, not an immutable commit SHA. |
| GitHub-hosted runner | `ubuntu-24.04` | `.github/workflows/ci.yml`; OS release label, not an image digest. |
| Compilers in current CI | `gcc-14` / `g++-14` and `clang-19` / `clang++-19` | `.github/workflows/ci.yml`, installed by `env/install-deps.sh build`; major package/tool names, not exact package builds. GCC 14 replaced the distro-default GCC 13 on 2026-09-25 (C++23 deducing `this`). |
| BESS project metadata | `0.1.0` | `meson.build`; project version, not a dependency. |
| C++ language level | C++23 | `meson.build`; language-standard requirement, not a compiler version pin. |

## C/C++ libraries and build tools

Meson discovers these through `pkg-config` or executable names. Except for the DPDK bootstrap pin and the libxdp minimum above, the build graph does not state version constraints:

| Dependency | Version declaration | Notes / source |
|---|---|---|
| DPDK (`libdpdk`) | 25.11.3 via `deps/dpdk.json` | Required; dynamically linked by default, optional static linking. |
| libxdp | `>=1.2.2` | Required only when AF_XDP support is enabled/required. |
| libbpf | None | Required with AF_XDP; no Meson minimum. |
| glog (`libglog`) | None in Meson | Required. Source compatibility notes discuss glog `>=0.7`; see historical references. |
| gflags | None | Required. |
| protobuf (`protobuf`) | None for C++ library in Meson | Required; separate Python `protobuf` range above. |
| gRPC C++ (`grpc++`) | None | Required. |
| libunwind | None | Required. |
| zlib | None | Required. |
| libpcap | None | Required. |
| NUMA (`numa`) | None | Required. |
| GoogleTest (`gtest`) | None | Required by test targets. |
| Google Benchmark (`benchmark`) | None | Required by benchmark targets. `benchmark_main` is optional; Meson falls back to the library. |
| `pkg-config`, `protoc`, `grpc_cpp_plugin`, `grpc_python_plugin` | None | Required discovery/code-generation tools. |
| Python 3, C/C++ compiler, Ninja | None | Python 3/compiler are selected by Meson; Ninja is used by the documented Meson build. |

`core/meson.build`, `protobuf/meson.build`, `pybess/meson.build`, and `sample_plugin/meson.build` add targets and reuse these root dependencies; they add no further version constraints.

## Python declarations and consistency

- `requirements.txt`: `scapy>=2.7.0`, `flask>=3.1.3`, `grpcio>=1.84.0`, `protobuf>=7.36.2,<8`.
- The host installer and both Dockerfile stages consume `requirements.txt`.
- CI uses the host installer and adds `coverage` without a version constraint.
- The root `README.md` binary quick-install uses the same four package lower bounds.
- No tracked Python lockfile was found; the ranges allow pip to resolve newer compatible releases.

## OS packages explicitly named

APT package versions are unspecified and resolve from Ubuntu 24.04 or newer
repositories. The host installer, Dockerfile, and CI share the package lists
below.

| Installer mode | Explicit package names |
|---|---|
| `runtime` | `python3`, `python3-pip`, `python3-venv`, `libnuma1`, `libpcap0.8t64`, `zlib1g`, `libunwind8`, `libbpf1`, `libxdp1`, `libgoogle-glog0v6t64`, `libgflags2.2`, `libprotobuf32t64`, `libgrpc++1.51t64`, `libc-ares2`, `libsystemd0`, `libcap2`, `libelf1t64`, `libarchive13t64`, `libjansson4`, `libgraph-easy-perl`, `tcpdump`. |
| `build` | `build-essential`, `gcc-14`, `g++-14`, `clang-19`, `ccache`, `meson`, `ninja-build`, `pkg-config`, `python3-pyelftools`, `xz-utils`, `libnuma-dev`, `libpcap-dev`, `zlib1g-dev`, `libunwind-dev`, `libbpf-dev`, `libxdp-dev`, `libgoogle-glog-dev`, `libgflags-dev`, `libgtest-dev`, `libbenchmark-dev`, `libgrpc++-dev`, `protobuf-compiler-grpc`, `libprotobuf-dev`, `protobuf-compiler`, `libsystemd-dev`, `libc-ares-dev`, `libcap-dev`. |
| `vm` | `cloud-image-utils`, `curl`, `numactl`, `openssh-client`, `qemu-kvm`, `qemu-utils`. |
| README binary quick-install | `python3-venv`, `libgraph-easy-perl`. |

## Container, VM, and external action references

| Component | Explicit reference | Pin status |
|---|---|---|
| CI runner | `ubuntu-24.04` | Release label only. |
| CI actions | `actions/checkout@v7`, `actions/cache@v6` | Major tags; mutable, not commit-SHA pinned. |
| Build-container base | `ubuntu:noble` (`env/Dockerfile`, `env/rebuild_images.py`) | Floating image tag; no digest. |
| Published build container | `nefelinetworks/bess_build:latest` (`container_build.py`) | Floating `latest`; no digest. `rebuild_images.py` also publishes a date-formatted tag. |
| Vhost VM base image | Ubuntu 24.04 cloud image from `cloud-images.ubuntu.com/noble/current` (`bessctl/conf/port/vhost/create_image.sh`) | Floating `current` URL; no image digest. |
| Vhost testpmd | `launch_vm.py` reads the directory from `deps/dpdk.json` and copies `install/bin/dpdk-testpmd` | Tracks the DPDK source pin. |

## Historical and observed version references (not current pins)

These explicit version mentions in repository documentation or compatibility code do not constrain the current build unless separately listed above.

- `MODERNIZATION.md` records migration **DPDK 19.11.4 → 25.11.3** and mentions DPDK 20.11+/21.11 behavior. The current authoritative pin is `deps/dpdk.json` at 25.11.3.
- `MODERNIZATION.md` records observed toolchains GCC 13.3/15/16, Clang 18.1.3/20, g++ 16, and MSVC 19.44; glibc 2.42+; and observed libraries glog 0.7/0.7.1, protobuf 36, gRPC 1.83, libxdp 1.6.3, and libbpf 1.7.0. It also mentions Python 3.12 compatibility. These are environment observations or compatibility references, not current constraints.
- The same notes record Ubuntu package builds `libxdp-dev 1.4.2-1ubuntu4` and `libbpf-dev 1:1.3.0-2build2`; these are observations from a disposable environment, not apt pins.
- A historical `MODERNIZATION.md` section says “current Ubuntu CI GCC 13.3 / Clang 18.1”, while the current workflow selects `gcc-14` and `clang-19`. Treat that prose as a stale snapshot.
- `core/bessd.cc` and its tests discuss glog `>=0.7` and an observed `google-glog 0.7.1-2`; this is compatibility context, not a Meson minimum.
- `core/utils/endian.h` mentions GCC 4.9+ as an optimization note, not a supported-toolchain declaration.

## Reproducibility summary

1. DPDK is the only major native dependency with an exact source archive and SHA-256 pin.
2. Python requirements are ranges, not a lock; host, Docker, CI, and the quick-install instructions use the same lower bounds.
3. Most C++ libraries and all APT packages are resolved from OS repositories without version constraints.
4. The build-container base and Ubuntu VM cloud image use mutable tags/URLs rather than image digests.
5. Current CI action references use major tags rather than immutable commit SHAs.
6. No tracked Poetry/Pipenv/npm/Cargo/Go lockfile or equivalent dependency lock was found. `requirements.txt` is the only tracked Python package list.
