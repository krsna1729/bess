#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 {runtime [requirements.txt]|build|vm}" >&2
  exit 2
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage
fi

mode="$1"
if [[ "$mode" != runtime && "$mode" != build && "$mode" != vm ]] ||
   [[ "$mode" != runtime && $# -eq 2 ]]; then
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

runtime_packages=(
  python3 python3-pip python3-venv
  libnuma1 libpcap0.8t64 zlib1g libunwind8 libbpf1 libxdp1
  libgoogle-glog0v6t64 libgflags2.2 libprotobuf32t64 libgrpc++1.51t64
  libc-ares2 libsystemd0 libcap2 libelf1t64 libarchive13t64 libjansson4
  libgraph-easy-perl tcpdump
)

build_packages=(
  build-essential clang-19 ccache meson ninja-build pkg-config
  python3-pyelftools xz-utils
  libnuma-dev libpcap-dev zlib1g-dev libunwind-dev libbpf-dev libxdp-dev
  libgoogle-glog-dev libgflags-dev libgtest-dev libbenchmark-dev
  libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler
  libsystemd-dev libc-ares-dev libcap-dev
)

vm_packages=(cloud-image-utils curl numactl openssh-client qemu-kvm qemu-utils)

case "$mode" in
  runtime) packages=("${runtime_packages[@]}") ;;
  build) packages=("${build_packages[@]}") ;;
  vm) packages=("${vm_packages[@]}") ;;
esac

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${packages[@]}"

if [[ "$mode" == runtime ]]; then
  script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
  repo_root="$(cd -- "$script_dir/.." && pwd)"
  requirements_file="${2:-$repo_root/requirements.txt}"
  if [[ ! -f "$requirements_file" ]]; then
    echo "Python requirements file not found: $requirements_file" >&2
    exit 1
  fi
  python3 -m pip install --break-system-packages --disable-pip-version-check \
    --no-cache-dir -r "$requirements_file"
fi
