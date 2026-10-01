# Standalone Out-of-Tree BESS Plugin Example

This directory contains a standalone out-of-tree plugin demonstrating how an external project (such as OMEC UPF) builds against an installed `bess-dev` package without vendoring BESS source code.

## Prerequisites
BESS development package installed on the system:
```bash
pkg-config --modversion bess-dev
```

## Build
```bash
meson setup build
meson compile -C build
```

## Run with BESS Daemon
```bash
bessd -f -m 0 --modules=build
```
