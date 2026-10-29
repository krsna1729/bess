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
bessd -f --mem_backing=normal --modules=build
```

`standalone_flow_count` admits its `WorkerFlowTable` through
`init_context().memory()` as `"flows"` before allocating it (D-198).
`standalone_range_gate` admits its immutable range classifier as `"classifier"`,
keeping the classifier by value on the packet path. The tiny, fixed shared
`standalone_appliance` policy remains owned by the instance registry rather than
a module's memory scope: it can outlive the module that first creates it and
has no argument-sized storage.
