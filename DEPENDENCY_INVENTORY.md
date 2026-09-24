# Dependency inventory

This is a source-level inventory of explicit dependency declarations in tracked repository files, checked at the current `develop` tree after CI action refs were upgraded. It records declarations and historical references; it does **not** report versions installed on a particular machine. “Unpinned” means the repository names a package or major/tag but does not lock its resolved version.

## Explicit version constraints and pins

| Dependency / tool | Explicit version in the repository | Source and interpretation |
|---|---|---|
| DPDK | **25.11.3**, exact archive and SHA-256 `3719acc586b310c4f60ba230683bf4f1e12c6f2f5bee11f7c01b1bbd0ded7490` | `deps/dpdk.json`; bootstrap downloads this archive and verifies the digest. |
| Meson | `>=1.2.0` | `meson.build`; minimum, not a lock. |
| libxdp | `>=1.2.2` when AF_XDP is enabled/required | `meson.build`, `tools/bootstrap_dpdk.py`; minimum only. |
| Python `scapy` | `>=2.5.0` | `requirements.txt`; lower bound only. |
| Python `flask` | `>=3.0.0` | `requirements.txt`; lower bound only. |
| Python `grpcio` | `>=1.60.0` | `requirements.txt`; lower bound only. |
| Python `protobuf` | `>=7.36.1,<8` | `requirements.txt`; bounded major range, not an exact pin. |
| GitHub Actions checkout | `actions/checkout@v7` | `.github/workflows/ci.yml`; major tag, not an immutable commit SHA. |
| GitHub Actions cache | `actions/cache@v6` | `.github/workflows/ci.yml`; major tag, not an immutable commit SHA. |
| GitHub-hosted runner | `ubuntu-24.04` | `.github/workflows/ci.yml`; OS release label, not an image digest. |
| Clang in current CI | `clang-19` / `clang++-19` | `.github/workflows/ci.yml`; major package/tool names, not exact package builds. GCC/G++ are selected as `gcc`/`g++` without a version. |
| Vagrant plugins | `vagrant-reload >=0.0.1`; `vagrant-cachier >=1.2.1` | `env/Vagrantfile`; minimum plugin versions. |
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

## Python declarations and inconsistencies

- `requirements.txt`: `scapy>=2.5.0`, `flask>=3.0.0`, `grpcio>=1.60.0`, `protobuf>=7.36.1,<8`.
- CI installs that file plus `coverage` without a version constraint (`.github/workflows/ci.yml`).
- `env/runtime.yml` separately installs `protobuf`, `grpcio`, `scapy`, and `flask` with Ansible `state: latest`; it does not consume `requirements.txt` or preserve its bounds.
- The root `README.md` quick-install example uses unversioned `pip install protobuf grpcio scapy` and omits Flask.
- No tracked Python lockfile was found; the ranges allow pip to resolve newer compatible releases.

## OS packages explicitly named

APT package versions are generally unspecified and resolve from configured distribution repositories. Suffixes such as `clang-19`, `g++-8`, and `clang-6.0` identify toolchain series, not full package versions.

| Source / condition | Explicit package names (unversioned unless the name itself has a series suffix) |
|---|---|
| `.github/workflows/ci.yml` current CI | `build-essential`, `clang-19`, `meson`, `ninja-build`, `pkg-config`, `python3-pip`, `python3-pyelftools`, `xz-utils`, `libnuma-dev`, `libpcap-dev`, `zlib1g-dev`, `libunwind-dev`, `libbpf-dev`, `libxdp-dev`, `libgoogle-glog-dev`, `libgflags-dev`, `libgtest-dev`, `libbenchmark-dev`, `libgrpc++-dev`, `protobuf-compiler-grpc`, `libprotobuf-dev`, `protobuf-compiler`, `libsystemd-dev`, `libc-ares-dev`, `libcap-dev`. |
| `env/build-dep.yml`, legacy distributions | `apt-transport-https`, `ca-certificates`, `g++`, `g++-7`, `make`, `libunwind8-dev`, `liblzma-dev`, `zlib1g-dev`, `libpcap-dev`, `libssl-dev`, `libnuma-dev`, `libgflags-dev`, `libgoogle-glog-dev`, `libgtest-dev`, `python`, `pkg-config`. |
| `env/build-dep.yml`, Ubuntu 24.04+ | `apt-transport-https`, `ca-certificates`, `g++`, `make`, `libunwind8-dev`, `liblzma-dev`, `zlib1g-dev`, `libpcap-dev`, `libssl-dev`, `libnuma-dev`, `libgflags-dev`, `libgoogle-glog-dev`, `libgtest-dev`, `libbenchmark-dev`, `libbpf-dev`, `libxdp-dev`, `meson`, `ninja-build`, `pkg-config`, `python3`, `python3-pyelftools`. |
| `env/build-dep.yml`, packaged gRPC path | `libc-ares-dev`, `libbenchmark-dev`, `libgrpc++-dev`, `libprotobuf-dev`, `protobuf-compiler-grpc`; apt versions unspecified. |
| `env/ci.yml`, Ubuntu 24.04+ | `clang`, `ccache`; `clang` is not series-pinned here. |
| `env/ci.yml`, pre-Ubuntu 24.04 | `g++-8`, `clang-6.0`, `ccache`. |
| `env/runtime.yml` | `apt-transport-https`, `ca-certificates`, `python`, `python-pip`, `libgraph-easy-perl`, `tcpdump`; unversioned. |
| `env/dev.yml` optional tools | `git`, `gdb`, `linux-tools-common`, `vim`, `lcov`, `python-autopep8`, `graphviz`; unversioned. |
| `env/kmod.yml` | `apt-transport-https`, `ca-certificates`, `build-essential`, `linux-headers-generic`, `linux-headers-{{ansible_kernel}}`; unversioned/latest. |
| `env/docker.yml` | `docker-ce` with `state: latest`, from Docker's `edge` apt channel; no version pin. |
| `env/Dockerfile` | `ansible` installed from apt without a version, then removed. |
| `README.md` binary quick-install example | `python`, `python-pip`, `libgraph-easy-perl`; unversioned. |

For Ubuntu versions below 18, `env/build-dep.yml` builds gRPC from source at **`v1.3.2`**, with unversioned `autoconf`, `libtool`, and `cmake`; protobuf and benchmark are built from that gRPC checkout. This is a legacy conditional path, not current CI.

## Container, VM, and external action references

| Component | Explicit reference | Pin status |
|---|---|---|
| CI runner | `ubuntu-24.04` | Release label only. |
| CI actions | `actions/checkout@v7`, `actions/cache@v6` | Major tags; mutable, not commit-SHA pinned. |
| Build-container base | `ubuntu:noble` (`env/Dockerfile`, `env/rebuild_images.py`) | Floating image tag; no digest. |
| Published build container | `nefelinetworks/bess_build:latest` (`container_build.py`) | Floating `latest`; no digest. `rebuild_images.py` also publishes a date-formatted tag. |
| Vagrant box | `bento/ubuntu-18.04` (`env/Vagrantfile`) | Box name without a version. |
| Legacy VM image download | Bento Ubuntu 18.04 VirtualBox box version `202003.31.0` (`bessctl/conf/port/vhost/create_image.sh`) | Explicit box version in the URL. |
| Legacy Vagrant provider | `virtualbox` | No VirtualBox version. |
| Legacy container/VM docs | Ubuntu 18.04 in `env/README.md`; Ubuntu 14.04 in `bessctl/conf/port/vhost/README.md` | Historical instructions; not the supported CI/container base. |

## Historical and observed version references (not current pins)

These explicit version mentions in repository documentation or compatibility code do not constrain the current build unless separately listed above.

- `MODERNIZATION.md` records migration **DPDK 19.11.4 → 25.11.3** and mentions DPDK 20.11+/21.11 behavior. The current authoritative pin is `deps/dpdk.json` at 25.11.3.
- `bessctl/conf/port/vhost/launch_vm.py` still hard-codes `deps/dpdk-19.11.4/build/app/testpmd`; this is stale relative to the current pin.
- `MODERNIZATION.md` records observed toolchains GCC 13.3/15/16, Clang 18.1.3/20, g++ 16, and MSVC 19.44; glibc 2.42+; and observed libraries glog 0.7/0.7.1, protobuf 36, gRPC 1.83, libxdp 1.6.3, and libbpf 1.7.0. It also mentions Python 3.12 compatibility. These are environment observations or compatibility references, not current constraints.
- The same notes record Ubuntu package builds `libxdp-dev 1.4.2-1ubuntu4` and `libbpf-dev 1:1.3.0-2build2`; these are observations from a disposable environment, not apt pins.
- A historical `MODERNIZATION.md` section says “current Ubuntu CI GCC 13.3 / Clang 18.1”, while the current workflow selects `clang-19` and leaves GCC unversioned. Treat that prose as a stale snapshot.
- `core/bessd.cc` and its tests discuss glog `>=0.7` and an observed `google-glog 0.7.1-2`; this is compatibility context, not a Meson minimum.
- `core/utils/endian.h` mentions GCC 4.9+ as an optimization note, not a supported-toolchain declaration.

## Reproducibility summary

1. DPDK is the only major native dependency with an exact source archive and SHA-256 pin.
2. Python requirements are ranges, not a lock; legacy install paths bypass those ranges and install unbounded/latest packages.
3. Most C++ libraries and all APT packages are resolved from OS repositories without version constraints.
4. Container images and the Vagrant box are tag/name based rather than digest/revision pinned.
5. Current CI action references use major tags rather than immutable commit SHAs.
6. No tracked Poetry/Pipenv/npm/Cargo/Go lockfile or equivalent dependency lock was found. `requirements.txt` is the only tracked Python package list.
