# BESS Core Architecture Contract

## 0. Non-Negotiable Principle

Appliance authors are free to build radically different programming models—VFP-style layered policy, OVS-style translated flow caches, Hoverboard-style hierarchical fast/slow paths, OMEC-style session compilation, or custom load-balancer state machines—without fighting the BESS architecture or first translating their model into a BESS-defined policy model.

### BESS decides:
- How packets execute efficiently (`bess::PacketRef`, `bess::PacketBatch`, vector extraction).
- How packet memory is represented, bounded, and safely mutated.
- How worker-local and shared state is published and reclaimed via QSBR RCU (`bess::rcu::RcuDomain`, `RcuPtr`).
- How stable identifiers map to immutable objects (`StrongId`, `ObjectTable`, `SlotTable`).
- How multi-resource modifications preserve referential correctness (`TransactionEngine`, `VISIBILITY_DEPENDENCY_ORDERED`).
- How a scope's whole policy switches without a torn read (`ScopeResource`, consistency `CONSISTENCY_SCOPE_SNAPSHOT`), and how that differs from referential visibility.
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
                  bess_eal (DPDK bring-up, memory, process options, thread placement)
                      ^
                      |
                  bess_packet (Packet mbuf view & mutation)
                      ^
                      |
                  bess_rcu (Quiescent-state RCU substrate)
                      ^
                      |
             bess_dataplane_core (StrongId, SlotTable, ObjectTable, Transactions, ScopeTable/ScopeResource, ExpiryWheel/TickRate, HandoffChannel/ContinuationTable)
                      ^
      +-----------+-----------+-----------+-----------+-----------+
      |           |           |           |           |           |
bess_classifier  bess_meter  bess_stats  bess_route  bess_flow
      ^           ^           ^           ^           ^
      +-----------+-----------+-----------+-----------+-----------+
                      |
               bess_execution (Worker, Task, Scheduler, TrafficClass, PacketPool, registries)
                      ^
                      |
               bess_framework (Module, Gate, Graph, Ports, Init context, Plugin loader)
                      ^
                      |
               bess_control (Desired-state pipeline, Transaction RPC)
                      ^
                      |
            bess_modules & drivers (Thin adapters over pure libraries)
```

---

## 2. Forbidden Dependency Edges (Build-Enforced)

Two checkers enforce this in CI. `tools/check_includes.py` rejects forbidden `#include` edges (rules 1-3, plus: `core/modules/**` must not include `runtime/**`, and `core/dataplane/**` must not include protobuf, gRPC, or the batteries built on it: `meter/`, `route/`, `classifier/`, `stats/`; tests and benchmarks are exempt). `tools/check_link_graph.py` reads the built static archives, resolves undefined symbols between BESS libraries, and compares the result with the allowlisted DAG in `tools/layer_dag.json`; it catches dependencies that enter through link configuration. Rules 4 and 5 are review rules today. A grandfathered violation would have to be listed in `layer_dag.json` with an owner and a removal phase, and the checker warns when one is no longer needed; the list is empty today (D-057). The measured graph is committed at `docs/baselines/dependency-graph.json`.

`core/flow/**` (M9, D-052) is covered by `check_includes.py` too: no `module.h`, `gate.h`, `framework/`, `runtime/`, `control/`, protobuf or gRPC, and no `worker.h` (which also catches `stats/current_worker.h`): a flow table learns who owns it from an injected owner token, not by asking the worker. Link-wise `bess_flow` may use `bess_classifier`, `bess_dataplane_core`, `bess_rcu` and `bess_utils` (`SharedFlowTable` is built on `ConcurrentExactTable`).

`core/dataplane/**` also may not include `flow/`, `gate.h`, `module.h` or `worker.h` (M10, D-053): the expiry engine (`dataplane/expiry_wheel.h`) is a generic substrate that `flow/` consumes through its `Observer` seam, so the edge runs `flow/ -> dataplane/`, never back, and a tick source (the scheduler's cached TSC, a test clock) is passed in as ticks, not read from a global or from the worker.

`dataplane/handoff.h` and `dataplane/continuation.h` (M11, D-054) are held to one more rule: they may not include the packet view (`packet.h`, `pktbatch.h`, `packet_pool.h`). A handoff channel moves an opaque `PacketHandle` (`packet_handle.h`, an `rte_mbuf *`), frees it only at teardown through `rte_pktmbuf_free`, and never looks inside a packet; the packet substrate stays below it. Both are header-only except `handoff.cc` (the DPDK-heap allocator and the C-linkage ring size call), so no new library and no new link edge: `bess_dataplane_core` already may use `bess_utils`.

1. `packet/**` may only depend on `utils/**` and low-level DPDK mbuf primitives. It must **never** include `framework/**`, `runtime/**`, `control/**`, `pb/**`, or `module.h`.
2. `dataplane/**` (core substrate) may depend on `rcu/**` and minimal `utils/**`. It must **never** include `framework/**`, `runtime/**`, `control/**`, `pb/**`, `flow/**`, `gate.h`, `module.h`, `worker.h`, or the batteries above it (`meter/**`, `route/**`, `classifier/**`, `stats/**`).
3. Reusable libraries (`classifier/**`, `flow/**`, `meter/**`, `stats/**`, `route/route_table.*`) are standalone C++ libraries. They must **never** include `module.h`, `runtime/**`, or `control/**`.
4. `bess_eal` (`runtime/{dpdk,memory,opts,path,startup,thread_placement}.*`, `utils/{dpdk_memory,bpf_program}.*`) is the bottom of the DPDK-facing stack: it must **never** include `worker.h`, `module.h`, `packet_pool.h`, `scheduler.h`, `traffic_class.h` or `runtime/runtime_state.h` (`check_includes.py`), and the libraries that bring the EAL up lazily (`classifier`, `meter`, `route`, `dataplane`) link it. `bess_execution` (workers, tasks, scheduler, traffic classes, the packet pool and the registries they publish through) needs no strong symbol from `bess_framework`; Module is reached only through its virtual interface (D-057). `framework/**` defines the graph contracts (Module, Gate, Port) on top of it and does not depend on concrete modules, drivers, or control-plane RPC orchestration.
5. Modules (`modules/**`) and drivers (`drivers/**`) are thin graph adapters. A module must **never** include another concrete module's internal header.

---

## 3. Transaction and Concurrency Semantics

A transaction asks for one of two consistency levels (`ApplyTransactionRequest.consistency`; `dataplane::Consistency` in C++; Decisions D-021, D-050). They are different promises, named apart, and a request for a level that its resources cannot provide is refused, never served as the other one.

1. **Referential** (`CONSISTENCY_REFERENTIAL`, the default; reported as `VISIBILITY_DEPENDENCY_ORDERED`). Any resource can take part.
   - Operations take effect one by one, in dependency order (referents before referrers): upserts by ascending rank, erases by descending rank.
   - A packet that can name a key finds it: a rule referencing an action or next hop never sees a missing target. A reader that reaches a new referrer and follows its reference finds the new referent or a newer one.
   - Deletions leave readable until a removal cascade has waited a grace period per rank, so a reader holding the old referrer still finds its referent.
   - A failure before publication is invisible. Nothing of a transaction is visible before its first publication step.
   - **Mixed generations are allowed**: while a multi-resource transaction publishes, a packet may see some of it applied and the rest not yet (a new referent under an old referrer, or the old classification beside the new actions). Use this level when every intermediate state is acceptable.
   - Zero worker pause during active transactions.
2. **Scope snapshot** (`CONSISTENCY_SCOPE_SNAPSHOT`; reported as `VISIBILITY_SCOPE_SNAPSHOT`). Only resources that are scope tables (`dataplane::ScopeResource`) can take part.
   - A *scope* is whatever a packet can name independently of the rules being replaced (a session, a policy group, a tenant); the application defines it. Its policy is one immutable `Version` value.
   - A transaction replaces each scope it touches from its complete old version to its complete new one by one pointer store. A packet operation binds the scope once (`ScopeTable::Lookup`, one acquire load per scope, not per table operation) and then sees one version in full, however many lookups it makes through it.
   - **No order between scopes**: two scopes in one transaction switch one after the other. The promise is per scope. Mutable state (meter tokens, counters) is not copied into a version; the version names it (a meter id) and the engine keeps it alive while any version names it.
   - Create new objects a version will name in a *referential* transaction first (nothing can name them yet), then switch the scope in a scope-snapshot transaction. A scope-snapshot request that includes any other resource (so it would become visible operation by operation) is refused with `Outcome::kUnsupported` in C++ and, over gRPC, `UNIMPLEMENTED` with error detail `UNSUPPORTED_TRANSACTION` (field `consistency`, object the resource, the reason in the message). Nothing is applied, nothing is recorded under the `request_id`, and a level the daemon does not know is `INVALID_ARGUMENT`.
   - A scope-snapshot transaction that cannot be fully prepared (a failed reservation, an exception, a missing referent) leaves every old scope visible.
   - Erasing a scope leaves its last version readable until the removal cascade, then `Lookup` returns null; a packet operation must handle a missing scope.
   - `VISIBILITY_ATOMIC` in the wire enum is deprecated and never sent. It promised "all at once", which nothing provides.

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
- **Experimental** (installed, may change without preserving source compatibility): the selected headers under `classifier/`, `dataplane/`, `flow/`, `meter/`, `rcu/`, `route/` and `stats/`. `flow/shared_flow_table.h` is not installed: it includes `classifier/concurrent_exact.h`, which is internal. Promotion to public is a decision recorded in `docs/decisions.md`.
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

Identifiers (`StrongId`) name objects through `SlotTable`/`ObjectTable`; an erased id is retired, not reused, until readers can no longer hold it (for example `Router` refuses to reuse a retiring `NextHopId`). A borrowed instance is an `InstanceLease`, which blocks `Destroy`. These rules bind batteries that hand out handles (flow ids, M9, implemented in `flow/`; handoff continuations, M11, implemented in `dataplane/`; hardware-offload handles, M20):

1. A handle that can outlive its object carries a generation; resolving a stale handle fails closed and never reaches a new object that reused the slot.
2. An asynchronous completion names the handle it was issued for and is checked against the current generation before it takes effect.
3. A handle is not dereferenced across a structural change (destroy, replace-scope) without re-resolving.
4. Resolution on the packet path is by cached pointer or index, never by name.

`FlowId`/`FlowHandle` (M9) is the first implementation; its tests show a stale handle failing to resolve after its slot is reused (`StaleHandleCannotReachAFlowThatReusedItsSlot`, worker and shared tables) and an expiry record unable to erase the flow that reused its slot (`IdleExpiryStale.ARecordThatOutlivedItsFlowCannotEraseTheFlowInItsSlot`, with the real expiry engine of M10; the engine's own `ExpiryHandle` carries a generation of its own, `ExpiryWheel.AStaleHandleCannotTouchTheTimerThatReusedItsNode`). `ContinuationHandle` (M11, `dataplane/continuation.h`) is the third: `ContinuationTableTest.StaleHandleCannotReachAContinuationThatReusedItsSlot` shows it for the table, and `HandoffThreadsTest.PuntServiceResumeFailsClosedForARetiredContinuation` shows it across threads: a worker retires continuations while their packets are with a service thread, and each of those packets comes back and fails closed. The remaining handle-issuing milestone (M20) must show the same test.

## 8. Battery Admission Criteria

A mechanism is admitted as a BESS library only if all of these hold. They follow from section 0 and the roadmap's layering, performance and correctness sections.

1. **Generic:** at least two different appliance models (for example a flow cache and a session compiler) would use it unchanged. It contains no appliance semantics (no PDR/FAR, VFP layer, OpenFlow action, VIP or firewall zone).
2. **Layered:** it fits the DAG in section 1 with no `module.h`, runtime or protobuf dependency, and a Meson library of its own once it exists.
3. **Three faces, one implementation:** a direct C++ library, a thin graph adapter in `modules/`, and, where control needs it, a binding outside the library. Bypassing the graph must stay possible.
4. **Explicit dependencies:** it receives the `RcuDomain` and other facilities by constructor; it never looks anything up per packet or through a global.
5. **Measured:** hot paths have a benchmark with a stated target, bytes per item and cache lines touched per lookup are reported, and a stable median regression above 3% in an existing deterministic benchmark is explained.
6. **Correct under concurrency:** worker ownership is explicit, lifetime follows sections 6 and 7, and the test set includes failure injection and a reference model where the state is non-trivial.
7. **Not a policy language:** it provides mechanism. If it needs a universal action object or rule syntax, it is out of scope.
