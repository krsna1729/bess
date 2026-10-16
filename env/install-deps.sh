#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 {runtime|build|vm}" >&2
  exit 2
}

if [[ $# -ne 1 ]]; then
  usage
fi

mode="$1"
if [[ "$mode" != runtime && "$mode" != build && "$mode" != vm ]]; then
  usage
fi

if [[ $EUID -ne 0 ]]; then
  echo "Run this installer as root (for example, with sudo)." >&2
  exit 1
fi

. /etc/os-release
if [[ "${ID:-}" != ubuntu ]] ||
   ! dpkg --compare-versions "${VERSION_ID:-0}" ge 24.04; then
  echo "BESS dependencies require Ubuntu 24.04 or newer." >&2
  exit 1
fi

# What bessd and bessctl (bessd, D-120) load, and tcpdump for bessctl capture.
# No Python (D-135).
runtime_packages=(
  libnuma1 libpcap0.8t64 zlib1g libunwind8 libbpf1 libxdp1
  libgoogle-glog0v6t64 libgflags2.2 libprotobuf32t64 libgrpc++1.51t64
  libc-ares2 libsystemd0 libcap2 libelf1t64 libarchive13t64 libjansson4 libyaml-0-2
  tcpdump
)

build_packages=(
  build-essential gcc-14 g++-14 clang-19 libclang-rt-19-dev clang-tidy-19 llvm-19 ccache mold meson ninja-build pkg-config
  python3 python3-pyelftools python3-yaml xz-utils
  libnuma-dev libpcap-dev zlib1g-dev libunwind-dev libbpf-dev libxdp-dev
  libgoogle-glog-dev libgflags-dev libgtest-dev libbenchmark-dev
  libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler
  libsystemd-dev libc-ares-dev libcap-dev libyaml-dev
)

vm_packages=(cloud-image-utils curl numactl openssh-client qemu-kvm qemu-utils)

case "$mode" in
  runtime) packages=("${runtime_packages[@]}") ;;
  build) packages=("${build_packages[@]}") ;;
  vm) packages=("${vm_packages[@]}") ;;
esac

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${packages[@]}"
