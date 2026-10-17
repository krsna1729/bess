# BESS Performance and Efficiency Contract

This contract defines the performance, memory, and concurrency invariants for all core BESS components and networking libraries.

---

## 1. Fast-Path Execution Invariants

1. **Zero Allocation**:
   - The packet processing path (`ProcessBatch`, `RunTask`, `Lookup`, `EmitPacket`) must perform **zero heap allocations** (`malloc`, `new`, `std::vector` resize).
   - All batch workspaces and scratch buffers must be pre-allocated or stack-allocated within bounded limits (`kMaxBurst = 32`).
   - Flow tables (`flow::WorkerFlowTable`, `flow::SharedFlowTable`, M9) allocate only in `Create()`: capacity is fixed, there is no resize or rehash, and a full table refuses the create (`EmplaceStatus::kFull`) instead of growing; the shared table also refuses, as `kPlacementFailed`, a key its rte_hash directory cannot place although slots are free (D-056). Lookup, create and erase of a worker-owned table allocate nothing.
   - The expiry engine (`dataplane::ExpiryWheel`, M10, D-053) allocates only in `Create()`; `Schedule`, `Refresh`, `Cancel` and `Poll` allocate nothing, a full engine refuses the arming (`kNoExpiry`), and `Poll` takes an explicit work budget: an expiry storm cannot monopolise a worker. Per-packet cost of keeping a flow alive is one store (owner-side `last_seen`) or one load-compare-store on a 32-byte node (`Refresh`); measured in D-053.
   - Handoff channels (`dataplane::HandoffChannel`, M11, D-054) and `dataplane::ContinuationTable` allocate only in `Create()`: one block per object (the channel's from DPDK's heap on the requested NUMA node), nothing afterwards. `TryPuntBurst`, `Dequeue`, `Close`, `Drain` and `Resolve` allocate nothing (`NothingAllocatesAfterCreate` in both test files), a full channel refuses and says so instead of growing (`PuntResult::refused`, the refused items stay with the producer), and a producer's loop never blocks on one.

2. **Near-Specialized Assembly for Typed Paths**:
   - For typed hot paths (e.g. `TypedExactTable`, `RangeClassifier`, `ScopeTable::Lookup`), code generation must approach optimal hand-written assembly.
   - Branching and comparisons must use direct scalar or SIMD instructions (e.g., unsigned integer comparisons for port ranges, 16-byte vector shifts for VLAN stripping (`core/arch/vlan.h`) and key extraction).
   - `docs/assembly-shape.md` is the review of what `core/arch`'s kernels actually compile to under the release flags, kernel by kernel, against each header's claim.

3. **Bound Genericity Outside the Packet Loop**:
   - Genericity (field extraction plans, hash seeds, schema layouts, output gate mappings) must be computed and baked at configuration, generation, or transaction commit time.
   - Inner per-packet loops must execute purely baked numeric offsets and pre-compiled mask operations.

4. **Zero Shared-Lock Contention**:
   - Worker threads must never acquire shared mutexes, spinlocks, or atomic read-modify-write loops during packet forwarding.
   - All reader synchronization relies on QSBR RCU (`bess::rcu::RcuDomain`) and acquire-load memory fences.
   - One sanctioned exception is a *writer* of a table that was built to be written from workers: `ConcurrentExactTable` with `Writers::kShared` and `flow::SharedFlowTable` serialize creates and erases on one spinlock (D-028, D-052). Their readers take no lock and execute no read-modify-write. Choose such a table only where new flows are rare; otherwise partition into `WorkerFlowTable`s.
   - A multi-producer or multi-consumer handoff channel (`HandoffTopology::kMpSc`, `kMpMc`, D-054) is a second explicit exception: its enqueue and dequeue are the ring's compare-and-swap on the shared head, bound at creation and never inspected per call, and its refusal path adds a shared counter increment. `kSpSc`, the topology of one punting worker and one service thread, executes no read-modify-write and no lock on its hot path.

---

## 2. Memory and Cache-Line Budgets

1. **Cache-Line Footprint**:
   - The invariant is *no avoidable memory indirection on the packet path*. The cache-line footprint of each lookup kernel is characterised in that kernel's decision record, and it is a measured or analysed target of that backend, not a global cap: a representation change may legitimately move it, and a number here is not a promise to callers.
   - **Classifier** (`rte_hash`-backed exact match): a hit reads the signature bucket line and then the key/value slot line (two lines at least, by the layout of `rte_hash`); a miss reads the bucket line alone. Not counted with hardware counters.
   - **Routing**: the `rte_lpm` `tbl24` probe for an IPv4 route of `/24` or shorter is one table load. Resolving the next hop adds the `NextHop` table load, so a full route resolution reads at least two lines; longer prefixes add a `tbl8` load.
   - **Metering & Accounting**: Per-worker arrays (`WorkerSlots`) guarantee that worker increments touch only cache lines exclusive to that worker's NUMA node, eliminating cross-core cache invalidation storms.
   - **Scope binding**: a packet operation reads a scope's whole policy with one acquire load (`ScopeTable::Lookup`, D-050) and routes every covered lookup through that immutable version; it adds a pointer load per scope, not a version check per table operation. Referential resources keep their lookup cost unchanged (nothing on their read path was touched). The load itself has not been benchmarked.
   - **Flow state** (M9, D-052): a `WorkerFlowTable` lookup reads one 64-byte index bucket (eight 16-bit tags and eight ids) and, only on a tag match, the slot record that holds the key, the generation and the start of the State (56 bytes for a 16-byte key with 32 bytes of State: one or two lines). A miss reads the index bucket alone unless that bucket has overflowed. `SharedFlowTable` adds the rte_hash directory's lines to the slot record's, and a hit reads the slot's generation (one acquire load, a plain load on x86) before it forms the State pointer, so a flow is visible for all its keys at once (D-056; the cost is measured there). Measured times, bytes per flow and the sizes that were not run are in D-052.

2. **Scale Bounds**:
   - Strong identifiers (`StrongId`) are scalar wrappers: trivially copyable, register-passable, the size of their representation, and compiled to the same code as the raw integer (checked for `InterfaceId`, D-049). An identifier is as narrow as its range allows when it is stored in a hot per-packet structure, and 32 bits when it is a handle's slot or can appear in packet metadata or a hardware mark (`ActionId`, `NextHopId`, `RouteDomainId`). `InterfaceId` is 16 bits because every `route::NextHop` holds one and 32 bits made a next hop 20 bytes (D-060); a runtime that puts an interface in a hardware mark widens it at that boundary. `WorkerId` is 16 bits because it never leaves the process. `GenerationHandle<Id>` is a 32-bit id plus a 32-bit generation: 8 bytes, one 64-bit compare.
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

## 4. Update-Under-Load Matrix

Roadmap §23.1 Gate C and Appendix J ("update-under-load matrix complete"). One row per mutable structure with recorded evidence; every number is quoted from the cited record. Host: the records' i9-13900H laptop (P = P-core, E = E-core; D-052, `docs/baselines/m0-baseline.json` metadata). Isolation, governor and run count differ per record, so compare rows only through their source. "not measured" means no record has the number.

| structure | lookup cost without updates | with updates at a nominal rate | at maximum rate | writer throughput | p50/p99 | reclamation backlog / BUSY frequency | source |
|---|---|---|---|---|---|---|---|
| ExactMatch rule → action → meter chain, in-process transactions (`BM_LookupsUnderTransactions`, 65K sessions half live, readers quiesce every batch; M lookups/s per reader, 1 / 4 readers) | P 56.5 / 49.3 (197 total); E 33.6 / 29.2 (117 total) | 10K sessions/s (~120K table ops/s): P 52.8 (−7%) / 47.2 (−4%), E 32.9 (−2%) / 28.3 (−3%). 100K sessions/s (~1.2M table ops/s): P 50.1 (−11%) / 45.6 (−8%), E 31.6 (−6%) / 27.1 (−7%) | writer flat out: P 46.5 (−18%) / 44.8 (−9%); E 31.0 (−8%) / 27.8 (−5%) | P 241K / 194K sessions/s; E 168K / 117K sessions/s. One-operation transaction 307 ns vs 44 ns direct table write before the amendment-4 trim (−24% after) | not measured | backlog not measured; "No retries: reclamation kept up" (amendment-3 run) | D-021 cost table, amendments 3-4 |
| ExactMatch through two modules, one transaction per session (`BM_ClassifyUnderTransactions`, 32K live sessions; Mpps per reader, 1 / 4 readers) | P 42.7 / 31.3; E 28.7 / 18.6 | 10K sessions/s: P 43.2 / 28.0, E 30.4 / 22.2. 100K sessions/s (200K rule changes/s): P 44.9 / 29.6, E 30.9 / 22.5 | flat out: P 37.6 (−12%) / 28.2 (−10%); E 29.9 / 21.5 | P 695K / 519K sessions/s; E 627K / 465K sessions/s | not measured | backlog not measured; "No transaction was refused busy" | D-022 |
| ExactMatch rules over the transaction RPC, live bessd (one worker, isolated two-CPU bessd, `live_transaction_bench`; these runs used its Python predecessor, D-139) | 31.0-33.1 Mpps (median 31.6) | 1,000 tx/s: 31.1-32.9 Mpps; 3,000 tx/s: 31.0-32.4 Mpps | 30.5-31.6 Mpps at 9.0-9.9K tx/s | 9.0-9.9K tx/s; 4,073 tx/s through the SDK on a fast (non-release) build (D-088) | client p99 0.2-0.3 ms (1.2-1.5 ms before D-027); p50 not measured | not measured (`bess_transactions_total{outcome="busy"}` counts it, D-076/D-088; no frequency recorded) | D-027, D-088 |
| Shared exact table, mode C (`rte_hash` LF + QSBR, 8-byte keys; 4 workers + 1 writer, P; total Mlookups/s) | 1M entries 232.6; 10M 134.6; E-cores 10M 72.6 | 1M mods/s: 1M entries 194.5 (0.995M applied), 4M 128.7 (0.990M), 10M 111.8 (0.981M); E-cores 10M 81.9 | 1M entries 248.9 at 4.5M mods/s; 4M 126.8 at 4.2M; 10M 104.9 at 4.3M | one writer 4.2-4.5M ops/s with 4 readers (14-24M with none); E 3.2M; single-thread LF churn 52-270 ns per op at 1K-10M entries | not measured (per-op means only) | 1K-entry table, 25% headroom: 231K stalls at 1M mods/s; none from 64K. At 4.3M ops/s ≈47 pending deletes at p99 grace, ≈540 at worst (D-010). 1K entries with a reader holding grace: 17% of adds fail at 95% load (quiescent every 64 ops), 75% at 75% load (every 1,024 ops); none at 64K (D-010) | D-004, D-010, MODERNIZATION §14.5 "G1.2a experiment results" E1/E2 |
| Worker-owned partitions, mode W (same harness as the row above) | 1M entries 229.5; 10M 87.8 | 1M mods/s: 1M entries 185.8 (0.993M applied), 4M 89.5, 10M 80.6 | 1M entries 109.0 at 36.6M ops/s; 4M 14.2 at 36.5M; 10M 10.1 at 34.3M (rings always full: workers spend their time applying) | 34-148M ops/s aggregate apply (P); 10-41M (E) | not measured | none by design: workers free deleted slots at once, no grace period | D-004, MODERNIZATION §14.5 E2 |
| Packet-path writers: `ConcurrentExactTable` `kShared` vs partitioned (`BM_PacketPathWriters`, each thread inserts, erases its oldest, looks up 32-key batches; P, 4 threads, inserts/s (lookups/s)) | not measured | 0.4% new flows: partitioned 1.54M (395M), shared 1.27M (325M), DPDK multi-writer 1.03M (262M); E 0.93M vs 0.81M | 3% new flows (highest recorded): partitioned 6.5M (209M), shared 1.9M (62M), DPDK multi-writer 2.0M (65M); E partitioned 4.9M, shared 1.6-1.9M | 1 thread, 3%: shared 2.9M (93M) = partitioned 2.9M (94M) | not measured | not measured | D-028 |
| `WorkerFlowTable` churn (`BM_WorkerChurn`, 16 B key, 32 B State, batch 32) | uniform hit 6.3 ns (64K), 19.4 ns (1M) | 1% of ops a create + erase: 8.7 ns/op (64K), 29.3 (1M); 10%: 9.5 / 31.3 | flat out 36.5M create+erase pairs/s, 13.7 ns/op (64K); 21.4M pairs/s (1M) | as the max column; two-key (alias) flow create+erase 49.7 ns (64K), 154 ns (1M) | run alone, ns: 64K create 15 / 32, erase 19 / 35; 1M create 26 / 174, erase 33 / 173; p99.9 45 (64K), 310-312 (1M) | not measured | D-052 |
| `SharedFlowTable` (`BM_SharedReaders` / `BM_SharedReaderWriter`, batch 32 uniform hit; P, M lookups/s total, 1 / 2 / 4 readers) | 64K 105 / 216 / 416; 1M 35 / 70 / 129 | not measured | one writer cycling 4,096 flows flat out: readers −22-24% at 64K and −4-6% at 1M (P), −2-16% and −3-6% (E). As shipped after D-056 (65,536 flows): readers 3-16% lower than before D-056, depending on the statistic (provisional) | 1.8-4.7M ops/s (D-052); +16% after D-056's layout change | 424-1,662 / 794-3,184 TSC ticks per op (0.14-1.1 µs) (D-052); create/erase latency −16-22% after D-056 | reclamation backlog peaked at 2,560-6,400 erased flows | D-052, D-056 |
| `Router`, route domains (32-wide `ResolveBatch`, reader CPU time) | 45.7 ns per batch | ~1.4M in-place `SetRoute`/`RemoveRoute` per s in the same domain: 48-49 ns; ~340 `ReplaceRouteSetAtomic` swaps/s in another domain: 48 ns | not measured | in-place route change 0.53 µs (1K routes), 7.96 µs (16K), 32.7 µs (64K); `SetNextHop` republish 1.09 µs (1,024 next hops), 80.7 µs (65,536) (K7). Enrolled, 1,024 routes, P: next-hop update 62 ns direct / 228 ns transaction, route re-point 221 / 591 ns, route add+remove 293 / 744 ns (D-023). `CreateDomain`+`RemoveDomain` 1.56 ms | not measured | not measured | D-023, D-046, MODERNIZATION §10 K7 |
| WildcardMatch, tuple space (ns per 32 keys, P CPU 2) | 1 tuple: 253 (1K rules), 556 (1M); 4 tuples: 1,288 (1K), 1,716 (1M); 8 tuples: 1,857 (1K), 2,463 (1M) | not measured | not measured | ~1 µs per add+delete at 1K-100K rules, 1-8 masks | not measured | not measured | D-014, D-024 |
| RCU grace-period churn (64-byte Source → Bypass → count, 10 µs quiescence cadence) | 233-239 Mpps, any cadence, no token churn | not measured | control thread starting 1M grace periods/s: 238 Mpps (1 worker), 648 Mpps (4 workers) | 1M grace periods/s (as driven) | grace period µs: 5.3 / 10.3 (1 pipeline), 8.7 / 11.3 (4 pipelines), 5.8 / 11.0 (module burning 10K cycles/batch), 299 / 324 (1M cycles/batch); p99 ≈11, p99.9 ≤60, max 125 over 1-4 pipelines (D-010) | not measured | D-010, D-012 |
| Expiry refresh over a `WorkerFlowTable` (`flow_expiry_bench`, every packet refreshes) | lookup only: 4.63 ns (1K flows), 24.5 ns (1M) | owner-side store 4.61 / 25.1 ns; wheel lazy refresh 6.06 / 41.8 ns; eager 10.13 / 76.8 ns | not measured | wheel ns, schedule / cancel / uniform refresh / drain per expiry: 64K 7.5 / 5.4 / 3.7 / 45.0; 1M 10.6 / 7.9 / 14.9 / 164.8; 10M 9.8 / 5.6 / 22.9 / 243.2 | not measured; worst budgeted (64-unit) poll 5.8 µs (64K), 342 µs (1M, one sample), 52 µs (10M) | not measured | D-053 and addendum, `docs/baselines/m10-expiry.json` |
| Decision cache, generation invalidation (ns per lookup, scalar / batch 32) | uniform hit 7.97 / 5.87 (64K), 35.85 / 19.55 (1M) | after one `Invalidate()` with half the entries stale: 16.51 / 9.32 (64K), 41.83 / 21.32 (1M) | not measured | `Invalidate()` is one atomic increment (65,536 entries made stale without touching them); install after a policy change 33 ns (64K), 63 ns (1M) | not measured | not measured (stale entries keep their slots until reinstalled or erased; no sweep) | D-062 |
| Handoff channel, SP/SC (`handoff_bench`, ns per burst at b = 1 / 8 / 16 / 32; powersave, loaded desktop) | round trip, no context: 234 / 230 / 245 / 316 | not measured | producer offering to a full channel: 4.3 / 8.6 / 17.6 ns per burst of 1 / 8 / 32; one refused call 2.4 ns (SP/SC), 6.6 ns (MP/SC) | one thread, enqueue + dequeue, b = 1 / 8 / 32: SP/SC 6.2 / 9.0 / 18.5; MP/SC 13.4 / 19.5 / 25.3; MP/MC 22.9 / 28.5 / 34.4 | not measured | refusal frequency not measured (`refused_full` counted; Queue posts `bess.queue_full`, D-089) | D-054, MODERNIZATION entry 146 |
| L2 FDB learning, owned (`Fdb` over `MacTable`; `PackedMacTable<Owner>` for Bridge) | `MacTable` batch 32 uniform hit 3.51 / 4.18 / 6.10 ns (1K / 64K / 1M); `PackedMacTable` −28.8% / −40.7% / −44.1% of that (D-073) | not measured (lookups were not run interleaved with learning) | not measured | learn (half new, at the learn limit) 13.2 / 16.8 / 64.8 ns at 1K / 64K / 1M; `PackedMacTable` learn −10.3% / −10.3% / −8.7%; aging 27 ns per entry (64K expiring at once), 107 ns (1M) | not measured | `MultiWriter` `Learn` returns `kBusy` on a failed `TryLock`; frequency not measured | D-064, D-073 |
| L2Forward, `PackedMacTable<SingleWriter>` (command-thread writer, worker readers) | batch 32 vs the deleted `l2_table`: hot −20.4 / −33.5 / −32.3%, uniform −17.0 / −32.7 / −15.3%, miss −33.0 / −43.2 / −40.3% (1K / 64K / 1M) | not measured | not measured | not measured | not measured | not measured | D-073 |
| NAT bindings, owned, growable (`nat_bench`, module path batch 32, outbound) | 16.4 ns (4K mappings), 18.7 ns (64K), 37.1 ns (1M) | not measured (growth migrates 64 slots per batch; no timing during a migration) | not measured | new mapping (allocate, bind, schedule, rewrite) 59 ns; `PortPool` allocation 5.1 ns (10% taken) to 12.3 ns (99%) | not measured | not measured (`bess.table_full`, `bess.nat_ports_exhausted` events, D-089; no frequency recorded) | D-068, D-078 |
| Shared NAT (`SharedFlowTable` store; creates under one spinlock) | one worker, `BM_TranslateShared` vs owned NAT: +32% (4K), +47% (64K), +34% (1M) per packet | growth 64 → 4,096 on the control thread while workers translate: 6 growths, `dropped_in_growth` 0 (test; no timing) | not measured | not measured (multi-worker creates) | not measured | not measured | D-079 |
| Conntrack, owned (`conntrack_bench`, `Track`) | TCP ACK, established: 14.69 ns (1,024 conns), 27.28 (65,536), 147.78 (1,048,576); `TrackBatch` at 1M 205 → 75 ns | TCP handshake + close (each connection created and closed), per packet: 17.66 ns (1,024), 20.68 (65,536) | not measured | expire 31.17 ns per connection (65,536 at once) | not measured | not measured | D-067 |
| Shared conntrack (per-connection lock, creates under the tracker lock) | not measured | not measured (test: 4 workers create 4,000 connections at once, each exactly once) | not measured | not measured | not measured | not measured | D-081 |
| Shared meter vs per-thread meters (packet-path writes; ns per check, total M checks/s) | exclusive `MeterState::Check` 5.42 ns | one shared meter, 2 threads: 74.2 ns, 27.0 M/s (exclusive per thread: 8.77 ns, 228.0 M/s) | 4 threads: shared 429 ns, 9.3 M/s; exclusive 8.86 ns, 451.4 M/s | as the max column | not measured | not applicable (no retirement) | MODERNIZATION §10 K5 |
| Worker-local counters vs a shared counter line (3 counters per batch) | worker `Update` 1.25 ns | 2 threads: worker-local 1,677 M/s total; shared `fetch_add` 51.9 M/s | 4 threads: 3,374 M/s vs 41.7 M/s | as the max column | not measured | not applicable | MODERNIZATION §10 K6 |

**Gaps against Gate C** (packet throughput with no updates, nominal rate, target high rate, writer throughput, update p50/p99, reclamation backlog, busy/backpressure frequency):

- No structure has all seven. The none / nominal / maximum sweep exists only for ExactMatch under transactions (D-021, D-022, D-027) and the mode C and W tables (§14.5 E2).
- Packets through a live graph under updates: only D-027 (one worker, ExactMatch, ≤9.9K tx/s). D-021 amendment 2's "packet Mpps under 1M modifications/s with workers online" and the live ExactMatch → Action → Meter → Router gate under updates (D-021 amendment 5, D-025 "Deferred") are not recorded.
- Update p50/p99: none for in-process transactions (D-021 amendment 2 lists it), Router, WildcardMatch, NAT, conntrack, FDB, decision cache or handoff. Recorded only for the flow tables (D-052, D-056) and the live client p99 (D-027).
- Time spent waiting for grace periods per update (D-021 amendment 2): not measured.
- Reclamation backlog: recorded only for `SharedFlowTable` and as pending-slot arithmetic for mode C (D-010). None under transactions, although the RCU backlog is exported (D-076) and evented (`bess.rcu_backlog`, D-089).
- Busy/backpressure frequency: no recorded rate of transaction `kBusy`, `Fdb::Learn` `kBusy`, handoff `refused_full`, `bess.table_full` or `bess.nat_ports_exhausted`. Recorded only as "none refused busy" (D-022), "no retries" (D-021 amendment 3) and the E2 stall count.
- Target high update rate: defined only for sessions (MODERNIZATION §14.5: 100K sessions/s; 1M modifications/s at 30 Mpps). No other structure has a stated nominal or target rate.
- Not measured under load at all: WildcardMatch, next-hop groups and neighbor updates (D-065), shared NAT on more than one worker, shared conntrack, NAT growth migration while traffic flows, FDB learning with concurrent lookups, L2Forward command writes, action and meter `SlotTable`s alone, route domain lifecycle (refused once enrolled, D-046).
- `SharedFlowTable` churn ran at 65,536 flows only, one writer; 1M flows and E-cores under churn were not run after D-056.
- Every row is from one laptop; D-004 asks for a server-class re-run. Several rows are single runs or flagged contaminated in their record.

---

### The live API, measured (2026-10-16)

Everything above is a bench binary calling the machinery in-process. The live
path -- a client, gRPC, protobuf, the daemon's control thread -- is a different
number, and it was measured on this host (20 lcores, `bessd 612cbff1`, `-m 0`
so no hugepages) with `tests/perf/live_transaction_bench`, 512 sessions, three
rounds:

| measurement | result |
|---|---|
| transactions per second at a 10,000/s target | 6,337 / 5,548 / 6,204 |
| the same at max | 5,927 / 5,267 / 6,404 |
| latency at 1,000 / 10,000 transactions per second | p50 193-240 / 106-134 us; p99 908-958 us |
| packet rate at 0 / 1,000 / 10,000 / max per second | 26.7 / 24.4 / 14.1 / 14.3 Mpps |

Two facts follow. First, the live rate is wire-bound, not engine-bound: a
transaction's p50 is about 110 us, so one synchronous client cannot ask for much
more than 9,000/s, and the measured 6,300/s is that ceiling with the daemon's
work on top. The 695K sessions/s of the row above is the same machinery with no
wire in the path; the two numbers differ by the API, not by a 110x regression.
Second, the packet-rate row above is not a cost of transactions: it is the
control thread stealing a worker's core-see the next table. Re-run as a rate
sweep with both placements (`--rates 0,1000,10000,max`, 512 sessions, 7 s a
point, client on an E-core, `powersave`, a fresh daemon per arm):

| churn target | 0 | 1,000/s | 10,000/s | max |
|---|---|---|---|---|
| Mpps, worker on the control CPU's SMT sibling | 27.56 | 25.92 | 22.35 (4,143 tx/s) | 22.32 (4,096 tx/s) |
| Mpps, worker on its own physical core | 26.84 | 27.01 | 27.62 (4,592 tx/s) | 27.66 (4,588 tx/s) |
| client p50 with separate cores | - | 230 us | 177 us | 178 us |

With a core each, the packet path is flat to the client's own ceiling (about
4,600 transactions/s) and the client's latency is better, so nothing is being
traded. The "every transaction costs the datapath about 80 us" this section used
to state (1,000/s -> -8%, 6,000/s -> -47%) was the co-located arm's shape; its
corollary -- that the cost is a fixed price per publication rather than per rule
rebuilt -- was measured under that placement and is withdrawn until re-measured.

**And the cost is contention with the writer, not the engine.** Measured on a
four-CPU confinement (`taskset -c 2-6`, worker on CPU 3, powersave), packets
alone against packets plus a 10,000/s churn (5,620 transactions/s achieved),
with user-space hardware counters on the daemon's whole process:

| | packets only | plus churn | per packet |
|---|---|---|---|
| packet rate | 24.23 Mpps | 17.65 Mpps | -27% |
| instructions | 61.10 G / 7.05 s | 53.39 G / 7.03 s | 358 -> 430 (+20%) |
| cycles | 26.52 G | 36.14 G | 155 -> 291 (+88%) |
| IPC | 2.30 | 1.48 | -36% |

**That table is SMT co-location, not a memory effect.** The daemon placed the
worker on CPU 3, the SMT sibling of its own control CPU 2, and said so
(`worker 0: worker CPU 3 shares a physical core with control CPU 2`). The same
two phases re-run with the two on different physical cores (`taskset -c 2,4`,
worker CPU 4, client on an E-core, `powersave`, user-space counters on the
daemon's process, 512 rules) remove the effect entirely:

| | packets only | plus a 10,000/s churn |
|---|---|---|
| worker on the control CPU's SMT sibling | 27.19 Mpps | 21.39 Mpps (3,561 tx/s, p50 239 us) |
| worker on a different physical core | 26.83 Mpps | 27.31 Mpps (4,117 tx/s, p50 202 us) |

On separate cores the packet path does not move -- and the client's latency
improves, so the comparison is not won by doing less work. The hardware-counter
table's IPC drop (2.30 without churn to 1.48 with it) is the worker sharing one
core's issue slots and caches with a busy writer. (The counters cover the whole
window, the packet rate only the client's run, so that table's per-packet column
is a ratio, not a measurement.) So the cost of a control-plane write on the
datapath, on one
worker, at a few thousand transactions a second, is below the noise floor once
the writer has a core of its own; what is *not* measured is many workers reading
one table while it is written, which is the case a partitioned layout would have
to win. One loose end closed: the 80 us figure came from this same harness, placed the
same way, so it is the co-located shape and not a property of transactions. One
lesson: the daemon honoured the harness's explicit `--worker-core 3` -- D-123
says an explicit core is the caller's to honour, never the placement's -- and
warned about it; without an explicit core its SMT-striped order takes a free
core's first thread (CPU 4 here), which `numa_placement_test.cc` pins
(`StripedWholeCoresOnePerCore`, `ExplicitWorkerOnControlSiblingWarns`). So pass
no core, or a first thread of a core the control CPU is not on.
`omarchy-benchmark --cpu/--cores` offlines unused siblings by default
(`--no-offline-siblings` keeps them) and adds a cgroup v2 isolated partition with
`--isolate`; it is the tool that keeps this honest.

### The live API's ceiling, and what it is not (2026-10-17, preliminary)

`live_transaction_bench --clients N` opens N connections from one process, each
with its own committer thread and its own slice of the rule space; the rate
reported is the aggregate and the latency the pooled one. The per-rate detail
line prints client CPU per transaction (the committer thread's own clock) and,
with `--daemon-pid`, the daemon's process-wide CPU from `/proc/<pid>/stat`
(utime+stime, which includes threads that have exited) minus its long-lived
packet workers. Measured on this host (20 lcores, 10 physical cores,
`powersave`, daemon confined to CPUs 2 and 4 -- control 2, worker 4 -- clients on
every other CPU, 512 sessions a client, 5 s a point, `-m 0`):

| clients | transactions/s | p50 | p99 | packet rate | client CPU/tx |
|---|---|---|---|---|---|
| 1 | 6,800 | 112 us | 308 us | 27.06 Mpps | 49 us |
| 2 | 11,122 | 149 us | 340 us | 23.25 Mpps | 55 us |
| 4 | 13,490 | 258 us | 536 us | 26.76 Mpps | 62 us |
| 8 | 13,262 | 519 us | 1,363 us | 26.34 Mpps | 82 us |
| 16 | 12,661 | 1,000 us | 4,267 us | 26.08 Mpps | 83 us |

The daemon side, measured on the 4-client arm with the instrument above:
**140.4 us of process CPU per transaction**, of which the packet worker's
busy-poll is 1.00 core over the window -- so the control path alone is **66.9 us
of CPU per transaction, about 0.9 core at 13,581 tx/s**. The aggregate stops at
about 13,300/s and more clients then only inflate latency (16 clients: p50
1,000 us, p99 4,267 us), while the packet path stays flat at 26-27 Mpps.

**Reading:** at the plateau the control path is using about one core, and bessd
in these runs had exactly one control CPU (it was pinned to CPUs 2 and 4). The
plateau is therefore consistent with the control path being CPU-saturated --
gRPC's sync-server pollers spin (they are part of that 0.9 core) and each
transaction costs its protobuf decode, plan, apply and response -- and *not*
with a serialized off-CPU wait. An earlier reading of this table said "neither
side is CPU-bound" on the strength of per-thread `schedstat` sums; that sum
undercounts (probed 2026-10-17: the process-wide `/proc/<pid>/stat` figure is
about three times the sum of the live threads' deltas), so it is withdrawn, as
is the "5.5 switches per transaction" that came with it (`/proc/<pid>/status`'s
switch counters moved by 0 over a 5 s, 70K-transaction window: the pollers do
not sleep). **Confounders in the table above:** the client's CPU set included
CPUs 3 and 5, the HT siblings of the daemon's control CPU 2 and worker 4 (the
2-client dip to 23.25 Mpps is plausibly that), and client CPU per transaction
includes the synchronous gRPC completion-queue work done on the calling thread.

**Next, in order.** (1) Re-run with the daemon free to use several control CPUs
(`taskset -c 2,6,8,10`, worker 4): if the plateau scales, it was one core's worth
of control work; if it does not move, the serialized resource is the
control-plane lock (`api_v2.cc:686-731` holds it across the whole apply) or
something under it. (2) A timer inside the handler split into lock wait / digest
/ apply -- `/proc` cannot see a lock. (3) Operations per transaction: the bench
prices them already (`--upserts N --erases M`, benchmarking.md), the lever that
works either way, since a session change is one transaction by construction. No
run of those is recorded against the `--clients` ceiling yet.
`--attach --block-offset` (added 2026-10-17, **not yet exercised**) is there for
the several-processes arm. Parked by decision 2026-10-17: the UPF comes first;
this resumes after it.

**Closing the churn question above** (the subsection before this one is a
different measurement: the API's ceiling, not the datapath's cost of a write).
The engine is innocent, and its own counters say so: 33,725 transactions applied
with zero busy, zero conflict, zero rejected, zero pending cascades, and *one*
RCU grace period for the whole run. The classifier's publication is correct by
design (build a replacement generation, publish it, retire the old one against a
grace period, never destroy a table on a worker) and a worker's per-publish work
is a single read per batch.

**What the fix direction actually is.** Not partitioning for its own sake.
`update_scale_bench` E2 measures control-thread churn into one shared lock-free
table against per-worker shards fed by per-owner op rings, and at 3 workers on
distinct P-cores (control on its own core, powersave, 2 s a point) it is a wash
at 65K keys -- 259.0 against 257.4 Mlookups/s at rest, 198.5 against 200.7 at
100k ops/s, 414.5 against 418.2 at 1M -- and not one at 1M keys, where the shared
arm reads 165.6 against 186.8, 164.5 against 190.3 and 153.6 against 160.6: a
13-16% gap in the sharded arm's favour, which the sentence here used to call a
wash. Two of these numbers are odd on their face and are not explained here: the
65K arm reads 414.5 Mlookups/s under 1M ops/s of churn against 259.0 at rest, and
the 1M-key gap is wider than the 65K one. At saturation the
sharded arm trades the datapath for throughput (447.6 against 25.3 Mlookups/s
with 8.4 against 79.4 Mops/s applied): the control thread applies directly into
the shared table while the sharded workers apply their own rings. The win D-028
recorded (3.4x) is a different case with a different writer: packet-path writers
each learning their own flows, where one shared table serializes them on its
lock (12 threads, 3% new flows: partitioned 6.5M inserts/s with 209M lookups/s
against shared 1.9M and 62M; at 0.4% new flows the two are within 10%). So the
property that pays is **the writer being the reader** -- an owner applying into
its own table between batches -- and the layout matters where several writers
contend. Batching transactions raises the control rate (one RPC and one commit
for many changes) but does not change who writes, so it does not remove this
cost.

### The same tools, re-run on the current tree (2026-10-18)

A re-run of the two subsections above, on `develop` at `487dc0c9` plus the
bench's multi-client arm, to check that their claims still hold rather than to
replace their numbers: the recipes differ (this one: `build/clang`, clang
`-O2 -g -march=x86-64-v3`, `bessd -m 0`, profile `balanced`, the client pinned to
one E-core -- cpu 12 -- for the single-client arms and to the E-cores 12-19 for
the client-count arms, a fresh daemon per arm, load 3.3-4.2).

| churn target | 0 | 1,000/s | 10,000/s | max |
|---|---|---|---|---|
| Mpps, worker on the control CPU's SMT sibling (cpu 1) | 31.71 | 29.94 | 26.12 (4,040 tx/s) | 26.08 (3,951 tx/s) |
| Mpps, worker on its own physical core (cpu 2) | 32.64 | 32.43 | 32.09 (4,442 tx/s) | 32.39 (4,300 tx/s) |
| client p50, separate cores | - | 226 us | 184 us | 189 us |

Same shape as 2026-10-16's sweep, and the placement warning fired by itself for
the co-located arm ("worker 0 on core 1 shares a physical core with another
worker or a control thread"), which is D-123's report read back through the SDK.
With a core of its own the packet path is flat and the client answers in under
190 us; co-located, the worker loses 5.6 Mpps at 4,000 transactions a second
(31.71 to 26.08).

The daemon's own counters over the separate-core arm, read with `bessctl
metrics`: 68,196 transactions applied, zero busy, zero conflict, zero rejected,
zero unsupported, and `bess_transaction_busy_retries_total` zero -- the daemon
never waited out a BUSY and retried. Whatever a transaction costs, it is not
pacing.

The client-count arms, at `max` and one round each (E-cores only, which is the
difference from the 2026-10-17 table above -- that run's clients had the P-cores
too, and these numbers are correspondingly lower):

| clients | transactions/s | p50 | p99 | packet rate | client CPU/tx |
|---|---|---|---|---|---|
| 1 | 4,350 (3 rounds: 4,343 / 4,364 / 4,387) | 187-189 us | 273-298 us | 31.6-31.7 Mpps | 101.8 us (0.68 cores) |
| 2 | 8,896 | 179 us | 314 us | 32.30 Mpps | 107.7 us (1.43 cores) |
| 4 | 12,716 | 261 us | 515 us | 32.38 Mpps | 106.4 us (2.10 cores) |
| 8 | 13,075 | 520 us | 1,306 us | 31.92 Mpps | 100.4 us (2.07 cores) |

The same plateau (about 13,000/s), the same latency shape, and the same
conclusion: the single client's ~4,400/s is that client's wire ceiling, not the
daemon's. The packet path does not pay for it here either -- 31.92 Mpps at 8
clients against 32.39 at one.

One limit of the bench, worth stating where its numbers are read: with
`--upserts` different from `--erases` the live set grows or shrinks, and a run
dies once the window leaves the `2 x --sessions` address space the traffic
generator covers (an erase of a key that is not there is refused). Equal counts
are what the tables above use.

**The session graph, for the same attribution.** The UPF's own bench
(`ci/bench-n4.sh MODES=establish`, its stage at `3f3716df`, 2,048 establishments
through the whole control path -- PFCP, the session compiler, one control_v2
transaction, the answer) reports establish p50 2.989 ms, p95 3.994 ms, p99
4.358 ms, 331.9/s -- within 6% of the 2.832 ms the same script recorded on
2026-10-17. The daemon's counters say the time is not pacing: over the bench arms
above, 68,196 transactions applied with `bess_transaction_busy_retries_total`
zero and no busy answers, and at 331/s the wire is nowhere near the ~4,400/s one
client reaches, so there was no BUSY to wait out. It is work per transaction: the
same transaction against the bench's four-module pipeline has a 187 us p50, so
the session graph's tables and its larger plan are what the other ~2.8 ms is.

## 5. Memory-at-Scale Matrix

Roadmap §23.1 Gate D, §24 and Appendix J ("memory-at-scale matrix complete"). Bytes are as each record measured them (slab, EAL heap growth, or the structure's own storage; the cell says which where the record does). "[derived]" marks the one product computed here from recorded factors.

| structure | bytes per entry | scale measured | total at the largest measured scale | source |
|---|---|---|---|---|
| `WorkerFlowTable` | 76 B/flow at full occupancy (slot record + free list + directory; 16 B key, 32 B State). By shape: 68 (8 B key), 92 (32 B key), 108 (48 B key); 60 / 108 / 172 with 16 / 64 / 128 B State; 316 with a 256 B side record. Two-key flow: 108 B vs 128 B for two tables | 1K, 64K, 1M; 10M only for 8 B key + 16 B State | 10M: 52 B/flow, about 0.5 GB | D-052, MODERNIZATION entry 132 |
| `SharedFlowTable` | 184 B/flow: 64 (slot, free list, ring) + ~120 `rte_hash` directory (EAL heap growth); 16 B key, 32 B State | 64K, 1M; 10M lookup rows (slab only) | 10M: slab 64 B/flow measured; directory not counted. "About 1.8 GB of EAL heap" is the record's estimate, not a measurement | D-052, D-056, MODERNIZATION entry 145, `docs/baselines/flow-shared-10m.json` |
| Expiry wheel | 32 B/timer plus 8 B owner-side (`owner_bytes`); alternatives: flat scan 8 B, heap 93 B, `rte_timer` 120 B | 64K, 1M, 10M | 10M: 320 MB; 1M: 32 MB of nodes | D-053 and addendum, MODERNIZATION entry 144, `docs/baselines/m10-expiry.json` |
| NAT bindings, owned | 125 B/mapping at 80% occupancy, about 100 at full; `Binding` object 40 B; legacy CuckooMap 93-101 B for its two entries | 4K, 64K, 1M | 1M bindings (6 addresses): about 132 MB with the wheel; 196,608 bindings (1 address): about 26 MB with the wheel; port bitmaps 8 KiB per (address, protocol class), 24 KiB per address | D-068, D-078 |
| NAT bindings, shared | not measured (`SharedFlowTable::memory_bytes` exists; no recorded value) | not measured | not measured | D-079 |
| Conntrack (owned, per-worker, shared) | not measured (key 40 B) | not measured | not measured | D-067, D-080, D-081 |
| Decision cache | 60 B/entry (16 B key) | 64K, 1M | not stated | D-062 |
| Handoff channel | 384 B object + ring; 16.75 B/item at full occupancy for 16 B items; ring slot 8-64 B per item | 1,024 items | 17,152 B | D-054 |
| FDB, `MacTable` | 64-byte bucket of four 64-bit keys with 16-bit values and flags, 50% load; timer handles in a separate cold array | 1K, 64K, 1M | 1M: 32 MB table (18-32 MB of it on transparent huge pages, varying by run) | D-064 |
| `PackedMacTable` (L2Forward, Bridge) | 8 B slot, 4-way 32-byte buckets, 50% load: twice the slot memory of `l2_table` per configured entry | L2Forward configurations only; benchmark sizes 1K / 64K / 1M have no recorded bytes | 64 KiB at L2Forward's default; 4 GiB at the largest accepted size (configuration bound, not a run) | D-073 |
| Classifier rules: `ConcurrentExactTable` (ExactMatch's table), measured as a flow backend | 152 B/flow including a 32 B State slab (16 B key; EAL heap + slab); bare `rte_hash` + slab 109 B; `EXT_TABLE` would add 8 B/entry (D-002) | 64K, 1M | not stated. ExactMatch module total (free-slot ring, defer queue, allocator overhead) not measured (D-010) | D-002, D-010, D-052 |
| Classifier rules: WildcardMatch | not measured | not measured | not measured | D-014, D-024 |
| Meters (`MeterSet`) | 64 B state + 8 B pointer per id | 1; 1,024; 16,384; 262,144; 1,048,576 meters | 1,048,576 meters: 64 MiB of state (pointer table not stated); 1,024 meters: `state_bytes` 65,536 | MODERNIZATION §10 K5, `docs/baselines/m0-baseline.json` |
| Stats / counters (`CounterSet`, histograms) | 3 counters × 64 worker slots: 4 KiB; 32 counters: 20 KiB; log2 histogram: 36 KiB | 64 worker slots | 36 KiB (histogram) | MODERNIZATION §10 K6, `docs/baselines/m0-baseline.json` (`storage_bytes` 4,096) |
| Event hub (worker rings + log) | 64 B per ring event; about 0.5 KiB per logged event | 256 events per worker ring; 16,384 logged events (the record's memory line; its Decision says the log keeps the last 65,536) | 1 MiB of rings; log about 8 MiB full | D-084 |
| Action / object tables (`ObjectTable`) | 64 B/object; next hop 16 B (D-060) | 10,240 objects | 696,388 B `storage_bytes`; 544 B `touched_bytes` (`BM_ObjectTableLookup<64>/10240/0`) | `docs/baselines/m0-baseline.json` (`object_table_bench`), D-060 |
| Routes (`rte_lpm`) | fixed 64 MiB tbl24 per table; per route 98 KB (1K routes), 1.5 KB (64K), 200 B (512K); `bytes_route` 98,321 at 1,024 routes | 1K, 64K, 512K routes | not stated (200 B/route at 512K); `Clear()` briefly holds two tbl24 | MODERNIZATION entry 34, §10 K7, `docs/baselines/m0-baseline.json` |
| Route domains | 64 MB tbl24 per domain regardless of its routes | 1, 4, 16 domains | 16 domains × 64 MB = 1,024 MB [derived]; 64 domains need 4.2 GB, not run | D-046 |
| RCU retirement | Retirer callable 48 B inline (built-in adapters use 16 B); retire queue is a reserved, vector-backed ring; bytes per retired object not measured | backlog 2,560-6,400 erased flows (`SharedFlowTable`); ≈47 (p99) / ≈540 (worst) pending `rte_hash` slots at 4.3M ops/s | high-water value not stated; transactions get `kBusy` past half the high-water mark or past 4,096 pending cascades | D-010, D-021 amendments 2 and 5, D-052, D-076 |
| Transaction scratch | not measured (scratch reused across transactions, D-021 amendment 4); wire: 131 B per typed rule (D-026) | request-id window 4,096 records (D-025); 50,000-rule transaction (D-026) | 6.55 MB per 50,000-rule message; 64 MiB message cap ≈510K typed rules | D-021, D-025, D-026 |

**Gaps against Gate D and §24** (bytes/item, high-water allocation, touched bytes, retirement backlog, queue occupancy; 1M / 10M / 50M for flow, NAT, conntrack):

- M0's memory-footprint set (D-047 "Not done") is still not a set: the entry 143 baseline (`m0-baseline.json`) carries footprint counters only for meters, stats, LPM/FIB and `ObjectTable`.
- 10M rows exist only for the worker flow table (smallest key and State), the expiry wheel and scan, and the shared table's slab (directory not counted). Missing at 10M: `SharedFlowTable` total, NAT, conntrack, decision cache (D-062 "No 10M row"), FDB, ExactMatch and WildcardMatch rules, meters (largest 1M) and routes (largest 512K).
- 50M estimates for flow, NAT and conntrack (§24) are not recorded.
- Conntrack bytes per connection: not measured in any mode. Shared NAT memory: not recorded.
- §24 fields missing for most structures: live vs capacity bytes (NAT alone gives 80% and full), allocation count during build/update, retirement high water, touched bytes per operation (only `ObjectTable`'s 544 B).
- `rte_hash` total (free-slot ring, defer queue, allocator overhead) not measured (D-010).
- Growth peaks not measured: owned NAT's 2× during migration (D-078), shared NAT's two tables until a grace period (D-079).
- 64 route domains (4.2 GB) not run (D-046).
- 4 KB vs hugepage backing at 10M unresolved (D-052 addendum).
- Transaction scratch and the RCU retire queue: no byte counts.

---

## 6. Benchmark Verification Methodology

1. **CPU Isolation**:
   - All official benchmark numbers must be recorded under core isolation via `omarchy-benchmark --cpu <N> --isolate --` (or strict `taskset`).
   - Benchmarks must disable CPU frequency scaling governors (`performance` mode) and offline unused SMT siblings to eliminate thermal and frequency noise.

2. **Drift-Cancelling Protocol (ABBA)**:
   - Comparative performance evaluations between versions or implementations must use `tools/ab_bench.py` running in paired ABBA sequence to eliminate ambient temperature and system drift.

3. **Burst Scaling**:
   - Batch scaling across burst boundaries ($16, 32, 64, 128, 256$) must be measured using `tools/batch_sweep.py` to observe cache residency transitions.
