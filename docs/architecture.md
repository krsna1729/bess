# BESS Core Architecture Contract

## 0. Non-Negotiable Principle

Appliance authors are free to build radically different programming models—VFP-style layered policy, OVS-style translated flow caches, Hoverboard-style hierarchical fast/slow paths, OMEC-style session compilation, or custom load-balancer state machines—without fighting the BESS architecture or first translating their model into a BESS-defined policy model.

### BESS decides:
- How packets execute efficiently (`bess::PacketRef`, `bess::PacketBatch`, vector extraction).
- How packet memory is represented, bounded, and safely mutated.
- How worker-local and shared state is published and reclaimed via QSBR RCU (`bess::rcu::RcuDomain`, `RcuPtr`).
- How stable identifiers map to immutable objects (`StrongId`, `ObjectTable`, `SlotTable`).
- How multi-resource modifications preserve referential correctness (`TransactionEngine`, `VISIBILITY_DEPENDENCY_ORDERED`).
- How single-instruction session switches occur without torn reads (`ScopeCell`, `VISIBILITY_ATOMIC`).
- How generic classifiers, meters, routing structures, and counters are implemented with near-assembly speed.
- How physical/virtual devices and workers are placed and scheduled.

### The application decides:
- What a session, connection, flow, policy, rule, group, tenant, or intent means.
- How its policy is compiled and which decisions are cached or offloaded.
- What gets punted or resumed to the control plane.
- Which BESS libraries it composes directly and which it bypasses.

---

## 1. Component Layering and Dependency DAG

BESS is organized as a strict directed acyclic graph (DAG) enforced at build time. Reusable networking algorithms are **pure C++ libraries** with zero dependencies on BESS module graphs, runtime singletons, or protobufs:

```text
                  bess_utils (Leaf utilities)
                      ^
                      |
                  bess_packet (Packet mbuf view & mutation)
                      ^
                      |
                  bess_rcu (Quiescent-state RCU substrate)
                      ^
                      |
             bess_dataplane_core (StrongId, SlotTable, ObjectTable, Transactions, ScopeCell)
                      ^
      +---------------+---------------+---------------+
      |               |               |               |
bess_classifier   bess_meter     bess_stats      bess_route
      ^               ^               ^               ^
      +---------------+---------------+---------------+
                      |
               bess_framework (Module, Gate, Graph, Task, Scheduler, Hooks)
                      ^
                      |
               bess_runtime (Workers, Memory, DPDK, Ports)
                      ^
                      |
               bess_control (Desired-state pipeline, Transaction RPC)
                      ^
                      |
            bess_modules & drivers (Thin adapters over pure libraries)
```

---

## 2. Forbidden Dependency Edges (Build-Enforced)

The build system and include verification checker (`tools/check_includes.py`) enforce the following invariants:

1. `packet/**` may only depend on `utils/**` and low-level DPDK mbuf primitives. It must **never** include `framework/**`, `runtime/**`, `control/**`, `pb/**`, or `module.h`.
2. `dataplane/**` (core substrate) may depend on `rcu/**` and minimal `utils/**`. It must **never** include `framework/**`, `runtime/**`, `control/**`, or `pb/**`.
3. Reusable libraries (`classifier/**`, `meter/**`, `stats/**`, `route/route_table.*`) are standalone C++ libraries. They must **never** include `module.h`, `runtime/**`, or `control/**`.
4. `framework/**` defines execution contracts (Module, Gate, Task). It does not depend on concrete modules, drivers, or control-plane RPC orchestration.
5. Modules (`modules/**`) and drivers (`drivers/**`) are thin graph adapters. A module must **never** include another concrete module's internal header.

---

## 3. Transaction and Concurrency Semantics

BESS provides two formal visibility guarantees for state mutation (Decision D-021):

1. **`VISIBILITY_DEPENDENCY_ORDERED`**:
   - Operations take effect in dependency order (referents before referrers).
   - A rule referencing an action or next-hop cannot see a missing target.
   - Deletions cascade in reverse dependency order, with physical destruction deferred until all readers clear a QSBR grace period.
   - Zero worker pause during active transactions.
2. **`VISIBILITY_ATOMIC`**:
   - Single-instruction atomic cutover enabled by `ScopeCell`.
   - Packages `{meter_id, next_hop_id}` into a 64-bit atomic word.
   - Packets observe either the entire old policy or the entire new policy, with zero torn reads across concurrent readers.

---

## 4. Architectural Anti-Goals

The following patterns are explicitly rejected and forbidden:
- **No universal `BessAction` object**: Actions are defined by the application/module, not centralized into a monolithic variant.
- **No BESS policy language**: BESS provides fast mechanisms, not high-level network policy ASTs.
- **No mandatory module graphs**: High-performance appliances can call BESS libraries directly without instantiating `Module` graphs.
- **No global runtime reach-through**: Reusable libraries receive their dependencies (`RcuDomain`, memory allocators) explicitly via constructors, never via global `runtime()` singletons.
- **No full-table rebuilds for live updates**: Tables support concurrent in-place updates with QSBR hazard management.
