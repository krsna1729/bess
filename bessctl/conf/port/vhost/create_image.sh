#!/bin/bash -e

# Copyright (c) 2014-2016, The Regents of the University of California.
# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice, this
# list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
# this list of conditions and the following disclaimer in the documentation
# and/or other materials provided with the distribution.
#
# * Neither the names of the copyright holders nor the names of their
# contributors may be used to endorse or promote products derived from this
# software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

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
