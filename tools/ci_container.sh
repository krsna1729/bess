#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Run tools/ci_profile.py in the CI toolchain (Ubuntu 24.04, gcc-14, clang-19).
#
#   tools/ci_container.sh gcc            # configure, build and verify as CI does
#   tools/ci_container.sh clang build    # one step
#   tools/ci_container.sh gcc info
#
# Use it when the compilers installed on the host are not the CI versions
# (tools/ci_profile.py info says which). Build trees go to build/ctr-<lane> and
# DPDK to deps/*/install-ctr-<lane>, so they never mix with host builds.
# The container is limited to 8 CPUs and 10 GB of memory.
set -euo pipefail

lane="${1:?usage: $0 gcc|clang [step]}"
step="${2:-all}"
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
image=bess-ci:ubuntu-24.04

docker=(docker)
if ! docker info >/dev/null 2>&1; then
  docker=(sudo -n docker)
fi

# Rebuild the image when its inputs change (a changed layer is a cache hit
# otherwise, so this is cheap).
"${docker[@]}" build -q -t "$image" -f "$root/env/ci.Dockerfile" "$root" >/dev/null

mkdir -p "$root/.scratch/ctr-home"
exec "${docker[@]}" run --rm --cpus 8 --memory 10g \
  --user "$(id -u):$(id -g)" \
  -e HOME=/src/.scratch/ctr-home \
  -v "$root:/src" -w /src \
  "$image" \
  python3 tools/ci_profile.py "$step" --compiler "$lane" --name "ctr-$lane" \
    --jobs 8 --no-ccache
