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

`check_includes.py` judges the target an include names: quoted includes resolve the way the compiler does (next to the including file, then under the `core/` include root), while angle includes search the include root directly. Every `..` path component is refused in both forms; there is no grandfather list (D-058's 359 legacy `..` includes were respelled root-relative, each to the same resolved header). The self-test runs as the Meson test `check_layer_includes_self_test` and in the `layers` CI step. New public includes stay root-relative (`"dataplane/strong_id.h"`); the `<bess/...>` spelling is a recorded non-goal for now (D-058).

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

Every installed header is **public**, **experimental** or **internal**. Packaging enforces it, not naming: `core/meson.build` installs a named list of headers (no recursive install), `protobuf/meson.build` installs only the generated `pb/*.pb.h` that list needs, and `tools/api_classes.json` classifies every installed file as `public` or `experimental`. `tools/check_installed_headers.py` runs against the staged install (`tools/ci_profile.py verify-install`) and fails if the installed set differs from that table in either direction (an unclassified header installed, a classified one missing), if an internal file (`runtime/`, `control/`, `*.grpc.pb.h`, `.pb.cc`, the control and test protocols, ...) is installed, if a **public** header includes an **experimental** one (an experimental type must not leak into the public contract), or if the generated headers installed are not exactly the closure the public headers include (`tools/public_proto_closure.py`, also a Meson test). Promoting or demoting a header edits the table and the install list in the same change, with a decision record.

- **Public** (supported source API for compatible BESS releases; plugins are rebuilt against the target release, the C++ ABI is not promised): the module, packet and port headers at `bess/core/`, `framework/module_init_context.h`, `framework/instance_registry.h`, `framework/plugin.h`, and the `utils/` helpers in the install manifest.
- **Experimental** (installed, may change without preserving source compatibility): the selected headers under `classifier/`, `dataplane/`, `flow/`, `meter/`, `rcu/`, `route/` and `stats/`, and the wire codec facade `framework/resource_codec.h` and `framework/resource_bindings.h` (D-094). `flow/shared_flow_table.h` is installed through `flow/shared_exact_index.h`, a boundary that keeps its backend (`classifier/concurrent_exact.h`) internal (D-074). Promotion to public is a decision recorded in `docs/decisions.md`.
- **Internal** (never installed): `runtime/`, `control/`, `drivers/`, `gate_hooks/`, `resume_hooks/`, the transaction engine, scheduler and daemon startup headers, tests and benchmarks.

`docs/plugin-api.md` is the plugin author's view of the same table. The roadmap's longer list of likely-public headers (typed classifier APIs, `StrongId`, `SlotTable`, meter and stats facades) stays experimental until each is promoted by a decision record.

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

## 9. Control Plane and Dataplane Responsibilities

Two kinds of thread run BESS code, and each owns different things. This is what the code does today (`core/control`, `core/worker.h`, `core/scheduler.h`, `core/dataplane/transaction_engine.h`); the layering of section 1 is what keeps it that way.

**The control plane owns structure and the single write path.**
- One lock (`ControlPlane`'s non-recursive mutex) serializes every structural change, every module command and every transaction. The gRPC services hold no lock of their own and no worker ever takes this lock: nothing in `worker.cc`, `scheduler.h` or `module.cc` includes `control/`.
- It creates and destroys ports, modules, gates, workers, traffic classes and hooks, loads plugins, validates desired state (`ValidatePipeline`, `DiffPipeline`, `PlanPipeline`), and applies it with a recorded generation (`ApplyPipeline`; a no-op apply neither pauses workers nor bumps the generation).
- It owns the wire. Protobuf, gRPC and error mapping stop at `control/`; `ControlPlane` itself takes plain C++ values.
- It is the only caller of `TransactionEngine::Apply`. A dataplane transaction arrives at `ControlV2Service::ApplyTransaction`, takes the control lock, is decoded through each resource's bound codec, applied by the engine with the semantics of section 3, and recorded under its `request_id` (`DataplaneTransactions`: a replay returns the recorded outcome, the same id with other contents is a conflict).
- It destroys what the dataplane stopped using: the engine retires replaced and erased objects against an RCU grace period and frees them on the control thread (`ReclaimRetired()`, run inside `Apply`), never on a worker (section 6).

**Workers own the packet path and worker-local state.**
- A worker runs scheduler tasks to completion. Between task invocations (by elapsed time, or by round count) it reports quiescence to the `RcuDomain`; an idle worker still does, so reclamation never waits on traffic.
- It reads published state only through what the publisher made safe for it: an `RcuPtr` or `ScopeTable::Lookup` pointer valid until its next quiescent state, or a pointer cached at `Init`/resume. It never resolves a name, takes a control lock, or waits for the control plane.
- It owns mutable high-frequency state itself (counters, meter tokens, worker-local flow tables, expiry wheels), identified by worker id passed in explicitly (section 4, no global reach-through). It never destroys a published object.

**How the two meet.**
- *Structural changes* (connecting modules, creating or destroying a module a worker reaches, moving a traffic class, a port) mutate non-RCU structures, so the control plane brackets them with a `WorkerPauser`, which parks the running workers and restarts those it parked. Most operations pause; some check first and pause only if an active worker can reach the objects (`ConnectModules`: "only pause when absolutely required").
- *Resource transactions* (tables, scopes, meters named by id) do not pause anything: they publish by pointer store and retire through RCU, one transaction at a time (section 3).
- *Resource bindings* keep the wire out of the dataplane (D-044). A `dataplane::Resource` knows nothing about encoding; the module that registers it binds a codec to it through `init_context().codecs()` (`framework/resource_bindings.h`, experimental and installed, so a plugin's resources are reachable too, D-094), and the control plane reads that binding to decode transactions and list resources. A resource with no binding is not reachable over RPC. The binding is removed with its handle, so a resource freed and reallocated at the same address cannot inherit a stale codec; declare the binding after the resource and reset it first.

What this forbids: control code on a worker, a worker calling into `control/`, a reusable library naming the control plane (section 2, rules 1-3), and a module freeing a published object directly instead of retiring it.

## 10. Fast-Path Invariants

Packet-path code runs in bounded batches. It must not allocate or wait for the
control plane; state and workspaces are prepared before the packet loop. Shared
mutable state requires an explicit synchronization design; worker-owned state is
preferred when the workload permits it. Keep typed operations close to their
machine representation and avoid unnecessary memory indirection.

These are architectural constraints, not fixed cache-line or instruction-count
promises. The backend-specific measurements and targets are in
`docs/performance-contract.md`.

