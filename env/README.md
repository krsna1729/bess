This directory contains the canonical dependency installer and Dockerfile.
Supported host systems are Ubuntu 24.04 or newer.

## Install dependencies on a host

Run from the repository root:

```sh
sudo bash env/install-deps.sh runtime
sudo bash env/install-deps.sh build
```

The `runtime` mode installs runtime libraries and the Python packages from
`requirements.txt`. The `build` mode adds the compiler, Meson/Ninja, and native
development dependencies. The `vm` mode installs QEMU/KVM, NUMA, and
cloud-image tools for the vhost VM helper:

```sh
sudo bash env/install-deps.sh vm
```

## Docker build environment

`env/Dockerfile` is the canonical definition. Its `runtime` stage installs the
same runtime dependencies as the host script; its final `build` stage adds the
build toolchain. Build either stage from the repository root:

```sh
docker build -f env/Dockerfile --target runtime -t bess-runtime .
docker build -f env/Dockerfile --target build -t bess-build .
```

The published `nefelinetworks/bess_build:latest` image is consumed by
`container_build.py`. That helper mounts the checkout and runs the
checksum-pinned DPDK bootstrap; build output remains in the mounted source
tree. Rebuild the published image with:

```sh
python3 env/rebuild_images.py noble64
```
