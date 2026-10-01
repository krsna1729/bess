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

Two checkers enforce this in CI. `tools/check_includes.py` rejects forbidden `#include` edges (rules 1-3, plus: `core/modules/**` must not include `runtime/**`, and `core/dataplane/**` must not include protobuf or gRPC). `tools/check_link_graph.py` reads the built static archives, resolves undefined symbols between BESS libraries, and compares the result with the allowlisted DAG in `tools/layer_dag.json`; it catches dependencies that enter through link configuration. Rules 4 and 5 are review rules today. A grandfathered violation must be listed in `layer_dag.json` with an owner and a removal phase, and the checker warns when one is no longer needed. The measured graph is committed at `docs/baselines/dependency-graph.json`.

1. `packet/**` may only depend on `utils/**` and low-level DPDK mbuf primitives. It must **never** include `framework/**`, `runtime/**`, `control/**`, `pb/**`, or `module.h`.
2. `dataplane/**` (core substrate) may depend on `rcu/**` and minimal `utils/**`. It must **never** include `framework/**`, `runtime/**`, `control/**`, or `pb/**`.
3. Reusable libraries (`classifier/**`, `meter/**`, `stats/**`, `route/route_table.*`) are standalone C++ libraries. They must **never** include `module.h`, `runtime/**`, or `control/**`.
4. `framework/**` defines execution contracts (Module, Gate, Task). It does not depend on concrete modules, drivers, or control-plane RPC orchestration.
5. Modules (`modules/**`) and drivers (`drivers/**`) are thin graph adapters. A module must **never** include another concrete module's internal header.

---

## 3. Transaction and Concurrency Semantics

Visibility is requested per transaction (Decision D-021). Today the control path provides one level:

1. **`VISIBILITY_DEPENDENCY_ORDERED`** (provided; the only level the transaction RPC reports):
   - Operations take effect in dependency order (referents before referrers).
   - A rule referencing an action or next-hop cannot see a missing target.
   - Deletions cascade in reverse dependency order, with physical destruction deferred until all readers clear a QSBR grace period.
   - Zero worker pause during active transactions.
2. **`VISIBILITY_ATOMIC`** (not provided yet):
   - `dataplane::ScopeCell` is the primitive: one 64-bit atomic word that switches a scope's whole policy at once. It is not wired into the transaction engine or the RPC, and `ApplyTransactionRequest` cannot request a visibility level. Wiring it, and refusing rather than downgrading an unsupported request, is milestone M8.

---

## 4. Architectural Anti-Goals

The following patterns are explicitly rejected and forbidden:
- **No universal `BessAction` object**: Actions are defined by the application/module, not centralized into a monolithic variant.
- **No BESS policy language**: BESS provides fast mechanisms, not high-level network policy ASTs.
- **No mandatory module graphs**: High-performance appliances can call BESS libraries directly without instantiating `Module` graphs.
- **No global runtime reach-through**: Reusable libraries receive their dependencies (`RcuDomain`, memory allocators) explicitly via constructors, never via global `runtime()` singletons.
- **No full-table rebuilds for live updates**: Tables support concurrent in-place updates with QSBR hazard management.

---

## 5. API Classification

Every installed header is **public**, **experimental** or **internal**. Packaging enforces it: the install manifest in `core/meson.build` names each installed header (no recursive install), and `tools/check_installed_headers.py` fails if a private header is installed or a public one is missing.

- **Public** (supported source API for compatible BESS releases; plugins are rebuilt against the target release, the C++ ABI is not promised): the module, packet and port headers at `bess/core/`, `framework/module_init_context.h`, `framework/instance_registry.h`, `framework/plugin.h`, and the `utils/` helpers in the install manifest.
- **Experimental** (installed, may change without preserving source compatibility): the selected headers under `classifier/`, `dataplane/`, `meter/`, `rcu/`, `route/` and `stats/`. Promotion to public is a decision recorded in `docs/decisions.md`.
- **Internal** (never installed): `runtime/`, `control/`, `drivers/`, `gate_hooks/`, `resume_hooks/`, `framework/resource_bindings.h`, the transaction engine, scheduler and daemon startup headers, tests and benchmarks.

`docs/plugin-api.md` is the plugin author's view of the same table.

## 6. RCU Lifetime Rules

From `core/rcu/rcu_domain.h` and `rcu_ptr.h`:

1. One `RcuDomain` per runtime; reader identity is the `WorkerId`. Workers register once, go online before dataplane execution, and go offline before they block.
2. A reader reports quiescence only between task invocations, never inside packet processing.
3. A pointer read from an `RcuPtr` (one acquire load) is valid until that worker's next quiescent state. Use it for one task invocation; never cache it across invocations.
4. Published objects are immutable. Mutable high-frequency state belongs in worker-local state, not in an RCU object.
5. A writer builds the replacement completely, publishes it, then starts the grace period and retires the old object against that token. Starting the grace period first lets a reader pass it and still acquire the old pointer.
6. Retired objects are destroyed on a control thread (`ReclaimReady()` or `Drain()`), never on a packet worker. A deleter is code in its owner, so the owner must outlive the last pending retirement (the `outstanding` counter exists for this).
7. Writers never wait on the packet path; past the retirement-queue bound the control side reclaims and waits.

## 7. Handle Lifetime Rules

Identifiers (`StrongId`) name objects through `SlotTable`/`ObjectTable`; an erased id is retired, not reused, until readers can no longer hold it (for example `Router` refuses to reuse a retiring `NextHopId`). A borrowed instance is an `InstanceLease`, which blocks `Destroy`. These rules bind batteries that hand out handles later (flow ids, handoff and hardware-offload handles; milestones M9, M11, M20):

1. A handle that can outlive its object carries a generation; resolving a stale handle fails closed and never reaches a new object that reused the slot.
2. An asynchronous completion names the handle it was issued for and is checked against the current generation before it takes effect.
3. A handle is not dereferenced across a structural change (destroy, replace-scope) without re-resolving.
4. Resolution on the packet path is by cached pointer or index, never by name.

These are contracts for code not yet written; each such milestone must show a test where a stale handle does not resolve to a new object.

## 8. Battery Admission Criteria

A mechanism is admitted as a BESS library only if all of these hold. They follow from section 0 and the roadmap's layering, performance and correctness sections.

1. **Generic:** at least two different appliance models (for example a flow cache and a session compiler) would use it unchanged. It contains no appliance semantics (no PDR/FAR, VFP layer, OpenFlow action, VIP or firewall zone).
2. **Layered:** it fits the DAG in section 1 with no `module.h`, runtime or protobuf dependency, and a Meson library of its own once it exists.
3. **Three faces, one implementation:** a direct C++ library, a thin graph adapter in `modules/`, and, where control needs it, a binding outside the library. Bypassing the graph must stay possible.
4. **Explicit dependencies:** it receives the `RcuDomain` and other facilities by constructor; it never looks anything up per packet or through a global.
5. **Measured:** hot paths have a benchmark with a stated target, bytes per item and cache lines touched per lookup are reported, and a stable median regression above 3% in an existing deterministic benchmark is explained.
6. **Correct under concurrency:** worker ownership is explicit, lifetime follows sections 6 and 7, and the test set includes failure injection and a reference model where the state is non-trivial.
7. **Not a policy language:** it provides mechanism. If it needs a universal action object or rule syntax, it is out of scope.
