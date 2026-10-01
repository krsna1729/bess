# BESS Performance and Efficiency Contract

This contract defines the performance, memory, and concurrency invariants for all core BESS components and networking libraries.

---

## 1. Fast-Path Execution Invariants

1. **Zero Allocation**:
   - The packet processing path (`ProcessBatch`, `RunTask`, `Lookup`, `EmitPacket`) must perform **zero heap allocations** (`malloc`, `new`, `std::vector` resize).
   - All batch workspaces and scratch buffers must be pre-allocated or stack-allocated within bounded limits (`kMaxBurst = 32`).

2. **Near-Specialized Assembly for Typed Paths**:
   - For typed hot paths (e.g. `TypedExactTable`, `RangeClassifier`, `ScopeCell`), code generation must approach optimal hand-written assembly.
   - Branching and comparisons must use direct scalar or SIMD instructions (e.g., unsigned integer comparisons for port ranges, SSE2/AVX2 vector shifts for VLAN stripping and key extraction).

3. **Bound Genericity Outside the Packet Loop**:
   - Genericity (field extraction plans, hash seeds, schema layouts, output gate mappings) must be computed and baked at configuration, generation, or transaction commit time.
   - Inner per-packet loops must execute purely baked numeric offsets and pre-compiled mask operations.

4. **Zero Shared-Lock Contention**:
   - Worker threads must never acquire shared mutexes, spinlocks, or atomic read-modify-write loops during packet forwarding.
   - All reader synchronization relies on QSBR RCU (`bess::rcu::RcuDomain`) and acquire-load memory fences.

---

## 2. Memory and Cache-Line Budgets

1. **Cache-Line Touch Budget**:
   - **Classifier**: At most **1 cache line** read per packet in steady-state hash table lookups (1 bucket load).
   - **Routing**: Exactly **1 memory access** for IPv4 routes `/24` or shorter via DIR-24-8 `rte_lpm` `tbl24`.
   - **Metering & Accounting**: Per-worker arrays (`WorkerSlots`) guarantee that worker increments touch only cache lines exclusive to that worker's NUMA node, eliminating cross-core cache invalidation storms.
   - **Session Action**: `ScopeCell` packs `{meter_id, next_hop_id}` into a single 64-bit word ($8\,\text{bytes}$), reading both continuation fields in a single memory access.

2. **Scale Bounds**:
   - Strong identifiers (`StrongId`) must be strictly 32-bit scalar wrappers (`sizeof(Id) == 4`), trivially copyable and register-passable.
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
