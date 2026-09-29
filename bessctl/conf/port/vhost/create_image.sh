#!/bin/bash -e

# Copyright (c) 2014-2016, The Regents of the University of California.
# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

# Build a QEMU guest from Ubuntu's 24.04 cloud image.
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
image_url=https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

curl --fail --location --retry 3 "$image_url" -o "$tmpdir/ubuntu.img"
echo "Converting Ubuntu 24.04 cloud image..."
qemu-img convert -c -O qcow2 "$tmpdir/ubuntu.img" vm.qcow2

ssh-keygen -q -t ed25519 -N '' -f "$tmpdir/vm.key"
cp "$tmpdir/vm.key" vm.key
chmod 400 vm.key
cat > "$tmpdir/user-data" <<EOF
#cloud-config
ssh_authorized_keys:
  - $(<"$tmpdir/vm.key.pub")
EOF
printf 'instance-id: bess-vhost\nlocal-hostname: bess-vhost\n' > "$tmpdir/meta-data"
cloud-localds vm-seed.iso "$tmpdir/user-data" "$tmpdir/meta-data"

echo "Done: vm.qcow2 and vm-seed.iso are ready. Run launch_vm.py from this directory."
