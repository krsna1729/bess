# BESS Performance and Efficiency Contract

This contract defines the performance, memory, and concurrency invariants for all core BESS components and networking libraries.

---

## 1. Fast-Path Execution Invariants

1. **Zero Allocation**:
   - The packet processing path (`ProcessBatch`, `RunTask`, `Lookup`, `EmitPacket`) must perform **zero heap allocations** (`malloc`, `new`, `std::vector` resize).
   - All batch workspaces and scratch buffers must be pre-allocated or stack-allocated within bounded limits (`kMaxBurst = 32`).
   - Flow tables (`flow::WorkerFlowTable`, `flow::SharedFlowTable`, M9) allocate only in `Create()`: capacity is fixed, there is no resize or rehash, and a full table refuses the create (`EmplaceStatus::kFull`) instead of growing. Lookup, create and erase of a worker-owned table allocate nothing.
   - The expiry engine (`dataplane::ExpiryWheel`, M10, D-053) allocates only in `Create()`; `Schedule`, `Refresh`, `Cancel` and `Poll` allocate nothing, a full engine refuses the arming (`kNoExpiry`), and `Poll` takes an explicit work budget: an expiry storm cannot monopolise a worker. Per-packet cost of keeping a flow alive is one store (owner-side `last_seen`) or one load-compare-store on a 32-byte node (`Refresh`); measured in D-053.

2. **Near-Specialized Assembly for Typed Paths**:
   - For typed hot paths (e.g. `TypedExactTable`, `RangeClassifier`, `ScopeTable::Lookup`), code generation must approach optimal hand-written assembly.
   - Branching and comparisons must use direct scalar or SIMD instructions (e.g., unsigned integer comparisons for port ranges, SSE2/AVX2 vector shifts for VLAN stripping and key extraction).

3. **Bound Genericity Outside the Packet Loop**:
   - Genericity (field extraction plans, hash seeds, schema layouts, output gate mappings) must be computed and baked at configuration, generation, or transaction commit time.
   - Inner per-packet loops must execute purely baked numeric offsets and pre-compiled mask operations.

4. **Zero Shared-Lock Contention**:
   - Worker threads must never acquire shared mutexes, spinlocks, or atomic read-modify-write loops during packet forwarding.
   - All reader synchronization relies on QSBR RCU (`bess::rcu::RcuDomain`) and acquire-load memory fences.
   - The one sanctioned exception is a *writer* of a table that was built to be written from workers: `ConcurrentExactTable` with `Writers::kShared` and `flow::SharedFlowTable` serialize creates and erases on one spinlock (D-028, D-052). Their readers take no lock and execute no read-modify-write. Choose such a table only where new flows are rare; otherwise partition into `WorkerFlowTable`s.

---

## 2. Memory and Cache-Line Budgets

1. **Cache-Line Touch Budget**:
   - **Classifier**: At most **1 cache line** read per packet in steady-state hash table lookups (1 bucket load).
   - **Routing**: Exactly **1 memory access** for IPv4 routes `/24` or shorter via DIR-24-8 `rte_lpm` `tbl24`.
   - **Metering & Accounting**: Per-worker arrays (`WorkerSlots`) guarantee that worker increments touch only cache lines exclusive to that worker's NUMA node, eliminating cross-core cache invalidation storms.
   - **Scope binding**: a packet operation reads a scope's whole policy with one acquire load (`ScopeTable::Lookup`, D-050) and routes every covered lookup through that immutable version; it adds a pointer load per scope, not a version check per table operation. Referential resources keep their lookup cost unchanged (nothing on their read path was touched). The load itself has not been benchmarked.
   - **Flow state** (M9, D-052): a `WorkerFlowTable` lookup reads one 64-byte index bucket (eight 16-bit tags and eight ids) and, only on a tag match, the slot record that holds the key, the generation and the start of the State (56 bytes for a 16-byte key with 32 bytes of State: one or two lines). A miss reads the index bucket alone unless that bucket has overflowed. `SharedFlowTable` adds the rte_hash directory's lines to the slot record's. Measured times, bytes per flow and the sizes that were not run are in D-052.

2. **Scale Bounds**:
   - Strong identifiers (`StrongId`) are scalar wrappers: trivially copyable, register-passable, the size of their representation, and compiled to the same code as the raw integer (checked for `InterfaceId`, D-049). An identifier that can appear in packet metadata or a hardware mark is 32 bits (`ActionId`, `NextHopId`, `InterfaceId`, `RouteDomainId`). `WorkerId` is 16 bits because it never leaves the process. `GenerationHandle<Id>` is an id plus a 32-bit generation: 8 bytes, one 64-bit compare.
   - Bounded packet buffers (`PacketStore`) must enforce strict global and per-flow capacity caps to prevent unconstrained memory ballooning under network congestion or paging delays.

---

## 3. Concurrency and Mutation Under Load

1. **Live Table Mutations**:
   - Tables supporting live mutation (e.g. `ConcurrentExactTable`, `ConcurrentMaskedTable`, `Router`) must apply single-entry inserts and deletions in-place with zero worker interruption.
   - Deleted entries must remain valid and readable until all worker threads report a quiescent state (`QSBR`), preventing use-after-free without locking.

2. **Transaction Footprint**:
   - Transaction preparation (`Reserve`) allocates all necessary staging descriptors ahead of time.
   - Transaction publication (`Publish`) is completely infallible, non-allocating, and finishes within a bounded execution window ($< 1\,\mu\text{s}$ per operation).

---

## 4. Benchmark Verification Methodology

1. **CPU Isolation**:
   - All official benchmark numbers must be recorded under core isolation via `omarchy-benchmark --cpu <N> --isolate --` (or strict `taskset`).
   - Benchmarks must disable CPU frequency scaling governors (`performance` mode) and offline unused SMT siblings to eliminate thermal and frequency noise.

2. **Drift-Cancelling Protocol (ABBA)**:
   - Comparative performance evaluations between versions or implementations must use `tools/ab_bench.py` running in paired ABBA sequence to eliminate ambient temperature and system drift.

3. **Burst Scaling**:
   - Batch scaling across burst boundaries ($16, 32, 64, 128, 256$) must be measured using `tools/batch_sweep.py` to observe cache residency transitions.
