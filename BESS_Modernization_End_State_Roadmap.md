# BESS Modernization End-State Roadmap

**Repository:** `krsna1729/bess`  
**Branch:** `develop`  
**Plan baseline:** `f4fdab03c10f9f0b39147a59230d2a678e6050a8`  
**Baseline date:** 2026-10-01  
**Status:** implementation roadmap / architecture contract  
**Language baseline:** C++23  
**Primary dataplane substrate:** DPDK 25.11.x  
**Build system:** Meson/Ninja

---

# Navigation

- [0. Non-negotiable outcome](#0-non-negotiable-outcome)
- [1. Executive summary](#1-executive-summary)
- [2. Pareto frontier](#2-pareto-frontier)
- [3. Current-state inventory](#3-current-state-inventory-at-f4fdab03)
- [4. Target architecture](#4-target-architecture)
- [5. Layer contracts](#5-layer-contracts)
- [6. Public API model](#6-public-api-model)
- [7. Performance doctrine](#7-performance-doctrine)
- [8. Correctness and hardening doctrine](#8-correctness-and-hardening-doctrine)
- [9. Efficiency doctrine](#9-efficiency-doctrine)
- [10. Transaction semantics](#10-transaction-semantics)
- [11. Management/control SDK target](#11-managementcontrol-sdk-target)
- [12. Appliance thought-experiment matrix](#12-appliance-thought-experiment-acceptance-matrix)
- [13. Milestone program overview](#13-milestone-program-overview)
- [14. Phase A — architecture enforcement](#14-phase-a--freeze-and-enforce-architecture)
- [15. Phase B — existing-battery correction](#15-phase-b--correct-existing-batteries-before-expansion)
- [16. Phase C — stateful substrate](#16-phase-c--missing-stateful-substrate)
- [17. Phase D — networking batteries](#17-phase-d--networking-batteries)
- [18. Phase E — acceleration and portability](#18-phase-e--acceleration-and-portability)
- [19. Phase F — hardening and product quality](#19-phase-f--hardening-and-product-quality)
- [20–33. Build/API/verification/critical-path plans](#20-proposed-end-state-repositorybuild-layout)
- [Appendices A–L](#appendix-a--current-file-ownership-map)
- [Final architectural statement](#final-architectural-statement)

## 0. Non-negotiable outcome

The modernization is successful only if this remains true:

> **Appliance authors are free to build radically different programming models—VFP-style layered policy, OVS-style translated flow caches, Hoverboard-style hierarchical fast/slow paths, OMEC-style session compilation, or something not yet imagined—without fighting the BESS architecture or first translating their model into a BESS-defined policy model.**

This is the top-level acceptance criterion. Every abstraction, directory, Meson target, public API, control API, transaction semantic, and networking battery must be evaluated against it.

BESS should decide:

- how packets execute efficiently;
- how packet memory is represented and safely mutated;
- how worker-local and shared state is published and reclaimed;
- how stable identifiers map to immutable objects;
- how multi-resource modifications preserve referential correctness;
- how generic classifiers, meters, routing structures, flow state, expiry, handoff, statistics, and networking mechanisms are implemented efficiently;
- how queues, devices, worker topology, RCU, and hardware flow resources are owned;
- how a remote controller safely discovers and updates generic resources.

The application should decide:

- what a session, connection, flow, policy, action, rule, group, layer, tenant, network instance, or intent means;
- how its policy is compiled;
- which decisions are cached;
- which flows are promoted;
- which flows are offloaded;
- what gets punted or resumed;
- how its control model is exposed to its own users;
- which BESS batteries it composes and which it bypasses.

The plan deliberately stops before BESS becomes a universal network-policy language, network operating system, or mandatory graph programming model.

---

# 1. Executive summary

The Pareto-optimal end state is:

> **A small execution/runtime core, a rigorously layered set of reusable high-performance C++ mechanisms and networking libraries, thin graph adapters, and a thin generic management/control SDK.**

The performance standard is stronger than “fast C++”:

> **For every important typed hot path, the reusable C++ abstraction should approach the best assembly we would reasonably hand-write for the target CPU. Runtime genericity is bound at configuration, generation, or batch boundaries rather than interpreted per packet.**

The public product surface is correspondingly small:

1. **BESS public C++ API**  
   A curated in-process API for module authors, appliance authors, and advanced battery authors. There is no separate “dataplane SDK” product. Public packet, classifier, flow, meter, routing, state/lifetime, and framework libraries are simply BESS libraries.

2. **BESS management/control SDK**  
   A thin out-of-process SDK over a stable generic wire protocol. It handles resource discovery, desired state, generations, transactions, idempotency, daemon epochs, timeout recovery, capability discovery, telemetry, and events. It does **not** define PDR/FAR/QER, VFP groups, OpenFlow, firewall policy, VIP policy, or any other appliance programming model.

3. **Graph composition remains first class**  
   Existing `.bess` pipelines and `Module` authors remain supported. Standard modules become thin adapters over the same libraries that specialized appliances call directly.

The modernization should not restart the codebase. Current `develop` already contains strong foundations:

- C++23 and Meson;
- modern DPDK integration;
- packet ownership/mutation/reshape/checksum work;
- RCU and quiescent-state integration;
- strong IDs;
- immutable `ObjectTable`;
- live `SlotTable`;
- generic classifier substrate with exact/masked/range backends;
- concurrent update modes;
- generic metering;
- worker-local statistics;
- LPM routing and next-hop publication;
- resource transactions with dependency-derived publication ordering and removal cascades;
- desired-state pipeline control;
- public transaction RPC semantics;
- external plugin packaging work;
- evidence-driven microbenchmarks and live performance tests.

The next phase is therefore about **locking down architecture before expanding batteries**.

---

# 2. Pareto frontier

The desired position across competing dimensions is:

| Dimension | Target |
|---|---|
| Fast-path performance | Near-specialized assembly for typed C++; runtime flexibility paid outside per-packet inner loops |
| Update performance | Live mutation where safe; rebuild/swap only when strict atomicity or backend semantics require it |
| Correctness | Explicit ownership, strong IDs, RCU lifetime, prepare/publish transactions, bounded reclamation, fail-closed behavior |
| Ergonomics | Easy graph users stay easy; appliance authors can bypass graphs without reimplementing BESS internals |
| Genericity | Generic mechanisms, not generic appliance semantics |
| Memory efficiency | Explicit bytes/item and touched-cache-line budgets for every scalable structure |
| Concurrency | Worker ownership first; shared structures only where workload demands them |
| Layering | Mechanically enforced by Meson targets and include-dependency checks |
| DPDK relationship | Reuse DPDK mechanisms; add lifecycle/ownership/value, not duplicate DPDK layers |
| Public API | Curated C++ libraries + framework contracts; internals not installed by accident |
| Remote control | Generic transaction/desired-state SDK; application intent remains above it |
| Compatibility | Existing modules/graphs continue to work; new paths are additive |
| Portability | Generic baseline + narrow measured ISA specialization |
| Hardware acceleration | Native `rte_flow` semantics with BESS ownership/lifetime/reconciliation |
| Maintainability | One implementation with direct-library, module-adapter, and control-binding faces |

Several tempting alternatives are dominated and should be explicitly rejected:

- a generic “network services” layer;
- a universal `BessAction` object;
- a BESS policy language;
- a separate dataplane SDK product;
- a universal hardware-offload IR layered over `rte_flow`;
- mandatory module graphs for sophisticated applications;
- global runtime service lookup from reusable libraries;
- making every update globally snapshot atomic;
- rebuilding entire tables for ordinary live updates;
- installing all internal headers and calling them an SDK.

---

# 3. Current-state inventory at `f4fdab03`

## 3.1 Strong foundations to preserve

### Build and language

- Meson/Ninja is the supported build graph.
- C++23 is the production language baseline.
- GCC and Clang are both first-class.
- DPDK is externally built and consumed through pkg-config.
- Generated protobufs live in the build tree.
- External plugin packaging and installed headers already exist.

### Packet substrate

The packet modernization has already addressed much of the hard low-level work:

- `PacketRef` and handle semantics around native `rte_mbuf`;
- packet chain cursor/read support;
- explicit payload writeability;
- in-place mutation primitives;
- deep-copy/reshape helpers;
- cross-segment prefix/suffix removal;
- software checksum semantics;
- PMD TX checksum finalization;
- semantic ownership rather than a duplicated packet representation.

This is the correct direction and should remain the base of all higher batteries.

### RCU and publication

Current code has:

- one runtime-owned `RcuDomain`;
- worker registration/online/offline/quiescent reporting;
- `RcuPtr<T>`;
- `SlotTable<Id,T>` for one-object live publication;
- `ObjectTable<Id,T>` for immutable generations;
- deferred reclamation and explicit reader lifetime contracts.

These are core substrate, not routing/classifier-specific features.

### Classifier substrate

Current classifier work already demonstrates the target pattern:

- typed author path;
- runtime-generic schema path;
- exact backends;
- masked/tuple-space backends;
- range backend;
- generation-level backend selection;
- packet extraction/result plans;
- measured backend specialization rather than one universal table.

This should be treated as the template for future batteries such as flow state.

### Generic metering

`core/meter/` already separates:

- profile specification;
- interned immutable profiles;
- mutable meter state;
- worker-exclusive versus shared placement;
- slab allocation;
- published generations.

This is a good model for separating immutable policy from mutable runtime state.

### Worker-local statistics

Current stats work already has:

- typed `WorkerId`;
- worker-local storage;
- grouped updates;
- seqlock-style snapshot semantics;
- generations and epochs;
- controller-side reset baselines.

This should become the standard building block for future batteries rather than each battery inventing counters.

### Routing

K7 established important reusable semantics:

- LPM route table;
- one-writer live updates;
- lock-free reads;
- QSBR/tbl8 reclamation;
- stable `NextHopId`;
- next-hop object publication;
- route-to-next-hop reference correctness.

The concept is strong; the new route-domain work needs consolidation into it rather than becoming a parallel router architecture.

### Resource transactions

The current transaction engine is one of the strongest pieces of the modernization:

- resource registration;
- declared dependency graph;
- `Reserve()` / infallible `Publish()`;
- no visibility on preparation failure;
- reference validation;
- dependency-derived publication order;
- deferred erase/removal cascades;
- RCU lifetime integration;
- allocation-free publication window;
- explicit publication footprints;
- bounded backlog/backpressure;
- teardown/destructor safety;
- failure-injection/model/stress testing;
- idempotent remote transaction semantics layered above it.

This should be generalized carefully, not replaced.

---

## 3.2 Current architectural seams to correct

### A. Meson names layers but does not yet enforce the target DAG

At current HEAD, `bess_dataplane` includes sources from:

- classifier;
- dataplane transaction core;
- meter;
- route;
- stats.

`bess_framework` includes broad implementation classes and links runtime/dataplane internals.

This means the source tree suggests stronger architecture than the linker/build graph actually enforces.

**Consequence:** future `flow/`, `l2/`, `nat/`, or `l3/` code could accidentally include `module.h`, runtime state, protobuf, or control internals and still compile.

### B. `runtime()` is still an author-visible reach-through

Several modules directly obtain:

- the global transaction engine;
- global RCU;
- other runtime state.

This was acceptable during migration but should not become the public extension model.

### C. Resource wire encoding leaks downward

`dataplane::Resource` currently carries optional codec knowledge while the concrete codec understands protobuf.

The transaction/resource core should know only resource semantics. Wire/schema binding belongs above it.

### D. Installed plugin headers expose too much

The current external plugin package is a useful proof that out-of-tree compilation works, but recursive installation of broad internal subtrees risks accidentally freezing implementation headers as API.

### E. Routing is splitting

`route::Router` and `MultiDomainRouter` currently represent two different ownership/control models.

The route-domain container also has a reader/writer synchronization problem if domains can be created while packet readers are active, and its bulk route operation is documented more strongly than its implementation guarantees.

### F. Routing contains graph semantics

Reusable `NextHop` currently contains `gate_idx_t`. That makes the reusable routing library aware of the BESS graph model.

The routing library should terminate in a logical interface/forwarding identifier; the graph adapter should map that to gates.

### G. `ActionTable` risks becoming an accidental universal action model

The current session vertical slice uses an action `{meter_id, next_hop_id}`. That is excellent as a transaction/reference test and useful module, but it must not become the generic BESS action abstraction.

### H. Old NAT/L2/LB modules contain reusable mechanisms trapped inside module code

The current NAT module combines flow lookup, lifetime, endpoint allocation, rewrite, checksum, and graph semantics.

L2 forwarding keeps its table in module-local code.

HashLB contains useful selection concepts but remains module-shaped.

These should be mined later, after the substrate/layer boundaries are enforced.

---

# 4. Target architecture

The target is a dependency DAG, not a “service stack”:

```text
                         application / appliance
          VFP-like / OVS-like / Hoverboard-like / OMEC / LB
                                |
                 application-specific model/compiler
                         /                  \
                        /                    \
           BESS public C++ libraries      control client
                 |                            |
     +-----------+-------------+              |
     |           |             |              |
 classifier    flow/state    networking       |
 meter/stats   cache/expiry  L2/L3/NAT/etc.   |
     \           |             /              |
      \          |            /               |
              dataplane substrate             |
       IDs / objects / RCU / resources        |
       transactions / handoff / lifetime      |
                       |                       |
                  packet substrate             |
                       |                       |
               execution/runtime core          |
       workers / scheduler / memory / ports    |
                       |                       |
                      DPDK                     |
                                               |
                                    generic control API
                                               |
                                    management/control SDK
```

Framework and networking libraries are siblings.

A graph-facing module depends on both:

```text
bess_l3 -----------+
                   +--> Router module
bess_framework ----+
```

A fused appliance may depend on `bess_l3` without depending on `bess_framework`.

---

# 5. Layer contracts

## 5.1 Runtime

Runtime owns execution environment:

- DPDK EAL/process lifecycle;
- NUMA/memory;
- worker threads;
- scheduler ownership;
- device/port lifecycle;
- queue ownership;
- thread placement;
- process-wide RCU lifetime;
- process-wide transaction-engine lifetime.

Runtime must not contain:

- route semantics;
- NAT semantics;
- firewall semantics;
- conntrack semantics;
- VFP/OVS/UPF semantics;
- policy compilation.

Runtime may own the lifetime of generic engines without knowing what their consumers mean.

## 5.2 Packet

Packet owns:

- borrowed and owning packet handles;
- packet batches;
- cursor/read helpers;
- writeability;
- reshape/topology operations;
- checksum semantics;
- packet-only edit mechanics.

Packet should not depend on:

- modules;
- runtime;
- control;
- routing;
- classifier;
- protobuf.

## 5.3 Dataplane substrate

Dataplane substrate owns:

- strong IDs;
- object/slot publication;
- generic resources;
- resource transactions;
- generic batch-stage helpers;
- expiry mechanism;
- handoff mechanism;
- continuation/generation handles;
- generic lifetime primitives.

It must not understand:

- protobuf;
- modules;
- route semantics;
- firewall/session semantics;
- control-RPC details.

## 5.4 Batteries / standard libraries

Examples:

- classifier;
- meter;
- stats;
- flow;
- L2;
- L3;
- NAT;
- tunnel;
- selection.

They may depend on packet/dataplane/RCU and selected DPDK libraries.

They must not depend on framework/runtime/control.

## 5.5 Framework

Framework owns:

- `Module` contract;
- graph integration;
- gate adapters;
- plugin registration;
- initialization capabilities;
- explicit application-instance lookup/binding;
- compatibility metadata mechanisms;
- resource/control registration adapters.

Framework should not implement networking mechanisms.

## 5.6 Modules

Modules are graph adapters and legacy-compatible composition pieces.

They may depend on framework and batteries.

Long-term module implementations should increasingly become thin wrappers.

## 5.7 Control

Control owns:

- desired-state pipeline API;
- resource discovery/binding;
- transaction wire protocol;
- daemon epoch/request-id semantics;
- capabilities;
- telemetry/events;
- RPC transport.

It does not own the application's intent model.

---

# 6. Public API model

There is no separate dataplane SDK product.

There are only two meaningful public surfaces:

## 6.1 BESS public C++ API

For in-process code:

- modules;
- appliance implementations;
- reusable third-party batteries.

It consists of curated public libraries.

A specialized appliance author should be able to use classifier/flow/meter/L3 libraries without depending on `Module`, `Gate`, or `ModuleGraph`.

## 6.2 BESS management/control SDK

For out-of-process controllers:

- `bessctl`;
- OMEC controller integration;
- VFP-like controllers;
- automation/orchestration;
- operations.

It abstracts generic distributed control semantics.

It does not define network intent.

---

# 7. Performance doctrine

## 7.1 Assembly-near typed C++

For hot typed paths, every reusable abstraction must be capable of compiling to the same essential machine-code/data-access pattern as a carefully written specialized implementation.

The optimization ladder is:

```text
ideal machine behavior
       |
specialized reference C++
       |
typed reusable BESS C++
       |
runtime-generic BESS path
```

Rung 2 should approach rung 1.

Rung 3 may have a bounded cost, but that cost should be paid at configuration/generation/batch boundaries where possible.

## 7.2 What “optimal” means

Evaluate:

- dependent-load chain;
- branch predictability;
- cache lines touched;
- stores/coherence traffic;
- fences/atomics;
- register pressure/spills;
- code size;
- memory-level parallelism;
- vectorization;
- working-set behavior;
- NUMA locality.

Instruction count alone is insufficient.

## 7.3 Runtime genericity is bound once

Bad:

```text
for every packet:
    inspect backend kind
    interpret schema
    dispatch implementation
```

Desired:

```text
configuration/generation:
    validate
    compile schema/plan
    choose backend
    bind function/body

packet batch:
    one bound call

inner loop:
    specialized implementation
```

## 7.4 Modern C++ use

Use when it preserves information or removes bugs without runtime tax:

- concepts;
- `if constexpr`;
- `constexpr` / `consteval`;
- strong types;
- `std::span`;
- `std::array` where size is genuinely fixed;
- `std::unique_ptr`;
- `std::expected`;
- `std::chrono` at semantic boundaries;
- `std::bit_cast`;
- `std::byteswap` / endian-safe types;
- RAII;
- policy templates.

Avoid hot-path use without evidence:

- `std::function`;
- `std::shared_ptr`;
- `std::any`;
- repeated `std::variant` visitation;
- exceptions;
- strings;
- generic service lookup;
- allocation.

## 7.5 Data layout is API design

Every scalable hot type needs:

- `sizeof`;
- alignment;
- hot/cold field identification;
- cache-line sharing analysis;
- mutable versus immutable split;
- bytes/item;
- cache lines/lookup;
- high-water memory.

A 32-byte avoidable overhead at ten million flows costs ~320 MB.

## 7.6 Assembly regression artifacts

For a small set of critical kernels, maintain machine-code expectations.

Examples:

- `StrongId` operations should disappear;
- `SlotTable::Lookup` should remain bounds/index/acquire-load/null-test class;
- worker-local counter update should not become atomic RMW;
- typed exact lookup should not gain allocation/type-erasure calls;
- fixed rewrite should not unexpectedly become a generic library call.

Use disassembly, compiler optimization reports, and `llvm-mca` where useful. Do not use brittle exact-byte assembly snapshots.

---

# 8. Correctness and hardening doctrine

Every mutable subsystem should eventually have four classes of evidence:

1. deterministic unit tests;
2. differential/model tests;
3. failure injection at each fallible preparation point;
4. concurrent-reader/writer stress.

For staged changes, on every injected failure compare:

- logical state;
- physical table state;
- reference ledger;
- generation;
- retirement/reclamation backlog;
- ability to retry successfully.

Packet paths fail closed on invalid/unresolved state.

Unsupported strict semantics are rejected rather than silently weakened.

No CPU RCU grace period may be used as proof that an object ID crossing an asynchronous queue or hardware MARK has been consumed.

Assurance is chosen per failure class, at the cheapest level that closes it; claims never exceed the evidence behind them (Appendix L).

---

# 9. Efficiency doctrine

Every scalable battery must report:

- bytes/object;
- bytes/capacity;
- touched bytes;
- lookup cache lines;
- allocation behavior;
- publication footprint;
- retirement backlog;
- update/write amplification;
- CPU cycles/packet or operation;
- throughput under realistic working sets;
- throughput under concurrent updates.

Performance benchmarks and memory-footprint benchmarks are peers.

---

# 10. Transaction semantics

The transaction API must distinguish two meanings commonly conflated under “atomic.”

## 10.1 Referential transaction

Default semantics:

- all fallible preparation occurs before publication;
- missing references are refused;
- erased referents remain alive for in-flight readers;
- publication order follows dependencies;
- no dangling reference is observable;
- failed preparation leaves nothing visible.

Readers may observe safe compatible old/new combinations while multiple resources publish.

This should remain the cheap default.

## 10.2 Scoped snapshot transaction

When an independently identifiable scope exists, strict per-scope snapshot semantics may be offered:

```text
ScopeCell -> immutable ScopeVersion
```

The packet operation loads the scope version once and uses it across all covered lookups.

Examples may include:

- VFP policy group/version;
- independently keyed subscriber/session scope;
- decision generation.

Strict snapshot atomicity should not force full global table rebuilds.

If snapshot semantics cannot be proven because matching scopes overlap or the scope cannot be identified independently, reject the requested operation.

---

# 11. Management/control SDK target

The management/control SDK is justified because the server now has semantics too subtle to expect every controller to implement independently.

It should understand:

- connection and daemon epoch;
- capability discovery;
- pipeline desired state;
- pipeline generations;
- resource discovery;
- typed or schema-bound resource handles;
- generic transactions;
- expected generations;
- request IDs and request digests;
- idempotent retry;
- timeout status recovery;
- `BUSY` versus conflict versus rejected versus unknown-after-restart;
- committed-with-cleanup-pending;
- telemetry;
- event streams.

It should not understand:

- PFCP;
- PDR/FAR/QER;
- VFP layers/groups;
- OpenFlow;
- firewall security policy;
- VIP/backend policy;
- routing protocol policy.

The ideal layering is:

```text
application control model
        |
application SDK/compiler
        |
BESS control SDK
        |
generic BESS gRPC/protobuf protocol
        |
BESS daemon
```

### Control client shape

Conceptually:

```text
Client
  |- Capabilities
  |- Pipeline
  |- Instances
  |- Resources
  |- Transactions
  |- Ports
  |- Telemetry
  `- Events
```

Resource names and wire schema details should be bound once into handles where possible rather than reconstructed for every operation.

Automatic retry must not be the only behavior. The SDK may provide bounded retry policies but preserve application control over ordering/deadlines.

---

# 12. Appliance thought-experiment acceptance matrix

The architecture is healthy only if all of these remain natural:

| Appliance | Application owns | BESS provides |
|---|---|---|
| Ethernet bridge/vSwitch | STP/controller policy | interface IDs, FDB, VLAN/bridge mechanics, flood groups, expiry, counters |
| VRF router | BGP/OSPF/routing policy | FIB, route domains, next hops/groups, adjacency, neighbor state, MTU/TTL/ICMP helpers |
| Stateful firewall | security policy | classifier, flow lifetime, conntrack, expiry, stats |
| NAT gateway | NAT policy | flow state, pools, mappings, rewrite/checksum mechanics |
| L4 load balancer | VIP/backend policy, health strategy | selection algorithms, affinity, NAT/DSR/tunnel mechanisms |
| OVS-like vSwitch | OpenFlow/OVSDB, recirculation/translation semantics | classifier, flow/decision cache, conntrack, punt/resume, compiled packet edits |
| VFP-like vSwitch | layers/groups/resources/policy compiler | generic resources, transactions, flow state, decision cache, packet edit mechanics, offload lifecycle |
| Hoverboard-style host datapath | hotness/promotion/hierarchy policy | flow state, handoff, default-path mechanics, stats, offload lifecycle |
| OMEC UPF | PFCP/PDR/FAR/QER/session compiler | classifier/range, object IDs, meter, flow/session state, route, tunnel mechanics, handoff/offload |
| IDS/DPI/IPsec | inspection/security semantics | affinity, reassembly/handoff mechanics, clone/ownership, acceleration lifecycle |

This matrix should become a long-lived architecture test, not just documentation.

---

# 13. Milestone program overview

The sequence is intentionally dependency ordered.

## Phase A — Freeze and enforce architecture

- M0: architecture contract + baseline;
- M1: Meson component split and forbidden-dependency enforcement;
- M2: curated public headers and package contract;
- M3: module initialization capabilities; reduce global runtime reach-through;
- M4: resource/schema/wire separation;
- M5: explicit application-instance ownership.

## Phase B — Correct existing batteries before expansion

- M6: consolidate route domains into Router;
- M7: introduce logical network identity (`InterfaceId`, etc.);
- M8: explicit transaction consistency levels and scoped snapshots.

## Phase C — Build missing stateful substrate

- M9: generic flow-state substrate;
- M10: expiry/timer substrate;
- M11: handoff/punt/resume;
- M12: decision cache;
- M13: bounded packet edit plan experiment.

## Phase D — Build networking batteries on the substrate

- M14: L2/FDB/bridge extraction;
- M15: L3 neighbor/adjacency/ECMP expansion;
- M16: generic member-selection algorithms;
- M17: conntrack;
- M18: NAT extraction;
- M19: tunnel libraries.

## Phase E — Acceleration and portability

- M20: hardware flow lifecycle/offload seam;
- M21: architecture portability and narrow ISA specialization.

## Phase F — Hardening, SDK and productization

- M22: sanitizer/fuzz/static-analysis expansion;
- M23: ergonomics/conformance tests;
- M24: reference appliances;
- M25: observability;
- M26: release/package/SBOM;
- M27: control SDK maturity.

The first six milestones should be treated as architectural prerequisites for major new batteries.


# 14. Phase A — Freeze and enforce architecture

## M0 — Architecture contract and immutable baseline

### Objective

Create a factual checkpoint before further architectural movement. The purpose is not bureaucracy; it is to make every subsequent change answerable against performance, memory, correctness, API, and dependency baselines.

### Deliverables

1. `docs/architecture.md`
2. `docs/performance-contract.md`
3. machine-readable dependency graph artifact
4. structured benchmark baseline for `f4fdab03`
5. external-plugin compile baseline
6. whole-suite correctness baseline
7. memory-footprint baseline for scalable structures

### `docs/architecture.md` must define

- the target dependency DAG;
- public/internal/experimental API classification;
- forbidden layer edges;
- fast-path invariants;
- control-vs-dataplane responsibilities;
- resource transaction semantics;
- RCU lifetime rules;
- async-handle lifetime rules;
- standard-library battery admission criteria;
- explicit anti-goals.

### Baseline benchmark set

Preserve current benchmark names where possible so history remains comparable.

At minimum capture:

#### Packet

- packet allocation/free;
- cursor fixed-width reads;
- segmented reads;
- writable check;
- in-place prepend/append/remove;
- deep-copy/ensure-writable;
- linearization/contiguous-range;
- checksum calculation;
- TX checksum preparation.

#### State/lifetime

- `RcuPtr` read;
- grace-period latency;
- `SlotTable::Lookup`;
- `SlotTable::Publish`;
- `ObjectTable::Lookup`;
- object-table build;
- transaction single op;
- session-shaped transaction;
- transaction under online readers;
- reclamation backlog behavior.

#### Classifier

Exact:

- small/direct/cuckoo/rte_hash variants where supported;
- typed path;
- runtime-generic path;
- hot/uniform/miss/mixed;
- 64/1K/16K/1M entries where relevant;
- key widths representative of real workloads;
- batch 1/8/16/32.

Masked/range:

- tuple-space sizes;
- wildcard distributions;
- range-count sweep;
- current K3.8 comparison to Cartesian ternary expansion.

#### Meter/stats

- meter hot path;
- shared versus exclusive meter;
- meter generation update;
- worker-local counter single update;
- grouped counter update;
- snapshot cost by workers × counters.

#### Routing

- single route lookup;
- batch route lookup;
- route update;
- next-hop lookup;
- route-domain/VRF sweep;
- route update with readers.

#### Queue/handoff precursor

- current Queue SP/SC and MP/MC configurations;
- latency and burst throughput;
- NUMA-crossing case if available.

### Baseline memory set

Record:

- `sizeof` of public hot types;
- object/slot table bytes/capacity;
- classifier bytes/rule;
- meter bytes/meter;
- stats bytes/worker/counter;
- route bytes/route where measurable;
- transaction scratch high water;
- RCU queued retirement high water.

### Assembly baseline

Generate disassembly or Compiler Explorer-like extracted kernels for:

- `StrongId` conversion/compare;
- `SlotTable::Lookup`;
- worker-local counter `Add`;
- typed exact lookup;
- representative packet field rewrite.

The checked-in artifact should describe expected machine-code **shape**, not exact opcodes.

### Correctness baseline

Capture:

- GCC full suite;
- Clang full suite;
- sanitizer subset;
- module integration suite;
- external sample-plugin build from installed tree;
- source-tree cleanliness after build/install/test.

### Exit criteria

M0 is complete only when:

- the architecture document contains the non-negotiable programming-model principle;
- the baseline can be rerun from one documented command or script;
- baseline results are attributable to exact commit/compiler/CPU/build flags;
- no code-path behavior changes are required to close the milestone.

### Do not do in M0

- do not move directories;
- do not rename public types;
- do not refactor hot loops;
- do not “clean up” benchmark code until baseline capture is complete.

---

## M1 — Meson-enforced dependency architecture

### Objective

Turn conceptual layers into build-enforced layers before adding new batteries.

### Problem at current HEAD

`bess_dataplane` currently collects classifier, meter, route, stats, and transaction sources. `bess_framework` links broadly across runtime and dataplane. The common include root makes forbidden includes easy.

The immediate danger is architectural entropy: new flow/L2/NAT code can accidentally become framework/runtime dependent while still building cleanly.

### Target Meson components

Initial split:

```text
bess_utils
bess_rcu
bess_packet
bess_dataplane_core
bess_classifier
bess_meter
bess_stats
bess_route
bess_execution        # if scheduler/task split is practical now
bess_framework
bess_runtime
bess_control
bess_drivers
bess_modules
bess_gate_hooks
bess_resume_hooks
```

Future components:

```text
bess_flow
bess_l2
bess_l3               # route may evolve/rename here
bess_nat
bess_tunnel
bess_selection
bess_offload
```

Do not force directory renames merely to achieve link separation. Library boundaries are more important than cosmetics.

### Desired dependency rules

At minimum:

```text
bess_packet -> bess_utils / DPDK packet primitives only

bess_dataplane_core -> bess_rcu + minimal utils
bess_classifier -> bess_dataplane_core + bess_rcu + selected DPDK
bess_meter -> bess_dataplane_core + selected DPDK
bess_stats -> bess_dataplane_core
bess_route -> bess_dataplane_core + bess_rcu + bess_packet + selected DPDK

bess_framework -> bess_packet + explicit execution/framework contracts
bess_runtime -> execution/framework + dataplane core + DPDK
bess_control -> runtime/framework + wire dependencies
bess_modules -> framework + batteries
```

### Forbidden dependency classes

The include checker must reject at least:

```text
packet/**       -> framework/**
packet/**       -> runtime/**
packet/**       -> control/**
packet/**       -> pb/**

dataplane/**    -> framework/**
dataplane/**    -> runtime/**
dataplane/**    -> control/**
dataplane/**    -> pb/**

classifier/**   -> module.h
classifier/**   -> runtime/**
classifier/**   -> control/**

meter/**        -> module.h
route/**        -> module.h          # once gate coupling is removed
flow/**         -> module.h
l2/**           -> module.h
l3/**           -> module.h
nat/**          -> module.h
```

Some temporary exceptions may be grandfathered with an allowlist. Every exception must have an issue/milestone owner and target removal phase.

### Include checker

Implement a simple project include graph checker rather than a full C++ parser.

It should:

- parse quoted project includes;
- canonicalize relative paths;
- map source/header path to logical layer;
- reject forbidden edges;
- print the exact edge and rule;
- run in CI;
- include a negative self-test proving it detects an intentionally bad edge.

Prefer simple deterministic tooling over clever static analysis.

### Link checker

Generate the actual Meson target dependency graph and compare it to an allowlisted DAG.

This catches dependencies that enter via link configuration rather than includes.

### Build-header hygiene

Begin moving new public includes toward canonical include syntax:

```cpp
#include <bess/dataplane/strong_id.h>
```

Do not mass-convert all source includes in this milestone unless it remains a mechanical no-codegen change.

Ban new `../..` cross-layer includes.

### Performance safeguard

M1 should be almost entirely build-graph changes.

Required evidence:

- binary code-size comparison;
- symbol/link map comparison where relevant;
- all baseline performance tests smoke-pass;
- no intentionally changed hot code.

### Exit criteria

- each major logical layer is a distinct Meson static library;
- forbidden dependency injection causes CI failure;
- `bess_classifier`, `bess_meter`, `bess_stats`, and `bess_route` no longer disappear inside a monolithic `bess_dataplane`;
- all existing unit/integration tests pass;
- installed external plugin still links successfully.

---

## M2 — Curated public C++ surface

### Objective

Redefine `bess-dev` from “headers sufficient to compile an external module” into “the intentional public C++ contract.”

### API classes

Every header should be classified as:

```text
public
experimental
internal
```

Possible implementation:

```text
include/bess/...           stable/public forwarding headers or source
include/bess/experimental
core/internal/...          never installed
```

Do not depend exclusively on namespace naming; packaging must enforce it.

### Public surface, first cut

Likely public:

```text
bess/framework/
    module.h
    plugin.h
    module_init_context.h
    instance_handle.h       # after M5
    graph-facing contracts

bess/packet/
    packet_ref.h
    packet_handle.h
    batch.h
    cursor.h
    mutation.h
    reshape.h
    checksum.h

bess/dataplane/
    strong_id.h
    object_table.h
    slot_table.h
    selected resource author APIs

bess/classifier/
    typed exact/masked/range APIs
    runtime classifier facade where intended

bess/meter/
    public meter specifications / set APIs

bess/stats/
    public counter/histogram APIs

bess/route or bess/l3/
    public routing types after M6
```

Potentially experimental at first:

```text
flow/
expiry/
handoff/
decision_cache/
offload/
packet/edit_plan
```

Internal:

```text
runtime/runtime_state.h
runtime/worker_manager internals
control/control_plane implementation
daemon startup internals
transaction scratch/internal registration structures
PMD implementation details
private backend-selection internals
tests/bench helpers not intended for consumers
```

### Do not prematurely freeze C++ ABI

Initial promise:

> A documented source API is supported for compatible BESS releases. External plugins are rebuilt against the target BESS release.

A small stable plugin-loader descriptor may use a C ABI.

### Plugin descriptor

Add something conceptually equivalent to:

```cpp
extern "C" const BessPluginDescriptor* bess_plugin_descriptor_v1();
```

Descriptor fields:

- descriptor ABI version;
- plugin name/version;
- required BESS API range;
- required optional capabilities;
- module/driver registration entry points or metadata.

Do not rely only on exported C++ symbols/static constructors for compatibility diagnostics.

### Package dependencies

A simple external module should not be required to pull gRPC simply because BESS's daemon uses gRPC.

Separate public compile dependencies by actual need.

The initial packaging can still be one `bess-dev` package while Meson internally exposes components.

### Conformance tests

Build external projects against **installed artifacts only**, not source-tree include paths:

1. trivial module plugin;
2. packet-manipulation plugin;
3. classifier-using plugin;
4. intentionally invalid plugin including an internal header.

The fourth must fail.

### Exit criteria

- recursive installation of internal subtrees is removed or tightly filtered;
- source-tree internal headers are not accidentally available in installed-plugin CI;
- external sample plugin remains ergonomic;
- API documentation labels stability level;
- C++ ABI stability is not overpromised.

---

## M3 — Initialization capabilities instead of global runtime reach-through

### Objective

Prevent `runtime::runtime()` from becoming the public dependency-injection model.

### Desired principle

Packet processing should never perform capability lookup.

Initialization/control may obtain explicit narrow capabilities once.

### Initial API shape

Prefer an explicit context:

```cpp
class ModuleInitContext {
 public:
  dataplane::ResourceRegistrar& resources() noexcept;
  InstanceRegistry& instances() noexcept;
  PortDirectory& ports() noexcept;
  const WorkerTopology& workers() const noexcept;

  // Advanced capabilities only if justified:
  rcu::RcuDomain& rcu() noexcept;
};
```

Do not use a fully generic:

```cpp
ctx.Get<ServiceT>();
```

for core facilities.

Explicit methods make dependencies visible in code review and allow future capability narrowing.

### Backward compatibility

Do not break every existing `Init(const Proto&)`.

A staged migration can:

1. set an init context on `Module` before `InitWithGenericArg`;
2. expose `init_context()` to new/migrated modules;
3. retain old signatures;
4. migrate internal modules incrementally;
5. deprecate direct global runtime access in public examples;
6. eventually forbid direct runtime includes from external/public framework code.

### First migration set

Move these first because they exercise resource/lifetime needs:

- `ActionTable`;
- `Meter`;
- `Router`;
- Exact/Wildcard resource registration paths.

### Performance

Zero packet-path impact is mandatory.

The context is resolved during construction/init only.

### Lifetime

Use references/non-null handles for context-owned capabilities.

Avoid `shared_ptr` simply to represent process-owned facilities.

### Exit criteria

- first transaction-aware modules no longer call `runtime::runtime()` directly;
- existing module source compatibility remains;
- no extra packet-path state lookup or indirection;
- forbidden-dependency checker can begin rejecting new direct runtime includes from selected public areas.

---

## M4 — Decouple resource semantics from wire/schema encoding

### Objective

Make the dataplane resource core independently usable without protobuf or an RPC concept.

### Current problem

A `Resource` may carry codec knowledge, while concrete typed codecs know protobuf message descriptors and serialized wire values.

### Target split

Dataplane:

```text
Resource
  name/identity
  declared references
  Reserve
  Publish staged operation
  Contains/References
  lifecycle
```

Binding/control:

```text
ResourceBinding
  Resource&
  optional external schema
  discovery metadata
  wire codec
```

Possible API:

```cpp
class ResourceSchema {
 public:
  virtual ~ResourceSchema() = default;

  virtual TypeDescriptor key_type() const = 0;
  virtual TypeDescriptor value_type() const = 0;

  virtual std::expected<ResourceKey, DecodeError>
  DecodeKey(TypeDescriptor, std::span<const std::byte>) const = 0;

  virtual std::expected<std::any, DecodeError>
  DecodeValue(TypeDescriptor, std::span<const std::byte>) const = 0;
};
```

The exact type-erased control-side representation may evolve. The important point is dependency direction.

Provide protobuf helpers above the generic schema:

```text
control/protobuf_resource_schema.h
```

or equivalent.

### Do not over-optimize `std::any` yet

Current `std::any` is control-side. Keep it until profiling says it materially limits target update rates.

The architectural fix is more important than replacing every type erasure mechanism immediately.

### Wire compatibility

RPC behavior and existing type URLs should remain compatible through the migration.

### Build proof

`bess_dataplane_core` must be buildable without:

- protobuf headers;
- gRPC;
- module headers.

### Exit criteria

- `Resource` no longer owns/wants an RPC codec;
- resource registration/control binding associates schema externally;
- current transaction RPC remains behaviorally compatible;
- dataplane-core library dependency graph proves protobuf-free construction.

---

## M5 — Explicit application-instance ownership

### Objective

Give sophisticated applications a clean way to share one application-owned object graph across multiple modules without global implicit services.

### Why needed

A vSwitch may need:

```text
VSwitchInstance
  |- policy compiler
  |- flow cache
  |- conntrack
  |- bridge/FIB
  `- offload state
```

A UPF may need:

```text
UpfInstance
  |- PDR classifier
  |- sessions
  |- application actions
  |- meters
  `- routing
```

These must not become runtime-global BESS “services.”

### Current `SharedObjectSpace`

Treat as legacy compatibility:

- global;
- implicit creation;
- `shared_ptr` based;
- type/name lookup.

Do not immediately delete it.

### New semantics

Provide explicit structural lifecycle:

```cpp
auto result = instances.Create<UpfInstance>("upf0", args...);
auto handle = instances.Lookup<UpfInstance>("upf0");
instances.Destroy("upf0");
```

Requirements:

- lookup never implicitly creates;
- duplicate create is explicit error;
- destroy is explicit;
- type mismatch is explicit error;
- lifecycle is observable/introspectable;
- packet path does not look up by string/name;
- packet path does not increment ownership refs.

### First lifetime model

Creation/destruction is a structural operation requiring appropriate quiescence or pipeline transition.

Contents may update live.

This is deliberately conservative.

### Packet/module usage

During initialization:

```cpp
auto instance = init_context().instances().Require<MyInstance>("name");
instance_ = instance.get(); // stable borrowed pointer under structural lifetime
```

The exact handle API should encode the structural lifetime contract.

### Control API

Management control may create/destroy application instances generically only if the application/plugin supplies a type/schema/constructor binding.

Do not create a universal instance-configuration language.

### Exit criteria

- at least one reference example shares state across two module instances;
- packet hot path performs zero registry lookup/refcount work;
- instance destruction while in use is rejected or safely structurally synchronized;
- `SharedObjectSpace` remains functional for compatibility but new examples use explicit instances.

---

# 15. Phase B — Correct existing batteries before expansion

## M6 — Consolidate route domains into the existing Router model

### Objective

Prevent K7.1 from becoming a second routing architecture.

### Required end state

One routing object owns both:

- route domains/FIBs;
- next-hop/forwarding objects.

Conceptually:

```text
Router
  |- domains
  |    |- d0 -> FIB
  |    |- d1 -> FIB
  |    `- dN -> FIB
  |
  `- next_hops / forwarding objects
```

Route identity:

```cpp
struct RouteKey {
  RouteDomainId domain;
  Ipv4Prefix prefix;
};
```

### Domain reader structure

Benchmark at least:

1. dense indexed `SlotTable<RouteDomainId, Domain>`;
2. immutable flat vector generation;
3. RCU-published sparse flat representation if domain IDs are sparse.

Do not use unsynchronized `std::map` lookup while writers may mutate the map.

If domain creation is structurally frozen after activation instead, encode and enforce that invariant explicitly. Do not rely on comments.

### Route update modes

Support different semantics explicitly.

#### Ordinary live route update

```text
SetRoute / RemoveRoute
```

- one-writer;
- live in-place backend update where safe;
- no FIB rebuild;
- fast control-path latency.

#### Strict atomic route-set replacement

```text
ReplaceRouteSetAtomic
```

- build/prepare a replacement FIB generation;
- validate all capacity/tbl8 requirements before publish;
- one version/pointer publication;
- retire old generation.

Do not label sequential visible upserts “atomic replacement.”

### Remove graph semantics

Replace `gate_idx_t` from reusable next-hop data.

Introduce or prepare for:

```cpp
InterfaceId interface;
```

or a more generic forwarding endpoint identifier if measurements/design show it is required.

The graph adapter maps interface/forwarding endpoint to an output gate.

### Preserve direct specialized use

A fused appliance must be able to:

```cpp
auto nh = router.Lookup(domain, ipv4);
```

without `Module`, metadata, or gates.

### Transaction integration

Routes and next hops remain generic resources with reference correctness.

Route-domain identity becomes part of the resource key or route resource organization without introducing one resource per domain unless measurement/ergonomics justify it.

### Performance gates

Current route-domain benchmark at the baseline reports ~1.69–2.41 ns/op in its measured sweep. Preserve that class of lookup performance.

Required matrix:

- 1/4/16/64 domains;
- hot single domain;
- batch containing one domain;
- mixed domains if the intended API supports it;
- small/large FIB;
- lookup during route updates;
- domain creation/removal lifecycle if supported live.

### Correctness gates

- overlapping prefixes across domains;
- default route per domain;
- unknown domain fail-closed;
- concurrent readers with domain updates;
- strict replacement failure leaves old generation;
- tbl8/capacity failure injection;
- no early next-hop ID reuse.

### Exit criteria

`MultiDomainRouter` as a separate architectural model disappears or becomes a compatibility wrapper over the unified Router.

---

## M7 — Logical network identities

### Objective

Remove accidental coupling between networking libraries and graph/device implementation types.

### Initial strong IDs

Introduce, as demanded:

```text
InterfaceId
BridgeDomainId
RouteDomainId
NeighborId
NextHopId
NextHopGroupId
FlowId
ContinuationId
```

Not every ID needs a full subsystem immediately.

### Representation requirements

For IDs that may enter packet metadata or hardware marks:

- explicit representation width;
- explicit invalid value;
- trivially copyable;
- no implicit cross-ID conversion;
- equality/order/hash only where semantically needed.

Static assertions:

```cpp
static_assert(sizeof(InterfaceId) == sizeof(uint32_t));
static_assert(std::is_trivially_copyable_v<InterfaceId>);
```

### `InterfaceId`

This is the most important cross-library identity.

Networking libraries may use it to mean a logical forwarding endpoint without knowing whether it ultimately maps to:

- PMD port/queue;
- vhost endpoint;
- representor;
- graph gate;
- tunnel endpoint;
- another internal datapath continuation.

The adapter/application supplies the mapping.

### Async handles

For IDs that can cross asynchronous queues or hardware:

```cpp
template <typename Id>
struct GenerationHandle {
  Id id;
  uint32_t generation;
};
```

Do not force this wrapper on purely synchronous RCU readers.

Use it where the lifetime model requires it.

### Exit criteria

- routing no longer needs `gate_idx_t` in its reusable object model;
- IDs remain zero-cost in assembly checks;
- no ambiguous raw integers are introduced for new cross-battery identities.

---

## M8 — Explicit transaction consistency semantics

### Objective

Prevent “atomic” from becoming an overloaded or misleading API promise.

### Public terminology

Use unambiguous names, for example:

```text
referential transaction
snapshot-scoped transaction
```

Avoid promising a globally simultaneous visibility point unless there is one.

### Referential transaction contract

Document observable states precisely.

Guarantee:

- all referenced objects exist when a visible referrer can name them;
- dependency order prevents dangling new references;
- deferred erasure prevents dangling old references;
- failure before publication is invisible;
- readers may observe compatible mixed generations during multi-resource publication.

### Snapshot scope

Add only when required by a real consumer.

A scope must satisfy:

- packet can identify scope independently of the mutable rules being replaced;
- all covered lookups can bind to one scope version;
- overlapping external rules cannot change winner semantics outside the version;
- mutable state such as meter tokens/counters remains separately owned and referenced rather than copied.

### Scope cell

Concept:

```cpp
class ScopeCell {
 public:
  const ScopeVersion* Read() const noexcept;
};
```

The packet operation reads it once.

### Performance

Snapshot mode should add a version/pointer load per scope, not a version check per table operation.

Referential mode must retain current lookup cost.

### API behavior

If the requested strict snapshot semantics cannot be guaranteed:

- reject explicitly;
- return a reason;
- never downgrade silently to referential consistency.

### Acceptance examples

Prove with at least two very different shapes:

1. session-scoped policy update;
2. VFP-like policy-group version.

The example application owns what “session” or “group” means.

### Exit criteria

- API/docs clearly distinguish semantics;
- adversarial reader interleavings enumerate allowed observations;
- model tests prove no observation outside contract;
- no default packet-path regression for existing referential resources.


# 16. Phase C — Missing stateful substrate

## M9 — Generic flow-state substrate

### Objective

Create the reusable state-lifetime primitive demanded independently by firewall, NAT, load balancer, VFP, OVS, Hoverboard, UPF, IDS/DPI, and other stateful appliances.

Do **not** start with TCP conntrack semantics.

The first milestone is generic state ownership and lookup.

### Core requirements

A flow substrate should support:

- typed key;
- typed application-owned state;
- stable optional `FlowId`;
- create;
- lookup;
- erase;
- ownership model;
- optional secondary/bidirectional aliases;
- integration with expiry;
- safe destruction/reuse;
- counters/hooks without forcing them on all users;
- clear control versus worker mutation semantics.

### Do not define “flow” semantically

BESS should not require five-tuples.

Valid keys may include:

- five tuple;
- tunnel key;
- subscriber/session key;
- VFP cached-policy key;
- L2 MAC/domain key;
- arbitrary appliance-defined trivially copyable key.

### Two ownership modes

The API must not hide fundamentally different concurrency costs.

#### Worker-owned state

Preferred where packet steering provides affinity.

Properties:

- table/shard owned by one worker;
- mutable `State` requires no inter-worker atomic operations;
- expiry runs on owner;
- state update is ordinary load/store;
- no coherence traffic for normal state mutation.

Possible shape:

```cpp
template <FlowKey Key,
          typename State,
          typename Hash = DefaultHash<Key>,
          typename Equal = std::equal_to<Key>>
class WorkerFlowTable;
```

#### Shared-directory/shared-state modes

Some workloads need shared lookup.

Separate questions:

1. shared directory, worker-owned state;
2. shared directory and shared mutable state.

Do not conflate them.

Reuse `rte_hash`/existing concurrent exact substrate where appropriate.

Shared mutable state should require explicit atomic/locking/application policy rather than silently adding atomics to all `State`.

### Flow key concept

A typed fast path may define:

```cpp
template <typename T>
concept FixedFlowKey =
    std::is_trivially_copyable_v<T>;
```

Do not require `has_unique_object_representations` universally because useful types may contain padding. Instead:

- either require a canonical byte representation;
- or require author-supplied hash/equality;
- or provide a helper trait/concept for byte-hashable keys.

Avoid hidden padding bugs.

### API semantics

Possible minimal API:

```cpp
auto* state = table.Find(key);

auto created = table.Emplace(key, args...);

bool erased = table.Erase(key);
```

For lifetime-safe ID resolution:

```cpp
FlowId id = ...;
State* state = table.Lookup(id);
```

Only provide APIs whose lifetime semantics can be stated precisely.

### Hot-path constraints

- no allocation on `Find`;
- no type erasure;
- no `std::function`;
- no string/resource lookup;
- no virtual call per packet;
- hash/equality inline for typed path;
- batch prefetch/staging allowed where measured;
- no hidden atomics in worker-owned mode.

### Capacity

Construction must make capacity/load semantics explicit.

Avoid resize-on-hot-path unless a separate dynamic table design is deliberately chosen.

For high-performance deployments, predictable fixed/reserved capacity is acceptable and often preferable.

### Backend strategy

Do not create a BESS hash implementation by default.

Evaluate:

- existing `ConcurrentExactTable`;
- DPDK `rte_hash`;
- current CuckooMap where specialized behavior matters;
- direct/small representations for tiny tables.

Use the classifier backend work as precedent.

### Flow IDs

Do not require every flow to have a `FlowId` if key-only lookup is sufficient.

If IDs are enabled:

- stable while live;
- generation protection for async escape;
- reuse policy explicit;
- no ABA after queued/hardware references.

### Secondary aliases

A generic alias mechanism is useful for bidirectional state:

```text
forward key ----\
                 -> one state
reverse key ----/
```

But do not bake TCP direction semantics into the generic table.

Measure whether aliases should be:

- two table entries to one ID;
- primary table + secondary index;
- application-managed second table.

Prefer the simplest measured representation.

### Benchmark matrix

Capacities/working sets:

- 1K;
- 64K;
- 1M;
- 10M where memory permits.

Key sizes:

- 8 B;
- 16 B;
- representative 5-tuple;
- 32 B;
- one larger realistic appliance key.

Value sizes:

- 16 B;
- 32 B;
- 64 B;
- 128 B;
- indirect larger state.

Access distributions:

- one hot flow;
- Zipf-like hot set;
- uniform hit;
- miss;
- 50/50 hit/miss.

Batch:

- 1;
- 8;
- 16;
- 32.

Mutation:

- lookup only;
- 1% churn;
- 10% churn;
- create/remove flat out.

Concurrency:

- worker local;
- 2/4/8 shared readers;
- one shared writer;
- reader+writer.

Measure:

- ns/lookup;
- aggregate M lookups/s;
- cycles/op;
- bytes/flow;
- cache misses;
- create/delete rate;
- writer interference;
- reclamation backlog;
- tail latency for create/delete if meaningful.

### Assembly targets

For worker-owned typed lookup:

- hash;
- bucket/index loads;
- equality;
- direct state address/ID resolve.

The typed wrapper should not add generic dispatch over the chosen backend.

### Correctness tests

- capacity exhaustion;
- duplicate create;
- erase/not found;
- ID reuse;
- alias lifetime;
- async generation handle;
- random model;
- concurrent lookup/delete;
- stalled RCU reader where used;
- failure during control-side allocation;
- worker ownership violation diagnostics.

### Exit criteria

- one worker-owned implementation with near-specialized C++ codegen;
- one viable shared lookup model;
- explicit bytes/flow numbers;
- state semantics independent of conntrack/NAT;
- no dependency on framework/runtime/control;
- at least three reference applications can express their state without additional core table implementation.

---

## M10 — Expiry and timed-state substrate

### Objective

Provide bounded, efficient lifetime scheduling for flow state, FDB entries, NAT bindings, conntrack, neighbor state, and cached decisions.

### Do not assume a timer wheel up front

Evaluate:

- DPDK timer facilities;
- hierarchical/timing wheel;
- bucketed worker-local expiry queue;
- lazy expiration;
- hybrid approaches.

Different state classes have different needs.

### Required semantic API

At a minimum:

```cpp
ExpiryHandle Schedule(Deadline when, ObjectId id);
void Refresh(ExpiryHandle&, Deadline when);
void Cancel(ExpiryHandle&);
size_t Poll(TimePoint now, size_t budget);
```

Or an equivalent representation where the owner embeds timer links.

### Performance principles

- worker-local timing should be lock-free from other workers;
- refresh of a hot flow should be cheap;
- `Poll` must have an explicit work budget;
- an expiration storm must not monopolize a worker;
- no per-timer heap allocation if avoidable;
- callback polymorphism should not dominate record size.

Prefer expiry records that contain an ID/index and let the owner perform semantic destruction.

### Time representation

Public control/config APIs may use `std::chrono`.

Hot state may use:

- TSC;
- converted monotonic ticks;
- wheel epochs.

Conversion policy must be explicit and tested for wrap/monotonicity.

### Accuracy classes

Not every state requires nanosecond precision.

Consider exposing coarse expiration classes:

```text
sub-millisecond
millisecond
second-scale
```

only if a real consumer benefits and the API remains simple.

Do not over-generalize early.

### Benchmark matrix

Record counts:

- 100K;
- 1M;
- 10M.

Distributions:

- all expire around same time;
- uniform over 1 second;
- uniform over 5 minutes;
- very long idle;
- heavy refresh;
- hot subset constantly refreshed.

Operations:

- schedule;
- refresh;
- cancel;
- poll with none ready;
- poll with budget ready;
- expiration storm.

Measure:

- bytes/timer;
- cycles/refresh;
- cycles/poll;
- worst budgeted poll duration;
- cache misses;
- throughput impact on a packet loop.

### Correctness tests

- refresh supersedes old deadline;
- cancellation;
- reuse of expired ID;
- stale timer record cannot expire a newer generation;
- time wrap where relevant;
- budget preserves eventual progress;
- random reference model.

### Exit criteria

- chosen mechanism justified by benchmark, not preference;
- generation/stale-record safety proven;
- bounded polling behavior;
- usable by flow, FDB, neighbor, and decision-cache batteries without semantic coupling.

---

## M11 — Handoff, punt, and resume

### Objective

Turn fast-path-to-slow-path transfer into a first-class generic mechanism without defining what “slow path” means.

Consumers include:

- OVS policy miss;
- VFP policy/flow miss;
- Hoverboard cold/default path;
- DPI;
- crypto;
- reassembly;
- neighbor resolution;
- UPF buffering;
- custom accelerator service threads.

### Reuse queue mechanisms

Build on DPDK ring capabilities and existing BESS queue measurements.

Do not create another general queue algorithm.

### Semantic distinction from ordinary Queue module

A handoff carries:

- packet ownership;
- bounded queue semantics;
- optional application context;
- optional continuation/resume handle;
- lifecycle/drain rules;
- topology/backpressure counters.

### Typed descriptor

Avoid `void*`/`std::any` for application context.

Possible API:

```cpp
template <typename Context>
class HandoffChannel {
 public:
  std::expected<void, HandoffError>
  TryPunt(PacketHandle packet, Context context);

  size_t Dequeue(std::span<PuntItem<Context>> out);
};
```

Requirements for `Context` may initially be:

- trivially copyable;
- bounded size.

Applications that need larger context can carry a generation-safe ID.

### Ownership

After successful `TryPunt`, producer no longer owns the packet.

On failure, API must make ownership unambiguous:

- failure returns packet;
- or no ownership transfer occurred.

Prefer type/API shape that makes double-free difficult.

### Continuation

A continuation must not be a raw graph pointer or reusable integer with CPU-only RCU assumptions.

Use:

```text
ContinuationId + generation
```

or another explicit lifetime token.

### Backpressure

Supported policies should be explicit and minimal:

- fail/drop;
- caller handles retry/alternate path.

Avoid hidden blocking in a worker hot path.

### Topology modes

At creation, bind:

- SP/SC;
- MP/SC;
- MP/MC as required.

Do not inspect topology per enqueue.

### NUMA

Expose creation placement and diagnostics.

Cross-NUMA handoff should be measurable rather than silently treated like local handoff.

### Resume

Do not define application semantics.

BESS should provide mechanism:

```text
resume token -> safe target
```

Application decides what state/context the token means.

### Benchmarks

Compare against current Queue/ring baseline:

- burst 1/8/16/32;
- SP/SC;
- MP/SC;
- same NUMA;
- cross NUMA;
- full/empty behavior;
- producer+consumer balanced;
- producer outruns consumer.

Measure:

- packets/s;
- ns/item;
- drop/failure cost;
- cache-line traffic;
- bytes/item;
- effect on producer packet loop.

### Correctness

- exact packet ownership accounting;
- queue destroy with queued packets;
- drain semantics;
- stale continuation generation;
- consumer crash/teardown policy;
- full queue;
- repeated create/destroy;
- random transfer/free stress under ASan/TSan where feasible.

### Exit criteria

- SP/SC equivalent path does not regress against optimized queue baseline without documented reason;
- packet ownership is mechanically clear;
- continuation reuse is safe;
- no application-specific miss reason is part of core.

---

## M12 — Decision cache

### Objective

Enable OVS/VFP-style “rich policy compiles to cheap flow decision” without defining the rich policy.

### Concept

```text
FlowKey
   |
DecisionCache
   |
DecisionId / application decision handle
```

Cache entry also carries enough validity metadata to detect stale decisions.

### First invalidation model

Use coarse generation invalidation:

```text
entry.policy_generation != current_generation -> miss
```

This gives O(1) global/group invalidation and avoids walking the cache.

Do not start with OVS-style dependency-aware wildcard invalidation.

### Typed generic API

```cpp
template <typename Key, typename DecisionId>
class DecisionCache;
```

An application defines `DecisionId`.

BESS must not require `ActionId`.

### Integration

On hit:

```text
lookup -> DecisionId -> application-owned immutable decision
```

On miss:

- return miss to application;
- optionally punt via M11;
- application compiles;
- application installs decision/cache entry.

### Lifetime

Decision ID lifetime must be coordinated with cache entries.

If entries are synchronous worker-local state, RCU may suffice for decision objects.

If cache IDs escape through handoff/hardware, use generation-safe handles.

### Performance target

Hit path should approach:

- one flow lookup;
- validity/generation comparison;
- one decision-object resolve.

No generic policy interpreter.

### Benchmark

- 64K / 1M / larger caches;
- hot/uniform;
- hit/miss ratios;
- invalid generation ratio;
- cache churn;
- decision object sizes;
- worker-local/shared variants where needed.

### Reference consumers

Build small tests with two deliberately different decision types:

```cpp
struct VfpDecision { ... };
struct OvsDecision { ... };
```

No common semantic fields beyond what the cache needs.

### Exit criteria

- cache remains semantically agnostic;
- generation invalidation is O(1);
- hit path codegen documented;
- miss integration does not require graph modules.

---

## M13 — Bounded packet edit-plan experiment

### Objective

Test whether a reusable packet-only compiled edit representation can reduce repeated parse/mutate/checksum work for dynamic policy engines without becoming a universal BESS action bytecode.

### Strict scope

Allowed concepts:

- remove prefix/suffix;
- prepend/append bytes;
- write fixed field;
- copy bytes/field;
- ensure writable/contiguous;
- set checksum intent;
- update known length/checksum fields where safely expressible.

Forbidden concepts:

- meter;
- route;
- firewall allow/drop policy;
- OpenFlow goto;
- VFP layer;
- PDR/FAR/QER;
- queueing policy;
- backend selection;
- control-flow language.

### Two execution paths

#### Typed compiled action

Application authors may write normal C++ structs/functions and should never be forced through the generic plan.

#### Runtime edit plan

Dynamic compilers may produce:

```text
small bounded sequence of packet edit ops
```

which is executed without heap allocation or virtual dispatch per op.

### Representation experiment

Compare:

1. variant/tagged operations;
2. compact opcode + fixed payload;
3. pre-bound function sequence;
4. generated specialized closure/function where practical.

Judge by code size, branch cost, memory footprint, and ability to fuse edits.

### Optimization opportunities

At compile/build-plan time:

- coalesce adjacent writes;
- precompute offsets;
- precompute checksum deltas;
- combine prepend material;
- reject impossible topology/headroom assumptions;
- derive required writable range once.

Do not execute redundant validation per packet if the plan and packet parse contract already prove a property.

### Benchmark

Cases:

- NAT IPv4/port rewrite;
- VXLAN encap/decap;
- representative VFP-like multi-field rewrite;
- UPF-like outer-header remove/add;
- no-op/one-write plan.

Compare:

```text
hand-written specialized C++
typed application action
runtime EditPlan
```

### Decision gate

If the runtime plan is materially slower and cannot be improved without becoming complex, keep it experimental and opt-in.

Do not make existing typed rewrites go through it.

### Exit criteria

M13 may close with either:

- a useful experimental/public plan;
- or a documented “not adopted” result.

Evidence, not feature count, is the goal.

---

# 17. Phase D — Networking batteries

## M14 — L2 bridge/FDB library

### Objective

Extract reusable L2 forwarding state from module-local implementation into a neutral library.

### Core types

```cpp
BridgeDomainId
InterfaceId

struct FdbKey {
  BridgeDomainId domain;
  MacAddress mac;
};

struct FdbEntry {
  InterfaceId interface;
  FdbFlags flags;
  ...
};
```

### Functions

- exact destination lookup;
- static entry programming;
- dynamic learning helper;
- expiry/aging integration;
- flood-group resolution;
- optional VLAN membership helper.

### Policy boundary

BESS supplies mechanism, not:

- STP;
- EVPN control policy;
- learning-security policy;
- controller intent.

Applications may disable learning entirely and program the FDB from a controller.

### Backend

Do not simply rename/move old `l2_table`.

Use its benchmark/correctness history as one candidate and semantic oracle.

Compare:

- existing specialized table;
- generic exact/flow substrate;
- any compact MAC-specialized representation justified by measurements.

A dedicated MAC table is acceptable if its performance/memory advantage is real and API remains neutral.

### Graph compatibility

Reimplement existing L2Forward behavior as a thin module adapter.

Preserve existing protobuf/config semantics during first migration.

### Performance

Before/after:

- small table;
- full 4-way table;
- 64K/1M;
- hot/uniform;
- hit/miss;
- dynamic learning churn.

Preserve the fixes already made to the legacy table.

### Correctness

- duplicate MAC;
- alternate bucket/move correctness;
- concurrent readers;
- learning/aging;
- static entry not aged;
- bridge-domain isolation;
- flood on unknown;
- invalid interface fail-closed.

### Exit criteria

- reusable L2 library has no `Module`/gate dependency;
- legacy module behavior is differential-tested;
- no unexplained throughput regression;
- aging uses generic expiry rather than module-specific timer machinery.

---

## M15 — L3 forwarding batteries

### Objective

Extend the corrected Router into a reusable L3 mechanism set without becoming a routing daemon.

### Components

Incrementally add:

- route domain;
- IPv4 FIB;
- eventually IPv6 FIB;
- forwarding/next-hop object;
- next-hop group;
- adjacency;
- neighbor state;
- interface binding;
- MTU;
- TTL/hop-limit handling;
- ICMP exception helpers.

### Separate FIB from adjacency

Desired resolution model:

```text
destination + RouteDomain
        |
       FIB
        |
 ForwardingId / NextHopGroupId
        |
 adjacency/next-hop resolution
        |
 InterfaceId + L2 rewrite state
```

This lets:

- many routes share forwarding objects;
- neighbor change avoid rewriting routes;
- ECMP group membership change independently;
- controller-programmed and ARP/ND-resolved neighbors share the same dataplane representation.

### Neighbor table

Neighbor state is a reusable mechanism.

Example states:

```text
incomplete
resolved
unreachable
```

Do not force ARP/ND protocol execution on all users.

Provide separately:

- neighbor state table;
- packet helpers for ARP/ND;
- optional resolver modules later.

### MTU and exceptions

The L3 library should expose enough information/mechanics for an application to choose:

- drop;
- punt;
- fragment if supported later;
- emit ICMP.

Do not hardwire one policy into lookup.

### IPv6

Do not block the architecture milestone on full IPv6 parity if the codebase lacks a ready high-performance backend.

Design IDs/adjacency/neighbor abstractions so IPv6 can enter without redoing ownership.

### Exit criteria

- graph-independent routing library;
- neighbor changes are object updates rather than route rebuilds;
- ECMP-ready forwarding object model;
- no BGP/OSPF policy enters core.

---

## M16 — Generic member-selection algorithms

### Objective

Share low-level selection algorithms between ECMP, load balancing, and other group-based choices without pretending those domains have the same semantics.

### Provide algorithms, not one universal group service

Candidate reusable pieces:

- stable hash range mapping;
- weighted choice;
- consistent hash;
- rendezvous hash if justified;
- round robin for control/low-contention uses.

### Typed API

Prefer free functions/policy objects:

```cpp
auto member = ConsistentHashSelector{}(group, key_hash);
```

over deep inheritance.

### Compile-time versus runtime

Known selector:

- template/policy type;
- fully inline.

Runtime-configurable selector:

- bind one function pointer or compact enum switch per batch/group operation;
- not per candidate member.

### State

Round-robin mutable cursors have different concurrency semantics from pure hash selection. Keep them explicit.

### Benchmark

- group sizes 2/4/8/32/128;
- weighted/unweighted;
- churn impact for consistent hashing;
- distribution quality;
- cycles/selection;
- memory/group.

### Exit criteria

- ECMP and LB can reuse algorithms;
- neither is forced into a common high-level object model.

---

## M17 — Conntrack networking library

### Objective

Build protocol-aware bidirectional connection state on top of M9/M10.

### Scope

- normalized IPv4/IPv6 tuple abstractions;
- forward/reverse direction mapping;
- TCP connection-state machine;
- UDP pseudo-state/timeouts;
- ICMP association where justified;
- timeout policy hooks/defaults;
- state query/update.

### Out of scope

- firewall rule semantics;
- NAT policy;
- VFP/OpenFlow conntrack action syntax;
- application authorization policy.

### Packet safety

Use checked packet/cursor APIs.

Do not replicate old module assumptions such as:

- fixed Ethernet header;
- untagged IPv4;
- contiguous headers without validation.

Fast paths may use a parsed packet descriptor once parsing/contiguity has been proven.

### Parsed packet descriptor

Consider a reusable packet parse result:

```cpp
struct ParsedFlowPacket {
  L3Kind l3;
  L4Kind l4;
  offsets...
  canonical tuple...
};
```

but keep it narrowly scoped and benchmark stack/register pressure.

Do not create a giant universal packet metadata object.

### State machine tests

- SYN/SYN-ACK/ACK;
- retransmit;
- FIN/RST;
- simultaneous close;
- invalid sequences;
- UDP create/refresh/expire;
- ICMP cases;
- forward/reverse consistency.

Use trace-driven tests and property/model tests.

### Performance

Benchmark:

- lookup + no state transition;
- common TCP transition;
- reverse direction;
- state refresh;
- expiry;
- malformed packet rejection.

### Exit criteria

- conntrack is a library over generic flow/expiry;
- firewall/NAT applications can consume it independently;
- no policy semantics embedded.

---

## M18 — NAT extraction

### Objective

Replace the monolithic legacy NAT implementation with reusable mapping/allocation/rewrite mechanisms built over generic flow and expiry.

### Decompose current responsibilities

Legacy NAT currently combines:

- packet parsing;
- endpoint key definition;
- mapping table;
- reverse mapping;
- address selection;
- port-range allocation;
- timeout;
- rewrite;
- incremental checksum update;
- graph direction/gates.

Extract:

```text
nat::Endpoint / tuple helpers
nat::AddressPool
nat::PortPool
nat::Binding
nat::BindingTable
nat::Allocator
nat::Translator
```

Exact naming may differ.

### Mapping state

Prefer one logical binding object referenced by forward/reverse aliases rather than unrelated duplicate state where it improves lifetime correctness.

Use generic flow aliases if they prove efficient.

### Port allocation

Benchmark:

- random start + probing;
- bitmap/hierarchical bitmap;
- per-worker partition;
- application-specified ranges.

The best allocator may depend on pool size/concurrency. Avoid one global lock.

### Rewrite

Reuse packet mutation/checksum helpers or M13 edit plan if proven beneficial.

The direct typed translator must remain available.

### Semantic compatibility

Treat current module behavior/RFC choices as the initial compatibility oracle unless intentionally changed with explicit documentation.

### Migration

1. build library;
2. differential unit tests versus current NAT;
3. module adapter uses new library behind same config;
4. before/after benchmark;
5. remove old private table only after parity.

### Exit criteria

- NAT library contains no gate/module dependency;
- packet safety improves;
- flow expiry is generic;
- existing NAT module remains compatible;
- memory/flow and allocation behavior are documented.

---

## M19 — Tunnel packet libraries

### Objective

Provide reusable encapsulation/decapsulation mechanics without importing application control semantics.

### Generic tunnel mechanics

Useful common pieces:

- outer Ethernet/IP/UDP header construction;
- encapsulation size/MTU accounting;
- checksum intent;
- decapsulation validation;
- metadata/identifier extraction;
- packet topology/headroom handling.

### Protocol helpers

Candidates:

- VXLAN;
- Geneve;
- GRE.

Use existing packet-format utilities where appropriate.

### GTP-U

Keep UPF-specific semantics outside generic BESS initially.

A packet codec/helper for GTP-U may become reusable, but:

- PDU session;
- QFI;
- PFCP linkage;
- FAR behavior

do not belong in BESS core.

### Performance

Compare:

- current VXLAN modules;
- direct packet library calls;
- edit-plan implementation if M13 adopted.

### Exit criteria

- standard graph modules become adapters;
- specialized appliance can call encap/decap directly;
- protocol semantics stay narrow.


# 18. Phase E — Acceleration and portability

## M20 — Hardware flow lifecycle and offload seam

### Objective

Allow radically different applications to compile their own decisions to NIC flow rules while BESS supplies safe resource ownership, asynchronous lifecycle, and reconciliation.

### Non-goal

Do **not** introduce a BESS hardware-flow IR that mirrors or partially reimplements `rte_flow`.

The application/compiler may use:

- native `rte_flow` pattern/actions;
- small BESS helpers for common safe construction;
- future vendor-specific extensions when explicitly requested.

### BESS responsibilities

BESS should provide a generic owner around hardware rules:

```text
FlowRuleHandle
FlowInstallRequest
FlowInstallCompletion
FlowRuleStats
FlowRuleOwner
```

Responsibilities:

- port/queue association;
- lifetime;
- asynchronous install completion;
- asynchronous failure reporting;
- batch creation/destruction;
- teardown/drain;
- statistics/query;
- generation-safe MARK/continuation mapping;
- reconciliation after reset/device disruption;
- bounded outstanding requests;
- backpressure;
- capability reporting.

### Application responsibilities

Application decides:

- which flow is worth offloading;
- the policy meaning;
- the match/action compilation;
- promotion/demotion policy;
- fallback behavior;
- whether software and hardware decisions are semantically equivalent.

### Completion semantics

Do not treat “rule handle returned” as “hardware has successfully installed rule” when using async APIs.

Expose explicit states:

```text
preparing
submitted
installed
failed
removing
removed
unknown/device-reset
```

### MARK/metadata identity

Any ID carried back from hardware may outlive CPU RCU observations.

Use:

- generation-tagged handles;
- explicit rule deletion completion;
- queue/device drain where required.

Do not reuse an ID while old hardware packets can still arrive carrying it.

### Capability discovery

Expose actual per-port/device capability, not guessed generic booleans.

Examples:

- supported item/action types;
- queue template support;
- async flow support;
- count/action statistics;
- transfer/switch-domain behavior;
- tunnel matching/encap support;
- mark width/metadata width.

Preserve native DPDK-specific escape hatches for advanced compilers.

### Software fallback

Every offloaded decision must have a defined software fallback path unless the application explicitly chooses otherwise.

BESS should help retain consistent lifetime between software decision and hardware rule but not dictate policy equivalence.

### Testing without hardware

Hardware absence must not block software architecture work.

Provide:

- fake/mock flow backend;
- lifecycle state-machine tests;
- injected async completion/failure;
- queue/reset simulation;
- capability fixtures.

Keep a separate real-hardware certification matrix.

### Real-hardware matrix

At minimum cover representative PMD families when lab availability permits:

- Intel;
- NVIDIA/Mellanox;
- virtio/representor environments where relevant.

Use actual PMD docs/source and datasheet semantics to validate inner/outer field interpretations and checksum/tunnel behavior.

### Exit criteria

- hardware lifecycle is application-policy-neutral;
- stale MARK/continuation reuse is impossible by contract;
- no BESS flow IR duplicates DPDK;
- software-only CI covers lifecycle;
- hardware lab remains a separate non-blocking gate until release certification.

---

## M21 — Portability and narrow ISA specialization

### Objective

Make x86 and ARM64 first-class without giving up measured ISA-specific wins.

### Source organization

Introduce or clarify:

```text
arch/
  generic/
  x86/
  arm64/
```

or equivalent implementation boundaries.

Do not scatter new architecture preprocessor branches through networking libraries.

### Baseline versus specialization

Production baseline should remain explicit.

For x86:

- portable deployment baseline such as `x86-64-v3` where policy permits;
- native builds for characterization.

When native materially outperforms baseline:

1. identify the CPU feature causing the gain;
2. isolate the kernel;
3. runtime-dispatch once;
4. preserve generic fallback.

### SIMD

Do not force project-wide C++26 merely for `std::simd`.

Use current available mechanisms where measured:

- compiler vectorization;
- intrinsics in narrow arch files;
- experimental SIMD if maintainable.

Migrate when compiler/toolchain support makes C++26 `std::simd` practical.

### Endianness/alignment

Continue removing undefined/architecture-assumed access patterns.

Use:

- endian-safe wrapper types;
- `memcpy`/`bit_cast` where required;
- explicit alignment contracts;
- checked parser APIs.

### Performance CI

Where CI hardware permits, maintain at least:

- one x86 build/test lane;
- one ARM64 build/test lane.

Microarchitectural benchmark comparability across hosted CI is poor; functional/compile portability is the CI gate, while controlled benchmark machines remain performance authority.

### Exit criteria

- generic libraries do not contain casual `#ifdef __x86_64__` growth;
- ARM64 compiles/runs supported suite;
- x86 specialization is isolated and benchmark-justified;
- no silent ISA dependence leaks from DPDK pkg-config flags.

---

# 19. Phase F — Hardening and product quality

## M22 — Sanitizer, fuzz, static-analysis and fault-injection program

### Objective

Scale the transaction engine's adversarial engineering discipline to the rest of the modern dataplane.

### Sanitizers

#### ASan + UBSan

Run broadly on unit/component tests that do not depend on unsupported EAL VA configurations.

Maintain explicit exclusions with reasons rather than silently dropping sanitizer coverage.

#### TSan

Use targeted harnesses for:

- RCU;
- transaction engine;
- flow table;
- expiry;
- FDB publication;
- neighbor publication;
- handoff lifecycle;
- application-instance lifecycle if concurrent;
- offload completion bookkeeping.

A full DPDK daemon under TSan is not required if the relevant concurrency logic is isolated in testable components.

### Fuzz targets

Prioritize parsing and state machines:

- `PacketCursor`;
- packet mutation lengths/topology;
- checksum preparation;
- classifier runtime schema;
- resource wire/schema decoder;
- route prefix parser;
- conntrack packet/state input;
- NAT packet parser/translator;
- tunnel decapsulation;
- control transaction decoder.

### Static analysis

Adopt a curated `clang-tidy` profile.

High-value groups:

- `bugprone-*`;
- selected `performance-*`;
- selected CERT integer/bounds checks;
- narrowing/conversion checks;
- lifetime/use-after-move;
- virtual/destructor correctness;
- suspicious `memcpy`/aliasing patterns.

Do not mass-apply readability/style checks that create churn without reducing risk.

### Fault injection framework

Generalize deterministic “fail Nth allocation/preparation” utilities used by transaction tests.

Target:

- flow creation;
- expiry record allocation;
- FDB/neighbor updates;
- NAT binding creation;
- route generation replacement;
- handoff creation;
- offload submission.

### Property/model testing

Every stateful battery should have a small reference model.

Examples:

- FlowTable key→state model;
- FDB key→interface model + aging;
- NAT forward/reverse binding model;
- conntrack state model;
- decision cache generation model.

### Exit criteria

No new stateful subsystem is called stable without:

- deterministic tests;
- model/differential coverage;
- fault injection;
- concurrency stress where shared.

---

## M23 — Ergonomics and API conformance

### Objective

Make usability a testable engineering property rather than a documentation afterthought.

### Author personas

Maintain concrete conformance examples for:

#### A. Graph user

Can build standard datapath without C++:

```text
ExactMatch -> Meter -> Router -> PortOut
```

#### B. Simple external module author

Can implement:

```cpp
class Foo final : public Module {
  void ProcessBatch(Context*, PacketBatch*) override;
};
```

without runtime/control internals.

#### C. Specialized module author

Can use public classifier/L3/flow libraries while still integrating into graph.

#### D. Appliance author

Can build an application-owned object graph using public BESS libraries without `Module`.

#### E. Battery author

Can implement a reusable third-party library using intentionally public low-level primitives without internal includes.

### API review tests

For each major public battery, require:

- smallest meaningful code sample;
- typed example;
- runtime-generic example if supported;
- ownership/lifetime example;
- update example;
- error handling example.

### Compile-time negative tests

Where feasible test that:

- wrong strong ID types do not convert;
- unavailable APIs cannot be called on inappropriate ownership modes;
- internal headers are absent from installed SDK;
- non-supported packet-context types fail concepts with clear diagnostics.

### Exit criteria

Every stable public API has at least one external installed-tree compile test.

---

## M24 — Reference appliances as architecture/SDK conformance tests

### Objective

Use small but realistic appliances to prove BESS is mechanism-oriented rather than secretly shaped around one use case.

These are not production replacements for OVS/VFP/OMEC.

They are architecture tests.

---

### R1 — Router appliance

#### Application-owned concerns

- static control policy for example;
- interface-to-device mapping.

#### BESS batteries

- interface IDs;
- route domains/FIB;
- next-hop groups;
- neighbor state;
- rewrite;
- stats.

#### Must demonstrate

- direct C++ path with no module graph;
- equivalent graph-adapter path;
- overlapping VRFs;
- ECMP;
- neighbor update without route rebuild.

---

### R2 — Stateful NAT appliance

#### Application-owned concerns

- pool configuration/policy.

#### BESS batteries

- packet parser;
- flow state;
- expiry;
- conntrack where chosen;
- NAT bindings;
- rewrite/checksum;
- stats.

#### Must demonstrate

- worker-owned flow path;
- reverse alias;
- timed expiration;
- no NAT-specific mechanism hidden in runtime/framework.

---

### R3 — Policy vSwitch

This is the most important neutrality test.

Define application-owned concepts deliberately unlike OMEC:

```text
Layer
Group
Rule
CompiledDecision
```

#### Fast path

```text
packet
  -> parse/key
  -> DecisionCache
      -> hit -> CompiledDecision -> execute
      -> miss -> punt/compiler -> install -> resume/fallback
```

#### BESS batteries

- runtime classifier if needed by compiler;
- decision cache;
- immutable decision objects;
- flow state;
- handoff;
- packet edits;
- routing/L2 pieces optionally;
- offload lifecycle optionally.

#### Must demonstrate

- BESS does not know what Layer/Group means;
- policy compiler can atomically switch a scoped group generation;
- cached decision invalidation is generation based;
- module graph is optional.

---

### R4 — Session datapath

Define application-owned concepts:

```text
Session
PdrLikeRule
SessionAction
QoSPolicy
```

Avoid naming it PFCP in generic example if unnecessary.

#### Fast path

```text
classify -> application decision -> meter -> routing/tunnel
```

#### Control path

One generic BESS transaction updates:

- classification resource;
- application decision resource;
- meter;
- route/next hop.

#### Must demonstrate

- application's action type is not `BessAction`;
- transaction references work across application and BESS resources;
- direct fused path and graph vertical slice remain possible.

---

### R5 — Hoverboard-style hierarchical mode

May be a mode of R3 rather than a separate binary.

Demonstrate:

```text
cold/unknown flow
  -> default/punt path
  -> application decides hotness
  -> installs fast decision
  -> optionally installs hardware flow
```

BESS must not define the hotness/promotion algorithm.

---

### Conformance criterion

A new core abstraction that makes one reference appliance significantly easier but makes another adopt unnatural concepts is suspect and requires review.

---

## M25 — Observability

### Objective

Expose generic operational state without contaminating packet loops with exporter-specific logic.

### Metrics

Libraries expose:

- `CounterSet`;
- histograms;
- snapshots;
- resource-specific schemas.

Exporter translates to:

- CLI;
- gRPC telemetry;
- Prometheus or other systems.

Do not embed Prometheus objects into dataplane libraries.

### Events

Control/event stream candidates:

- transaction commit/reject/busy;
- resource lifecycle;
- RCU/backlog pressure;
- flow-table capacity pressure;
- expiry backlog;
- punt queue full;
- neighbor state transition;
- offload install/failure/reset;
- port capability/state changes.

Events are control-plane observations, not per-packet logs.

### Tracing

Evaluate DPDK tracing/telemetry before creating duplicate low-level tracing machinery.

### Exit criteria

- packet-path counters remain worker-local/cheap;
- exporter can be disabled without changing hot layout substantially;
- operationally important bounded-backpressure conditions are observable.

---

## M26 — Release, packaging, SBOM and reproducibility

### Objective

Turn the modernized architecture into a supportable product boundary.

### Release artifacts

Target as appropriate:

- source release;
- OCI/container;
- distro package(s);
- standalone daemon package;
- development headers/package;
- generated language control clients.

### Metadata

Include:

- BESS version/commit;
- plugin API version;
- compiler and version;
- C++ standard;
- DPDK version and source checksum;
- build ISA;
- enabled optional features;
- linked library versions;
- SBOM.

### Reproducibility

Build from a clean checkout using documented inputs.

CI should install to staging and compile an external consumer from staging.

### Compatibility

Document:

- wire protocol compatibility;
- public C++ source compatibility policy;
- plugin compatibility;
- experimental APIs;
- deprecated APIs.

### Exit criteria

No release requires source-tree include paths or generated source artifacts to be committed.

---

## M27 — Management/control SDK maturity

### Objective

Provide a small, safe client layer for the generic control semantics already present in the daemon.

### Why it exists

Without an SDK, every controller would need to correctly reimplement:

- request ID generation;
- request digest semantics;
- daemon epochs;
- expected generations;
- idempotent retries;
- timeout recovery;
- transaction status lookup;
- busy/conflict/rejected distinctions;
- resource discovery;
- serialization/schema binding.

That duplication is high-risk and offers no appliance differentiation.

### Wire protocol is primary compatibility boundary

Keep generic protobuf/gRPC protocol self-describing enough for unsupported languages to implement clients directly.

The SDK is a convenience/correctness layer.

### Language priorities

Prioritize real consumers.

Likely:

- Python for `bessctl`, testing, operations;
- Go if OMEC/controller consumers require it;
- C++ where useful for native orchestration/tests.

Do not hand-maintain elaborate SDKs in many languages without consumers.

### Shared semantic specification

Define language-neutral behavior for:

```text
Client
Capabilities
Pipeline
Instances
Resources
Transactions
Ports
Telemetry
Events
```

Each language binding should feel idiomatic.

### Resource binding

Raw protocol may identify resources by name and serialized schema.

SDK should support bind-once handles:

```text
discover -> validate schema -> ResourceHandle -> typed operations
```

Repeated operations should not require application code to rebuild resource-name/type-url boilerplate.

### Transaction result model

Preserve distinctions:

```text
committed
committed_cleanup_pending
busy
conflict
rejected
unknown_after_restart
transport_error
```

A transport timeout is not automatically transaction failure.

### Retry helpers

Provide optional policies:

- no retry;
- bounded retry on `BUSY`;
- transport retry with same request ID;
- status query after timeout.

Do not automatically retry semantic conflict/rejection.

### Desired-state pipeline

SDK may provide builders for pipeline specs and snapshots.

Do not duplicate server planner logic.

### Capability discovery

Expose:

- daemon API version;
- available resource types;
- optional batteries/plugins;
- port/device capabilities;
- offload capabilities.

Application decides what to do with capabilities.

### Exit criteria

- at least one real controller migration uses SDK;
- raw generated stubs remain usable;
- no appliance-specific semantic type enters generic SDK;
- timeout/restart/idempotency behavior has black-box integration tests.

---

# 20. Proposed end-state repository/build layout

Exact directory names may evolve. Dependency direction matters more.

```text
core/
  packet/
    packet_ref.*
    packet_handle.*
    batch.*
    cursor.*
    mutation.*
    reshape.*
    checksum.*
    edit_plan.*              # only if M13 adopted

  rcu/
    rcu_domain.*
    rcu_ptr.*

  dataplane/
    strong_id.*
    object_table.*
    slot_table.*
    resource.*
    transaction_engine.*
    batch_stages.*
    expiry.*                 # generic mechanism
    handoff.*
    continuation.*

  classifier/
    public facades
    exact/masked/range backends
    runtime schema/plans
    internal backend implementations

  flow/
    flow_table.*
    alias/index.*
    decision_cache.*
    conntrack.*              # protocol-aware networking library may be split

  meter/
    ...

  stats/
    ...

  l2/
    interface.*
    fdb.*
    bridge.*
    vlan.*

  l3/
    route_domain.*
    fib.*
    next_hop.*
    next_hop_group.*
    adjacency.*
    neighbor.*
    icmp_helpers.*

  selection/
    hash_range.*
    consistent_hash.*
    weighted.*
    round_robin.*

  nat/
    address_pool.*
    port_pool.*
    binding.*
    translator.*

  tunnel/
    common.*
    vxlan.*
    geneve.*
    gre.*

  offload/
    flow_owner.*
    flow_capabilities.*
    continuation_map.*

  execution/
    task.*
    scheduler.*
    traffic_class.*
    worker-facing execution contracts

  framework/
    module.*
    graph.*
    gate.*
    plugin.*
    init_context.*
    instance_registry.*
    resource_binding.*
    compatibility metadata

  runtime/
    runtime_state.*
    worker_manager.*
    memory.*
    dpdk.*
    startup.*
    device/port ownership

  control/
    desired-state pipeline
    resource discovery
    transaction RPC adapter
    telemetry/events
    protobuf schemas/adapters

  modules/
    thin adapters / compatibility modules

  drivers/
    PMD / AF_XDP / pcap / virtual devices

  internal/
    non-public helpers where appropriate
```

Do not mechanically move every existing file to match this picture. Use it to guide ownership as code is touched.

---

# 21. Proposed Meson target graph

A more enforceable end-state graph:

```text
bess_utils
   ^
   |
bess_packet

bess_rcu
   ^
   |
bess_dataplane_core
   |      |       |
   |      |       +------------------+
   |      |                          |
   v      v                          v
classifier  meter  stats  flow  l2  l3  selection  nat  tunnel
       \      |      |      |    |   |       |      |     /
        \     |      |      |    |   |       |      |    /
         +---------------- public batteries -------------+

execution/contracts -----------------------------+
                                                 |
packet/dataplane/batteries ---> framework -------+
                                                 |
framework + execution + dataplane ---> runtime
                                                 |
runtime + framework + control bindings ---> control
                                                 |
framework + selected batteries ----------> modules
```

### Enforcement detail

For every target define:

- allowed project link dependencies;
- allowed include-layer dependencies;
- public headers;
- private headers.

CI prints the graph and rejects new cycles.

---

# 22. Public-header and package policy

## 22.1 Stable public

Examples after maturity:

```text
bess/framework/module.h
bess/framework/plugin.h
bess/framework/init_context.h

bess/packet/packet_ref.h
bess/packet/packet_handle.h
bess/packet/batch.h
bess/packet/cursor.h
bess/packet/mutation.h
bess/packet/reshape.h

bess/dataplane/strong_id.h
bess/dataplane/object_table.h
bess/dataplane/slot_table.h

bess/classifier/...
bess/meter/...
bess/stats/...
bess/l2/...
bess/l3/...
```

## 22.2 Experimental

Initially:

```text
bess/experimental/flow/...
bess/experimental/handoff/...
bess/experimental/offload/...
```

Promote only after conformance/performance/lifetime semantics stabilize.

## 22.3 Internal

Never installed as supported API:

```text
RuntimeState internals
WorkerManager internals
ControlPlane internals
daemon startup
transaction scratch structs
private backend representations
PMD private objects
test hooks
```

### Umbrella package

One `bess-dev` package is sufficient initially.

The internal build can remain highly componentized.

Do not multiply distro packages unless real deployment needs justify it.

---

# 23. Performance verification program

## 23.1 Four performance gates

### Gate A — kernel/microbenchmark

Any change touching a hot primitive runs its targeted microbenchmarks.

### Gate B — realistic pipeline

Run composite paths:

- exact classification;
- classifier → decision;
- classifier → decision → meter;
- decision → route;
- stateful NAT;
- policy cache hit;
- punt/resume;
- offload mark resolve where possible.

### Gate C — update-under-load

For mutable structures:

- packet throughput with no updates;
- nominal update rate;
- target high update rate;
- writer throughput;
- update p50/p99;
- reclamation backlog;
- busy/backpressure frequency.

### Gate D — memory

Track:

- bytes/item;
- high-water allocation;
- touched bytes/cache lines;
- retirement backlog;
- queue occupancy memory.

## 23.2 Regression thresholds

Do not use a single simplistic threshold for all benchmarks.

Suggested policy:

- >3% stable median regression in a deterministic hot microbenchmark requires explanation/investigation;
- noisy benchmarks use paired ABBA or enough samples for confidence;
- a regression may be accepted only if there is a documented Pareto gain elsewhere (correctness, memory, generality) and no cheaper alternative;
- typed zero-cost APIs are held to stricter standards than runtime-generic APIs.

## 23.3 Assembly/codegen gate

For selected kernels, compare:

- load/store count;
- calls;
- indirect branches;
- atomics;
- fences;
- spills;
- code size;
- `llvm-mca` throughput/critical path where informative.

The abstraction should “disappear” when type/config information is static.

## 23.4 Working-set discipline

Every table benchmark should clearly distinguish:

- hot/L1-ish;
- L2;
- LLC;
- memory-resident;
- pre-generated fixed batch versus changing working set.

Avoid misleading fixed 4K-key samples that accidentally remain cache resident.

## 23.5 CPU isolation

Continue the existing evidence-driven discipline:

- isolate benchmark threads;
- ensure helper/readers are on distinct intended CPUs;
- report CPU topology;
- use paired comparisons to distinguish code change from machine drift.

---

# 24. Memory verification program

For every new table/state battery include a benchmark/report with:

```text
configured capacity
live occupancy
storage_bytes
bytes/live item
bytes/capacity item
touched bytes/op
allocation count during build/update
retirement high water
```

At least one sparse-occupancy point must be included.

For flow/NAT/conntrack, estimate real deployment memory at:

- 1M;
- 10M;
- 50M where arithmetic is meaningful.

Do not optimize lookup throughput by silently multiplying memory.

---

# 25. Correctness verification program

## 25.1 Invariants common to stateful resources

- no dangling reference;
- no premature ID reuse;
- no publication failure after visible commit begins;
- no unbounded blocking under internal lock;
- no destructor after owning plugin unload;
- no lost update due to reset/snapshot;
- no stale async handle resolving to a new object;
- fail-closed on unknown/unresolved identifiers.

## 25.2 Failure injection

Inject at each:

- allocation;
- backend reserve;
- table insertion reservation;
- object construction;
- schema decode;
- handoff channel creation;
- route/FIB generation build;
- hardware flow submission.

If operation rejects, visible state must match pre-operation model.

## 25.3 Concurrency

Tests should deliberately pin readers/writers and stall quiescence.

Include:

- writer flat out;
- reader never quiesces for bounded interval;
- multiple readers;
- teardown during outstanding retirement;
- async queue not drained;
- hardware completion delayed in mock backend.

---

# 26. Ergonomic verification program

For every stable public facility ask:

### Simple use

Can the obvious case be written without learning internal BESS machinery?

### Specialized use

Can an expert control ownership/backend/layout without forking implementation?

### Zero-cost typed use

Can the compiler see key widths/types and inline the hot body?

### Runtime-defined use

Can a generic module/controller configure equivalent behavior without per-packet schema interpretation?

### Error model

Does the API distinguish expected operational failure from programmer bug?

### Lifetime

Can a caller tell from API/documentation how long every returned pointer/span/handle is valid?

### Composition

Can it be used without `Module` if it is meant to be a battery?

---

# 27. Management/control SDK detailed contract

## 27.1 Stable wire concepts

Wire protocol should expose generic objects:

- daemon metadata/epoch;
- capabilities;
- pipeline spec/snapshot/generation;
- ports;
- plugin/application instance discovery where supported;
- resources/schema;
- transactions;
- operation results;
- telemetry/events.

## 27.2 Request identity

SDK creates a request ID before first submission.

Transport retries reuse the same request ID and request digest.

Reusing request ID with different contents is an error.

## 27.3 Timeout recovery

On transport timeout:

1. retry same request ID when transport semantics make sense; or
2. query transaction status.

If daemon epoch changed and status cannot be known, return explicit unknown-after-restart.

Never convert that into a false “failed, safe to retry as new.”

## 27.4 Resource handles

A bound resource handle should cache:

- resource identity;
- key schema;
- value schema;
- capability flags;
- daemon epoch/binding validity.

On reconnect/epoch change, handles either rebind or report invalidation explicitly.

## 27.5 Errors

Avoid one generic exception/error string.

Expose machine-readable classes plus diagnostic message.

Examples:

```text
TransportError
Busy
Conflict
Rejected
SchemaMismatch
UnknownResource
UnsupportedCapability
UnknownAfterRestart
```

## 27.6 Application-specific wrappers

Encourage applications to layer their own SDK:

```text
OMEC Session API
  -> BESS generic transaction SDK

VFP Group API
  -> BESS generic transaction SDK
```

Do not merge those application wrappers into generic BESS.

---

# 28. Compatibility and migration policy

## 28.1 Existing modules

Do not remove or force immediate rewrite of:

- `Module`;
- `ProcessBatch`;
- metadata attributes;
- gates;
- `.bess`;
- existing commands.

Modern modules can become thin wrappers gradually.

## 28.2 Deprecation policy

Deprecate only when:

1. replacement exists;
2. equivalent internal consumer migrated;
3. external sample demonstrates migration;
4. performance is no worse without accepted rationale;
5. at least one release/documented transition period.

## 28.3 Internal compatibility

Internal headers have no compatibility promise once removed from installed SDK.

This freedom is necessary to continue optimizing layout.

## 28.4 Plugin unloading

No plugin may unload while:

- its module instances exist;
- its resources are registered;
- RCU-retired objects with plugin destructors remain;
- handoff items reference plugin code/types;
- async offload completion callbacks reference plugin code.

Track these explicitly.

---

# 29. Risk register

## R1 — Over-generalizing flow state

**Risk:** designing a universal flow engine before real consumers.

**Mitigation:** worker-owned typed table first; add shared/alias features only with reference-appliance consumers and benchmarks.

## R2 — Recreating VPP/OVS internally

**Risk:** “batteries included” becomes a network stack or policy engine.

**Mitigation:** battery admission matrix; reference appliances own programming model.

## R3 — Framework becomes service locator

**Risk:** `InitContext`/instances evolve into arbitrary global dependency lookup.

**Mitigation:** explicit core capabilities, explicit instance lifecycle, no per-packet lookup.

## R4 — Performance abstraction tax

**Risk:** reusable libraries are measurably slower than bespoke code.

**Mitigation:** specialized-reference/typed/runtime three-rung benchmarks plus assembly inspection.

## R5 — Memory explosion

**Risk:** pointer-rich generic state costs hundreds of MB at scale.

**Mitigation:** bytes/item gates and sparse occupancy benchmarks.

## R6 — Snapshot atomicity overreach

**Risk:** global generations/rebuilds destroy update performance.

**Mitigation:** referential default; scoped snapshots only when provable.

## R7 — Public API freezes internals

**Risk:** installed headers make optimization impossible.

**Mitigation:** curated stable/experimental/internal classification.

## R8 — DPDK duplication

**Risk:** BESS reimplements ring/hash/timer/flow abstractions.

**Mitigation:** benchmark DPDK primitive first; wrap only lifecycle/ergonomics BESS uniquely needs.

## R9 — Hardware blocks software progress

**Risk:** lack of NIC lab halts architecture.

**Mitigation:** mock backend; separate hardware certification.

## R10 — Reference apps become production frameworks

**Risk:** architecture examples grow their own product scope.

**Mitigation:** keep deliberately small; use them as conformance tests, not feature competitors.

---

# 30. Critical-path ordering

The true critical path is:

```text
M0 baseline
  |
M1 build layering
  |
M2 public surface
  |
M3 init capabilities
  |
M4 resource/wire separation
  |
M5 instance ownership
  |
M6 routing consolidation
  |
M7 IDs
  |
M8 transaction semantics
  |
M9 flow state
  |
M10 expiry
  |
M11 handoff
  |
M12 decision cache
  |
  +---- M14 L2
  +---- M15 L3
  +---- M17 conntrack -> M18 NAT
  +---- M20 offload
```

M13 edit plan can proceed experimentally after packet/lifetime boundaries are stable.

M21 portability and M22 hardening run partially in parallel after major boundaries settle.

M27 control SDK can mature incrementally, but its stable schema should follow M4/M8 semantics.

---

# 31. First implementation tranche: concrete commit-sized sequence

The first wave should avoid simultaneously changing architecture and packet behavior.

Suggested sequence:

### Commit 1 — architecture contract

- add target DAG;
- add public/internal policy;
- document non-negotiable appliance freedom;
- no source behavior change.

### Commit 2 — dependency graph tooling

- generate Meson target graph;
- include-layer checker;
- CI negative test;
- no source behavior change.

### Commit 3 — split `bess_dataplane` build target

Create independent classifier/meter/stats/route libraries while preserving existing source paths.

### Commit 4 — public-header manifest

Replace recursive install with explicit manifest while keeping current supported sample compiling.

### Commit 5 — plugin descriptor/version

Add compatibility metadata without changing module hot path.

### Commit 6 — module init context

Introduce context plumbing but keep legacy direct APIs.

### Commit 7 — migrate transaction-aware modules off `runtime()`

Start with ActionTable/Meter/Router/ExactMatch registration.

### Commit 8 — resource binding extraction

Move codec/schema association outside `dataplane::Resource`; preserve wire behavior.

### Commit 9 — explicit instance registry

Add lifecycle and tests; no existing module migration required yet.

### Commit 10 — route-domain correction

Fold domain identity into existing Router architecture, correct atomicity naming/semantics, remove unsafe domain map path.

### Commit 11 — `InterfaceId` and route graph decoupling

Move reusable next hop away from `gate_idx_t`; adapt Router module.

### Commit 12 — architecture/reference examples checkpoint

Add minimal direct-library router/session examples and rerun full baseline.

Only after these should M9 flow-state implementation begin.

---

# 32. Code-review checklist for all modernization commits

Every review should answer:

### Architecture

- Which layer owns this type?
- Which layer may depend on it?
- Is there a new forbidden dependency?
- Does this impose an appliance semantic on generic BESS?

### Performance

- Is this packet/control path?
- What machine code/data access should the hot path generate?
- Did typed abstraction add calls/branches/atomics?
- What benchmark proves the choice?
- What is the working-set size?

### Memory

- bytes/object?
- bytes/capacity?
- sparse behavior?
- cache lines touched?
- retirement backlog?

### Correctness

- ownership documented?
- pointer/span lifetime documented?
- failure before publication invisible?
- ID reuse safe?
- async escape handled?
- concurrency test?
- does every claim match its evidence? (a test is not a proof; a bounded exploration is not an unbounded proof)
- if the change touches a protocol with a C++ state model (Appendix L), did the model and its action-to-code mapping change with it?

### Ergonomics

- typed author path?
- runtime-defined author path if required?
- graph adapter separate?
- controller semantics clear?

### DPDK

- does DPDK already provide this primitive?
- if wrapping it, what BESS-specific value is added?
- if replacing it, what measurement justifies replacement?

### Compatibility

- public API change?
- wire change?
- existing module behavior?
- external plugin impact?

---

# 33. Definition of done for the end state

The modernization is not complete merely when milestones are coded.

It is complete when all of the following are demonstrably true.

## Architecture

- build graph enforces intended DAG;
- reusable networking libraries have no framework/runtime/control dependency;
- runtime has no appliance/network-policy semantics;
- framework implements integration, not networking.

## Performance

- typed core paths approach specialized C++ assembly;
- runtime genericity is paid outside packet inner loops;
- representative pipelines meet or exceed agreed baseline;
- update-under-load performance is characterized and within targets;
- memory cost at large scale is documented and acceptable.

## Correctness

- stateful resources have failure/concurrency/model tests;
- transaction semantics are precise;
- async handles cannot ABA;
- no plugin teardown occurs while its objects/code remain reachable;
- the strongest claim each document makes about a critical contract is backed by a mechanism able to establish it (Appendix L.1.5).

## Ergonomics

- graph user remains simple;
- external module author uses curated API;
- appliance author can build without Module;
- controller author uses thin generic SDK;
- battery author need not include internals.

## Neutrality

Reference applications prove:

- VFP-style policy model is natural;
- OVS-style translated cache model is natural;
- Hoverboard-style promotion/hierarchy is natural;
- session/UPF-style compiler is natural;
- none are forced into a universal BESS action/policy model.

## Product

- installed SDK contains only intended headers;
- external installed-tree builds pass;
- release metadata/SBOM exist;
- control protocol behavior is versioned/documented;
- hardware validation is tracked separately from software completeness.


# Appendix A — Current-file ownership map

This is a migration guide, not a demand for one large directory-moving commit.

| Current area/file | Long-term owner | Notes |
|---|---|---|
| `core/packet*.{h,cc}` | packet | Keep packet mechanics independent |
| `core/rcu/*` | rcu/dataplane substrate | Stable low-level lifetime |
| `core/dataplane/strong_id.h` | dataplane public | Core zero-cost type |
| `core/dataplane/object_table.h` | dataplane public | Generic immutable ID store |
| `core/dataplane/slot_table.h` | dataplane public | Generic live publication |
| `core/dataplane/resource.h` | dataplane core | Remove wire codec association |
| `core/dataplane/transaction_engine.*` | dataplane core | Keep control protocol out |
| `core/classifier/*` | classifier battery | Independent Meson lib |
| `core/meter/*` | meter battery | Independent Meson lib |
| `core/stats/*` | stats battery | Independent Meson lib |
| `core/route/route_table.*` | L3/route battery | Keep direct reusable table |
| `core/route/router.*` | L3/route battery | Remove graph gate dependency |
| `core/route/route_domain.h` | fold into Router/L3 | Do not retain parallel architecture |
| `core/framework/resource_codec.h` | control/framework binding | Remove `dataplane` namespace ownership |
| `core/framework/exact_match_table.h` | legacy/internal or classifier compatibility | Do not present as modern framework concept |
| `core/shared_obj.*` | framework legacy compatibility | Freeze; replace new use with explicit instances |
| `core/module.*` | framework | Public graph contract, simplify dependencies over time |
| `core/gate.*` | framework/execution | Graph integration |
| `core/metadata.*` | framework compatibility | Keep for graph users; fused applications need not use it |
| `core/runtime/runtime_state.*` | runtime internal | Never normal external API |
| `core/runtime/worker_manager.*` | runtime internal | Keep behind capabilities |
| `core/task.*`, scheduler/traffic class | execution/runtime | Clarify public/internal boundary |
| `core/port.*` | runtime/framework device boundary | Split physical device lifecycle from logical `InterfaceId` |
| `core/drivers/pmd.*` | drivers/runtime | Hardware-specific |
| `core/modules/action_table.*` | module/reference vertical slice | Do not elevate its `Action` to universal type |
| `core/modules/nat.*` | compatibility module over future NAT library | Extract mechanisms |
| `core/modules/l2_table.h` | candidate input to L2 library | Benchmark rather than blindly move |
| `core/modules/l2_forward.*` | compatibility module over L2 library | Thin adapter |
| `core/modules/hash_lb.*` | compatibility module + selection algorithm source | Extract only proven generic pieces |
| `core/modules/vxlan_*` | modules over tunnel library | Preserve config behavior |
| `core/control/*` | control | Desired state/wire adapters |
| `pybess` / `bessctl` | control client/operator surface | Evolve toward thin SDK use |

---

# Appendix B — Public C++ API sketches

These are directional examples. Exact names should be validated against real code and benchmarks before freezing.

## B.1 Strong IDs

```cpp
struct InterfaceIdTag;
using InterfaceId =
    bess::dataplane::StrongId<InterfaceIdTag, uint32_t>;

struct DecisionIdTag;
using DecisionId =
    bess::dataplane::StrongId<DecisionIdTag, uint32_t>;
```

Desired properties:

```cpp
static_assert(sizeof(InterfaceId) == sizeof(uint32_t));
static_assert(std::is_trivially_copyable_v<InterfaceId>);
```

No implicit conversion between unrelated IDs.

---

## B.2 Direct routing library

```cpp
bess::l3::Router router(config, rcu);

auto result = router.SetRoute(
    RouteDomainId{4},
    Ipv4Prefix::Make(...).value(),
    NextHopId{17});

const auto* hop =
    router.Lookup(RouteDomainId{4}, dst);
```

No module/gate/protobuf requirement.

Graph adapter separately translates resolved forwarding identity to gate.

---

## B.3 Worker-owned flow state

```cpp
struct FiveTuple {
  bess::utils::be32_t src;
  bess::utils::be32_t dst;
  bess::utils::be16_t sport;
  bess::utils::be16_t dport;
  uint8_t proto;
};

struct Connection {
  DecisionId decision;
  uint64_t last_seen;
  uint32_t flags;
};

bess::flow::WorkerFlowTable<FiveTuple, Connection> flows(capacity);

if (auto* c = flows.Find(key)) {
  c->last_seen = now;
}
```

The hot path should look like a specialized table lookup and direct field store.

---

## B.4 Decision cache

```cpp
bess::flow::DecisionCache<FiveTuple, DecisionId> cache(...);

auto hit = cache.Lookup(key, policy_generation);
if (!hit) {
  // application compiles policy
}
```

No BESS-defined decision structure.

---

## B.5 Application instance

```cpp
class VSwitchInstance {
 public:
  PolicyCompiler compiler;
  bess::flow::DecisionCache<FlowKey, DecisionId> cache;
  bess::dataplane::SlotTable<DecisionId, CompiledDecision> decisions;
};

auto created =
    ctx.instances().Create<VSwitchInstance>("vsw0", args...);
```

Module initialization:

```cpp
instance_ =
    ctx.instances().Require<VSwitchInstance>("vsw0").get();
```

Packet path uses `instance_` directly.

---

## B.6 Typed handoff

```cpp
struct PolicyMiss {
  FlowKey key;
  uint32_t generation;
};

auto channel =
    ctx.handoffs().Create<PolicyMiss>(config);

auto r = channel.TryPunt(std::move(packet),
                         PolicyMiss{key, gen});
```

No arbitrary `void*` context.

---

## B.7 Management client transaction

Conceptual language-neutral behavior:

```text
routes = client.resources.bind("router/routes", RouteKey, RouteValue)
hops   = client.resources.bind("router/next_hops", NextHopId, NextHop)

tx = client.transactions.begin(expected_generation)

tx.upsert(hops, NextHopId(7), hop)
tx.upsert(routes, RouteKey(vrf, prefix), RouteValue(7))

result = tx.commit()
```

SDK owns request identity and timeout semantics.

Application owns why these two operations belong together.

---

# Appendix C — Performance/codegen targets by facility

| Facility | Typed hot-path target |
|---|---|
| `StrongId` | identical to representation integer |
| `SlotTable::Lookup` | bounds/index + acquire pointer load |
| `ObjectTable::Lookup` | bounds/validity/address calculation |
| worker counter `Add` | ordinary worker-local load/add/store |
| typed exact classifier | backend-native hash/probe/result resolve |
| runtime exact classifier | one bound dispatch/batch, backend-native inner loop |
| worker flow lookup | backend-native hash/probe + state resolve |
| decision-cache hit | flow lookup + generation check + decision resolve |
| route lookup | native LPM lookup + next-hop resolve |
| meter exclusive | profile/state loads + `rte_meter` operation, no lock |
| meter shared | explicit measured synchronization only |
| FDB lookup | MAC/domain hash/probe + `InterfaceId` result |
| handoff SP/SC | ring-native enqueue/dequeue + ownership bookkeeping |
| fixed rewrite | fixed width loads/stores/checksum delta |
| stats update | no global atomic RMW |

Any unexpected:

- allocator call;
- refcount;
- string operation;
- virtual dispatch;
- generic schema decode;
- control lock

inside these paths is a review blocker until explained.

---

# Appendix D — Benchmark matrix summary

The full benchmark suite should be generated from reusable parameter matrices rather than hand-writing only favorable points.

## D.1 Tables

Axes:

```text
capacity:
  64
  1K
  16K
  64K
  1M
  larger as memory permits

occupancy:
  10%
  50%
  90%

key:
  8 B
  16 B
  5-tuple
  32 B

payload/state:
  8 B
  32 B
  64 B
  128 B

batch:
  1
  8
  16
  32

access:
  hot
  uniform hit
  miss
  50/50
  realistic skew

mutation:
  none
  nominal
  high
  flat out
```

Not every cross product belongs in every CI run. Maintain:

- exhaustive characterization job/manual suite;
- smaller stable regression subset.

## D.2 Stateful lifetime

Axes:

- flow create/remove;
- expiry refresh;
- alias;
- stalled reader;
- asynchronous queue outstanding;
- generation reuse.

## D.3 Pipelines

At least:

```text
classifier
classifier -> decision resolve
classifier -> decision -> meter
decision -> route
flow cache -> decision
stateful NAT
L2 FDB
punt -> service worker -> resume
```

## D.4 Current evidence anchors

At plan baseline `f4fdab03`:

- range backend benchmark reports approximately 23.2× improvement in its documented comparison (about 253.9 vs 11.0 M matches/s);
- route-domain benchmark reports roughly 1.69–2.41 ns/op in the measured sweep up to 64 domains.

These are attribution anchors, not eternal universal targets. Future comparisons must preserve workload definitions.

The transaction-engine decision record also contains measured session/update-under-reader results. Preserve those benchmark shapes so future engine/lifetime changes remain comparable.

---

# Appendix E — ADRs to add or refresh

Architecture decisions should be short, factual, and tied to evidence.

Recommended ADR/decision topics:

1. **Layer DAG and forbidden dependencies**
2. **Public / experimental / internal API classification**
3. **ModuleInitContext capability model**
4. **Resource/wire-schema separation**
5. **Application-instance structural lifetime**
6. **Unified route-domain model**
7. **Logical InterfaceId versus gate/port**
8. **Referential versus scoped-snapshot transaction semantics**
9. **Flow ownership modes**
10. **Expiry backend selection evidence**
11. **Handoff ownership/continuation lifetime**
12. **Decision-cache coarse generation invalidation**
13. **EditPlan adoption or rejection**
14. **FDB backend choice**
15. **Neighbor/adjacency representation**
16. **Conntrack state semantics**
17. **NAT allocator/backing representation**
18. **Hardware-flow ownership and MARK lifetime**
19. **Plugin compatibility/versioning**
20. **Control SDK retry/epoch semantics**

ADRs should state rejected alternatives and benchmark/correctness evidence.

---

# Appendix F — Milestone dependency/status matrix

| Milestone | Depends on | Blocks | Primary proof |
|---|---|---|---|
| M0 baseline | current HEAD | all | reproducible benchmark/test snapshot |
| M1 layering | M0 | new batteries | Meson/include graph negative tests |
| M2 public API | M1 | stable external battery use | installed-tree consumer |
| M3 init capabilities | M1/M2 | clean module migration | no direct runtime reach-through |
| M4 resource/schema | M1 | control SDK stability | dataplane builds protobuf-free |
| M5 instances | M3 | appliance composition | shared state without global service |
| M6 routing consolidation | M1–M4 | L3 expansion | one route architecture |
| M7 IDs | M6 partially | L2/L3/offload | zero-cost strong identity |
| M8 consistency | transaction core | strict scoped users | adversarial visibility model |
| M9 flow | M1/M2 | conntrack/NAT/cache | typed assembly + scale benchmark |
| M10 expiry | M9 consumer input | flow aging etc. | bounded poll + stale safety |
| M11 handoff | M7 | slow paths/offload | ownership stress + queue benchmark |
| M12 decision cache | M9/M10 | policy vSwitch | cache hit assembly/perf |
| M13 EditPlan | packet stable | optional | benchmark-based adopt/reject |
| M14 L2 | M9/M10/M7 | bridge appliance | legacy differential |
| M15 L3 | M6/M7 | router appliance | FIB/neighbor/ECMP tests |
| M16 selection | consumer demand | ECMP/LB | distribution + perf |
| M17 conntrack | M9/M10 | firewall/NAT | state-machine/model tests |
| M18 NAT | M9/M10, optionally M17 | NAT appliance | legacy differential |
| M19 tunnel | packet | appliance encap | module differential |
| M20 offload | M7/M11 | hardware paths | mocked lifecycle + lab |
| M21 portability | boundaries stable | broad release | ARM/x86 matrix |
| M22 hardening | continuous | stable labels | sanitizer/fuzz/model |
| M23 ergonomics | public APIs | stable SDK | installed examples |
| M24 references | core batteries | end-state proof | 4+ appliance exemplars |
| M25 observability | stats/resources | operations | telemetry/event integration |
| M26 release | M2/M21/M22 | release | install/SBOM/repro |
| M27 control SDK | M4/M8/control APIs | controller adoption | black-box timeout/retry tests |

---

# Appendix G — Explicit review anti-patterns

The following should trigger architectural review.

## G.1 Appliance semantics in generic core

Examples:

```text
PDR
FAR
QER
VFP Layer
OpenFlow action
VIP
firewall zone
```

appearing in `dataplane/`, `runtime/`, or generic battery interfaces.

## G.2 Network semantics in runtime

Examples:

```cpp
runtime().router()
runtime().neighbor_table()
runtime().nat()
```

## G.3 Module dependency in reusable batteries

Examples:

```cpp
#include "module.h"
gate_idx_t
AddMetadataAttr(...)
```

inside classifier/flow/L2/L3/NAT library code.

## G.4 Wire semantics in dataplane core

Examples:

```cpp
google::protobuf::Any
grpc::...
type_url
```

inside generic resource/lifetime implementation.

## G.5 Per-packet generic lookup

Examples:

```text
resource name string
InstanceRegistry name
service lookup
backend name
schema parser
```

in packet loop.

## G.6 Hidden ownership transfer

A function accepting/returning packet handles where it is unclear who frees on failure.

## G.7 “Atomic” without observation contract

Any API described as atomic must state exactly what concurrent readers can observe.

## G.8 Full rebuild for ordinary point update

Requires explicit evidence that backend semantics demand it or performance remains acceptable.

## G.9 New home-grown DPDK-equivalent primitive

Before adding:

- ring;
- hash;
- timer;
- flow IR;
- memory pool;

document why DPDK's primitive cannot satisfy the requirement.

## G.10 Abstraction benchmark only on hot cache

Any new table abstraction benchmarked only on a tiny working set is incomplete.

## G.11 Type erasure where type is statically known

Typed authors should not pay runtime schema/type-erasure costs.

## G.12 Shared mutable state by default

Prefer worker ownership. Shared synchronization is a workload choice, not default convenience.

---

# Appendix H — Concrete “end-state feel” examples

## H.1 Simple graph user

Nothing becomes harder:

```text
src :: ExactMatch(...)
    -> Meter(...)
    -> Router(...)
    -> PortOut(...)
```

Graph users benefit from improved libraries under modules without learning them.

---

## H.2 External specialized module

```cpp
class TenantClassifier final : public Module {
 public:
  CommandResponse Init(const Args&) {
    // compile typed/runtime schema once
  }

  void ProcessBatch(Context* ctx, PacketBatch* batch) override {
    // public classifier API, no RuntimeState
  }
};
```

---

## H.3 Fused VFP-style appliance

```cpp
class VfpDatapath {
 public:
  Decision Process(PacketRef pkt) noexcept {
    FlowKey key = ParseKey(pkt);

    if (auto id = cache_.Lookup(key, generation_)) {
      return *decisions_.Lookup(*id);
    }

    return Miss(key);
  }

 private:
  VfpPolicyModel policy_;               // application
  PolicyCompiler compiler_;             // application
  DecisionCache<FlowKey, DecisionId> cache_; // BESS
  SlotTable<DecisionId, CompiledDecision> decisions_; // BESS mechanism,
                                                       // application value
};
```

BESS does not know `VfpPolicyModel`.

---

## H.4 OVS-like translated cache

```text
rich tables
   |
application translator
   |
CompiledDatapathDecision
   |
DecisionCache
   |
packet hot path

miss -> HandoffChannel -> translator/control worker
```

No BESS OpenFlow interpreter.

---

## H.5 Hoverboard-style promotion

```text
Lookup fast decision
   |
 miss/cold
   v
default path / punt
   |
application observes/hotness logic
   |
install software decision
   |
optional native rte_flow promotion
```

BESS supplies state/handoff/offload lifecycle only.

---

## H.6 OMEC-style session compiler

```text
PFCP/session model
      |
OMEC compiler
      |
+-----+---------+---------+---------+
|               |         |         |
classifier   SessionAction meter    route
resource       resource    resource resource
       \          |           |       /
        +------ generic BESS transaction
```

BESS never needs a `PDR` core type.

---

# Appendix I — Milestone reporting template

Every completed milestone should append a short factual entry to the modernization log:

```markdown
## Mx — <name> — COMPLETE — <date>

Commit(s):
- abcdef12 — ...
- ...

Architecture:
- what dependency/ownership changed
- what did not change

Correctness:
- unit tests
- differential/model
- fault injection
- concurrency

Performance:
- baseline
- after
- workload
- CPU/compiler
- interpretation

Memory:
- bytes/item before/after
- high water

Compatibility:
- module graph
- public C++ API
- wire protocol
- external plugin

Known remaining limits:
- ...

Next dependency:
- My
```

Avoid marketing language.

---

# Appendix J — Release-blocking final acceptance checklist

## Architecture

- [ ] build DAG has no forbidden edge
- [ ] no reusable battery depends on Module/runtime/control
- [ ] runtime has no network-policy semantics
- [ ] no generic BESS action/policy language exists
- [ ] graph adapters wrap libraries rather than contain unique mechanisms

## C++ public API

- [ ] installed headers are manifest-driven
- [ ] stable/experimental/internal distinction documented
- [ ] direct appliance build does not require framework
- [ ] no source-tree internal include in reference appliances
- [ ] plugin compatibility descriptor works

## Performance

- [ ] typed classifier near specialized reference
- [ ] typed flow lookup near specialized reference
- [ ] routing baseline retained
- [ ] meter/stats baseline retained
- [ ] decision-cache hit characterized
- [ ] handoff compared to optimized ring/Queue
- [ ] update-under-load matrix complete
- [ ] memory-at-scale matrix complete
- [ ] assembly review complete for critical kernels

## Correctness

- [ ] transaction lifecycle tests pass
- [ ] flow model/stress pass
- [ ] expiry stale-generation tests pass
- [ ] handoff ownership stress pass
- [ ] FDB model/aging pass
- [ ] routing snapshot/referential tests pass
- [ ] conntrack state model pass
- [ ] NAT differential pass
- [ ] offload lifecycle mock tests pass
- [ ] plugin unload lifetime test passes

## Appliance neutrality

- [ ] router reference
- [ ] stateful NAT reference
- [ ] VFP/OVS-style policy-vSwitch reference
- [ ] session compiler reference
- [ ] hierarchical promotion path
- [ ] none requires application model to become BESS graph/policy IR

## Control

- [ ] request IDs/digests
- [ ] daemon epochs
- [ ] timeout recovery
- [ ] BUSY/conflict/rejected distinctions
- [ ] resource handles/discovery
- [ ] desired-state generation handling
- [ ] application-specific semantics remain outside BESS SDK

## Product

- [ ] GCC
- [ ] Clang
- [ ] ARM64 supported build
- [ ] sanitizer subset
- [ ] fuzz targets
- [ ] installed-tree plugin
- [ ] package/SBOM
- [ ] hardware certification status explicit

---

# Appendix K — Source/evidence basis

This plan is pinned to `krsna1729/bess` `develop` at:

```text
f4fdab03c10f9f0b39147a59230d2a678e6050a8
2026-10-01
```

That HEAD contains the foundational range and VRF benchmark commit and follows the preceding K1–K7, G1, transaction, performance-clawback, plugin-package, and reorganization work discussed during this modernization review.

Important source areas reviewed while forming the plan include:

```text
MODERNIZATION.md
docs/decisions.md
docs/plugin-api.md
meson.build
core/meson.build

core/dataplane/
core/classifier/
core/meter/
core/stats/
core/route/
core/runtime/
core/control/
core/framework/

core/modules/action_table.*
core/modules/session_pipeline_test.cc
core/modules/nat.*
core/modules/l2_table.h
core/modules/l2_forward.*
core/modules/hash_lb.*
```

The document intentionally builds on the current direction rather than replacing it.

---

# Appendix L — Assurance doctrine and machine-checked specifications

Source: an external discussion of formal verification for BESS (TLA+, Lean, Bend, Vx), reviewed 2026-10-04 against
the roadmap at `d677f186`. Its central advice fits this roadmap's Pareto discipline: do not add a "formal
verification phase"; choose, per failure class, the cheapest technique that actually closes it. L.1 is what BESS
adopts now, in C++ and in its existing test and review machinery. L.2 is future work, each item with an entry gate.

## L.1 Adopted now (C++ and process)

### L.1.1 Assurance ladder

| Level | Technique | BESS use |
|---|---|---|
| A0 | C++ types, concepts, `static_assert`, layout checks | strong IDs, ownership and policy axes as types (`table_policy.h`), slot layouts |
| A1 | deterministic unit, differential, property and reference-model tests; fuzzing; sanitizers; fault injection; mutation checks | default for every table, parser, packet algorithm and resource operation (M22) |
| A1+ | small-scope exhaustive exploration of an abstract state machine, written in C++ (gtest) | concurrent and lifecycle protocols, below |
| A2 | TLA+/TLC bounded model checking | future (L.2) |
| A3 | Lean theorems about functional semantics | future (L.2) |
| A4 | implementation refinement proofs | not planned |

A subsystem moves up a level only when the level below leaves a material failure class open. Importance alone is
not a reason.

### L.1.2 Small-scope protocol models in C++ (A1+)

The protocols that have caused or nearly caused review findings will get a compact abstract state machine in C++
(none exists yet; these three are the next assurance work items). A test enumerates every interleaving of its actions up to a small bound (for example 2 readers, 2 generations,
3 resources, 2 requests) and asserts the invariants in every reachable state. The model holds only the semantic
state (resources, references, generations, readers, owners, epochs), never the production containers, allocators
or DPDK calls. It is a test, not a proof: it excludes violations only within the explored bound.

| Protocol | Actions | Invariants |
|---|---|---|
| M8 transaction visibility | Prepare, RejectPrepare, Publish (per resource), Abort, ReaderObserve, Retire, Reclaim, Retry, Restart | a visible reference names a visible target; prepare/abort failures are invisible; no reclaim while reachable; no stale-generation commit; one execution per request id; scoped reads come from one generation |
| M11 handoff ownership | Enqueue, Dequeue, Drop, Drain, Teardown, Resume, Reject | at most one owner per packet; a freed packet never becomes live; resume only at the current generation; teardown leaves no packet unaccounted |
| M27 control retry/epoch | Send, Deliver, Commit, LoseReply, Query, Retry, RestartDaemon, Rediscover | same id and digest: at most one application; same id, other digest: refused; a timeout is never reported as failure; no status from an old epoch reported as certain; a stale handle is never sent |

Each model's counterexamples become permanent concrete C++ regression tests against the production code. The
existing reference models (`fdb_model_test`, `nat_model_test`, `conntrack_model_test`,
`decision_cache_model_test`) stay A1; the M27 SDK's scripted recovery tests are the A1 base the M27 model extends.

### L.1.3 Model-to-code mapping

Each A1+ model's decision record carries a table that maps every model action to the production function or
window that realizes it (for M8: Prepare to the engine's reserve/prepare phase, Publish to the infallible
publication window, Reclaim to RCU reclamation after quiescence, Restart to the daemon-epoch change). A change
that breaks a row must change the model or the code in the same commit. The model and the code cannot drift
apart silently.

### L.1.4 Single reference semantics for transforming code

Code that transforms a specification into a faster representation (classifier canonicalization and backend
choice, range splitting, the M13 EditPlan optimizer if adopted, incremental checksum updates) is tested against one
straightforward reference implementation of the meaning: `reference(spec, input) == optimized(compile(spec),
input)` over generated specifications and inputs. The reference is the documentation of the semantics. It is
the object a later Lean definition (L.2) would formalize.

### L.1.5 Claim precision

Documents, decision records and PR descriptions must not describe tests as proofs, a bounded exploration as an
unbounded proof, or a statement about a model as a statement about the C++, compiler or DPDK implementation unless
a correspondence argument exists. This is in the review checklist (§32) and the definition of done (§33).

### L.1.6 Placement and capacity validation (from the Vx discussion)

These are already roadmap direction; the discussion sharpens them. Each lands in the milestone it belongs to, and
none adds hot-path cost:

- requested placement (NUMA node, queue-to-worker, device) is validated at init or control time into a bound plan;
  the packet path consumes only the bound result;
- memory admission per NUMA node and resource: a publication that would exceed a declared budget is refused
  before it is visible, not discovered at allocation failure on a worker;
- move-only ownership in public APIs wherever ownership transfers (packets, handoff, async completions).

## L.2 Future work (entry gates)

| Item | Gate to start | Kill criterion |
|---|---|---|
| TLA+/TLC pilot: M8 transaction publication (`spec/tla/Transactions.tla`) | the A1+ C++ model exists and its invariants are stable for one release | after the pilot: ADOPT if it found a real defect or states the contract materially better than the C++ model; LIMIT to that protocol if useful only there; STOP if it costs more than the risk it removes |
| TLA+ for M20 async offload lifecycle (MARK IDs, install/remove/reset, delayed completions) | before the rte_flow async/template path is built (D-070's hardware matrix is blocked on hardware) | same three-way decision |
| TLA+ for M11 handoff and M27 retry/epoch | the M8 pilot decides ADOPT | same |
| Lean pilot: classifier semantics (exact, masked, range, precedence, default) and one canonicalization theorem | L.1.4's reference semantics is stable | STOP if it becomes a parallel implementation to maintain |
| Lean for EditPlan semantic equivalence | M13 adopted | M13's optimizer must stay small enough to have a precise semantics; otherwise M13 itself is reconsidered |
| Lean for checksum algebra (incremental equals full recomputation) | after the classifier pilot decides ADOPT | proves the mathematics only, never the inline asm; differential tests, the compiler matrix and mutation checks keep covering that |
| TLAPS (unbounded TLA+ proofs) | a model has repeatedly found real defects and is an enduring artifact | not before |

Rules for any adopted formal work:

- Artifacts live in `spec/` (`spec/tla/`, `spec/lean/`), outside the production dependency graph. No production
  target depends on them.
- Toolchains are pinned (the TLC release; `lean-toolchain`) and run in a pinned container, as the DPDK and Go
  generator pins are.
- CI is asymmetric: tiny models and the proof library on PRs (seconds); larger scopes and liveness checks
  nightly; the largest configurations before release.
- A model much larger than the protocol it states is modelling implementation accidents. Shrink the model.
- Two tools at most (TLA+ and Lean).

Not planned at any level: proofs of DPDK, `rte_hash`, `rte_lpm`, PMDs, firmware, the scheduler, the C++ memory-model
correspondence, compiler correctness, or the whole module graph. Bend and Vx do not enter the dependency graph;
Bend's automatic parallelism does not fit packet execution, and a heterogeneous IR is a watch item only.

---

# Final architectural statement

The modernization should stop at the following boundary:

> **BESS is responsible for near-assembly-optimal packet execution, safe and efficient state lifetime, generic publication/transaction machinery, reusable networking mechanisms, graph integration, and generic remote control semantics.**

> **The appliance is responsible for its programming model, policy meaning, policy compiler, cache/offload strategy, and high-level intent.**

If an implementation decision makes VFP, OVS, Hoverboard, OMEC, a firewall, a load balancer, or a future unknown appliance adopt an unnatural BESS semantic model merely to reuse the runtime, that decision is off the target Pareto frontier.

If a reusable BESS abstraction forces a typed hot path to pay a substantial machine-code cost compared with specialized C++, that abstraction is also off the target Pareto frontier.

The desired codebase is therefore neither a bag of modules nor a full network stack.

It is:

> **a rigorously layered, near-zero-overhead C++ packet-processing runtime and standard library, with optional graph composition and a small transactional management/control surface.**
