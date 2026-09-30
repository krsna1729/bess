# Core component architecture

This document describes ownership and dependency direction in `core/`. The module-author path stays the same: a module uses the ordinary `ProcessBatch`/`EmitPacket` model and may opt into shared dataplane mechanisms where needed.

## Terms

- **Framework**: the module-facing contracts and the execution mechanics that honor them: module lifecycle, graph/gate model, metadata, packet-batch interface, scheduling, and extension interfaces. The framework is hosted by BESS; it is not the daemon.
- **Runtime**: one live BESS instance. It owns instance registries and runtime services, initializes the process, starts/stops workers, and assembles the framework with control services and extensions.
- **Libraries**: reusable mechanisms consumed by the framework and modules. “Library” describes a reusable component and a build artifact; it is not a separate layer above or below the framework.
- **Modules**: concrete packet-processing behavior built on framework contracts and selected libraries. A module must not depend on another concrete module.
- **Adapters/extensions**: port drivers and concrete gate/resume hooks. Their contracts belong to the framework; their implementations are assembled by the runtime.
- **Control plane**: RPC, desired-state planning, validation, and orchestration of runtime services. It is a runtime subsystem, but remains a distinct component; it does not own module implementations.
- **Utilities**: low-level, cross-cutting helpers. A utility must not depend on module, framework, runtime, or control-plane code. Domain-specific code belongs with its owning component instead.

## Dependency rules

An arrow means “may depend on.”

```text
modules, drivers, hook implementations ──depend on──> framework API + selected libraries/runtime API
framework engine ────────────────uses──> runtime services + packet/dataplane libraries
runtime instance implementation ──uses──> framework Module/Port/worker contracts
runtime host ────────────────────composes──> framework + control + extensions
bessd ───────────────────────────is the composition root for the runtime host
```

More specifically:

1. Dataplane libraries may use low-level support, RCU, and packet/data APIs. They may not include control-plane, daemon, module, or protobuf-RPC implementation headers.
2. Framework code may use packet/dataplane libraries and runtime service APIs, but not concrete modules, drivers, hooks, or control-plane orchestration.
3. Modules, drivers, and hook implementations use framework APIs, required libraries, and only the runtime services exposed through the runtime API. They do not include `control/` implementation headers or one another's implementation headers.
4. Control-plane code uses runtime services and generic dataplane/resource APIs. It does not depend on concrete module implementations.
5. Runtime owns instance state and composes control, framework, built-in modules, drivers, hooks, and the daemon entry point. Runtime services use framework-owned Module/Port/worker contracts, while framework code calls runtime services; the runtime host closes this pair. The daemon is not a dependency of libraries.
6. Meson targets expose only the compile dependencies and include roots each component needs. Component tests link the component under test and its dependencies; full-runtime tests remain explicit integration tests.

## Ownership map

| Component | Owns | Current implementation |
|---|---|---|
| Framework | Module/graph/gate/metadata contracts; packet-batch and port interfaces; scheduler/worker execution; hook and resource-codec contracts | `bess_framework`: framework source group in `core/meson.build`, `framework/resource_codec.h`, `framework/exact_match_table.*`, `route/router.cc`, `stats/worker_histogram.*` |
| Packet library | Packet representation, cursors, checksums, mutation and reshape | `bess_packet`: `packet.cc`, `packet_reshape.cc`, `packet_checksum.cc`, `packet_tx_checksum.cc`; runtime-backed `packet_pool.cc` remains in `bess_framework` |
| Dataplane libraries | RCU, classifier backends, generic resource/object tables and transactions, meters, route-table algorithms, statistics primitives | `bess_rcu`, `bess_dataplane`: `rcu/`, `classifier/`, `dataplane/`, `meter/`, `route/route_table.*`, `stats/counter_set.*` |
| Runtime | Live instance state/registries, worker management, platform initialization, startup, extension registration/loading, daemon composition | `bess_runtime`: runtime-owned sources in `runtime/`; `bess_host`: `bessctl.cc`, `bessd.cc`, `debug.cc`; `main.cc` is the composition root |
| Control plane | RPC services, control semantics, desired-state validation/diff/planning/transactions | `bess_control`: `control/` |
| Extensions | Concrete built-in modules, port drivers, gate hooks, resume hooks | `bess_modules`, `bess_drivers`, `bess_gate_hooks`, `bess_resume_hooks`; no extra `extensions/` parent is required |
| Utilities | Only leaf helpers with no framework/runtime/module dependency | `bess_utils` / `utils/`; framework-specific ExactMatchTable is owned by `framework/` |

These are Meson source/link owners. Core targets use only the `core/` include root; generated Protobuf include roots are added through `bess_proto_headers_dep` only for components that consume generated messages. In particular, `route/route_table.*` is a reusable table library, while `route/router.*` integrates routing with BESS packet and gate APIs; `stats/current_worker.h` and `stats/worker_histogram.*` are framework/runtime adapters, not statistics primitives.

## Boundary status

Runtime state and worker management live under `runtime/`; platform initialization, option definitions, and memory management are built into `bess_runtime`. Modules consume those APIs without depending on `control/`. The typed resource-codec contract and framework exact-match helper live under `framework/`; the control adapter extracts `Any` type/value bytes and delegates typed decoding.

Meson separates `bess_runtime`, `bess_framework`, `bess_packet`, `bess_dataplane`, `bess_control`, `bess_host`, utilities, modules, drivers, and hooks. Component tests link their owning implementation closure; control/daemon tests explicitly link the full runtime. The module-authoring API and packet-processing path remain unchanged.
