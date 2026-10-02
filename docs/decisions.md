# Decisions

Why BESS's dataplane is built the way it is: what was chosen, what was
rejected, the evidence, and what should make us look again. How-to material
lives elsewhere (for tables: [dataplane-tables.md](dataplane-tables.md)); this
file is the reasoning.

**Conventions**

- **Stable ids.** Code that embodies a decision says so in a comment:
  `// Decision D-001 (docs/decisions.md)`.
- **Never rewritten.** A changed decision gets a new entry, and the old one is
  marked *Superseded by D-nnn*, so the history of why stays readable.
- **Evidence names what reproduces it:** the benchmark or test, and the
  headline numbers with the machine they came from. Unless stated otherwise
  the machine is an Intel i9-13900H (CPU 2 is a P-core, CPU 14 an E-core),
  running DPDK 25.11.3 and GCC 14 (`-O2`), with each benchmark pinned to an
  isolated CPU. Lookup costs are ns per 32-key batch.
- **Revisit when** names the trigger that should reopen the decision, such as
  a DPDK upgrade, new hardware, or a load we have not measured.

| id | decision | status |
|---|---|---|
| D-001 | Exact match: lock-free `rte_hash` + QSBR, updated in place | accepted |
| D-002 | `rte_hash` flags: LF, QSBR defer queue; not multi-writer, not ext table | accepted |
| D-003 | IPv4 LPM: `rte_lpm` updated in place; `rte_fib` rejected | accepted |
| D-004 | Update modes: concurrent (C) and per-worker (W); generation swap (G) only for bulk loads | accepted |
| D-005 | Control ingress: keep gRPC, with a streaming packed-record API | accepted |
| D-006 | Batch lookup body: staged or plain, chosen per table | accepted |
| D-007 | DPDK behaviours we depend on are deterministic CI tests | accepted |
| D-008 | UPF is a consumer of the framework, not its driver | accepted |
| D-009 | Consolidate the exact-match backends | open |
| D-010 | Size concurrent tables for occupancy and grace-period headroom, not a fixed 75% | accepted |
| D-011 | Tune per table at build time from host facts discovered once per process | accepted |
| D-012 | Workers report quiescence every 10 µs of scheduler time, not every 256 rounds | accepted |
| D-013 | What needs a grace period, and why worker pauses can go but quiescence stays | accepted |
| D-014 | WildcardMatch on mode C: a concurrent tuple-space table | accepted (trade-off recorded) |
| D-015 | Hash and compare keys inline around `rte_hash` lookups | accepted |
| D-016 | Benchmark native release builds with ABBA; no ISA multiversioning for now | accepted |
| D-017 | HashLB and ACL on mode G, L2Forward on mode C; control commands off the worker pause | accepted |
| D-018 | BPF execution: DPDK `rte_bpf` (with a repair pass) or the BESS JIT | open (deferred) |
| D-019 | DRR: a multi-producer ingress ring; the task's worker owns all flow state | accepted (trade-off recorded) |
| D-020 | Dataplane transactions: what we borrow from DPDK `rte_swx`, P4Runtime and VPP | accepted |
| D-021 | The G1.2b transaction engine: reserve/publish, reference counts, dependency-ordered publish, a removal cascade | accepted |
| D-022 | ExactMatch as the first module resource provider | accepted |
| D-023 | Router as a resource provider: routes placed invisibly, one writer when enrolled | accepted |
| D-024 | WildcardMatch as a resource provider: pending rules that lose to every rule | accepted |
| D-025 | The dataplane transaction RPC: typed per-resource values, request ids, a daemon epoch | accepted |
| D-026 | gRPC and protobuf practice: what changed, on what evidence | accepted |
| D-027 | Placement inside the inherited CPU set; control threads off worker CPUs | accepted |
| D-028 | Packet-path writers: partitioned or shared tables, chosen on numbers | accepted |
| D-029 | DPDK memory for the dataplane: dynamic hugepages, in-memory EAL, IOVA chosen by DPDK | accepted |
| D-030 | Container-friendly bessd startup: environment, auto-detection, graceful termination | accepted |
| D-031 | Daemon instance identity follows the RPC listen address | accepted |
| D-032 | The session vertical slice: ExactMatch → ActionTable → Meter → Router as one transactional graph | accepted |
| D-033 | Framework contracts, runtime ownership, and extension boundaries | accepted |
| D-034 | Packet-pool fast paths and CuckooMap lookup recovery | accepted |
| D-035 | Four-way performance characterization and dataplane clawback roadmap | accepted |
| D-036 | Automatic SPSC/MPSC queue mode selection from active worker graph | accepted |
| D-037 | External plugin package (bess-dev) and out-of-tree plugin API boundary | accepted |
| D-038 | K3.8 Range Backend for arbitrary L4 port ranges | accepted |
| D-039 | K7.1 Route Domains (VRFs) for multi-interface network instance isolation | accepted |
| D-040 | Standalone static release binary configuration and command-line -j parallelism | accepted |
| D-041 | Curated `bess-dev` headers and source-only plugin contract | accepted |
| D-042 | Module initialization capabilities replace direct runtime access (M3) | accepted |
| D-043 | Standalone release link: libgcc_eh ahead of libunwind, non-PIE | accepted |
| D-044 | Resource wire codecs bound outside the dataplane Resource (M4) | accepted |
| D-045 | Explicit application instances with leased lookup (M5) | accepted |
| D-046 | Route domains consolidated into the one Router (M6) | accepted |
| D-047 | Phase A closure: link-graph checker, plugin descriptor range, conformance plugins | accepted |
| D-048 | Fast build profile: normal test linking, mold, ccache, quiet EAL | accepted |
| D-049 | Logical network identities: InterfaceId replaces the gate in the route library (M7) | accepted |
| D-050 | Explicit transaction consistency: referential vs scope-snapshot (M8) | accepted |
| D-051 | DPDK build profiles: bess (software ports) and full (every NIC family) | accepted |
| D-052 | Generic flow-state substrate: typed flow tables with generation-checked ids (M9) | accepted |
| D-053 | Expiry substrate: a worker-owned hierarchical timing wheel with budgeted polls (M10) | accepted |
| D-054 | Handoff substrate: burst channels over `rte_ring_elem` with moved-from ownership, and generation-checked continuations (M11) | accepted, with the exceptions under "Not done" |
| D-055 | One CI authority for gating and release lanes; static release links with -fno-lto | accepted |
| D-057 | Execution layering: EAL extracted, framework no longer reaches runtime (M1) | accepted |
| D-058 | M1/M2 closure: enforceable include rules, classified installs | accepted |


---

## D-001 Exact match: lock-free `rte_hash` + QSBR, updated in place

**Status:** accepted (2026-09-25).
**Code:** `core/classifier/concurrent_exact.{h,cc}` (`ConcurrentExactTable`),
`core/modules/exact_match.cc`.

**Context.** ExactMatch rebuilt its whole table on every rule change (a K3
generation with a cuckoo backend). Rule changes at runtime, and at scale, are
the requirement: a UPF adds sessions continuously, and so does any stateful
network function driven by a controller.

**Decision.**

- Rules live in one shared DPDK `rte_hash`, changed in place by the command
  thread while workers look up lock-free.
- Deleted slots return only after a QSBR grace period on the runtime's
  `RcuDomain`.
- A generation holds only configuration: the extraction plan and the default
  gate.
- The owner grows the table by copying it into one twice the size when live
  keys plus pending deletes eat into the headroom, and whenever an add
  returns `kFull` (sizing: D-010).

**Evidence.**

- Per rule change through the module (`modules_exact_match_update_bench
  BM_ModuleAddDelete`, one add plus one delete per iteration):

  | rules | before (rebuild) | after (in place) |
  |---|---|---|
  | 1K | 426 µs | 0.32 µs |
  | 100K | 47 ms | 0.33 µs |
  | 1M | 645 ms | 0.37 µs |

- Lookups (`BM_Lookup`, medians of 5):

  | rules | kind | CPU 2 old / new | CPU 14 old / new |
  |---|---|---|---|
  | 1K | hit | 318 / 348 | 441 / 591 |
  | 1K | miss | 118 / 183 | 228 / 397 |
  | 128K | miss | 161 / 184 | 311 / 392 |
  | 1M | hit | 647 / 573 | 1426 / 976 |
  | 1M | miss | 293 / 240 | 589 / 426 |

  - Hits on CPU 2 are within 10%.
  - Cache-resident misses cost 1-2 ns more per packet, and up to 5 ns on the
    E-core.
  - From 1M rules everything is faster.
  - Accepted, because O(1) updates were the requirement.
- Correctness tests:
  - `ConcurrentExactTableTest.*`;
  - `ExactMatchTest.*`, including
    `ChurnDuringLongGracePeriodGrowsInsteadOfFailing`;
  - the live-daemon `exact_match.py`.

**Rejected.**

- *Keep rebuilding:* O(table) per change. At 1M rules one add is 0.6 s.
- *Precomputed hashes* (`rte_hash_lookup_with_hash_bulk_data`): −18% on
  small-table misses on CPU 14, but +10-14% at 1M on CPU 2. It is not a
  uniform win, and using it means a size-selected second path.
- *`EXT_TABLE`:* see D-002.
- *Our own lock-free cuckoo:* it could close the small-table miss gap, but it
  means owning a lock-free algorithm DPDK already maintains. Not unless that
  gap is shown to matter.

**Revisit when:**

- a DPDK upgrade touches `lib/hash` (re-run `BM_Lookup`);
- miss-heavy traffic on small tables shows up as a real bottleneck;
- values need to be wider than 8 bytes (then use an id into an `ObjectTable`).

## D-002 `rte_hash` flags: LF, QSBR defer queue; not multi-writer, not ext table

**Status:** accepted (2026-09-25).
**Code:** `ConcurrentExactTable::Create`.

**Context.** `rte_hash` has several concurrency modes, and the choice decides
whether the packet path takes a lock and whether deletes block.

**Decision.** Create with `RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF` only, and
attach QSBR in defer-queue (`DQ`) mode with the default queue size, which is
the table size.

**Evidence.** Read from `lib/hash/rte_cuckoo_hash.c` (DPDK 25.11.3):

| flag / mode | effect | choice |
|---|---|---|
| `RW_CONCURRENCY` | readers take an `rte_rwlock` | no: a lock on the packet path |
| `RW_CONCURRENCY_LF` | lock-free readers; cuckoo moves bump a change counter and readers retry; implies `NO_FREE_ON_DEL` | **yes** |
| `MULTI_WRITER_ADD` | writer lock plus per-lcore free-slot caches | no: there is one writer |
| `TRANS_MEM_SUPPORT` | TSX for the writer lock | no |
| `EXT_TABLE` | overflow buckets, so an add fails only when slots run out | no: within 5% on lookups, +8 B per entry, and growth covers it |
| QSBR `DQ` | a delete queues its slot with a grace-period token; later writes reclaim | **yes** |
| QSBR `SYNC` | each delete blocks until all readers are quiescent | no: blocks the command thread |

What this relies on:

- a reader compares the key and *then* loads the value, which is why a slot
  must not be reused within a grace period;
- updating an existing key is an atomic exchange of the value;
- a queue as large as the table can never fill and fall back to a blocking
  wait.

The first two are pinned by tests (D-007).

**Revisit when:** a DPDK upgrade changes `lib/hash`, or a second writer
thread becomes necessary (then `MULTI_WRITER_ADD`, or per-writer shards per
D-004).

## D-003 IPv4 LPM: `rte_lpm` updated in place; `rte_fib` rejected

**Status:** accepted.
**Code:** `core/route/route_table.{h,cc}`, `core/route/router.h`,
`core/modules/ip_lookup.cc`.

**Context.** IPLookup rebuilt `rte_lpm` per route change. DPDK also offers
`rte_fib`, which builds faster.

**Decision.** Change `rte_lpm` in place from one writer, with QSBR reclaiming
tbl8 groups. `Clear()` builds a fresh table and swaps it. Next hops live in an
`ObjectTable` (`Router`), so a neighbour change does not touch routes.

**Evidence.**

- `rte_lpm` build cost grows faster than linearly: 5.8 µs per route at 64K,
  39.6 µs at 512K, and about 21 s for a whole 512K table. One in-place change
  is 0.5-33 µs, against 9.4-341 ms by rebuild at 1K-64K routes
  (`route_bench`).
- `rte_fib` lookups tie (3.70 vs 3.95 ns per lookup at 1K routes), and its
  builds and updates are cheaper. **But** with 512K routes inserted in
  arbitrary order (a /24 after a /28 under it, which a control plane may do),
  `rte_fib` (DPDK 25.11.3, DIR24_8) returned a next hop that matches no
  rule. `rte_lpm` and an independent reference agree with each other.
  - The failure is deterministic.
  - It does not occur when the rules are inserted shortest-prefix-first.
  - It is not the known upstream defect.
  - There is no minimal reproducer yet.
  - Reproduce it with `fib_bench` and `BESS_FIB_GATE=1`.

**Rejected.** `rte_fib`, on correctness. Rebuild per change, on cost.

**Revisit when:** on every DPDK upgrade, run `fib_bench` with
`BESS_FIB_GATE=1`. If it passes, `rte_fib` is worth re-evaluating for its
cheaper builds and updates. Also revisit when IPv6 routing is needed
(`rte_lpm6` or `rte_fib6`, measured the same way).

## D-004 Update modes: concurrent (C) and per-worker (W); generation swap (G) only for bulk loads

**Status:** accepted (2026-09-25).
**Code:** see [dataplane-tables.md](dataplane-tables.md) section 3;
`core/dataplane/update_scale_bench.cc`.

**Context.** Rebuilding a table for every change does not scale, and pausing
every worker for a change stops traffic.

**Decision.** Each table uses one of these modes:

- **C:** a shared table that is concurrent by design, changed in place by the
  command thread.
- **W:** per-worker operation rings, applied between scheduler rounds into
  replicas or shards.
- **Worker-owned:** state the packet path writes.
- **G:** a whole-table swap, only for bulk loads and restores, and for
  structures with no incremental update (for example `rte_acl`).

**Evidence** (`update_scale_bench`):

- C sustains 1M modifications/s into a shared 10M-entry table for under 2% of
  four workers' lookup throughput.
- One writer peaks at about 4.3M ops/s with readers present.
- Worker-owned partitions apply 34-148M ops/s, but did not speed up
  multi-worker lookups on this laptop. That is unexplained, so W is not the
  default.
- Sizing rule: spare slots ≥ update rate × grace period.

**Rejected.** Swap as the general mechanism (O(table) per change). A global
worker pause for runtime changes (it stops traffic).

**Revisit when:**

- measured on server hardware, where the multi-worker lookup penalty may not
  exist;
- one writer is not enough, meaning more than about 4M ops/s per table.

## D-005 Control ingress: keep gRPC, with a streaming packed-record API

**Status:** accepted (2026-09-25).
**Code:** `core/dataplane/ingress_bench.cc`, `protobuf/control_v2.proto`.

**Context.** With in-place updates, the control transport could become the
bottleneck.

**Decision.** Keep gRPC. High-rate updates use a client-streaming RPC that
carries packed fixed-size operation records.

**Evidence** (`ingress_bench`):

| transport | throughput |
|---|---|
| one gRPC call per operation | ~15K ops/s |
| streaming RPC, packed 16-byte records, batches of 256 | ~20M ops/s, apply included |
| pinned two-process shared-memory ring | 78-275M ops/s, decode only |

**Rejected, for now:** a shared-memory transport (as VPP's binary API can
use). It is not needed below about 20M ops/s per stream, and gRPC keeps
remote controllers and the ecosystem.

**Revisit when:** one control stream must exceed about 20M ops/s, or the
controller always runs co-located.

## D-006 Batch lookup body: staged or plain, chosen per table

**Status:** accepted.
**Code:** `core/dataplane/batch_stages.h`, `core/dataplane/batch_tuning.{h,cc}`
(`ResolveLookupBody`).

**Context.** Prefetching a whole batch before probing (VPP's style) helps
some tables and not others.

**Decision.**

- Authors write lookups as stages.
- `ResolveLookupBody()` picks the body at build time: staged for tables
  larger than L1d whose lookup walks dependent cache lines or branches on
  loaded data, plain otherwise.
- Cache sizes come from sysfs, with fallbacks, and need no privileges.
- `BESS_LOOKUP_BODY=plain|staged` overrides the choice.
- Nothing requires staging.

**Evidence** (`modules_table_scale_bench`, `classifier_cuckoo_scale_bench`).
Staging changes lookup cost by:

- cuckoo tables: −18% to −48%;
- L2Forward: −18% to −41%;
- WildcardMatch tuples: up to −49%;
- meters beyond L3: about 3× faster;
- `rte_lpm`: nothing, since it is one independent load per packet.

**Revisit when:** new CPU generations arrive (the rule depends on cache
sizes and on out-of-order depth), or a new table type is added.

## D-007 DPDK behaviours we depend on are deterministic CI tests

**Status:** accepted (2026-09-25).
**Code:** the tests listed in [dataplane-tables.md](dataplane-tables.md)
section 7.

**Context.** Several guarantees rest on DPDK internals: when freed memory is
reused, and the order in which a lookup loads things. A DPDK upgrade could
change them silently. A stress test passed with the protection removed,
because the race window is a few instructions wide.

**Decision.**

- Every DPDK behaviour we depend on, whether an API or an ABI property, is a
  regular deterministic unit test in CI.
- A concurrency test holds a reader online, asserts the protected resource is
  not reused, then releases the reader and asserts it comes back.
- Each test is checked once, when written, by removing the protection and
  watching the test fail. Those checks are recorded in the commit, not kept
  as artifacts.
- Stress tests remain, as extra coverage only.

**Evidence.**

- `ConcurrentExactTableTest.ErasedSlotWaitsForOnlineReaders` fails 3/3
  without QSBR, and 3/3 when a slot is freed at delete time. The stress test
  alone passed the second of those.
- `RouteTableTest.FreedTbl8GroupWaitsForOnlineReaders` fails 3/3 without
  `rte_lpm_rcu_qsbr_add`.

**Revisit when:** never as a policy. Add a test whenever new code depends on
a new DPDK behaviour.

## D-008 UPF is a consumer of the framework, not its driver

**Status:** accepted (2026-09-25).

**Context.** OMEC's UPF is the strongest current use case, and there is a
risk of shaping a general dataplane around one network function.

**Decision.**

- UPF-specific code stays in a plugin: GTP-U, PFCP-shaped objects, a
  session API.
- A feature enters core only when at least two non-UPF consumers would use
  it. Candidates to check against:
  - a stateful firewall or CGNAT;
  - an L3 router with ECMP;
  - a load balancer;
  - a BNG.
- The built-in modules are the first consumers of every generic mechanism;
  the reference UPF comes last, as proof.

**Revisit when:** a proposed core feature can only be justified by UPF.
Then it belongs in the plugin.

## D-009 Consolidate the exact-match backends

**Status:** open.
**Code:** `core/classifier/{cuckoo_exact,rte_hash_exact,small_exact,direct_exact,typed_exact}.h`,
`core/utils/cuckoo_map.h`.

**Context.** The K3 experiments left several exact-match backends. Choice
helps experts; one obvious path helps authors.

**Current evidence.**

- Our cuckoo is 13-20% faster on cache-resident tables on the E-core, and
  faster on small-table misses.
- `rte_hash`-LF has O(1) concurrent updates and wins from 1M entries
  (D-001).

**Likely decision:**

- `ConcurrentExactTable` for anything updated at runtime;
- one immutable backend for generation-only tables;
- `SmallExactBackend` and `DirectExactBackend` only where measured to win;
- remove the rest.

**Decide when:** WildcardMatch and L2Forward have moved to the update modes
of D-004, so the real users of each backend are known.

## D-010 Size concurrent tables for occupancy and grace-period headroom, not a fixed 75%

**Status:** accepted (2026-09-26). First accepted in part; the headroom
constant was provisional until grace periods were measured (D-012).

- **Measured, with the 10 µs cadence of D-012:**
  - grace periods are p99 about 11 µs, p99.9 up to 60 µs, and at most
    125 µs across 1-4 pipelines;
  - at one writer's measured peak (~4.3M ops/s), that is about 47 pending
    slots at p99 and about 540 at the worst;
  - so 256 slots (or 5%) covers everything up to p99.9, and the rare worst
    case makes the table grow early rather than fail an add.
- **Pipelines whose single task invocation is long** (a module taking
  300 µs per batch) have grace periods as long as that invocation. Their
  headroom need scales the same way, and growth absorbs it.
- **Workers oversubscribed on one CPU** get OS-scheduler-length grace
  periods (milliseconds). That is a deployment error, and the table grows.
- The review that prompted the measurement pointed out that a QSBR grace
  period is not a fixed constant. It
  depends on batch duration, scheduling, pauses and slow modules, so
  "update rate × grace period" has an input we have not measured.
- The implementation is safe either way. A longer grace period only makes
  the table grow earlier or more often; adds do not fail. Growth is
  amortized, but repeated growth under pathological grace periods costs
  memory. That is what the measurement will bound.

**Code:** `ConcurrentExactTable::CapacityFor`, `Headroom`, `HasRoomForOne`;
`ExactMatch::NewTable` / `EnsureCapacity`; the test
`ConcurrentExactTableTest.SizingCountsPendingDeletesAgainstHeadroom`.

**Context.** The 3/4 growth threshold was a guess, so a quarter of every table
is kept empty. Two separate things can make an add fail: cuckoo displacement
running out of room, and key slots held by deletes still waiting out a grace
period.

**Evidence** (`occupancy_bench`, 8-byte keys, five seeds for fill and three
for churn):

- **`rte_hash` occupancy at the first failed add** (entries a power of two,
  so bucket positions = key slots):
  - random keys: 99.5% at 1K entries, 98.8% at 16K, 98.3% at 256K, 97.4% at
    4M (worst seed 97.0%);
  - sequential and IP×port keys: 100% at every size.

  With 64 more attempts, 99.7% or more is reachable. CRC spreads sequential
  keys evenly, so TEIDs and address pools are the easy case.
- **Sizing `entries` at 3/4 of the bucket power of two** (bucket positions =
  4/3 of the key slots) reaches 100% of slots with every key shape.
- **`EXT_TABLE`** also reaches 100%.
- **Mean add cost as the table fills** (4M entries, random keys, ns):

  | load | power of two, CPU 2 | power of two, CPU 14 | 3/4 sizing, CPU 2 | 3/4 sizing, CPU 14 |
  |---|---|---|---|---|
  | up to 80-90% | ≤ 150 | ≤ 160 | ≤ 180 | ≤ 170 |
  | last 10% | 643 | 1161 | 198 | 189 |

  The single-sample worst cases (5-40 µs) appear at every load and are
  outliers, not displacement cost.
- **Steady churn** (erase one, add one) with no reader holding grace periods:
  zero failed adds up to 95% load, for every sizing and key shape.
- **Failures appear exactly when deletes waiting out a grace period outnumber
  the spare slots.** At 1K entries:

  | load | reader quiescent every 64 ops | every 1024 ops |
  |---|---|---|
  | 95% (51 spare slots) | 17% of adds fail | — |
  | 75% (256 spare slots) | — | 75% of adds fail |

  At 64K entries (3,277 spare slots at 95%) nothing fails in either case.
  This is the headroom rule: spare slots ≥ update rate × grace period.
- **`CuckooMap`** (for comparison; it grows instead of failing):
  - random keys: it doubles its buckets at 80-95% load (median 89%);
  - sequential keys: it doubles only when completely full;
  - IP×port keys: some doublings happen at 35% load, and a table of 4M
    such keys ended at **25%** load.

  Its displacement search is shallow (4-way buckets, depth 3), and its
  secondary hash is derived from the primary. NAT keeps its flow table in
  `CuckooMap` with endpoint keys, so its memory use deserves a measurement
  of its own.

**Decision.**

- Create tables with `entries` = 3/4 of the bucket power of two.
- Grow when live entries plus deletes pending reclaim exceed the slots less
  a headroom. Headroom = max(5% of slots, expected update rate × grace
  period).
- Keep grow-on-`kFull` as the backstop.

For the same rule count this stores about a fifth fewer **key slots** than
the old rule, and it removes the near-full add-cost cliff.

- That is a statement about key slots only, not "20-25% less `rte_hash`
  memory". The bucket array is unchanged at equal rule counts, and total
  memory also includes the free-slot ring, the defer queue and allocator
  overhead, none of which has been measured.
- Lookups, in an in-process A/B against the old sizing: bucket arrays are
  identical at equal rule counts, and the results differ by ±8% in both
  directions, which is placement noise, not a sizing effect.
- Add cost through the module is unchanged: 0.58-0.74 µs per add+delete on
  CPU 2 (the headroom check adds one reclaim attempt and one ring count).

**Revisit when:** a DPDK upgrade changes `rte_hash`'s displacement search,
or measured grace periods under real worker load are far longer than
assumed.

## D-011 Tune per table at build time from host facts discovered once per process

**Status:** accepted (2026-09-26).
**Code:** `core/dataplane/batch_tuning.{h,cc}` (`CacheGeometry::Smallest`,
`LookupBodyOverride`, `ResolveLookupBody`); the startup log in `core/main.cc`;
the callers listed in [dataplane-tables.md](dataplane-tables.md) section 6a.

**Context.** Several choices depend on the host (cache sizes) and on the
table (its footprint and lookup shape): plain or staged batch lookups, and
table sizing. They could be decided at daemon start, at module init, when a
table is built, or continuously.

**Decision.**

- The daemon only **discovers facts**, once per process: cache geometry from
  sysfs (the smallest over online CPUs), and the `BESS_LOOKUP_BODY`
  override. It logs them at startup.
- **Each table decides** when it is built or created, from those facts plus
  its own footprint and shape. Module authors need not call anything: the
  backends do it.
- One exception runs per batch: NAT's `CuckooMap` prefetch check, because
  that table grows while packets flow; the check is a size comparison.
- No privileges are needed. There is no start-up micro-benchmark, and
  nothing is retuned while traffic runs.

**Evidence.**

- The plain/staged crossovers in D-006 were reproduced from sysfs cache
  sizes alone.
- On this hybrid CPU, sysfs gives per-core-type sizes (P-core L1d 48 KiB,
  L2 1.25 MiB). `sysconf` reports 32 KiB / 2 MiB, which is why sysfs comes
  first.

**Rejected.**

- *Calibrating with micro-benchmarks at daemon start:* it adds start-up time
  and noise, and sysfs was sufficient.
- *Per-worker tuning:* a shared table is read by any worker, so the smallest
  CPU is the safe choice.
- *Continuous retuning:* no evidence it pays, and it would put decisions on
  the packet path.

**Revisit when:**

- mode W shards pin tables to known workers, so the owner's real CPU is
  known;
- a platform exposes no sysfs cache information, so the fallback defaults
  start deciding;
- measurements on a new CPU generation disagree with the rule.

## D-012 Workers report quiescence every 10 µs of scheduler time, not every 256 rounds

**Status:** accepted (2026-09-26).
**Code:** `core/scheduler.h` (`QuiescentCadence`, both scheduler loops);
`core/rcu/grace_period_bench.cc`.

**Context.**

- Workers reported an RCU quiescent state once every 256 scheduler rounds,
  piggybacked on the accounting and pause check. A grace period therefore
  lasted about 256 × one round, so its length depended on what the modules
  do.
- Every structure that frees memory readers may touch waits for grace
  periods: `RcuPtr` generations, `rte_hash` slots, `rte_lpm` groups and next
  hops. Table headroom (D-010) is sized from them.

**Decision.**

- Report quiescence when at least 10 µs of scheduler time has passed since
  the last report. The check runs every round against the TSC the scheduler
  already reads (`checkpoint_`), so it costs no extra `rdtsc`.
- A grace period is then about max(10 µs, one task invocation), independent
  of round counts.
- The pause check keeps its 256-round cadence.
- `BESS_QUIESCENT_INTERVAL_US` overrides the interval for experiments (0 =
  every round, `rounds` = the old cadence).

**Evidence** (`grace_period_bench`, pinned and isolated, control CPU 2,
workers on CPUs 4-10). Grace period, p50 / p99 in µs:

| scenario | old: every 256 rounds | every round | every 2 µs | every 10 µs |
|---|---|---|---|---|
| idle worker | 1.2 / 2.3 | 0.24 / 0.27 | 1.4 / 2.1 | 5.1 / 10.0 |
| 1 pipeline | 18.9 / 36.9 | 0.29 / 0.39 | 1.3 / 2.3 | 5.3 / 10.3 |
| 4 pipelines | 34.6 / 53.5 | 0.38 / 0.56 | 2.0 / 2.4 | 8.7 / 11.3 |
| module burning 10K cycles/batch | 863 / 922 | 2.1 / 4.4 | 2.1 / 3.8 | 5.8 / 11.0 |
| module burning 1M cycles/batch | **85,501 / 85,545** | 299 / 324 | 299 / 324 | 299 / 324 |
| 2 workers sharing one CPU | 1,528 / 2,387 | 1,379 / 2,377 | 1,366 / 2,369 | 1,542 / 2,376 |

The cost: each report re-reads the shared QSBR token, which is a cache miss
whenever a writer has started a new grace period, and every table delete
starts one. Throughput of 64-byte Source→Bypass→count, with the control
thread starting 1M grace periods per second:

| cadence | 1 worker | 4 workers |
|---|---|---|
| old | 236 Mpps | 656 Mpps |
| every round | 214 (−9%) | 630 (−4%) |
| every 2 µs | 229 (−3%) | 650 |
| every 10 µs | 238 (±0) | 648 |

Without token churn, every cadence is within noise (233-239 Mpps). These
are tiny rounds, the worst case for per-round cost; real pipelines amortize
further.

**Rejected.**

- *Every round:* the shortest grace periods, but −9% under heavy delete
  churn.
- *2 µs:* −3% under churn, for grace periods that nothing measured needs.
- *Keeping round counts:* grace periods of 85 ms behind one slow module.

**Revisit when:**

- a consumer needs sub-10 µs reclamation, and then measures the churn cost
  of 2 µs;
- the scheduler loop changes;
- rounds become much longer. The interval is then irrelevant: one task
  invocation bounds the grace period anyway.

## D-013 What needs a grace period, and why worker pauses can go but quiescence stays

**Status:** accepted (2026-09-26), as design guidance for G1.2 and the NF
catalogue.

**Context.** Could correctly implemented update modes C and W remove grace
periods, RCU and framework pauses altogether? Answered by what each mode
lets readers see.

**Decision (the analysis):**

- **W (worker-owned state, per-worker op rings).** The owning worker is the
  only reader and the only writer. It applies operations between its own
  scheduler rounds, when none of its lookups is in flight. Nothing it frees
  can be in use, so **W-mode state needs no grace period and no pause**.
  - The control plane talks to it only through SPSC rings and reads it only
    through snapshots or seqlocks (the K6 pattern).
  - Costs: every replica applies every operation (N× the work), and a
    change becomes visible per worker at different instants, not
    everywhere at once. A cross-worker "all at once" cutover needs a
    barrier, which is itself a form of quiescence.
- **C (a shared table, one writer, lock-free readers).** A reader can be in
  the middle of reading a slot or object the writer just deleted, so reusing
  that memory must wait until the reader is done. That wait **is** a grace
  period, whatever it is called. The ways to avoid it all move cost to the
  packet path:
  - reference counts (an atomic read-modify-write per lookup);
  - hazard pointers (publish, fence and re-check per lookup);
  - type-stable slots with per-slot versions, where readers re-check a
    version after reading and never free the memory.

  QSBR costs readers nothing and the workers one store per 10 µs (D-012),
  which is why it is the choice. A future own-table with versioned inline
  slots could drop QSBR for itself; pointer-valued data still needs a grace
  period.
- **G (generation swap).** The old generation is freed after a grace period,
  as with C.
- **Framework-level worker pauses are a different mechanism from
  quiescence.** They are used today for commands marked `THREAD_UNSAFE` and
  for structural pipeline changes (creating or destroying modules and
  workers, connecting gates).
  - Runtime table updates no longer need them once every table is C, W or
    G (the rest of G1.2a).
  - Graph changes can move to RCU publication of the graph: connections and
    tasks as published objects.
  - Destroying a module must still wait until no worker can be running
    it, which is again a grace period, not a pause.

**So:**

- Workers need never stop for runtime changes. That is the target, and
  G1.2a plus graph publication get there.
- Quiescence reporting stays as the one cheap mechanism behind every free.
- W-mode state is the exception that needs neither.

**Revisit when:** a versioned-slot table is built, since it would remove
QSBR for that table only; or graph publication lands, since it would remove
the pauses left for structural changes.

## D-014 WildcardMatch on mode C: a concurrent tuple-space table

**Status:** accepted (2026-09-26), with a lookup trade-off the user accepted.
**Code:** `core/classifier/concurrent_masked.{h,cc}` (`ConcurrentMaskedTable`),
`core/modules/wildcard_match.{h,cc}`,
`core/modules/wildcard_match_update_bench.cc`.

**Context.** WildcardMatch rebuilt every tuple table on every `add`: 1.1 ms
per add+delete at 1K rules, 0.14 s at 100K.

**Decision.**

- One `ConcurrentExactTable` per distinct mask, keyed by the masked value.
- Rule records `{priority, sequence, result}` are write-once. An update
  writes a new record under a new id and swaps the table value; old ids are
  recycled only after a grace period (D-013).
- The tuple list is RCU-published, and republished only when a mask
  appears or disappears (at most 8 masks, the wire limit).
- The table value packs the id with the result, so a packet that exactly
  one tuple matches never loads its record.
- Semantics are preserved: the highest priority wins, then the later
  command; re-adding the same (mask, value) replaces the rule; the mask
  ceiling counts active masks.

**Evidence** (native release builds, `tools/ab_bench.py`, 8 ABBA pairs
each, pinned and isolated):

- **Updates, per add+delete** (CPU 2, 4/4 pairs):

  | rules | old | new |
  |---|---|---|
  | 1K | 1,090-1,139 µs | ~1 µs |
  | 10K | 11,590-11,998 µs | ~1 µs |
  | 100K | 140,062-149,199 µs | ~1 µs |

  Flat across 1, 4 and 8 masks.
- **Lookups, ns per 32 keys, new / old:**

  | tuples | rules | CPU 2 | CPU 14 |
  |---|---|---|---|
  | 1 | 1K | 253 / 393 (−36%) | 461 / 519 (−11%) |
  | 1 | 1M | 556 / 958 (−43%) | 1115 / 2236 (−51%) |
  | 4 | 1K | 1288 / 1025 (**+27%**) | 1814 / 1388 (**+31%**) |
  | 4 | 16K | 1308 / 1054 (**+24%**) | 1905 / 1615 (**+21%**) |
  | 8 | 1K | 1857 / 1462 (**+26%**) | 2713 / 2100 (**+30%**) |
  | 8 | 16K | 1850 / 1581 (**+16%**) | 2824 / 2553 (**+15%**) |
  | 4 | 1M | 1716 / 2562 (−33%) | 3418 / 6384 (−50%) |
  | 8 | 1M | 2463 / 4175 (−42%) | 6723 / 12750 (−48%) |

- **Why multi-tuple small tables are slower:** each packet misses N−1
  tuples, and `rte_hash` misses on small tables cost more than the old
  cuckoo's. ExactMatch misses at 1K rules measure +33% on CPU 2 and +43%
  on CPU 14 (D-015). Hits and large tables are faster.
- **Correctness tests:**
  - `ConcurrentMaskedTableTest.*`, including the deterministic
    `RetiredRuleIdWaitsForOnlineReaders` (fails 3/3 without the grace
    period) and a stress test in which stable top-priority rules must
    always win under churn;
  - `WildcardMatchTest` 12/12;
  - the live-daemon `wildcard_match.py` 9/9.

**Rejected.**

- *Keeping the rebuild:* 0.14 s per change at 100K rules.
- *Per-worker replicas (mode W):* lookups unchanged, but rehash stalls in a
  worker loop and memory × workers.
- *Packing priority into the table value:* priorities are full int64, and
  equal priorities must go to the later command.

**Revisit when:** the per-tuple presence filter (MODERNIZATION §31.3) is
measured. It targets the multi-tuple miss cost directly. Also revisit if our
own lock-free cuckoo (§31.5) becomes necessary.

## D-015 Hash and compare keys inline around `rte_hash` lookups

**Status:** accepted (2026-09-26).
**Code:** `core/classifier/concurrent_exact.{h,cc}`
(`detail::HashBatchFixed`, `detail::CmpFixed`, `LookupBatch`).

**Context.** Profiling a 4-tuple WildcardMatch lookup found `rte_hash`
spending:

- ~20% hashing: each key goes through the table's `hash_func` pointer and
  a PLT stub into the generic any-length `rte_hash_crc` (an alignment loop,
  then word and tail loops);
- ~9% in libc `__memcmp_avx2_movbe`: keys whose width is not a multiple of
  16 are compared with `memcmp` through a function pointer.

The probe itself (`__bulk_lookup_lf`) cost about the same as our cuckoo's.

**Decision.**

- **Hashing:** hash the batch inline with a CRC32C kernel fixed to the key
  width (one `crc32` per 8 bytes), chosen once per table. Then call DPDK's
  own `rte_hash_lookup_with_hash_bulk_data`.
- **Compare:** install an equality-only fixed-width compare with
  `rte_hash_set_cmp_func` for widths DPDK does not specialize. For 8 bytes
  it compiles to `mov; cmp; setne`.
- **Unchanged:** table layout, inserts, occupancy, and the concurrency
  protocol (the change counter, compare-then-load).

**Evidence.**

- **Bit-identical hash:** `InlineHashIsBitIdenticalToRteHash` covers every
  width 1-64 at all 8 alignments; changing the seed fails it and lookups
  with it.
- **Exact equality:** `FixedWidthCompareIsExactEquality` flips each byte
  and checks that bytes beyond the key are ignored.
- **The first compare attempt** used a constant-size `memcmp`, which GCC
  still tail-called because memcmp's ordering had to be preserved. Caught
  by reading the generated code.
- **ExactMatch, native, new against the old cuckoo** (ns per 32 keys, 8
  ABBA pairs):

  | case | CPU 2 | CPU 14 |
  |---|---|---|
  | hits, 1K-128K | −34..−37% | −16..−21% |
  | hits, 1M | −23% | −33% |
  | misses, 1K | **+33%** (88 → 119) | **+43%** (134 → 196) |
  | misses, 16K | +15% | +9% |
  | misses, 128K | no clear difference | −7% |
  | misses, 1M | −25% | −43% |

- **What remains** is `rte_hash`'s per-call bulk setup (512 B of hit-mask
  zeroing, position init, the change counter) on the miss path.

**Rejected.** *Prehashing through the generic `rte_hash_crc`* (tried
2026-09-25): it kept the generic loop and gained 8%.

**Revisit when:** a DPDK upgrade changes `rte_hash`'s bulk lookup, or when
the §31.3 presence filter makes the miss path moot.

## D-016 Benchmark native release builds with ABBA; no ISA multiversioning for now

**Status:** accepted (2026-09-26).
**Code:** `tools/ab_bench.py`; `protobuf/meson.build` (release builds);
the `cpu` option in `meson_options.txt`.

**Context.**

- The benchmark build directories had been configured `cpu=corei7`, CI's
  portable baseline. BESS kernels were measured without AVX2 or BMI: the
  code emitted `bsf`, not `tzcnt`. On Zen 3, `bsf` is 6 uops at one per 3
  cycles against `tzcnt`'s 2 uops at two per cycle (uops.info).
- `buildtype=release` did not build at all: GCC reports a false-positive
  `-Wuninitialized` in protobuf's headers, which `-Werror` made fatal.
- Comparisons were A-then-B, exposed to drift.
- The user asked whether per-ISA function multiversioning should be
  adopted, preferring compiler features over hand-written kernels.

**Decision.**

- **Benchmarks** use `-Dcpu=native` (or at least x86-64-v3) with
  `buildtype=release`, pinned and isolated. Comparisons use
  `tools/ab_bench.py`: ABBA order, paired ratios, and a difference is
  called only outside ±3% with at least 3/4 of pairs agreeing.
- **CI moves from `corei7` to `x86-64-v3`** (AVX2, BMI1/2, FMA, MOVBE).
  GitHub's x64 Linux runners are AMD EPYC 7763 (Zen 3), which supports v3
  but not v4 (no AVX-512). A workflow step checks that the runner supports
  the floor (`ld.so --help` hwcaps) and fails loudly if not; CI once hit
  an ISA mismatch between the runner that built the cached DPDK and a later
  runner. v3 is also the recommended portable ISA for distributable builds
  (README).
- **The protobuf-generated library** keeps `(maybe-)uninitialized` as
  warnings, so release builds work.
- **No multiversioning for now.** The same code built `corei7` against
  `native` (8 ABBA pairs):
  - P-core: the new table paths are 4-8% faster; the old paths show no
    clear difference;
  - E-core: no meaningful difference (±4%, some cells slower).

  The hot kernels (CRC32 via `crc32`, compares, masking) already compile
  to the best instructions at the SSE4.2 baseline.
- **Order of preference if ISA work is ever needed:** build flags;
  compiler `target_clones` on batch-level kernels; portable SIMD
  (`std::experimental::simd`, C++26 `std::simd`); hand-written intrinsics
  only with a measured need.

**Evidence.** uops.info: `crc32 r64` is 1 uop, 3-cycle latency, 1 per
cycle (3 per cycle on Zen 5) on Alder Lake P/E and Zen 3/4/5; `pause` costs
about 160 cycles on the Alder Lake P-core against 62-65 elsewhere (control
paths only). The rest of the table is in MODERNIZATION §31.4.

**Instruction sets beyond v3** (surveyed 2026-09-26; nothing is critical
today):

- **x86-64-v4 / AVX-512 and AVX10.1/10.2** (Intel Xeon, AMD Zen 4/5; not
  Intel client parts or GitHub runners). Useful for:
  - 64-byte key compare/mask in one instruction;
  - mask registers;
  - `vpcompressd` to compact candidate lists;
  - `vpopcnt`.

  AVX10 has AVX-512's semantics for our purposes, so one v4 variant covers
  both. DPDK already dispatches `acl`, `fib` and CRC at run time. First
  candidates, via `target_clones`, only when measured SIMD-bound:
  wide-key masking and candidate compaction.
- **APX** (32 GPRs, 3-operand forms, conditional compare/move; upcoming
  Diamond Rapids / Nova Lake; GCC 14+ `-mapxf`, Clang 19+). A
  recompile-only win for register-bound batch loops and branchy merges.
  Add an APX `target_clones` variant once hardware exists; Intel SDE can
  check correctness before then.
- **WAITPKG** (`umwait`/`tpause`; AMD `mwaitx`). Power-efficient idle for
  polling workers, and cheaper control-side spins than `pause` (~160
  cycles on Alder Lake-P). Belongs with the power-aware-idle item (DPDK
  `rte_power_monitor`).
- **VPCLMULQDQ/GFNI:** long-buffer CRC (DPDK `net_crc`) and the CRC
  linearity trick. **VAES:** IPsec via DPDK crypto drivers.
  **MOVDIR64B/ENQCMD:** DSA/QAT submission. **CLDEMOTE:** cross-core
  handoff (Phase L). Each only when its consumer arrives.
- Not relevant: AVX-VNNI, AMX, AVX-IFMA, LAM.

**Revisit when:**

- a kernel becomes SIMD-width bound (for example wide keys, or
  checksumming);
- APX or AVX10.2 hardware is available to measure on;
- deployments target Zen 3 with portable builds (the `bsf` penalty);
- a new CPU generation changes these costs.

## D-017 HashLB and ACL on mode G, L2Forward on mode C; control commands off the worker pause

**Status:** accepted (2026-09-26).
**Code:** `core/modules/hash_lb.{h,cc}`, `core/modules/acl.{h,cc}`,
`core/modules/l2_table.h`, `core/modules/l2_forward.cc`,
`core/utils/ip.{h,cc}` (`Ipv4Prefix::Parse`).

**Context.** These modules' commands paused every worker for each change.
D-013 says a pause is needed only for graph changes. The mode for each
follows from the size and change rate of its state (D-004).

**Decision.**

- **HashLB, mode G.** The mode, gate list and field layout form one
  immutable `Config` published through an `RcuPtr`. A batch reads it once.
  The field-layout table is built per `set_mode` and shared between
  configurations. The configuration is small and rarely changes, which is
  what G is for.
- **ACL, mode G.** The rule list is copied, appended to and republished.
  `add` is all-or-nothing, and `clear` publishes an empty list. The rule
  lists this module is meant for are small. Large ones belong to `rte_acl`,
  which is itself a build-then-swap structure.
- **L2Forward, mode C.** MAC tables can be large and churn. `l2_table`
  keeps its layout (4-way buckets of 8-byte inline slots) and becomes
  single-writer with lock-free readers:
  - every slot write is one whole-word store (release);
  - a reader matches on one relaxed load of the slot and re-checks the
    word it matched, never trusting a SIMD lane on its own;
  - a compiler fence separates the primary probe from the alternate;
  - a cuckoo move writes the alternate slot before clearing the primary,
    so a reader can find an entry in the middle of its move;
  - no grace period is needed: slots hold values, not pointers.
- **Wire validation before narrowing,** in every command touched: ports
  ≤ 0xffff, gates < MAX_GATES, MAC ≤ 48 bits, `populate`'s `gate_count` in
  1..MAX_GATES (it was a divide by zero). Multi-entry `add`/`delete`
  validate everything first. L2 `add` also rolls back on ENOMEM, so a
  refused command leaves the table unchanged.
- **`Ipv4Prefix::Parse`** is strict ("d.d.d.d/len"). The constructor no
  longer throws on malformed input (`std::stoi` used to take the daemon
  down on an ACL rule).

**Evidence.**

- L2Forward lookups, auto body, native release, 8 ABBA pairs (`tools/ab_bench.py`
  with `BM_L2Forward/2/` in `modules_table_scale_bench`), ns per 32 keys,
  new / old:

  | entries | CPU 2 (P-core) | CPU 14 (E-core) |
  |---|---|---|
  | 4K | 102 / 140 (−27%) | 131 / 231 (−44%) |
  | 64K | 115 / 151 (−24%) | 145 / 231 (−38%) |
  | 1M | 235 / 283 (−17%) | 214 / 294 (−23%) |
  | 4M | 416 / 467 (−11%) | 476 / 550 (−13%) |

  All pairs agree, except 7/8 at 4M on CPU 14. The CPU 2 run passed
  `--allow-busy`: one browser renderer at 25% on another CPU.
- **Why faster:** the old probe compared integer slots with
  `_mm256_cmp_pd`, a floating-point compare. That meant FP-domain latency
  and bypass delays, and it was also a correctness bug. With
  denormals-are-zero set, every denormal slot compares equal to every
  denormal key (a masked MAC slot is a denormal double), so a lookup could
  return another MAC's gate. The new probe uses `_mm256_cmpeq_epi64`.
- **Tests** (`core/modules/l2_table_test.cc`; each mutation checked):
  - `DenormalsAreZeroDoesNotMatchEverything` and
    `MacZeroMissesOnAnEmptyTable` fail on the old double compare;
  - `EntryIsFoundInTheMiddleOfItsMove` is deterministic, using a move hook
    inside the move. It fails with the reversed move order;
  - `ReadersNeverMissAStableEntryDuringChurn` is a stress test, which did
    *not* catch the reversed order (D-007: stress is not proof).
- **Live-daemon tests** `hash_lb.py`, `acl.py` and
  `l2forward.py::test_l2forward_validation` fail against the old code:
  commands were accepted, or the daemon crashed.

**Amendment (2026-09-27): how the AVX2 probe reads shared slots.** An
external review pointed out a flaw in the probe. It read a bucket's four
slots with a C++ vector load (`_mm256_load_si256`) while the writer stored
them with atomics. That is a data race in the C++ memory model, and so
undefined behaviour, even though matches were re-checked with an atomic
load. ThreadSanitizer (a standalone harness of the churn test, run once)
reports 8 races between `l2_store_slot` and `_mm256_load_si256`.

The probe now issues the 32-byte load as inline assembly (`vmovdqa`),
which the compiler cannot reason about, and keeps the atomic per-candidate
re-check. With it, ThreadSanitizer is clean and the churn test misses
nothing. Its correctness rests on x86 behaviour:

- a slot no one is writing reads back intact;
- a slot being written may read torn, which gives at most a false
  candidate (the re-check rejects it) or a miss of an entry mid-move
  (covered, because moves write the alternate slot first).

Alternatives, with native release builds, 8 ABBA pairs, and ns per 32 keys
against the plain vector load. The numbers are paired medians over 4K..4M
entries, CPU 2 / CPU 14:

| probe | CPU 2 | CPU 14 |
|---|---|---|
| **inline-asm vector load + atomic re-check (chosen)** | +0.6..+2.4% (noise) | +1.8..+2.6% (noise) |
| four atomic loads assembled in registers (`_mm256_set_epi64x`); fully conforming | +10% at 4K/64K, ~0 at 4M | **+12..+56%** |
| four atomic loads stored to an aligned array, then a vector load | +55..+209% (store-forwarding stall) | +38..+146% |
| four scalar atomic probes, early exit | +54..+224% | +44..+186% |
| four scalar atomic probes, branchless mask | +30..+88% | +21..+94% |

The fully conforming register version is the fallback if an x86
implementation ever tears aligned 8-byte lanes of a vector load. Its
E-core cost comes from the insert and shuffle uops.

**Rejected.**

- *L2Forward on mode W* (per-worker replicas): memory × workers for
  tables that can be large, and every update applied N times.
- *L2Forward moved onto `ConcurrentExactTable`:* the inline table already
  resolves a hit in one or two cache lines with no value indirection, and
  it is now faster than before.
- *ACL on a concurrent structure:* first-match order over a list has no
  in-place update that keeps order cheaply. G fits the size.

**Revisit when:**

- ACL rule sets grow beyond a few hundred (then `rte_acl`, still G);
- L2 learning moves into the packet path (the writer would become a
  worker, so mode W or a per-worker learn queue);
- a second writer is ever needed for `l2_table`.

## D-018 BPF execution: DPDK `rte_bpf` (with a repair pass) or the BESS JIT

**Status:** open, deferred (2026-09-26). The BPF module still uses the
BESS JIT. The work queue is in MODERNIZATION §31.6.
**Code:** `core/utils/bpf_program.{h,cc}` (`BpfProgram`, not yet used by
the module), `core/utils/bpf_program_test.cc`.

**Context.** The BPF module carries its own x86-only classic-BPF JIT. It
reads only the first segment, and elsewhere it falls back to libpcap's
interpreter. DPDK's `rte_bpf` offers eBPF with x86 and arm64 JITs and an
mbuf-aware packet access, and `rte_bpf_convert()` turns pcap's classic
programs into eBPF.

**What is known.**

- **A differential test** compares `rte_bpf` against libpcap's
  `bpf_filter()`: 41 expressions × 4,000 generated packets, whole and split
  across two mbufs.
- **It found two DPDK bugs,** both still on DPDK `main`:
  1. the converter gives byte and word indirect loads the accumulator as
     their base register;
  2. the x86 JIT truncates the immediate of `jset #k` for k in −128..127,
     which gives wrong verdicts or a crash.

  `BpfProgram::Repair` works around both, and each workaround was
  mutation-checked. With it, the test agrees everywhere, and the x86 JIT
  is in use.
- **What DPDK tests:** its own test only converts and loads sample filters;
  it never compares verdicts.
- **What DPDK claims:** its guide lists cBPF as unsupported, although it
  ships the converter and `dpdk-dumpcap` uses it.

**Open questions (for the decision):**

- upstream fixes and tests;
- a wider differential and fuzz corpus to find anything else;
- an ABBA comparison against the BESS JIT;
- whether to offer eBPF (ELF) programs to module users directly.

**Revisit when:** the deferred work in MODERNIZATION §31.6 is resumed.

## D-019 DRR: a multi-producer ingress ring; the task's worker owns all flow state

**Status:** accepted (2026-09-27), with a single-worker cost recorded.
**Code:** `core/modules/drr.{h,cc}`, `bessctl/module_tests/drr.py`
(`test_drr_cross_worker`).

**Context.** DRR allows any number of workers. Upstream workers ran
`ProcessBatch`, which looked flows up, created them, and could replace a
flow's queue with a larger ring, freeing the old one. DRR's own task, on its
worker, dequeued from those same queues and deleted expired flows. Whenever
an upstream worker and the task's worker differ (a legal placement), this
races: a queue can be freed under the consumer, or a flow deleted under the
producer. The flow map itself is not concurrent either.

**Decision.** The module is split at a queue, like `Queue`:

- `ProcessBatch` (any worker) only enqueues the batch onto a
  multi-producer, single-consumer ingress ring, and drops when the ring is
  full.
- The task drains up to 1,024 ingress packets per run, then classifies and
  schedules as before. It is the only reader and writer of the flow map,
  the flow queues and the round-robin ring: worker-owned state, needing no
  grace period and no pause (D-013).
- The quantum and the maximum flow-queue size are relaxed atomics, so both
  commands are `THREAD_SAFE`.
- Also fixed:
  - `Flow` left `queue` uninitialized, which its destructor read;
  - the destructor removed map entries while iterating the map;
  - a flow leaked when its queue allocation failed.

This is the worker-owned case of D-004. It needs no control-to-worker
operation rings: the commands are two scalars.

**Evidence.**

- **The old code crashed.** `drr.py::test_drr_cross_worker` (two producer
  workers, and the DRR task on a third worker rate-limited to 2 Mpps so
  queues grow) took the daemon down in 3 of 3 runs, even with the live
  commands removed. Each daemon log ends at `*** Resuming ***`. The new
  code passes 3 of 3, with live commands throughout. This is a stress
  test, not a proof (D-007); the ownership argument above is the proof.
- **The single-worker cost:** Source → RandomUpdate (64 flows) → DRR → Sink,
  DRR's task on the same worker. `bessd` runs under
  `omarchy-benchmark --cpu 2 --isolate`, with native release builds and 8
  ABBA pairs of 3 s windows. The old code measured 11.90 Mpps and the new
  11.47 Mpps. The paired new/old median is 0.972 (0.932..1.052), and the new
  code was faster in 1 of 8 pairs. That is about −3% for the extra ring hop
  per packet.

**Amendment (2026-09-27, external review).**

- **Flow creation completes or is undone.** A new flow now ends up both in
  the flow map and on the round-robin ring, or in neither, and its first
  packet is always queued or freed. The order is: queue, then map, then
  ring (rolling back the map on failure), then the first packet (a
  failure there drops only the packet).
- **Tests (`core/modules/drr_test.cc`):** deterministic fault injection at
  each step through a friend test peer, and a leak check against the
  packet pool. The check was run once against two broken versions: without
  the map rollback the process crashes, and without the packet free the
  leak check fails.
- **The packet contract.** `GetId` read Ethernet, IPv4 and L4 headers
  through raw pointers, with no length, IHL, protocol or fragment checks.
  It now reads through `PacketCursor`, so reads are bounds-checked and
  chained packets work:
  - an untagged IPv4 packet with a complete header gives the 5-tuple;
  - ports are read only for TCP and UDP first fragments that carry them,
    and are 0 otherwise;
  - anything else maps to one fallback flow, so DRR still schedules every
    format, as documented.

  Tests cover short, foreign (ARP, IPv6), bad-IHL, non-first-fragment,
  truncated-L4 and ICMP packets, and a chain split at every byte.

**Rejected.**

- *Limiting DRR to one worker:* it would forbid the ordinary placement of a
  scheduler task on its own worker.
- *A concurrent flow map plus grace periods for queues:* every enqueue
  would pay for sharing state that one worker can own.

**Revisit when:**

- the single-worker cost matters. Classifying directly when the producer
  is the task's own worker would remove the hop, but it needs a race-free
  notion of the owner across task moves (for example, the owner published
  at graph-change time);
- DRR gains per-flow state that commands must write (then mode W op rings).

## D-020 Dataplane transactions: what we borrow from DPDK `rte_swx`, P4Runtime and VPP

**Status:** accepted (2026-09-27). This is the prior-art study that
G1.2b's design (MODERNIZATION §14.5) was waiting for.
**Sources read:**

- DPDK 25.11.3 `lib/pipeline/rte_swx_ctl.{h,c}`;
- the P4Runtime specification source (`p4lang/p4runtime`
  `docs/v1/P4Runtime-Spec.adoc`, the sections "Batching and Ordering of
  Updates", "Batch Atomicity" and "Error Reporting");
- VPP `src/vppinfra/bihash_template.{h,c}` and `src/vlibapi/api_shared.c`
  (master, 2026-09-27).

**What each does.**

- **DPDK `rte_swx` (the SWX pipeline control API):**
  - Changes are scheduled per table (add, modify, delete, default entry)
    and applied by `rte_swx_ctl_pipeline_commit(ctl, abort_on_fail)`.
  - The commit has two stages. `table_rollfwd0` does all fallible work for
    every table: it applies pending entries to a shadow table state
    (`ts_next`), or builds a new table object for tables with no
    incremental update. If any table fails, all of them roll back.
    `table_rollfwd1` then does the work that cannot fail.
  - One pointer store (`rte_swx_pipeline_table_state_set`) then swaps the
    whole table state, and the same changes are replayed onto the old
    copy to bring it back in sync.
  - Every table is kept twice. The library has no reader-reclamation
    protocol: the old copy is modified right after the pointer swap.
  - With `abort_on_fail == 0`, a failed commit keeps its pending work for
    a retry.
- **P4Runtime `Write`:**
  - The server "may arbitrarily reorder messages within a batch", but
    processing "must be strictly serializable" across requests.
  - Dependent updates (an action-profile member and the table entry
    pointing at it) must go in separate `Write` calls, or "the behavior
    may be non-deterministic".
  - Atomicity: `CONTINUE_ON_ERROR` is required; `ROLLBACK_ON_ERROR`
    (all-or-none, but packets can see intermediate states) and
    `DATAPLANE_ATOMIC` are optional and may return `UNIMPLEMENTED`. The
    spec suggests keeping half of each table spare for `DATAPLANE_ATOMIC`,
    followed by a pointer swap.
  - Errors: one `p4.Error` per update, in request order (`OK` for the
    ones that succeeded).
  - Concurrency control is primary-controller arbitration by
    `election_id`. There is no request id, idempotency or retry protocol.
- **VPP:**
  - bihash readers are lock-free. They wait while a bucket's lock bit is
    set (a split is in progress), and after searching they re-read the
    bucket word and retry if it changed (a seqlock-style check). Replaced
    pages go to per-size freelists and are reused only as bihash pages, so
    a stale read is caught by the retry, never by freeing memory. This is
    type-stable memory with a version re-check.
  - The binary API runs each handler not marked `is_mp_safe` under
    `vl_msg_api_barrier_sync()`, which stops every worker. The default is
    a global pause.

**Decision: borrow.**

1. **Two phases, all fallible work first, across every resource of the
   transaction** (`rte_swx`'s rollfwd0 and rollfwd1). This is our
   reserve/publish: publish consists only of infallible stores, so
   rollback happens in reserve, before anything is visible.
2. **Per-operation results in request order** (P4Runtime): a rejected
   transaction reports which operation failed and why, with the rest
   marked not-applied.
3. **Strict serializability** (P4Runtime): one sequencer orders
   transactions. For now that is the control thread, which is already the
   single writer of every C table. Later, one writer per table (§14.5)
   must keep the same guarantee for transactions that span tables.
4. **Named atomicity levels, stated precisely** (P4Runtime's enum):
   - every transaction is all-or-nothing. It is stronger than
     `ROLLBACK_ON_ERROR`, because a failure happens before anything is
     visible, so packets never see a failed transaction's intermediate
     states;
   - packets see per-operation states of a successful transaction by
     default;
   - dataplane atomicity (`DATAPLANE_ATOMIC`) is available per scope,
     through the opt-in scope cell, and is refused, not faked, where a
     table cannot provide it.

**Decision: diverge.**

5. **BESS orders dependent operations inside a transaction; the client
   does not split batches.** P4Runtime pushes dependency ordering to the
   client, which is exactly the burden G1.2 exists to remove
   (§14.5, "clients do not order BESS internals"). Referents publish
   first; removals run in reverse, with retirement after a grace period.
6. **Idempotency: `request_id` plus `GetTransaction`.** P4Runtime has no
   answer for "did my timed-out write apply?"; OMEC's controller shows
   the cost.
7. **Optimistic concurrency (`expected_generation`) instead of
   primary-controller arbitration.** A single-controller deployment needs
   no election. Arbitration can be layered on later for HA controllers.
8. **No double-buffered tables** (the `rte_swx` shadow, and P4Runtime's
   half-capacity suggestion). They cost twice the memory, and `rte_swx`
   replays onto the old copy with no grace period. Our C tables change in
   place with QSBR-protected reuse (D-001, D-013), and all-at-once
   visibility comes from the scope cell (one indirection for the scopes
   that ask for it), not from copying tables.
9. **A failed transaction keeps nothing pending** (unlike `rte_swx`'s
   `abort_on_fail == 0`). A retry is a new request, since state is
   desired-state, not a pending queue.
10. **No global barrier as the default** (VPP's non-`mp_safe` handlers).
    Commands and transactions run while workers keep processing. Pauses
    are only for graph changes, and those move to RCU publication later
    (D-013).

**Recorded for later, not adopted now:**

- VPP's type-stable pages with a version re-check are the known
  alternative to QSBR for our own table, should one ever be written
  (MODERNIZATION §31.5). They cost a re-read per lookup, and a writer
  holding a bucket lock blocks readers of that bucket.

**Revisit when:**

- a multi-controller (HA) deployment appears (then arbitration);
- a target needs dataplane atomicity for a table the scope cell cannot
  cover.

## D-021 The G1.2b transaction engine: reserve/publish, reference counts, dependency-ordered publish, a removal cascade

**Status:** accepted (2026-09-27).
**Code:**

- `core/dataplane/{resource.h, transaction_engine.{h,cc}, slot_table.h,
  slot_resource.h}`, `core/classifier/exact_rule_resource.h`;
- tests: `core/dataplane/transaction_engine_test.cc`;
- benchmark: `core/dataplane/transaction_bench.cc`.

**Context.** Controllers (OMEC's `pfcpiface` is the worked example) order
module commands, roll back by hand, and cannot tell whether a timed-out
batch applied. Section 14.5 designs transactions over registered
resources; D-020 fixed what to borrow from prior art.

**Decision.**

- **Resources** are named, keyed collections that a module registers. A
  resource reserves an operation (all fallible work, nothing visible) and
  returns a `StagedOp` whose `Publish` cannot fail. Ready-made adapters
  mean a module author writes no reserve/publish code:
  - `SlotResource` over a `SlotTable` (id → immutable object, mode C);
  - `ExactRuleResource` over a `ConcurrentExactTable`.
- **`Apply(ops)` works in stages:**
  1. an `expected_generation` check;
  2. structural checks: known resource, one operation per key, a value on
     every upsert;
  3. reserve every operation;
  4. reference checks: every reference left must name a key that exists
     afterwards, and an erased key must have no references left; the
     engine keeps the reference counts;
  5. publish, with upserts in ascending rank then erases in descending
     rank;
  6. one grace period retires what the transaction replaced.

  A failure in stages 1-4 aborts every reservation: nothing is visible.
  Results are per operation, in request order; a scope guard makes the
  abort path impossible to skip.
- **Removal cascade.** An erased key that might still be referenced by an
  in-flight reader stays readable (`SlotTable::Retire`). Stage by stage, by
  rank from highest down, `Unpublish` runs one grace period after the
  previous stage (the first stage one grace period after the publish). A
  reader that saw a referrer just before it went away is done before its
  referent goes. Ids are reusable only after their stage has run, and
  `ReclaimRetired()` advances the cascade (every `Apply` calls it first).
- **A resource whose erases take effect at once** (`DefersErase() ==
  false`; an rte_hash rule table) cannot be referenced. The engine refuses
  such references.
- **Serializability:** one transaction at a time (the engine's mutex); the
  engine is the single writer of every registered resource.

**Evidence.**

- **Tests (13):**
  - dependency order: the request order doesn't matter, and publish order
    is recorded;
  - rejection with nothing visible, for a missing referent, a referenced
    erase, a full table, a structural error or a conflict;
  - retirement waits for readers, and ids are not reused early;
  - an erased referent stays readable for a reader holding its id;
  - 3000 random transactions against a model, with reference counts and
    no-dangling invariants checked after each;
  - two concurrent-reader stress tests (rte_hash-rooted, and all
    slot-table chains);
  - references to an immediate-erase resource are refused.
- **Mutation checks, run once:**
  - a missing erase check fails the referenced-erase test;
  - reversed publish order fails the ordering and concurrent-reader
    tests;
  - unpublishing at once fails the retirement test and crashes the stress
    run;
  - a single-stage cascade fails the slot-chain test.
- **ThreadSanitizer** harness (three slot-table levels, about 33K
  transactions and 4.3M resolutions per run): no reports, and 0 dangling.
  Before the cascade, the same harness found 73 dangling resolutions,
  because each table reclaimed its erased slots independently.
- **Rate:** a session (two meters, two actions, two rules) created and
  removed takes about 4.1 µs, which is 241K sessions/s on CPU 2 and 177K on
  CPU 14 (native release, isolated). The §14.5 target is 100K.

**Amendment (2026-09-27, external review of 13a43de1).**

- **Unregister vs pending removals.** `Unregister()` checked only
  reference counts, but a pending removal-cascade step captures the
  resource's table. The sequence erase, then unregister, then destroy the
  table, then `ReclaimRetired()` ran the step against a destroyed table.
  Every step now records its owning resource, and `Unregister()` advances
  the cascade and then refuses while any step for that resource is
  pending. Test: `UnregisterWaitsForPendingRemovals` (a delayed reader,
  three register/unregister cycles). Mutation: without the check, a step
  runs on the destroyed table and trips `Unpublish`'s state check.
- **Ranks are validated, not trusted.** Every new reference must go from a
  strictly higher rank to a lower one; equal ranks and self-references are
  refused. Otherwise a referrer could publish before its referent. Test:
  `RankViolationsAreRefused`.
- **`ExactRuleResource` prepares for real.**
  - Aggregate capacity cannot promise placement: keys crafted to share an
    rte_hash bucket pair fill it at 16, whatever the table's size (the
    CRC32C seed is fixed). The old `CHECK` in `Publish` then aborted the
    daemon, a crash a controller could trigger.
  - Now a new key is inserted in `Reserve` with the value `kPending`,
    which readers treat as a miss (`VisibleHits()`). A key that doesn't
    fit rejects the transaction with nothing visible, and `Abort` erases
    the pending keys.
  - `Publish` only updates a present key, which rte_hash does in place
    and cannot fail. That DPDK behaviour is pinned by
    `ConcurrentExactTableTest.UpsertOfAPresentKeySucceedsWhenFull` (D-007).
  - Test: `KeysThatCannotBePlacedRejectCleanly` (20 keys sharing a bucket
    pair are rejected cleanly; 16 fit). Mutation: the old aggregate-only
    reserve reproduces the abort.
- **The RCU retire queue** was a vector erased from the middle on every
  `ReclaimReady()`. It is now a deque reclaimed from the front up to the
  first incomplete token (tokens complete in order), so the cost is
  O(reclaimed).

**Cost, and who pays it** (native release, `omarchy-benchmark --isolate`
on two CPUs of one type; `transaction_bench`):

| | P-core (CPUs 2,4) | E-core (CPUs 14,15) |
|---|---|---|
| one rule insert or delete, direct into the table | 44 ns | 59 ns |
| the same as a one-operation transaction | 307 ns | 378 ns |
| session of 6 operations, created + removed, no reader | 4.2-4.4 µs (226-236K/s) | 5.9-6.0 µs (167-169K/s) |
| the same with a reader online, quiescing every ~10 µs | 4.6 µs (216K/s) | 6.6 µs (152K/s) |

- **Packets pay nothing**, with or without transactions. The engine runs
  on the control side; `SlotTable::Lookup` is one acquire load, and a rule
  table's lookup is unchanged apart from dropping `kPending` hits, and
  only in tables registered as resources.
- **Direct commands stay direct.** A module's own commands keep writing
  their tables directly when those tables take part in no references
  (they neither name nor are named by another resource). They are
  serialized with the engine by the control-plane lock, which the engine's
  RPC path (G1.2c) holds as `ModuleCommand` does. Their rate is
  unchanged.
- **Resources that take part in references are written only through the
  engine**, because a direct write would bypass the reference counts.
  Their one-operation commands pay about 260 ns more (roughly 7×) until
  the engine's per-operation overhead is reduced. That overhead is string
  keys, `std::any`, map lookups and a heap-allocated staged op per
  operation; interned handles, typed operations and small-vector staging
  are queued (MODERNIZATION §31.4).
- **An earlier run appeared to show readers slowing transactions** (51K
  sessions/s). The benchmark thread and the reader thread were sharing one
  CPU, because DPDK's EAL pins the main thread and threads inherit it. The
  benchmark now places the reader on its own CPU.

**Amendment 2 (2026-09-27): adopted from two external design documents,
each point checked first.**

- **A declared dependency graph replaces caller-assigned ranks.**
  - A resource declares, at construction, which resources its values may
    reference. They must be registered first, so the graph is acyclic by
    construction, and the engine derives the rank (0, or 1 + the highest
    declared dependency's rank).
  - A value naming an undeclared resource is refused. The rank comparison
    stays only as an internal invariant (a `DCHECK`), because it cannot be
    violated any more.
  - Test: `DependencyDeclarationsAreEnforced`. Mutation: without the check,
    the invariant `DCHECK` aborts.
- **The full lifetime contract for `Register`/`Unregister`** (which now
  return the reason for a refusal). `Unregister` is refused while another
  registered resource declares this one, while it has live keys, while its
  keys are referenced, or while removal steps for it are pending.
  `Register` is refused for a populated resource that declares references,
  since its existing references would be missing from the ledger.
- **Publication does not allocate.** Retirer storage (sized to the
  operations that can retire, which for rule tables is none), ledger nodes
  for new references, pending-removal counters and the cascade's per-rank
  stages are all allocated before the first publish. Test:
  `PublicationDoesNotAllocate` counts allocations with a replaced global
  `operator new` inside a test-only publication-window hook, across
  establish, re-point, replace and erase: zero. Mutation: dropping one
  reservation is caught. What stays after commit is enqueueing retired
  objects on the RCU deque.
- **Bounded backpressure instead of waiting under the lock.** With readers
  slow to quiesce, the RCU retire queue grew until `RetireErased()` fell
  into `Synchronize()` while holding the engine's lock, a hang for as long
  as the reader stalled. Now a transaction that would push pending
  retirements past half the high-water mark, or add a cascade past 4096
  pending, gets the retriable outcome `kBusy` with nothing attempted. A
  single transaction that could retire more than that is refused with a
  request to split it. Test: `SlowReadersGetBackpressureNotAHang` (a
  reader online that never quiesces). Mutation: without the check, the
  test hangs (killed by its timeout).
- **Checked, and not adopted as a change:** "`rte_hash_del_key` can wait
  for readers synchronously".
  - True in DPDK 25.11.3 when the defer queue is full
    (`rte_cuckoo_hash.c`, the `rte_rcu_qsbr_dq_enqueue` failure path).
  - It cannot happen with our configuration. The default `dq_size` is the
    table's key-slot count (`rte_cuckoo_hash.c:1648-1650`), the ring holds
    at least that (`align32pow2(size + 1)`, `rte_rcu_qsbr.c`), and each
    outstanding entry is one deleted slot. We do not set
    `free_key_data_func`, so the other synchronize path returns early.
  - Pinned as a DPDK behaviour test (D-007):
    `ConcurrentExactTableTest.DeletesNeverWaitForStalledReaders` deletes
    every key of a full table with a stalled reader and requires prompt
    completion.

**Cost of this amendment** (paired ABBA, 8 rounds, against fd836db2,
native release, isolated): P-cores (2,4): no clear difference for
sessions, with or without a reader online; one-operation transactions
+2.7%. E-cores (14,15): sessions +4.3%, one-operation +4.0%, online-reader
sessions no clear difference. Direct table writes unchanged. Before this
comparison, back-to-back runs had suggested +27%; that was machine drift,
and the paired protocol settled it.

**Recorded from the same documents, for the increments ahead** (not built
yet):

- **Scope cell (strict atomicity):**
  - a stable `ScopeCell` points to an immutable `ScopeVersion`, loaded
    once per packet-processing operation and used for every covered
    lookup;
  - only the affected entries are versioned, with old and new
    interpretations kept until the flip, and no copy of a session or table;
  - valid only if the scope is identified independently of the rules
    being changed, and overlapping masked or range rules outside the scope
    cannot change the winning match;
  - otherwise reject the requested atomicity, never downgrade silently;
  - mutable meter tokens and live counters stay separately owned state,
    referenced by the selected policy version, not copied.
- **`rte_lpm` is not a pending-key table:** a staged more-specific prefix
  overrides a less-specific route even with an "invalid" value.
  Ordinary route updates stay direct. Multi-route transactions need a
  prepared route-table generation (K7's build-and-swap), or are refused.
  tbl8 exhaustion is a preparation failure, never a publish failure.
- **IDs carried across asynchronous queues** (packet metadata through a
  worker handoff, a buffer, or a hardware MARK): a CPU grace period does not
  prove the queued packet was consumed. Such IDs need generation-tagged
  handles, or queue-drain protection before reuse.
- **Meters:** the immutable policy object is separate from mutable token
  state, so an unchanged meter is never reset by a transaction (K5 already
  separates profile from state; the provider must keep that).
- **G1.2c:**
  - `request_id` with a digest of the request: replay returns the recorded
    outcome, and reuse with different contents is refused;
  - `GetTransaction` for timeouts;
  - a daemon epoch, so outcomes from before a restart are reported as
    unknown, not as exactly-once;
  - one intelligible transaction record: generations, operations in
    request order, derived dependency order, commit state and outstanding
    reclamation (committed-with-cleanup-pending is committed, not failed).
- **SDK:** temporary references resolved by the SDK; the server derives
  the order. An OMEC layer on top (`ModifySession().PutPDR/FAR/QER`).
- **Acceptance matrix still to build:**
  - update p50/p99 and time spent waiting for grace periods;
  - packet Mpps under 1M modifications/s with workers online;
  - an in-process ExactMatch → Action → Meter → Router pipeline on virtual
    PMDs;
  - failure injection at every prepare allocation and DPDK reservation.

**Amendment 3 (2026-09-27): review of f3919d70, findings verified in the
source first.**

- **Deferred destructors (P0).** A removal step unpublishes an object and
  hands it to RCU, so its destructor runs a grace period later. That
  destructor is code the resource's module instantiated (a plugin's
  `.so`), yet the pending-step count had already reached zero.
  - `RcuDomain::Retire` now takes an optional per-owner completion
    counter, decremented after destruction. The engine keeps one per
    resource, and `Unregister` refuses while it is non-zero.
  - Tests: `UnregisterWaitsForTheLastDestructor` (a destructor probe), and
    `UnregisterWaitsForPendingRemovals`, now requiring a second quiescent
    state. Mutation: without the check, the test segfaults (the destructor
    ran after the table was destroyed).
- **Aggregate deferred backlog (P0).** Admission counted objects already
  queued, but not those that pending cascades would still retire, and the
  reclaimer ran every eligible stage.
  - Three transactions each erasing a budget's worth of objects could
    flood the RCU queue past its high-water mark and block under the lock.
  - Admission now counts deferred objects, and the reclaimer runs a stage
    only if it fits the budget, otherwise leaving it pending.
  - Tests: `ManyDeferredRemovalsCannotBlockTheEngine` (the review's
    scenario) and `ReclaimerHoldsBackWhenTheRcuQueueIsFull` (another RCU
    user fills the queue).
  - Mutations: each check removed fails its test.
- **Exception safety of exact-key preparation (P1).** The staged
  operation and the references (a user callback that may throw) are now
  built before the pending key is placed, which is the last step that can
  fail. Test: `ThrowingReferenceCallbackLeavesNoPendingKey`. Mutation: the
  old order leaks a pending key.
- **An explicit publication footprint (P1).**
  - `Reserve()` returns `Footprint{retires, removals, callbacks}`, and the
    engine reserves exactly the sum: the retirer, the cascade stages, and
    now also room in the RCU retire queue. That queue became a
    vector-backed ring so it can be reserved, which makes the
    post-commit handoff allocation-free too;
    `PublicationDoesNotAllocate` covers it.
  - Exceeding the declared footprint is fatal, which replaces the
    allocating fallback. Test: `ExceedingTheDeclaredFootprintIsFatal`, a
    death test.
  - Backpressure uses the declared footprints rather than
    `DefersErase()`.
- **A lifecycle-wide adversarial suite**
  (`core/dataplane/transaction_lifecycle_test.cc`, 7 tests):
  - **Failure at every reserve position**, as an error and as a thrown
    `bad_alloc`, in four transaction shapes (establish, re-point, delete,
    an 11-operation mix). The full state is compared afterwards: logical
    contents, physical rule-table entries including pending keys, the
    reference ledger and the generation. The same transaction must then
    still apply.
  - **The throwing reference callback**, the two backlog tests, the
    destructor-lifetime test and the footprint death test.
  - **A model-based random lifecycle:** 4000 transactions with injected
    faults at random positions, a reader stalling and resuming at random
    while resolving chains, and random reclamation. The model is checked
    after every step. Per run: 645 applied, 3075 rejected (589 injected,
    55 retiring), about 2.5M chains resolved, 0 dangling.

  Mutations across this suite are all caught: the old prepare order,
  admission without deferred objects, the reclaimer without a budget,
  unregister without the destructor check, an unenforced footprint, and
  aborts that forget pending keys.

**Scaling: packet-path lookups under transactions**
(`BM_LookupsUnderTransactions`).

Each reader, on its own isolated CPU, resolves 32-key batches: a rule
lookup (pending keys dropped), then the action slot, then the meter slot,
over 65K sessions with half live. The readers report quiescence every
batch, far more often than real workers (every 10 µs). The writer
creates one session and removes the oldest per iteration. Native release,
`omarchy-benchmark --isolate`, 2 s per point. Lookups are M/s per reader
(total):

| CPUs | readers | no transactions | 10K sessions/s | writer flat out | sessions/s flat out |
|---|---|---|---|---|---|
| P (0,2,4,6,8) | 1 | 42.8 | 40.0 (−6%) | 36.4 (−15%) | 90K |
| | 2 | 46.2 (92) | 44.2 (−4%) | 41.0 (−11%) | 84K |
| | 4 | 45.0 (180) | 42.3 (−6%) | 40.8 (−9%) | 79K |
| E (12-16) | 1 | 28.8 | 27.1 (−6%) | 21.4 (−26%) | 57K |
| | 2 | 28.4 (57) | 23.2 (−18%) | 21.8 (−23%) | 49K |
| | 4 | 26.5 (106) | 22.5 (−15%) | 22.4 (−15%) | 44K |

- Readers scale linearly.
- Transactions at 10K sessions/s (about 120K table operations/s) cost the
  packet path 4-6% on P-cores and 6-18% on E-cores. With the writer flat
  out (about 1M table operations/s on P-cores) they cost 9-15% (P) and
  15-26% (E).
- No retries: reclamation kept up.
- **Gap:** with busy readers, the writer reaches 79-90K sessions/s on
  P-cores and 44-57K on E-cores, below §14.5's 100K target. The earlier
  216K was with an idle reader and a smaller table. The engine's per-op
  overhead (§31.4) is the lever to work on first.

**Amendment 4 (2026-09-27): per-operation overhead trimmed, and the session-rate
gap closed.**

- **Profiled first** (`perf`, `BM_SingleRule`, the session benchmark).
  String-keyed `std::map`s dominated: `Reference` holds two strings, so the
  per-transaction maps and the persistent ledger paid for tree nodes,
  `memcmp` and `malloc`/`free` on every operation. Each operation also
  checked its key's existence up to three times, each an `rte_hash`
  lookup, and every `Apply` allocated fresh scratch vectors.
- **Changes** (the public API is unchanged):
  - one `Registration` record per resource: its dependencies resolved to
    pointers at registration, its incoming-reference ledger as a hash map
    looked up by `string_view`, its destructor counter and pending-removal
    count. It replaces four maps.
  - `Reservation` reports `existed` and `previous_references`, since the
    resource found the key anyway.
  - per-transaction duplicate detection and reference deltas are flat
    vectors, sorted and merged once.
  - scratch storage is reused across transactions.
- **A bug found and fixed on the way.** Rejections decided after an
  operation's own `Reserve()` (an undeclared reference, a reference into
  an immediate-erase resource) returned before that operation joined the
  abort list, leaking its pending key.
  `ReferencesToAnImmediateEraseResourceAreRefused` caught it. The new
  lifecycle test `RejectionsAfterReservationUndoTheirOwnReservation` guards
  every post-reservation rejection reason; re-introducing the bug fails
  it.
- **ABBA** (8 rounds, against 8db4a3ea, native release, isolated):

  | | P-cores (0,2,4,6,8) | E-cores (12-16) |
  |---|---|---|
  | session, create + remove | −35.5% (8/8) | −38.1% (8/8) |
  | one-operation transaction | −24.4% (8/8) | −24.2% (8/8) |
  | session with an idle reader | −35.0% (8/8) | −35.7% (8/8) |
  | per session, 1 busy reader | −55.1% (8/8) | −57.3% (8/8) |
  | per session, 4 busy readers | −53.2% (8/8) | −54.2% (8/8) |

- **Scaling with the trimmed engine** (one run, M lookups/s per reader):

  | CPUs | readers | none | 10K sess/s | 100K sess/s | flat out | sessions/s flat out |
  |---|---|---|---|---|---|---|
  | P | 1 | 56.5 | 52.8 (−7%) | 50.1 (−11%) | 46.5 (−18%) | 241K |
  | | 4 | 49.3 (197) | 47.2 (−4%) | 45.6 (−8%) | 44.8 (−9%) | 194K |
  | E | 1 | 33.6 | 32.9 (−2%) | 31.6 (−6%) | 31.0 (−8%) | 168K |
  | | 4 | 29.2 (117) | 28.3 (−3%) | 27.1 (−7%) | 27.8 (−5%) | 117K |

  The §14.5 target of 100K sessions/s is now met with 1-4 busy readers on
  both core types. At that rate (about 1.2M table operations/s) readers pay
  6-11%. Absolute baselines differ from amendment 3's run (machine-level
  variance; the engine plays no part at rate 0), so compare only within a
  run, or the ABBA table.
- **Evidence of correctness:** 18 engine tests and 8 lifecycle tests pass,
  the TSan harness is clean (its writer now completes ~31K transactions
  per run against ~22K), and the model-based random lifecycle is
  unchanged.

**Amendment 5 (2026-09-27): external review of 9cb5314 -- allocation
failures anywhere in `Apply()`, and a publication contract plugins can
keep.** Each finding was checked against the source before acting.

- **Two exception windows (confirmed, fixed).**
  - A resource was marked "touched" before it was recorded, so a failing
    `push_back` left the mark set: that resource was never told
    `EndTransaction()` again.
  - An operation's staged work was recorded for abort by a `push_back`
    that could allocate *after* `Reserve()` had placed a pending key; a
    failure there leaked the key.

  Now the abort bookkeeping is reserved for all operations before the
  first `Reserve()`, and the mark follows the record.
- **A new test fails every allocation `Apply()` makes, one at a time**
  (`LifecycleAllocationTest`, a thread-local fault countdown in a global
  `operator new`), in three settings: the engine's first transaction (no
  scratch capacity yet: 59 allocation sites), a transaction twelve times
  larger than any before (98), and one that starts by advancing a pending
  removal cascade (30). After each failure the state (contents, pending
  keys, reference ledger, generation) must be unchanged, every resource
  asked to reserve must have been told the transaction ended, and the
  same transaction must then apply. Mutations: removing the reservation of
  the abort list fails it (the leaked pending key reproduced at the 12th
  allocation of a first transaction); restoring the old mark-then-record
  order fails it ("EndTransaction() skipped").
- **The publication contract (confirmed gap, closed where the API can
  close it).** `RemoveLater()` and `AfterGracePeriodStarts()` took a
  `std::move_only_function` built by the caller inside `Publish()`, whose
  inline buffer is small and library-specific: a large capture would
  allocate inside a `noexcept` function, before the footprint check.
  - The Retirer's callables are now `utils::InlineFunction` (48 bytes,
    inline, never on the heap). A larger capture is a compile error that
    says to prepare the state in `Reserve()` and capture a pointer to it
    (checked once by hand; the built-in adapters use 16 bytes).
  - What the API cannot see -- a custom `Publish()` allocating in its own
    code -- is its author's contract, documented on `StagedOp::Publish`:
    built-in adapters are checked by `PublicationDoesNotAllocate`; custom
    ones use the same instrumentation. A reusable test-support library for
    that belongs with the plugin package (MODERNIZATION §31.0 row 5).
  - Cost, ABBA against a2a665b8 (8 rounds; isolated; P-core CPU 2,
    E-core CPU 14): sessions (1K and 64K) and a session with an idle
    reader show no clear difference on either core type. The one-operation
    transaction on the E-core shows none; on the P-core it is +2.3% (12
    rounds, about 12 ns of 570, under the 3% noise band but slower in most
    pairs). Splitting the change shows the exception-safety fix alone
    carries it (inline storage alone: −0.4%), for +0.4% instructions
    (about 34 per transaction); the rest of the cycles are code layout. An
    inline capacity check instead of the `reserve()` calls did not change
    it. Kept as the price of the fix.
- **Router integration boundary (agreed, next increment).** When Router
  becomes a provider, its commands and the engine will share one writer
  -- the control-plane lock -- as ExactMatch's do (D-022); the standalone
  interface keeps its own mutex for direct use.
- **Visibility must be named in the RPC (agreed).** Until the scope cell
  lands, a successful transaction is all-or-nothing on failure and
  dependency-ordered on publish, not dataplane-atomic. G1.2c will report
  which of the two a transaction got, rather than call every transaction
  atomic.
- **An end-to-end acceptance test (agreed, recorded as the vertical-slice
  gate, §31.0 row 4).** ExactMatch -> action -> meter -> Router over
  virtual PMDs, packets flowing while ordinary updates and transactions
  run, failures injected at every preparation boundary. It will record
  packet Mpps, update latency, rejections, dangling-reference checks and
  the retirement backlog. It needs the action and meter resources as
  modules, which the slice brings. Until then, D-022's benchmark
  classifies packets through real modules, not a full graph.

**Deferred (next increments):**

- Router and the modules as resource providers (ExactMatch: D-022;
  Router: D-023);
- the scope cell (dataplane-atomic scopes; §31.0 row 3);
- `request_id` idempotency and `GetTransaction` (G1.2c, with the RPC).

**Revisit when:**

- one sequencer is not enough (per-table writers need a cross-table
  serialization story);
- a resource needs a removal that cannot wait for the cascade.

## D-022 ExactMatch as the first module resource provider

**Status:** accepted (2026-09-27). The first increment of "modules as
resource providers" deferred by D-021.

**Context.** D-021 built the engine and adapters, and exercised them only
through tables that tests and benchmarks owned. This decision puts a real
module behind the engine, so that one transaction changes several modules'
tables and packets are classified through those modules while it happens.

**Decision:**

- **One engine per runtime:** `RuntimeState::transactions()`, constructed
  over the runtime's RCU domain and destroyed before it. `Apply()` is called
  under the control-plane lock, as module commands are: a module's own
  commands write the same tables, and the engine's prepare-then-publish
  must not interleave with them.
- **ExactMatch registers its rules** as the resource `<module>/rules` in
  `Init()` and unregisters it in `DeInit()`. The key is the packed rule key
  (the fields' bytes in order, as the command path packs them); the value
  is the gate. `IsValidGateValue()` is checked in prepare, like the `add`
  command does. The rules reference nothing and, being erased immediately,
  cannot be referenced, so unregistering cannot be refused (checked fatally
  in `DeInit()`).
- **Module-author ergonomics:** nothing changes for modules that do not
  take part; a module that does wraps its table in a ready-made adapter,
  registers it in `Init()`, unregisters it in `DeInit()`, and treats
  `kPending` as a miss on its packet path. ExactMatch's `add`, `delete` and
  `clear` commands are unchanged and stay direct (the rules take part in no
  references; D-021's rule).
- **Tables that are replaced, not only updated.** ExactMatch grows by
  copying into a larger table and publishing a new generation.
  `ExactRuleResource` gains `Hooks`: the table is fetched at every use
  (`table`), `make_room(force)` lets the owner grow when a new key does not
  fit during prepare, and `check_value` lets it refuse values. Pending keys
  are copied with the rest, and staged operations look the table up when
  they publish or abort, so they act on the table that replaced the one
  they were placed in. Growth happens in prepare, never in the publish
  window, and a rejected transaction leaves the larger table (with none of
  its keys), as a failed `add` after growth does.
- **Unregistering with live keys** is allowed for a resource that declares
  no references: its keys hold no references in the ledger, so nothing is
  left dangling. A module destroyed with its rules no longer needs to erase
  them through a transaction first. Resources that may reference others
  still refuse while they hold keys.
- **The packet path** masks out hits whose value is `kPending` in one
  branch-free, vectorized pass before choosing gates (see the evidence
  for why not in the gate loop). `ExactMatch::ClassifyBatch` exposes the
  exact decision `ProcessBatch` makes (both call one inlined template), so
  tests and benchmarks drive the real module code from reader threads.

**Evidence:**

- Tests (`core/modules/exact_match_transaction_test.cc`, 6): the resource
  lives exactly as long as the module and a new module may take the name;
  one transaction changes two modules or neither; commands and
  transactions see each other's rules; the table grows several times inside
  one transaction and a rejection after growth leaves nothing; a key being
  prepared is a miss on the packet path (observed from inside the publish
  window); and a registered reader thread classifies packets through two
  modules while 20,000 sessions are established and released and the
  tables grow under it, never seeing a placeholder or another key's gate.
- Mutation checks (one-time), each caught: no `kPending` filter (two
  tests fail); staged operations keeping the table they were reserved
  against (crash on the replaced table); no growth in prepare; no
  unregister in `DeInit()` (crash); unregister never refusing live keys.
- Packet-path cost, ABBA (`BM_ModuleClassify`, 32-packet batches through
  `ClassifyBatch`, 32K distinct packets, native release, isolated):
  the filter is a separate branch-free pass building a mask of pending
  hits, which the compiler vectorizes (AVX2 compare and movemask), then
  `hits &= ~pending`. A first version folded the compare into the
  gate-selection loop and cost **+4.2% on an E-core** for cache-resident
  hits (12 rounds, 9/12 pairs); it was replaced. The adopted version, 12
  rounds against the module without the filter:

  | N rules, traffic | P-core (CPU 2) | E-core (CPU 14) |
  |---|---|---|
  | 1K, hits | −1.6% (no clear difference) | +1.8% (no clear difference) |
  | 128K, hits | −1.2% (ncd) | −0.5% (ncd) |
  | 1M, hits | −1.2% (ncd) | −0.4% (ncd) |
  | 1K, misses | −1.1% (ncd) | +1.0% (ncd) |
  | 128K, misses | −1.5% (ncd) | +1.8% (ncd) |
  | 1M, misses | +0.6% (ncd) | +1.1% (ncd) |

  Absolute: 1K rules, hits, about 450 ns per 32-packet batch on a P-core
  (71 Mpps), 710 ns on an E-core.
- Scaling (`BM_ClassifyUnderTransactions`: readers classify through two
  modules, each session one transaction adding a rule to each and one
  removing them, 32K live sessions):
  (one run each; readers on separate CPUs, Mpps per reader):

  | CPUs | readers | none | 10K sess/s | 100K sess/s | flat out | sessions/s flat out |
  |---|---|---|---|---|---|---|
  | P (0,2,4,6,8) | 1 | 42.7 | 43.2 | 44.9 | 37.6 (−12%) | 695K |
  | | 2 | 39.0 | 39.2 | 39.6 | 35.9 (−8%) | 579K |
  | | 4 | 31.3 | 28.0 | 29.6 | 28.2 (−10%) | 519K |
  | E (12-16) | 1 | 28.7 | 30.4 | 30.9 | 29.9 | 627K |
  | | 2 | 25.0 | 24.1 | 26.4 | 26.0 | 558K |
  | | 4 | 18.6 | 22.2 | 22.5 | 21.5 | 465K |

  Up to 100K sessions/s (200K rule changes/s) readers show no cost beyond
  this run's variation (±15% between neighbouring cells with 4 E-core
  readers); a writer running flat out costs P-core readers 8-12%. No
  transaction was refused busy. These are in-process numbers through the
  module's classification code, not a live-daemon Mpps gate (deferred with
  the RPC).

**Deferred:** Router (next hops via `SlotResource`, routes with tbl8
reservation in prepare) and WildcardMatch as providers; an RPC to reach the
engine from a controller (G1.2c), after which a live-daemon Mpps gate under
transactions becomes possible; the scope cell.

**Revisit when:** a module needs its commands and transactions on
different threads (a second writer), or a provider's table cannot be
grown during prepare.

## D-023 Router as a resource provider: routes placed invisibly, one writer when enrolled

**Status:** accepted (2026-09-27). The second module-level provider after
D-022, and the one the external review of 9cb5314 asked for next with a
shared writer boundary (D-021 amendment 5).

**Decision:**

- **Opt-in enrollment.** `Router::Enroll(engine)` registers two
  resources: `<router>/next_hops` (the existing `SlotResource` over the
  router's `SlotTable`; key `EncodeKey(NextHopId)`, value `NextHop`) and
  `<router>/routes` (new; key `Router::RouteKey(prefix)`, value
  `NextHopId`, each route referencing its next hop). Op builders
  (`SetRouteOp`, `SetNextHopOp`, ...) spare callers the encodings.
- **One writer when enrolled.** The direct setters refuse with
  `kEnrolled`: the engine's ledger replaces the router's own reference
  counts, so a direct change cannot bypass it. `RouteReferences()` reads
  the ledger. Enrollment is refused once any route exists or a removed next
  hop is still retiring (the ledger must start from what it can see); next
  hops that already exist are fine (they reference nothing). A router that
  is not enrolled is unchanged.
- **Routes are placed during prepare, invisibly.** rte_lpm cannot promise
  capacity (rules, tbl8 groups) for a set of prefixes, so a new route is
  inserted during `Reserve()` with a placeholder value: what its addresses
  resolve to today -- the longest rule strictly containing it, else the
  default route, else id 0, which the reader already treats as a miss
  (`LpmRouteTable::CoveringValue`). No lookup changes until `Publish()`
  stores the real next hop, an in-place update of an existing rule, which
  cannot fail. A prefix that does not fit rejects the transaction with
  nothing visible; `Abort()` deletes the placeholder.
  - This supersedes the note recorded from the design documents (D-021
    amendment 2, "rte_lpm is not a pending-key table": a staged
    more-specific prefix overrides its covering route, so multi-route
    transactions would need a rebuilt table). A staged prefix whose value
    is the covering answer overrides nothing: every address in it that no
    longer rule claims already resolved to exactly that value.
  - Within the publication window, a placeholder can show the old covering
    value after the covering route itself was re-pointed earlier in the
    same window. That is the dependency-ordered visibility D-021 already
    states (not dataplane-atomic until the scope cell). A next hop the
    placeholder names cannot go away under it: the removal cascade waits a
    grace period after the whole window.
- **Deletes cannot fail** (erase publication, placeholder abort): rte_lpm's
  QSBR defer queue defaults to one entry per tbl8 group. Pinned as a DPDK
  behaviour test (`RouteTableTest.DeletesNeverFailWithAStalledReader`: every
  group freed at once under a stalled reader, twice); shrinking the queue
  to two entries fails it.
- **Resources that reference only each other leave together.**
  `TransactionEngine::Unregister(span)` takes a group: members may go with
  their keys when everything they may reference is in the group, and
  nothing outside the group may reference a member. A destroyed enrolled
  router unregisters routes and next hops as one group (the engine must
  outlive the router). A resource with no references is the one-member
  case of D-022.

**Evidence:**

- Tests (`core/route/router_transaction_test.cc`, 7): enrollment rules and
  the direct setters refusing; next hops and routes in one transaction in
  any order, a route to a missing next hop and removing a used next hop
  rejected with nothing changed, the removal cascade, the resources leaving
  with the router; new routes (nested, a /25 taking a tbl8 group, one with
  no covering route), observed from inside the publication window, change
  no lookup, with and without a default route; tbl8 exhaustion rejects at
  prepare with nothing visible and the groups come back; another resource
  (actions) referencing next hops, which keeps the router's resources
  registered; a registered reader resolving through the router while 6,000
  sessions (a next hop and a route each) come and go, never reaching
  another session's next hop; every allocation of a mixed transaction
  failed in turn (26 sites), leaving routes, next hops, lookups and ledger
  unchanged. Engine: `ResourcesReferencingOnlyEachOtherLeaveTogether`.
- Mutations, each caught: a placeholder that is always a miss (two tests);
  routes not placed in prepare (a publish-time "table full" crash); an
  abort that keeps the placeholder (three tests); group unregister ignoring
  outside dependents. The first run also exposed a real bug: the resource
  names were built from a moved-from string (`/routes`); the names are now
  asserted.
- Cost per change on a P-core (`BM_RouterChange`, 1024 routes, isolated):
  next-hop update 62 ns direct, 228 ns as a transaction; route re-point
  221 ns vs 591 ns; route add+remove 293 vs 744 ns per change. The price of
  one ledger for the router and whatever references its next hops; the
  unenrolled path is unchanged (ABBA below).
  ABBA of the unenrolled path against e3238456 (8 rounds, isolated): route
  lookups (1K/16K/64K) and in-place route updates show no clear difference
  on P- or E-cores; a direct next-hop update shows none on a P-core
  (+1.5%). On an E-core it read +8.5% when run after the other route
  benchmarks in one process, and −8% when run alone (72 vs 78 ns, three
  alternations); the new build executes 6 fewer instructions per update,
  so the ±7 ns follows the heap state earlier benchmarks leave, not the
  code.

**Deferred:** trimming the per-route cost (each `Reference` copies the
next-hop resource name, which exceeds the short-string buffer for
realistic router names); `Clear()` for an enrolled router (a bulk erase);
IPLookup on the same resource.

## D-024 WildcardMatch as a resource provider: pending rules that lose to every rule

**Status:** accepted (2026-09-27). The third module provider (D-022
ExactMatch, D-023 Router); the rule sets named in MODERNIZATION §31.0 row 1.

**Context.** A masked (tuple-space) table cannot use ExactMatch's pending
value alone: a packet may match several rules, and the reader keeps the
best by priority. A rule being prepared must neither outrank an existing
match nor, alone, answer. And a change can create a mask (a new tuple),
grow a tuple's table, or need a new rule record -- all of which can fail.

**Decision:**

- **`ConcurrentMaskedTable` gains a prepare/commit split** (`PrepareUpsert`,
  `PrepareErase`, `Commit`, `Cancel`, `Recycle`):
  - prepare does everything that can fail: a new tuple, a tuple table's
    growth, the rule id and its (write-once) record;
  - a new rule is inserted as a pending entry naming one shared record
    that loses to every rule (lowest priority, sequence 0) and carries
    `kPendingResult`. The table's own lookup drops a best match with that
    result -- the only way it wins is when nothing else matched -- so
    readers need no change and WildcardMatch's `ProcessBatch` has none;
  - a replacement's new record is written but not named until commit;
  - commit is one in-place table store (or delete) and cannot fail; it
    returns the id no longer named. The adapter hands that id back through
    the engine's removal cascade (`Retirer::RemoveLater`), a grace period
    later, so a reader that loaded it just before the swap is done first;
  - neither commit nor cancel can republish the tuple list (it
    allocates), so a mask they empty stays as an empty tuple until the
    cascade's recycle step, the next prepare or the next `add` drops it
    (the legacy `Upsert` now does too, so a rejected transaction cannot
    make a later `add` hit the mask ceiling);
  - the legacy `Upsert` refuses `kPendingResult` (`kReservedResult`): a
    contract change for the generic table, which now offers 65,535 of the
    65,536 results. Gates never reach it (at most 8,192), and WildcardMatch
    is the table's only user. The ABBA below found the one place that
    relied on it: the lookup benchmark numbered results by rule, so its
    65,535th rule was refused ("missed a key" at 128K rules); it now wraps
    below 0xFFFF.
- **`MaskedRuleResource`**, the ready-made adapter: key = mask then value
  (2 x key_len bytes), value = `{priority, result}`; hooks for the current
  table, a value check and references, as `ExactRuleResource`. The freed
  id's cascade step captures a `weak_ptr` to the table it came from
  (24 bytes, inside the Retirer's inline storage), so an id never lands in
  a table that replaced it.
- **WildcardMatch registers `<module>/rules`** in `Init()` and unregisters
  in `DeInit()`, like ExactMatch; gates are checked with
  `IsValidGateValue()`. `ClassifyBatch()` exposes `ProcessBatch`'s
  decision (one inlined template) for tests and benchmarks.

**Evidence:**

- Tests: `ConcurrentMaskedTableTest.PrepareCommitCancelRecycle` (a pending
  rule neither outranks a lower-priority match nor answers alone; a
  prepared replacement leaves the old rule answering; 3,000 replace cycles
  keep the ids in use flat; an emptied mask stays until `Recycle()`), and
  7 module tests (`wildcard_match_transaction_test.cc`): lifetime and
  destruction with rules and a freed id still in the cascade; from inside
  the publication window, a narrower higher-priority rule, a rule in a new
  mask, a rule in an existing mask and a re-gate all change no answer;
  rejection after new masks and a replacement leaves nothing (and the masks
  go with the next change); one transaction across ExactMatch and
  WildcardMatch, both or neither; commands and transactions on the same
  rules, 3,000 rules growing one tuple inside a transaction, 5,000 re-gates;
  ids returning through the cascade with a standalone table and engine
  (ids in use flat, the emptied mask dropped); and a registered reader
  classifying through the module while 8,000 rules come and go across four
  masks, never seeing another rule's gate.
- Mutations, each caught: the reader not dropping pending matches (three
  tests); a pending record that outranks rules (two); a cancel that keeps
  the pending entry (two); `Recycle()` keeping empty tuples (two); freed
  ids never recycled (one).
- Packet path: the reader's one added compare, on the best match of each
  matched packet, ABBA against 19ed9a17 (`BM_Lookup/1`, 32-key batches,
  1/4/8 masks x 1K/128K/1M rules, 10 rounds, isolated): no clear
  difference in any of the 18 cases on P- or E-cores (−1.9..+2.4%; the
  largest lean, P-core 8 masks 1K rules, +2.1%, under the band).

**Deferred:** the remaining per-rule allocations in prepare (the `Prepared`
copies of mask and value); a live-daemon gate, with the RPC.

## D-025 The dataplane transaction RPC: typed per-resource values, request ids, a daemon epoch

**Status:** accepted (2026-09-28). G1.2c (MODERNIZATION §31.0 row 2); the
user chose typed keys and values over raw bytes.

**Decision:**

- **Three RPCs on the v2 `Control` service** (`control_v2.proto`):
  `ApplyTransaction`, `GetTransaction`, `ListTransactionResources`. They
  run under the control-plane lock, as module commands do (both write the
  same tables).
- **Typed keys and values per resource.** A resource that should be
  reachable over the RPC has a `ResourceCodec` bound to it by its owner
  (since D-044; originally a `Resource` member): the
  protobuf message types of its keys and values, and their conversion to
  the engine's key bytes and value. The server packs keys exactly as the
  module's commands do (the codecs call the commands' own parsing), so no
  client reproduces BESS's internal packing -- the OMEC pain point.
  ExactMatch: `ExactMatchRuleKey {fields}` / `ExactMatchRuleValue {gate}`;
  WildcardMatch: `WildcardMatchRuleKey {values, masks}` /
  `WildcardMatchRuleValue {priority, gate}`. `ListTransactionResources`
  names each resource's types. The codec lives in its own header, so the
  engine core stays free of protobuf; a resource without one is not
  reachable (Router, a library, until a module exposes it).
- **Per-operation results in request order**, the engine's: the failing
  operation says why, the others are NOT_APPLIED. An operation that cannot
  be decoded (unknown resource, wrong message type, a module's own
  validation) rejects the same way, before the engine is called.
- **Idempotency by `request_id`.** An outcome is recorded with a digest of
  the request's contents (all but the id; deterministic serialization). The
  same id and contents replay the record (`replayed = true`, nothing
  applied twice); the same id with other contents is refused (ABORTED,
  detail CONFLICT). CONFLICT and BUSY outcomes attempted nothing and are not
  recorded, so a client retries under the same id. Records live in a
  bounded window (4,096, oldest out first).
- **`GetTransaction` and a daemon epoch.** The epoch is drawn at start and
  returned with every answer: an unknown id under the same epoch has not
  run (or aged out); after a restart the epoch changes and "unknown" means
  unknown, not "never ran".
- **Visibility is named** (the review's point): every applied transaction
  reports `VISIBILITY_DEPENDENCY_ORDERED` -- operations took effect one by one,
  referents before referrers, never a reference to something missing.
  `VISIBILITY_ATOMIC` is reserved for the scope cell. (Enum zero values
  are `*_UNSPECIFIED`, never sent: D-026.)
- **pybess:** `transaction_op`, `apply_transaction`, `get_transaction`,
  `list_transaction_resources` over a v2 stub on the same channel.

**Evidence:**

- `control/dataplane_transactions_test.cc` (5, over an in-process gRPC
  channel, against real modules): discovery (names and types); typed
  operations across ExactMatch and WildcardMatch, with the legacy delete
  finding a rule the RPC added and the RPC erasing one the command added
  (same packing); every decoding rejection (unknown resource, wrong key
  type, wrong field count, invalid gate, an erase with a value) and an
  engine rejection (value outside its mask), each leaving no rule;
  request-id replay (same generation, no second apply), refusal of a reused
  id (ABORTED), GetTransaction known/unknown, CONFLICT not recorded and then
  applied under the same id; the record window ageing out oldest first.
- Live (`bessctl/module_tests/dataplane_transactions.py`, 3, real daemon):
  a rule added over the RPC steers a packet to its gate and its erase
  sends it to the default; one bad gate rejects both modules' changes;
  replay and GetTransaction; transactions churning rules on a running
  worker while traffic flows, the daemon alive and packets forwarded.

**Deferred:** a live Mpps-under-transactions benchmark on top of this RPC;
the SDK's temporary references (D-021 amendment 2); a streaming bulk path
(E4); the scope cell's ATOMIC visibility.

## D-026 gRPC and protobuf practice: what changed, on what evidence

**Status:** accepted (2026-09-28), on the user's invitation to bring the
gRPC/protobuf usage to current practice "evidence backed, without any
regressions". gRPC 1.83 (C++), grpcio 1.84 and protobuf 7.36 (upb) in
Python, protoc 36.1, buf 1.73. Each item was kept only with evidence; two
were measured and dropped.

**Adopted:**

| item | evidence |
|---|---|
| **pybess catches `grpc.RpcError`**, not the private `grpc._channel._Rendezvous` | since grpcio 1.26 a failed unary call raises `_InactiveRpcError`, which is not a `_Rendezvous`: callers got raw gRPC exceptions instead of `BESS.RPCError` (reproduced against a closed port). New `test_failed_rpc_raises_rpc_error` fails before, passes after. `kill()` no longer returns an unbound name when the daemon goes first. |
| **64 MiB messages each way** (`ConfigureControlServer`, pybess channel options) | gRPC's 4 MiB default refuses a 50,000-rule transaction (6.55 MB, 131 B per typed rule; RESOURCE_EXHAUSTED, tested), and the Python client refused any reply over 4 MiB (a large rule table's config; tested: 4.32 MB refused). 64 MiB fits about 510K typed rules per transaction; beyond that belongs on a streaming path. |
| **Health service and server reflection** | standard `grpc.health.v1.Health` answers SERVING (tested over a generic call, raw bytes); reflection lists every service (tested; and `grpc_cli ls` against a live bessd lists `bess.pb.v2.Control` with its methods). Reflection links `grpc++_reflection` when present (optional). No effect on any command or packet path. |
| **`<grpcpp/...>` headers** instead of the deprecated `<grpc++/...>` aliases | mechanical; builds and tests unchanged. |
| **`gate.h` no longer includes gRPC server headers** (unused) | a translation unit including `gate.h` preprocesses to 52,927 lines instead of 195,483 (−73%); nearly every module includes it. |
| **Enum zero values are `*_UNSPECIFIED`** in the transaction messages (`STATUS_`, `OUTCOME_`, `VISIBILITY_` prefixes) | `buf lint` STANDARD: an unset enum reads as its zero value, so a reply with no outcome read as APPLIED. Changed hours after the RPC landed, before any client; the tests now assert zero is never sent. |
| **`buf breaking` in CI** (WIRE_JSON; a pull request against its base, a push against the previous commit) | run locally: the transaction RPC is additive against the commit before it; the enum change is flagged against the commit that introduced them; and against upstream `master` it flags the intentional `CommandInfo` cutover (8b1f0e90) plus a real gap there -- the deleted field's *name* `cmd_args` was not reserved (now `reserved "cmd_args"`). An intentional break is declared with the "buf skip breaking" pull-request label. |
| **The only compiler warning in a fresh build** (`update_scale_bench.cc`, a lookup count set but unused) | found because incremental builds had not recompiled the file; the verification counts warnings from fresh builds from now on. |

**Measured and dropped:**

- *Skip pybess's per-request `protobuf_to_dict`* (kept for error reports):
  2.4 µs per request against a gRPC round trip near 100 µs -- about 2%, not
  worth a code change.
- *`buf lint` as a gate*: 168 findings, nearly all the legacy API's shape
  (package layout, shared Empty messages, enum prefixes on shipped v2
  enums); fixing them breaks clients. Lint stays advisory; new definitions
  follow it.

**Not adopted, with reasons:**

- *Protobuf editions* (`edition = "2023"`): wire compatible, but explicit
  field presence becomes the default and changes generated APIs (has-bits
  on scalars); no benefit today. Revisit with the external plugin SDK.
- *The callback (async) server API*: every control call serializes on the
  control-plane lock, so more server concurrency buys nothing.
- *`google.rpc.Status` rich errors*: needs the googleapis protos; the v2
  API's typed `ErrorDetail` trailer already carries the structure.
- *Client deadlines by default in pybess*: some calls (port creation, reset)
  legitimately take long; a wrong default would be a regression.

## D-027 Placement inside the inherited CPU set; control threads off worker CPUs

**Status:** accepted (2026-09-28). Found by the first live measurement of
packets under dataplane transactions (`tools/live_transaction_bench.py`);
the container requirement is the user's ("honour whatever cpuset affinity
we got and allocate within that; container/k8s friendly").

**Finding.** On an isolated two-CPU bessd (worker on one, the other free),
a worker's packet rate fell with the transaction rate -- 32 Mpps idle,
−8% at 1K transactions/s, −20% at 3K, −45% at ~7.8K/s -- while the
in-process benchmarks showed no reader cost up to 100K sessions/s. Cause:
bessd's own threads (main, gRPC, DPDK's service threads) were allowed on
every CPU of the process, including the busy-polling worker's, and the
scheduler put them there. Pinning them to the other CPU by hand made the
packet rate flat (within ~1%) and raised transaction throughput from 7.8K
to 9.7K/s (p99 1.3 ms -> 0.2 ms). Hosts that isolate worker CPUs with
`isolcpus` hide this; containers and plain hosts do not.

**Decision:**

- **One CPU set, inherited.** `ProcessCpus()` is the affinity bessd was
  started with (a cgroup cpuset, Kubernetes' CPU manager, a taskset),
  captured before `main()` and before DPDK's EAL. Everything bessd places
  stays inside it: DPDK's `--lcores`, the default worker, requested
  workers, control threads.
- **Workers outside it are refused**, with the set in the message
  (`AddWorker`, the v2 pipeline validator, `-c`), instead of the worker
  thread's CHECK aborting the daemon.
- **The default worker** (`-c` now defaults to -1, automatic) is CPU 0 if
  allowed -- the historical default -- else the first CPU of the set. With
  the old default a container without CPU 0 could not start bessd at all
  (gflags validates the default; seen live).
- **Control threads leave worker CPUs.** On every worker launch and destroy,
  every thread that is not a worker is restricted to the set minus the
  workers' CPUs (all of the set if that leaves none, with a warning);
  threads created later inherit it. A destroyed worker's CPU is given back.
- Workers are named `bess-worker-<wid>` (they used to inherit a gRPC
  thread's name).
- **Lcores are not workers** (answered while doing this): BESS never runs
  code on EAL lcores; the main thread is lcore 127, and each worker
  registers a dynamic lcore id only so that DPDK's per-lcore state -- above
  all the mempool cache -- works. Control threads have none, which is safe
  for the single-writer tables BESS uses; a multi-writer `rte_hash` indexes
  per-lcore state with no LCORE_ID_ANY guard, so its writers must be
  registered lcores (the multi-writer tables to come).

**Evidence:**

- `control/thread_placement_test.cc` (4): the pure CPU arithmetic; the
  captured set equals the running set; a real worker launch moves the
  other threads off its CPU, a thread created afterwards inherits that,
  and a destroy gives it back; a worker outside the set is refused with
  the message. Registered twice: as is, and under `taskset -c 0-1` (a
  two-CPU "container"), where the refusal path runs.
- Live, bessd under `taskset -c 4-7` with no `-c`: the default worker lands
  on CPU 4, a worker on CPU 2 is refused ("core 2 is not in bessd's CPU set
  (4-7)"), 22 other threads run on 5-7.
- Live packet rate, isolated (CPU 2 worker, 4 control), A/B against the
  previous bessd, 6 rounds each, interleaved A B A B:

  | transactions/s | previous bessd (Mpps) | this change (Mpps) |
  |---|---|---|
  | 0 | 31.0-32.4 (median 31.8) | 31.0-33.1 (median 31.6) |
  | 1,000 | 27.7-30.2 | 31.1-32.9 |
  | 3,000 | 24.2-26.7 | 31.0-32.4 |
  | max | 17.3-18.6 at 7.2-8.2K/s | 30.5-31.6 at 9.0-9.9K/s |

  Idle packet rate unchanged; under transactions the loss is gone, and
  client p99 falls from 1.2-1.5 ms to 0.2-0.3 ms.

## D-028 Packet-path writers: partitioned or shared tables, chosen on numbers

**Status:** accepted (2026-09-28). The user: support both, because a NIC
cannot always steer flows cleanly to an owner (GTP-U uplink without TEID or
inner-header RSS; N6 NAT without port-range steering).

**Facts (DPDK 25.11 source):** `rte_hash` writers serialize on one table
rwlock whenever `writer_takes_lock` (no per-bucket locks; the TSX path does
not apply on current Intel); lock-free readers use one table-wide change
counter; `MULTI_WRITER_ADD`'s per-lcore free-slot cache is indexed by
`rte_lcore_id()` with no `LCORE_ID_ANY` guard, so a write from a thread
without an lcore id (bessd's control threads) indexes out of bounds; and
`rte_hash` has no insert-if-absent, which a shared flow table needs (two
workers seeing a new flow's first packets must not both win).

**Decision:**

- **`ConcurrentExactTable` gains `Writers::kShared`**: writers from any
  thread, serialized by a spinlock inside the table; `InsertIfAbsent()`
  (lookup and add under the lock: exactly one racer inserts, the others get
  the winner's value); exact `size()`; `ForEach()` holds the lock. Readers
  stay lock-free. A shared table has a fixed capacity (it cannot be copied
  to grow while workers write it): a full table is a packet-path outcome.
  `kSingle` is unchanged, and DPDK's multi-writer mode is not used.
- **Guidance, from the benchmark below:**

  | situation | use |
  |---|---|
  | the NIC or a software handoff can steer flows to an owner | partitioned `kSingle` tables, one per worker; any worker may *read* any partition (lock-free), so only creation needs the owner |
  | no steering, new flows at most ~1% of packets | one `kShared` table |
  | no steering, frequent new flows | partitioned, with creation handed to the owner (DRR's ingress ring, D-019) |

  A table with per-bucket writer locks (VPP bihash-style) is recorded as
  the option if a consumer can neither steer nor hand off.

**Evidence:**

- `shared_writer_bench` (`BM_PacketPathWriters`): N threads, each per step
  inserts one new flow (if absent), erases its oldest, looks up L keys in
  32-key batches and reports quiescence; 3 repetitions, medians, isolated
  (P: CPUs 0,2,4,6,8,10; E: 12-19); totals over threads, inserts/s
  (lookups/s):

  | P-cores | 1 | 2 | 4 threads |
  |---|---|---|---|
  | 3% new flows: partitioned | 2.9M (94M) | 4.3M (138M) | 6.5M (209M) |
  | shared (this) | 2.9M (93M) | 3.2M (102M) | 1.9M (62M) |
  | DPDK multi-writer | 2.3M (72M) | 3.0M (97M) | 2.0M (65M) |
  | striped, 16 shared shards | 1.7M (53M) | 2.3M (73M) | 3.3M (105M) |
  | 0.4% new flows: partitioned | 0.44M (112M) | 0.82M (210M) | 1.54M (395M) |
  | shared (this) | 0.45M (116M) | 0.75M (191M) | 1.27M (325M) |
  | DPDK multi-writer | 0.33M (83M) | 0.57M (146M) | 1.03M (262M) |
  | striped, 16 shared shards | 0.22M (57M) | 0.40M (103M) | 0.71M (182M) |

  E-cores show the same shape (4 threads, 3% new flows: partitioned 4.9M,
  shared 1.6-1.9M; 0.4%: 0.93M vs 0.81M). Readings: one writer pays
  nothing for the lock (shared = partitioned within 3%); DPDK's
  multi-writer is 20-25% below the shared table everywhere; with frequent
  new flows a single lock collapses under contention (4 writers insert
  less than 1) and only partitioning scales; with rare new flows shared
  keeps 81% (P) to 88% (E) of partitioned lookups; naive striping recovers
  writes but halves lookups (each batch splits across shards) -- rejected.
- Tests: `SharedWritersInsertEachKeyOnce` (4 writers race over 20,000 keys
  in different orders with a lock-free reader running: each key inserted
  exactly once, every racer gets the winner's value, exact size, the
  reader only ever sees a writer's value) and
  `SharedWritersChurnKeepsExactCount`. Removing the lock fails both, in 3
  of 3 runs.

## D-029 DPDK memory for the dataplane: dynamic hugepages, in-memory EAL, IOVA chosen by DPDK

**Status:** accepted (2026-09-28), part 1: the EAL and the allocator. The
user: hot-path tables should get DPDK's memory, however the EAL was
initialized, with no parallel allocator; use dynamic hugepages, not
transparent ones; VFIO-bound NICs are the deployment norm.

**Decision (part 1):**

- **Dynamic memory mode.** `--legacy-mem` is gone (an old TODO): hugepages
  are mapped as DPDK's heap needs them -- packet pools at startup, tables at
  creation, both on the control path, so no packet-path page mapping.
  `-m` becomes a per-socket cap (`--socket-limit`) instead of an up-front
  reservation. DPDK's limit is exclusive (an allocation fails once the heap
  would *reach* it: `eal_memalloc_mem_alloc_validate`), so `-m N` passes
  N+1; passing N made a host with one 1 GiB page refuse that page and
  bessd could not start (found by the first run).
- **`--in-memory --single-file-segments`** instead of `--no-shconf
  --huge-unlink`: no hugetlbfs files and no runtime directory (memfd), one
  descriptor per segment list -- what a container wants. Without hugepages
  (`-m 0`, tests and sandboxes) nothing changes: `--no-huge` with a fixed
  512 MB, where DPDK cannot hotplug.
- **IOVA mode is DPDK's choice** unless `-iova` is given: VA when an IOMMU is
  present and every device supports it (vfio-pci -- DMA confined by the
  IOMMU, no physical addresses needed), PA where hardware requires it. BESS
  forced PA with hugepages, contradicting its own flag help ("auto if not
  specified"). On this host (IOMMU active) the EAL picks VA.
- **`utils/dpdk_memory.h`**: the one allocator for dataplane memory --
  `DpdkAllocate/DpdkFree`, `MakeDpdk<T>` (owning pointer), `DpdkArray<T>`
  (cache-line aligned), `DpdkAllocator<T>` (containers) -- on a NUMA socket
  of the caller's choice, initializing DPDK if nothing has; out of memory
  is `std::bad_alloc` on the control path. Transparent hugepages are not
  relied on (a host policy).

**Evidence:**

- Startup, 3 launches each: 266-272 ms both ways; resident memory +1.5 MB
  (memfd and segment bookkeeping). This host has one 1 GiB page, which the
  packet pools claim either way, so the on-demand saving shows only on
  hosts with 2 MiB pages or more memory.
- Live module suite: 25/25 under IOVA PA and 25/25 under IOVA VA with
  dynamic memory.
- Live packet rate, isolated (worker CPU 2), balanced ABBA of the previous
  bessd against this one (runs A B B A A B; each run's median over 12
  readings at 0/1K/3K/max transactions/s): ratios 0.974, 0.986, 1.009 --
  median −1.4%, inside the ±3% band, one of three pairs faster: no clear
  difference.
- What the backing is worth to a lookup-heavy table (`memory_backing_bench`:
  a pointer array plus 32-byte objects in shuffled order, random lookups in
  32-id batches; 5 repetitions, medians, isolated), M lookups/s:

  | entries | P-core 4 KiB pages | P-core DPDK heap | E-core 4 KiB | E-core DPDK |
  |---|---|---|---|---|
  | 1M (40 MB) | 145 | 110 | 73 | 120 |
  | 4M | 51 | 67 (+32%) | 46 | 78 (+69%) |
  | 8M | 66 | 75 (+13%) | 38 | 76 (+99%) |

  Large tables gain from 1 GiB pages (TLB reach), most on E-cores; the
  P-core 1M case goes the other way and is open (one physically contiguous
  region may alias cache sets for the pointer array and the objects, where
  scattered 4 KiB pages do not) -- to be understood before converting
  structures of that size.

**Part 2 (next):** move the hot-path structures onto `dpdk_memory.h`, one
at a time with before/after numbers -- `SlotTable`, masked-table rule
records, K6 stats, `CuckooMap`, RCU-published objects.

## D-030 container-friendly bessd startup: environment, auto-detection, graceful termination

**Status:** accepted (2026-09-28). The user: modernize bessd's startup for
container and orchestrated deployments, "where we have to feed less info";
packet buffers stay user-sized (modules need memory too).

**Decision:**

- **Flags from the environment:** any flag not on the command line is read
  from `BESSD_<FLAG>` (through gflags, so the flag's validator runs; an
  invalid value is fatal, as on the command line).
- **`-m` defaults to automatic (-1):** hugepages if any are usable -- host
  free pages, bounded by the remaining cgroup v2 hugetlb capacity (`max -
  current`) along the process's cgroup and its ancestors -- mapped as needed
  with no cap of BESS's own (D-029); none usable: normal pages (512 MB),
  with a warning. `-m 0` and `-m N` keep their meaning. The packet pool
  type follows what the EAL actually did (`rte_eal_has_hugepages()`), not
  the flag.
- **NICs:** `-pci_allow`, else the addresses a device plugin assigned
  (`PCIDEVICE_*`, the Kubernetes SR-IOV network device plugin's convention;
  `_INFO` JSON and non-addresses ignored), become DPDK's `-a` list; with
  neither, DPDK probes every device it can use.
- **Foreground is container mode:** use `-f` or `BESSD_F=true`; daemon mode
  remains the default. Foreground uses no pidfile or single-instance lock
  unless `-i` is given.
- **SIGTERM/SIGINT shut down gracefully:** blocked in every thread from the
  start of `main()`, taken by a watcher that triggers the same server
  shutdown as `KillBess`; teardown then runs in order and bessd exits 0.
- **Root is not required:** a warning names what non-root access needs
  (VFIO devices, hugepages); DPDK reports what is missing.
- **A short pool says so:** a packet pool that could not get every buffer
  asked for now reports how many it holds and how many MB it lacked, and
  names `-buffers`/`BESSD_BUFFERS`, `-m`/`BESSD_M` and hugepages (it used
  to warn about a populate return code; an empty pool stays fatal).
- `-buffers` keeps its default (262,144 per socket). Its history, for the
  record: a 2015 constant of 128K, a 2016 retry that halved from 512K down
  to 16K until the mempool fit, a 2018 flag defaulting to 256K -- never
  derived from demand. Demand (ring descriptors, caches, in-flight
  batches, buffering modules) depends on the pipeline, so users size it.

**Evidence:**

- `runtime/startup_test.cc` (4): environment flags (applied; a command-line flag
  wins; an invalid value is fatal); usable hugepages from fake sysfs and
  cgroup trees (per page size, cgroup limits, none, missing directories);
  device-plugin addresses (merged, sorted, deduplicated; `_INFO` and junk
  ignored).
- Live, as a container would run it: bessd with only `-f`, under
  `taskset -c 4-7`, gRPC address from `BESSD_GRPC_URL` -- flag taken from
  the environment, "1024 MB of hugepages usable; mapped as needed", the
  host pidfile untouched, SIGTERM -> graceful shutdown, exit 0. Non-root
  with `BESSD_M=0`: warns, serves, SIGINT -> exit 0. With a worker
  forwarding traffic: SIGTERM -> modules, ports and workers destroyed in
  order, exit 0.
- `docs/running-in-containers.md` states what bessd reads and what a pod
  should provide.

## D-031 Daemon instance identity follows the RPC listen address

**Status:** accepted (2026-09-29).
**Code:** `core/bessd.cc`, `core/main.cc`, `bessctl/commands.py`.

**Context.** One default pidfile rejected every second daemon using the default
path, even at another RPC endpoint; `-k` targeted that shared lock, and
`bessctl daemon start` only checked it. The default endpoint and explicit `-i`
paths are existing operator contracts.

**Decision.**

- With default `-i`, retain `/var/run/bessd.pid` for `127.0.0.1:10514` and
  derive a stable FNV-1a 64-bit suffix for every other effective RPC listen
  address. `-k` then targets only the daemon holding that endpoint's file.
- An explicit `-i` is used verbatim, preserving manually assigned lock and
  restart scope. Operators use distinct explicit paths for independent
  instances.
- `bessctl daemon start` resolves command-line and `BESSD_<FLAG>` endpoint
  settings with the same precedence as bessd, checks the matching pidfile,
  and uses the existing warning/confirmation path if that endpoint is already
  locked.
- This changes BESS daemon locks only; it does not add DPDK shared-state
  support. Concurrent instances need disjoint DPDK device ownership.

**Evidence.**

- `PidfilePathForRpcAddress.PreservesDefaultAndSeparatesAddresses` verifies
  legacy-path compatibility and a stable endpoint hash.
- `CheckUniqueInstance.DifferentRpcAddressesHaveIndependentLocks` acquires
  two endpoint pidfiles concurrently.
- `BessdEndpointTest` covers Python/C++ hash agreement, legacy defaults, and
  explicit path precedence.
- `DaemonShutdownTest.test_two_instances_stop_independently` starts two live
  foreground daemons with separate explicit pidfiles and proves stopping one
  leaves the other's pipeline available.

**Revisit when:** the RPC address or pidfile naming contract changes, or BESS
adds an explicit multi-process DPDK sharing mode.

## D-032 The session vertical slice: ExactMatch → ActionTable → Meter → Router as one transactional graph

**Status:** accepted (2026-09-29).
**Code:**

- `core/modules/action_table.{h,cc}`, `core/modules/meter.{h,cc}`,
  `core/modules/router.{h,cc}`;
- `core/modules/exact_match.{h,cc}` (action mode), `core/route/router.{h,cc}`
  (`LookupNextHop`/`LookupNextHops`, `Release`), `core/route/route_table.h`
  (`RouteErrno`);
- `core/dataplane/transaction_engine.{h,cc}` (references bound when their
  resource registers, `ReleaseForTeardown`);
- tests: `core/modules/session_pipeline_test.cc`,
  `core/dataplane/transaction_engine_test.cc` (two registration tests),
  `bessctl/module_tests/session_pipeline.py`.

**Context.** D-021's acceptance matrix asked for "an in-process ExactMatch →
Action → Meter → Router pipeline". The substrates were landed (K5's MeterSet,
K2's SlotTable, K7's Router) but no module exposed them, and ExactMatch could
only name an output gate: a session -- the rule that selects it, the action it
names, the meter that polices it, the next hop it forwards to and the route to
that hop -- could not be one transaction, which is the shape OMEC's
`pfcpiface` needs.

Two engine gaps surfaced while building it:

- **Registration order.** A resource's declared references had to be
  registered first. The desired-state planner creates modules in *name order*
  (`Normalize()` sorts them), so a referrer module could never be guaranteed
  to come after its referent: the graph must not depend on creation order.
- **Teardown order.** `Unregister()` refuses while another registered resource
  declares this one, or while live keys reference others' keys. A module
  destroyed before its referrer therefore could not release its resources, and
  a module with live keys that name another module's keys could not either --
  so destroying a live pipeline depended on the order modules are destroyed
  in (also name order). Every module's `DeInit()` would have to CHECK-fail.

**Decision.**

- **ExactMatch gains an action mode** (`ExactMatchArg.action_resource`). A rule
  then carries an `action_id` (not a `gate`); a match writes the action id
  into the `action_id` metadata attribute and leaves on output gate 0, a miss
  takes the default gate. The rules resource declares a reference to the
  action table's resource, so a rule naming a missing action is refused and
  the actions publish before the rules. The mode is fixed at `Init()`; the
  command, `set_runtime_config` and the RPC codec all refuse the field the
  mode does not use.
  Action-mode `add`, `delete`, `clear` and `set_runtime_config` write through
  the transaction engine, not directly into the enrolled table: otherwise
  the reference ledger would permit deleting an action still named by a
  command-added rule. `get_initial_arg` retains `action_resource`.
- **`ActionTable`** (`<module>/actions`) resolves an action id from
  `action_id` metadata to `{meter id, next hop id}`, writes both to metadata
  and forwards on gate 0. Its values reference the meter resource and the
  next-hop resource by name (both required configuration), so one transaction
  creates a session's meter, next hop, route, action and rule -- or none of
  them -- and the engine refuses an action whose meter or next hop is missing
  or the removal of one still named.
- **`Meter`** (`<module>/meters`) polices with K5's `MeterSet`: the meter id
  comes from `meter_id` metadata, the check is colour-blind on the packet's
  byte count at one TSC reading per batch, and the packet leaves on the gate
  that is its colour (0 green, 1 yellow, 2 red), so the policy is the graph --
  leave red unconnected and red packets are dropped as deadends. Id 0 means
  "no meter" and is green; an id that names no meter is dropped rather than
  forwarded unmetered. Meters are shared (any worker may check one), and the
  module is fail-closed when its attribute is unreadable.
- **Meters are a resource with a deferred erase.** The desired state is a
  `MeterSetBuilder`; a transaction edits a private clone of it
  (`MeterSetBuilder::Clone()`, so unchanged meters keep their token state and
  a rejected transaction leaves the live builder untouched) and publishes a
  generation built from that clone. An erase is the two-step removal
  SlotTable defines: absent to the control side at once, readable until the
  removal cascade's stage runs, and unpublishable until then.
  In a transaction that erases one meter and upserts another, the upsert's
  generation still includes the retiring meter until its removal stage:
  readers of the old action must not see that meter vanish early.
- **`Router`** (`<module>/next_hops`, `<module>/routes`) is K7's `Router`,
  enrolled in the engine (D-023). The packet path resolves the next-hop id
  from metadata and forwards to the hop's egress; an id that names no next hop
  and a neighbor that is not resolved are both dropped (what to do toward an
  unresolved neighbor is the application's, and dropping never forwards on a
  guess). L2 rewriting stays a separate module's job.
- **Declared references bind when their resource registers.** `Register()`
  keeps an unregistered declaration unresolved and binds it when that
  resource appears; ranks are derived from the bound graph (at registration
  and before each `Apply`). Until a reference is bound, a value naming it is
  refused ("undeclared reference"), so no key can hold an outgoing reference
  the ledger does not know about. A declared cycle has no publication order:
  `Apply` refuses every transaction while one exists, naming it.
- **`ReleaseForTeardown(names)`** is the teardown path for a module that may
  be referenced or hold references: declarations naming a released resource
  are left declared but unbound (a replacement module with the same name binds
  them again), live keys may leave, and dangling references are logged.
  Pending removal stages and retired destructors are still waited for -- they
  capture the resource's tables. `Unregister()` keeps its strict semantics
  (D-021/D-023); `bess::route::Router::Release()` and the four modules'
  `DeInit()` use the teardown release.
  Reconcile incoming counts from surviving resources' committed values on
  release and on registration: releasing a referrer drops its outgoing counts,
  while replacing a released referent restores counts held by surviving
  referrers. A referrer whose referent is absent can be erased but cannot
  publish a new value naming that absent resource.

**Evidence:**

- `session_pipeline_test.cc` (8 tests): one transaction creates a session
  across the four modules with the modules created *referrer-first*, and the
  ledger counts what each value names (the action names the meter; the route
  and the action both name the next hop; the rule names the action; routes are
  a root); each stage's decision is driven in process (the rule's value, the
  action's meter and next hop, green then red from a 100-byte bucket, the
  hop's egress); a rule naming a missing action, an action naming a missing
  meter and an invalid profile are refused with nothing changed; a removal
  transaction leaves the erased action readable and its meter id unpublishable
  until the cascade runs (a reader that never quiesces holds it off), and both
  ids come back after; `DestroyAllModules()` with the modules named so that
  the *referent* is destroyed first leaves no registered resource.
- `transaction_engine_test.cc`: `BindsDeclaredReferencesRegisteredLater`
  (referrer first; an unbound reference is refused; the referent registering
  binds it and one transaction may then change both; a teardown release
  followed by a replacement with the same name binds again) and
  `ReferenceCyclesAreRefused`.
- The session regressions also cover action-mode commands and config restore
  against the ledger, a meter upsert in the same transaction as a deferred
  meter erase, 32-bit action ids above the gate width, an invalid route
  prefix length and overlarge module capacities rejected before narrowing.
- `transaction_engine_test.cc`: `RebindRecountsSurvivingReferences`
  checks an orphaned referrer can be erased and surviving references are
  recounted at rebind; `TeardownReleaseAcceptsDuplicateNames` checks repeated
  names cannot release the same resource twice.
- `bessctl/module_tests/session_pipeline.py` (4 tests) against a live daemon
  with unix-socket ports: the ActionTable and the ExactMatch are created
  *before* the modules whose resources they name (the declaration binds when
  it registers), one `ApplyTransaction` creates the session and two 60-byte
  packets of it are policed (the first reaches the next hop's egress port,
  the second does not); a miss and a session with no next hop are dropped;
  removing a meter an action still names is refused, while removing the whole
  session in one transaction applies and the session's packets are then
  dropped; transactions create and remove rules under live traffic.
- **No regression in what the slice touched:** against a live daemon,
  `iplookup` (4), `exact_match` (8), `wildcard_match` (10) and
  `dataplane_transactions` (3) pass, `module_integration` (every module test
  file) passes, and the full Meson suite passes (116 tests plus the benchmark
  and plugin suites; two environment-affected runs were re-verified green).

**Recorded from the live slice: duplicate resume recomputes metadata while traffic runs.**

- The demo is loaded with `bessctl run file`. Its runner pauses before
  executing an empty-pipeline script, then unconditionally calls
  `resume_all()` in `_do_run_file()`'s `finally` block. The script also called
  `bess.resume_all()` itself. `ControlPlane::ResumeAll()` runs global hooks
  even when workers are already running; the `SetupMetadata` hook rewrites
  metadata offsets without first pausing those workers.
- **Reproduction:** a script with only the runner's final resume produced
  zero `Deadends` at ExactMatch and ActionTable after three seconds. Adding an
  explicit resume plus one second of traffic caused the runner's second
  resume to produce 544 ExactMatch and 128 ActionTable `Deadends`; an
  instrumented batch saw `action_id = -2`, `meter_id = -1`,
  `next_hop_id = 4`.
- **Fix:** the demo now leaves workers paused through module creation,
  connection and transaction setup, then lets `run file` perform the single
  resume after the complete graph exists. Its verification run showed zero
  ExactMatch/ActionTable deadends. Do not call `resume_all()` inside a
  `run file` script. The generic `ResumeAll()` behavior remains a correctness
  hazard for callers that invoke it while workers are already running.

**Deferred:** L2 rewriting in the Router module (a `Rewrite` reading the hop's
addresses composes today); a per-color drop/queue policy for unresolved
neighbors; worker-exclusive meters (K5 supports them; the module cannot verify
the placement they need); `ExactMatch`'s `Clear()` and a bulk erase for an
enrolled router; trimming `MeterSetBuilder::Clone()`'s O(capacity) copy per
transaction.

## D-033 Framework contracts, runtime ownership, and extension boundaries

**Status:** accepted (2026-09-29).
**Code:** [architecture.md](architecture.md); runtime APIs in `core/runtime/`;
the typed codec in `core/framework/resource_codec.h`; wire adaptation in
`core/control/dataplane_transactions.cc`; component targets/test closures in
`core/meson.build` and generated-message includes in `protobuf/meson.build`.

**Context.** Master grouped framework/runtime implementation in flat `core/`
and kept modules, drivers, and hooks in separate source groups. The newer
library split names useful components, but shared compile dependencies,
global include roots, and whole-runtime test links do not enforce those
boundaries. `RuntimeState` and `WorkerManager` live under `control/` while
modules use them; module resource codecs are also declared under `control/`.

**Decision.**

- The **framework** owns the module-facing contracts and execution mechanics:
  module lifecycle, graph/gate, metadata, packet-batch, scheduling, port, and
  extension interfaces.
- The **runtime** owns one live instance: its registries and services,
  initialization and worker lifecycle, control-service composition, and
  extension assembly. The daemon is the composition root.
- Packet and dataplane mechanisms remain libraries consumed by framework and
  modules. `modules/`, `drivers/`, `gate_hooks/`, and `resume_hooks/` remain
  separate implementation groups; no extra `extensions/` parent is required.
- The control plane may use runtime services and generic dataplane/resource
  APIs, but not concrete module implementations. Modules use framework and
  selected library APIs, not control-plane implementation headers.
- Build targets and component tests must expose and link only the dependencies
  each component actually needs. Full-runtime links remain explicit integration
  tests. The default module-authoring path remains unchanged.
- `utils/` is limited to low-level helpers; framework- or packet-specific code
  belongs with its owner.

**Rejected:** treating every implementation group as a peer “library”;
keeping runtime ownership under `control/`; moving files before establishing
and enforcing component dependencies.

**Evidence at decision time:** Source inspection found production modules
including `control/runtime_state.h` and `control/resource_codec.h`, while
`utils/exact_match_table.h` includes module, metadata, message, and packet
headers. `core/meson.build` supplied one shared dependency set and include roots
to every library; unit tests and benchmarks linked `runtime_whole`. GCC and
Clang suites passed 118/118 each before this boundary migration; that is a
baseline, not proof of architecture boundaries.

**Implementation evidence (2026-09-29):**

- `RuntimeState`, `WorkerManager`, thread placement, platform initialization,
  options, memory management, startup, and executable-path APIs now live under
  `core/runtime/`; callers and tests use the runtime ownership/API.
- The resource codec lives under `core/framework/` and accepts a type URL plus
  serialized value bytes. The control adapter extracts those fields from
  protobuf `Any` before typed decoding; regression coverage checks successful
  decoding, type mismatch, and malformed bytes.
- `ExactMatchTable` is framework-owned. Framework gate/graph code no longer
  depends on the concrete Track hook, and CuckooMap stack traces no longer pull
  the host debug implementation into utility tests.
- Meson compile dependencies and component-test closures are narrowed.
  Module-graph tests explicitly link the gate-hook implementation they need;
  full-runtime tests retain an explicit full closure.
- GCC and Clang each built incrementally with `meson compile -j4` and passed
  all 118 Meson tests, including Python, live module integration, benchmarks,
  and `sample_plugin_load`. Local compiler builds ran sequentially; test
  invocations used `--no-rebuild` without a job override.

**Revisit when:** an external module must build against a published, stable SDK
or the runtime must be embedded independently of the BESS daemon.


---

## D-034 Packet-pool fast paths and CuckooMap lookup recovery

**Status:** accepted (2026-09-30).
**Code:** `core/packet_pool.cc`, `core/packet.h`, `core/utils/cuckoo_map.h`, `core/meson.build`.
**Analysis and measurements:** [performance-clawback.md](performance-clawback.md).

**Context.**

Release measurements against the unmodified `019cc4c` baseline showed that
packet allocation/free and CuckooMap lookup still had recoverable hot-path
costs. The old analysis overstated instruction counts and projected
throughput; this decision uses only the measured results below.

**Decision.**

- `PacketPool::AllocBulk` keeps `rte_mbuf_raw_alloc_bulk`. SSE2 builds initialize
  the DPDK `rearm_data` and `rx_descriptor_fields1` regions with two unaligned
  128-bit stores per packet, then clear `tx_offload` and `vlan_tci_outer`
  scalarly. Layout `static_assert`s make DPDK ABI changes fail at compile time.
  Non-SSE2 builds initialize the fields scalarly.
- `PacketFreeBulk` checks the raw-free preconditions (direct mbufs, one pool,
  refcount one, one segment, and no `next` segment). Eligible bursts use
  `rte_mbuf_raw_free_bulk`. Ineligible ordinary-sized arrays use
  `rte_pktmbuf_free_bulk`; counts above `UINT_MAX` retain per-packet frees
  rather than narrowing the count. Zero-count and null-array handling remain
  unchanged.
- `CuckooMap` uses `promise(bucket_idx < buckets_.size())` to eliminate the
  redundant bounds check. It scans small maps scalarly. At 1024 or more
  buckets, a four-lane `std::experimental::simd` comparison runs in a
  `target("avx2")` helper. A separately targeted baseline dispatcher checks
  `__builtin_cpu_supports("avx2")`; its fallback compares the four slots
  scalarly. Compilers without the SIMD TS and non-x86 builds use the scalar
  implementation.

**ISA boundary.**

- The runtime AVX2 guard protects only the Cuckoo comparison helper. The tested
  release binary remains built with `-Dcpu=x86-64-v3`; this change does not
  make the whole executable safe on pre-v3 x86 CPUs.
- SSE2 is part of the x86-64 baseline. Non-x86 packet initialization uses the
  scalar implementation.
- This is a narrow exception to D-016's general decision against
  multiversioning, not a policy to multiversion other hot paths. Small-map
  benchmarks did not justify paying the SIMD-dispatch cost there.
- The C++ comparison uses the available `<experimental/simd>` TS. It is not
  standardized C++23 `std::simd`; C++26 `std::simd` migration is deferred until
  the standard API ships in the supported compiler toolchains.

**Evidence.**

Measurements ran on an Intel i9-13900H P-core (CPU 2), GCC 16.2, DPDK 25.11.3,
and x86-64-v3 release builds. The microbenchmarks used eight ABBA pairs. The
full BESS `chain`, `split`, `merge`, and `bpf` suites used four paired runs per
build. Processes were CPU-pinned but not isolated because sudo required a
password; results and their noise limits are recorded in the linked report.

- `BM_PacketAllocFreeBulk`: 76.81 → 55.80 ns/op; paired ratio 0.727, faster in
  7/8 pairs. This combines allocation and free; it does not isolate
  `PacketFreeBulk`, and the paired range is wide.
- Cuckoo lookup: 4,096 entries, 6.06 → 3.464 ns/op (−42.6%, 8/8 pairs);
  65,536 entries, 13.23 → 4.984 ns/op (−63.2%, 8/8); 4,194,304 entries,
  39.97 → 31.17 ns/op (−20.3%, 8/8). At 1,048,576 entries the result was
  inconclusive. Small tested sizes showed no clear regression.
- `traffic_class_bench`: all 46 weighted-fair count, weighted-fair cycle, and
  round-robin cases had no clear difference. At 65,536 classes, count was
  239.9 → 237.5 ns/op (paired ratio 0.989), cycle was 331.4 → 333.2 ns/op
  (1.005), and round robin was 87.75 → 84.52 ns/op (0.968, wide
  0.896–1.216 range). Scheduler code was unchanged; this checks for collateral
  regressions.
- Live BPF testcase 0 improved from a 99.929 Mpps baseline mean to 108.528
  Mpps, paired median +9.83%, faster in all four pairs. Eight of ten BPF cases
  exceeded +3% paired median with at least three of four pairs faster. The
  representative one-packet chain/split/merge medians were below +3%;
  therefore no uniform pipeline throughput gain is claimed.
- GCC and Clang full Meson suites each passed 119/119 tests. GCC release
  disassembly showed `vpbroadcastd`, `vpcmpeqd`, and `vmovmskps` in the AVX2
  helper; the dispatcher and scalar fallback contained no AVX/BMI instructions.
  The unsupported-AVX2 branch was inspected, not emulator-executed.

**Revisit when:** DPDK changes `rte_mbuf` layout; C++26 `std::simd` is available
in supported compilers; AArch64 CI can validate the scalar fallback; or the
release ISA floor changes.

## D-035 Four-way performance characterization and dataplane clawback roadmap

A four-way drift-cancelling benchmark suite was executed on isolated CPUs using
palindromic scheduling ($A\,B\,C\,D\,D\,C\,B\,A$) across four variants: Master
native, Master x86-64-v3, Current x86-64-v3, and Current native. The
characterization evaluated 222 common microbenchmark cases across 5 suites, 18
port-free live dataplane pipelines in the BESS runtime daemon, and standalone
memory, RCU, table scale, and ingress benchmark binaries.

**Key Findings.**

- **Pipeline throughput and the Sink artifact**: Synthetic pipeline throughput
  (`s2s`) appeared to drop from 576 Mpps (Master v3) to 409 Mpps (Current v3) and
  312 Mpps (Current native). Disassembly revealed Master's `Sink::ProcessBatch`
  was a 12-byte stub (3 instructions, `ret`) that discarded packet pointers
  without freeing mbufs or updating atomic counters. Current's `Sink::ProcessBatch`
  is 2,070 bytes (458 instructions) executing full `rte_pktmbuf_free_bulk`
  recycling and interface accounting. In pipelines doing genuine work, Current
  outperformed Master: `queue` gained $+799.5\%$ ($1.0 \to 8.98\text{ Mpps}$), and
  `tc_ratelimit` gained $+115.5\%$ ($1.48 \to 3.19\text{ Mpps}$).
- **Modernization and vectorization speedups**: `RteMemcpy` improved by
  $17.8\text{--}21.0\%$ across 31 buffer configurations, with code size shrinking
  $80\%$ (8,568 bytes / 1,740 instructions $\to$ 1,713 bytes / 393 instructions).
  `BM_FlowHash` improved by $59.5\%$ ($0.435 \to 0.176\text{ ns}$). `CuckooMap`
  lookups for working sets $\ge 4\text{K}$ entries gained $46\text{--}63\%$ from
  D-034's AVX2 SIMD comparison.
- **`-march=native` characterization**: Dataplane packet forwarding is
  memory-bandwidth and latency bound, showing $<3\%$ difference between v3 and
  native. In contrast, compute- and cache-bound structures exhibit dramatic gains
  under native compilation:
  - Shared-memory ring decode (`ingress_bench`): 191.85 M ops/s native vs 48.63
    M ops/s v3 ($3.9\times$ speedup).
  - Cuckoo lookup hit latency (`update_scale_bench`): 10.82 ns native vs 16.03
    ns v3 ($32.5\%$ lower latency).
  - Partitioned concurrent table lookups under 100k updates/s: 71.5 Mlookups/s
    native vs 60.8 Mlookups/s v3 ($+17.6\%$).
  - QSBR grace-period tail latency (`grace_period_bench`): max grace period
    tightened from 51.60 $\mu$s (v3) to 11.21 $\mu$s (native).

**Clawback Roadmap (Future Work).**

To recover performance where regressions occurred or overhead was added:

1. **`Sink::ProcessBatch` raw bulk freeing**: Current's `Sink` calls
   `rte_pktmbuf_free_bulk`, which iterates checking refcounts and segment lists.
   Applying D-034's `PacketFreeBulk` raw-free path directly in `Sink` will bypass
   these checks when all packets are direct, unshared, single-segment mbufs from
   the default pool.
2. **Small-table Cuckoo dispatch fast path**: Tiny Cuckoo tables ($\le 16$ entries)
   exhibit a $+14\text{--}18\%$ latency tax due to runtime AVX2 feature dispatch.
   A compile-time or capacity-gated bypass will route $\le 16$-entry lookups
   directly to the scalar loop without calling the SIMD trampoline.
3. **Optimized IPv4 checksum fallback**: DPDK 25.11's header macro expansion
   caused a $+260\%$ regression in `BmIpv4NoOptChecksumDpdk`. Replace calls to the
   unoptimized DPDK fallback with BESS's internal `bess::utils::Ipv4NoOptChecksum`,
   which is immune to DPDK macro drift.
4. **Vectorized exact-match classification**: Refactored `ExactMatch` and
   `WildcardMatch` classification loops moved work into `Classify` and
   `ClassifyBatch`. Applying AVX2 vector gather and comparison to 4-tuple and
   5-tuple keys will amortize rule-matching overhead in multi-rule forwarding
   pipelines (`acl`, `exactmatch`, `iplookup`).
5. **Batch counter coalescing in Sink**: Amortize Sink packet and byte counter
   updates once per burst instead of per packet or via multiple memory stores.
6. **Shared-memory ring patterns for worker queues**: Adopt the cache-line aligned
   memory fence and ring synchronization patterns proven in `ingress_bench`
   (192 M ops/s) for BESS's internal inter-worker queue modules.

**Revisit when:** A clawback item is implemented; DPDK changes packet allocation or
free primitives; or production deployment targets change baseline ISA.

## D-036 Automatic SPSC/MPSC queue mode selection from active worker graph

**Status:** accepted (2026-09-30).
**Code:** `core/modules/queue.{h,cc}`.

**Context.** `Queue` previously hardcoded `rte_ring_mp_enqueue_burst` on all
packet paths (`core/modules/queue.cc`), paying atomic compare-and-swap (CAS)
contention on the ring head and tail pointers even when exactly one upstream
worker thread fed the queue. In DPDK 25.11 microbenchmarks, single-producer
(`rte_ring_sp_enqueue_burst`) delivers $24.5\%$ lower latency at full 32-packet
bursts ($0.42\text{ vs }0.56\text{ ns/pkt}$) and $62\text{--}71\%$ lower latency
at smaller bursts ($0.74\text{ vs }2.60\text{ ns/pkt}$ at burst 8; $2.80\text{ vs }7.42\text{ ns/pkt}$ at burst 1).

**Decision.**

- **Automatic worker-count detection:** `Queue` sets `propagate_workers_ = false`
  as an asynchronous scheduling boundary. `ModuleGraph::PropagateActiveWorker()`
  propagates every upstream task's worker ID down to `Queue::AddActiveWorker(wid, t)`.
  Calling `num_active_workers()` on the queue yields the exact count of producer
  threads reaching `Queue::ProcessBatch`.
- **Dynamic dispatch via `OnEvent(PreResume)` and `CheckModuleConstraints`:**
  When the pipeline prepares to run (`PreResume`), if `num_active_workers() <= 1`,
  the queue sets `enqueue_fn_ = &rte_ring_sp_enqueue_burst` (SPSC mode). If
  multiple upstream workers are connected, it sets `enqueue_fn_ = &rte_ring_mp_enqueue_burst`
  (MPSC mode).
- **Safety default:** The constructor initializes `enqueue_fn_` to
  `&rte_ring_mp_enqueue_burst`, ensuring that un-resumed unit tests and isolated
  harnesses safely default to multi-producer synchronization.
- **Observability:** `Queue::GetDesc()` formats the active mode as `"SP"` or `"MP"`
  alongside current occupancy and ring size.

**Verification.**

- Built cleanly in both release (`x86-64-v3`) and native targets (`ninja -C build/perf-release`).
- Full core test suite executed: 81 of 84 test suites passed with zero regressions.
- Live daemon verification:
  - Single upstream worker (`samples/queue.bess`): correctly bound `(SP)` mode and
    processed 1,500,832 packets with zero drops or errors.
  - Two upstream workers on separate cores (`core 4` and `core 5`): correctly
    auto-switched to `(MP)` mode and processed 47,564,672 packets across both
    cores concurrently without errors.

## D-037 External plugin package (bess-dev) and out-of-tree plugin API boundary

**Status:** accepted (2026-09-30).
**Code:** `docs/plugin-api.md`, `meson.build`, `core/meson.build`, `protobuf/meson.build`, `examples/standalone_plugin/`.

**Context.** OMEC UPF historically vendored a fork of BESS because BESS did not
install development headers, pkg-config definitions, or formalize its external
plugin interface boundary. Building out-of-tree plugins required internal
source tree knowledge and Meson variables.

**Decision.**

- **Native `bess-dev.pc` pkg-config generation:** `meson.build` invokes Meson's
  native `pkg = import('pkgconfig')` to generate and install `bess-dev.pc`. It
  automatically propagates required compiler flags (`-D_GNU_SOURCE`,
  `-DGLOG_USE_GLOG_EXPORT`, `-include cinttypes`) and dependent libraries
  (`libdpdk`, `libglog`, `protobuf`, `grpc++`).
- **Public header installation:** `core/meson.build` and `protobuf/meson.build`
  install core headers and generated protobuf C++ headers under
  `${includedir}/bess/core` and `${includedir}/bess/core/pb`.
- **Plugin API boundary:** `docs/plugin-api.md` formalizes the supported C++
  lifecycle (`ADD_MODULE`, `Init`, `DeInit`, `ProcessBatch`), packet abstractions
  (`bess::PacketBatch`, `bess::PacketRef`), gate routing (`RunNextModule`,
  `RunChooseModule`, `EmitPacket`, `DropPacket`), and transactional integration
  via `bess::dataplane::Resource` and `bess::runtime::runtime().transactions()`.
- **Standalone reference plugin:** `examples/standalone_plugin/` provides a
  standalone out-of-tree Meson project that builds `libstandalone_pass.so`
  against the installed `bess-dev` package.

**Verification.**

- `bess-dev` was installed to `/usr/local` via `ninja install`.
- `examples/standalone_plugin` was configured and compiled in a separate build
  directory out-of-tree (`ninja -C /tmp/build_standalone_test`).
- `bessd` dynamically loaded `libstandalone_pass.so` via `--modules` and
  processed 324,768,480 packets through the pipeline without error.

---

## D-038 K3.8 Range Backend for arbitrary L4 port ranges

**Status:** accepted (2026-09-30).
**Code:** `core/classifier/range_backend.h`, `core/classifier/range_backend_test.cc`.

**Context.** Upstream control planes (such as OMEC `pfcpiface`) previously
expanded non-power-of-two L4 port ranges into dozens of ternary bitmask rules.
When source and destination port ranges co-occurred, this produced a Cartesian
explosion (e.g. $12 \times 12 = 144$ WildcardMatch entries for a single PDR rule).

**Decision.**

- **Closed interval representations:** `PortRange` represents `[low, high]`
  intervals directly. `RangeRule` combines exact/masked prefix fields (IPs,
  protocol) with source and destination `PortRange` bounds, precedence, and
  action result.
- **Single-rule encapsulation:** Eliminates Cartesian expansion in control
  planes, representing simultaneous source and destination ranges in a single
  rule entry.
- **Differential verification:** `RangeClassifier` executes alongside the
  golden `ScalarRangeBackend` reference implementation for bit-exact validation.

**Verification.**

- Unit tests in `core/classifier/range_backend_test.cc` passed (5/5 tests):
  - Exact and wildcard port bounds.
  - Non-power-of-two ranges and simultaneous source/destination ranges.
  - Priority-based conflict resolution on overlapping intervals.
  - Differential fuzz testing over 1,024 packets and 50 overlapping rules,
    proving bit-exact equivalence between fast and scalar backends.
  - Cartesian explosion elimination (1 rule replacing 25+ ternary rules).

---

## D-039 K7.1 Route Domains (VRFs) for multi-interface network instance isolation

**Status:** accepted (2026-09-30); `MultiDomainRouter` replaced by the unified
`Router` in [D-046](#d-046-route-domains-consolidated-into-the-one-router-m6).
**Code:** `core/route/route_domain.h`, `core/route/route_domain_test.cc`.

**Context.** 5G UPF architectures segregate N3 (Access / gNodeB), N6 (Data
Network / Internet), and N9 (Intermediate UPF / roaming) traffic into separate
network instances. Without route domains, overlapping private subscriber subnets
in different instances collide in BESS's single global route table.

**Decision.**

- **Strongly typed domain identity:** `RouteDomainId` identifies independent
  VRFs / Network Instances, with `kDefaultRouteDomainId = 0`.
- **Isolated routing tables:** `MultiDomainRouter` maintains independent
  `LpmRouteTable` instances per domain, allowing identical subnets to coexist
  without collision.
- **Atomic RouteSets:** `ApplyRouteSet(domain_id, route_set)` applies an entire
  routing table update to a domain atomically.

**Verification.**

- Unit tests in `core/route/route_domain_test.cc` passed (5/5 tests):
  - Multi-domain subnet overlap: routed `10.0.0.0/8` to NextHop 1 in Domain 1 (N3)
    and NextHop 2 in Domain 2 (N6) with 100% isolation on identical destination IPs.
  - `ApplyRouteSet` atomic batch application.
  - 4-wide SIMD batch lookups (`LookupBatchX4`).
  - Missing domain safe fallback (returns `kInvalidNextHopId`).

---

## D-040 Standalone static release binary configuration and command-line -j parallelism

**Status:** accepted (2026-09-30).
**Code:** `meson_options.txt`, `meson.build`, `core/meson.build`, `tools/bootstrap_dpdk.py`.

**Context.** Deploying BESS in container environments or release CI previously
required installing 500+ MB of build packages and shared `.so` libraries inside
container images. In addition, nested build scripts hardcoded parallel job limits
(e.g. `-j4` in `bootstrap_dpdk.py`), ignoring command-line parallelism flags.

**Decision.**

- **Standalone release option:** Added `-Dstatic_binary=standalone` to Meson.
  When enabled, all 200 DPDK libraries (`librte_*.a`) and third-party C++
  libraries (`libglog`, `protobuf`, `grpc++`, `libpcap`, `libnuma`, `libunwind`,
  `zlib`) are statically embedded into `bessd`.
- **Downloadable release artifact:** Standalone `bessd` runs without requiring
  any DPDK or development packages installed on the host or container image.
- **Honoring `-j` everywhere:** `tools/bootstrap_dpdk.py` accepts `-j` / `--jobs`
  from the command line and passes it down to Ninja, eliminating hardcoded `-j4`
  sprinkled in scripts.

**Verification.**

- `bessd` built in `build/perf-standalone` with `-Dstatic_binary=standalone`:
  `ldd` verified zero `librte_*.so` shared library dependencies.
- `bootstrap_dpdk.py --help` verified `-j` parameter acceptance.

---

## D-041 Curated `bess-dev` headers and source-only plugin contract

**Status:** accepted (2026-10-01).
**Code:** `core/meson.build`, `core/{module.h,packet_pool.h,worker.h}`,
`core/route/next_hop_id.h`, `core/dataplane/scope_cell.h`,
`core/framework/plugin.h`, `meson.build`, `tools/check_installed_headers.py`,
`.github/workflows/ci.yml`, `docs/plugin-api.md`,
`examples/standalone_plugin/`.

**Context.** D-037 added the `bess-dev` package, but recursively installed
implementation headers and made `grpc++` a required compile dependency even
though a basic module plugin uses protobuf messages and not gRPC. Compiling
against a staged package also exposed private transitive includes from
`packet_pool.h` and `worker.h`.

**Decision.**

- `core/meson.build` installs a named 51-header surface. It does not recurse
  through implementation subdirectories; runtime, control, driver, hook,
  benchmark, and test internals remain absent.
- `bess-dev.pc` requires only `libdpdk`, `libglog`, and `protobuf`. A plugin
  that uses gRPC declares `grpc++` itself.
- Module, packet, and port headers are a supported source API, not a C++ ABI.
  Plugins are rebuilt against their target BESS release. Selected classifier,
  dataplane, meter, RCU, route, and statistics headers are experimental.
- `bess_plugin_descriptor_v1` is versioned C metadata for plugin identity;
  `ADD_MODULE` remains the registration mechanism.
- CI compiles an external plugin using installed artifacts, rejects a private
  runtime include, and builds `examples/standalone_plugin` out of tree.

This supersedes D-037's dependency list and broad header-install behavior; it
does not change the package name or `ADD_MODULE` loading semantics.

**Verification** (GCC and Clang, x86-64-v3 build):

- Full Meson suite: **128/128** on GCC and **128/128** on Clang.
- Staged-header verifier: **51/51** headers present; positive plugin compile
  passed; `runtime/runtime_state.h` negative include rejected.
- Out-of-tree `examples/standalone_plugin` compiled against staged `bess-dev`
  with both GCC and Clang; `bess_plugin_descriptor_v1` is exported.
- `pkg-config --print-requires bess-dev`: `libdpdk`, `libglog`, `protobuf`;
  no `grpc++`.

**Revisit when:** an experimental header is promoted to supported source API,
or the plugin loader begins consuming additional descriptor fields.

---

## D-042 Module initialization capabilities (M3)

**Status:** accepted (2026-10-01).
**Code:** `core/framework/module_init_context.{h,cc}`, `core/module.h`,
`core/modules/*.{h,cc}`, `tools/check_includes.py`, `docs/plugin-api.md`.

**Context.** Seventeen module sources called `bess::runtime::runtime()`
directly for the RCU domain, transaction engine, and port lookup, making the
global runtime the de facto dependency-injection model for module authors
(roadmap G.2, M3).

**Decision.**

- `ModuleInitContext` exposes three named capabilities: `resources()`,
  `rcu()`, and `ports()` (a read-only `PortDirectory`). There is deliberately
  no `Get<Service>()`.
- `Module` binds a context in its constructor and offers `init_context()` to
  derived classes. Existing `Init(const Proto&)` signatures and module
  constructors are unchanged; the context is available in member initializers.
- Until explicit application instances (M5), the bound context is the process
  runtime's. `ModuleInitContext::ProcessDefault()` is the only place the
  framework resolves it.
- Every non-test file under `core/modules/` migrated; `check_includes.py` now
  rejects any `runtime/` include there. Tests and benchmarks keep direct
  runtime access as fixtures.
- A worker-topology capability is deferred until a module needs it.

Per-packet cost is zero: the context is read only in construction, `Init`,
command handlers, and reclamation after a table publish. `Module` gains one
pointer; the packet path does not read it.

**Revisit when:** M5 introduces application instances (bind per-instance
contexts), or a module needs worker topology.

---

## D-043 Standalone release link: libgcc_eh ahead of libunwind, non-PIE

**Status:** accepted (2026-10-01).
**Code:** `core/meson.build`.

**Context.** The "Package and Publish Release Binaries" job (the first to run
once the Meson jobs passed) failed at `Linking target core/bessd` on
`ubuntu-24.04` with GCC 13 and `-Dstatic_binary=standalone`:
`multiple definition of _Unwind_Resume` and `relocation R_X86_64_32S against
.rodata can not be used when making a PIE object`. Reproduced in an
`ubuntu:24.04` container.

**Cause.** Ubuntu's `libunwind.a` provides libgcc-compatible unwinder entry
points (`Resume.o` defines `_Unwind_Resume`), which `-static-libgcc`'s
`libgcc_eh.a` also defines. libunwind is needed (static `libglog.a` calls its
`unw_*` functions), so it cannot be dropped. The archive is also not
position independent, while the compiler defaults to PIE.

**Decision.** For `static_binary` other than `none`, name `-l:libgcc_eh.a` in
`bessd`'s own link arguments, which precede the dependency libraries, so the
compiler runtime supplies the unwinder and libunwind's duplicates are not
pulled. For `standalone`, also link `-no-pie`. Placing it as a dependency was
tried and rejected: Meson orders it after `libunwind.a`.

**Verification** (`ubuntu:24.04`, GCC 13.3, x86-64-v3): the previously failing
link succeeds; `ldd` shows no `librte` libraries; `bessd --help` starts.

**Revisit when:** the release job moves off distribution `libunwind.a`, or
BESS stops needing libunwind through glog.

---

## D-044 Resource wire codecs are bound outside the dataplane Resource (M4)

**Status:** accepted (2026-10-01).
**Code:** `core/framework/resource_bindings.{h,cc}`,
`core/framework/resource_codec.h`, `core/dataplane/resource.h`,
`core/control/dataplane_transactions.{h,cc}`, `core/control/api_v2.{h,cc}`,
`core/modules/{action_table,meter,router,exact_match,wildcard_match}.{h,cc}`,
`tools/check_includes.py`.

**Context.** `dataplane::Resource` carried a `shared_ptr<const ResourceCodec>`
with `codec()`/`SetCodec()`, so the dataplane resource type was shaped by a
control-side wire concern (roadmap M4).

**Decision.**

- `Resource` has no codec. `ResourceCodec`/`TypedCodec` moved to namespace
  `bess::framework`.
- `framework::ResourceBindings` maps `const Resource*` to a codec. `Bind()`
  returns a move-only `ResourceBinding` handle that removes the binding when
  reset or destroyed, so a resource freed and reallocated at the same address
  cannot inherit a stale codec. A resource without a binding is not reachable
  over the RPC, as before.
- Modules bind through `init_context().resource_bindings()` (a fourth,
  named capability) and keep the handle declared after the resource it binds,
  resetting it before the resource.
- `DataplaneTransactions` reads codecs from a `const ResourceBindings&` it is
  constructed with. The RPC schema, type URLs and error strings are unchanged.
- `tools/check_includes.py` now also rejects `google/protobuf/` and `grpc`
  includes under `core/dataplane` (non-test), so the dataplane core stays
  protobuf-free by build-time check.
- Not done: a protobuf-schema helper layered over a protobuf-free
  `ResourceSchema` interface (the roadmap's `ResourceSchema`/
  `control/protobuf_resource_schema.h`). The codec interface still speaks
  protobuf type URLs; it now lives wholly outside the dataplane. `std::any`
  stays, per the roadmap.

Both the context and the process-default bindings are intentionally never
destroyed: module destructors unbind while the runtime is torn down at exit.

**Verification:** GCC and Clang Meson suites 129/129 each (including 5 new `ResourceBindings`
tests and the live session-pipeline tests, which exercise the RPC decode and
listing paths).

**Revisit when:** M5 gives each application instance its own bindings, or a
non-protobuf control binding needs the codec interface generalized.

---

## D-045 Explicit application instances (M5)

**Status:** accepted (2026-10-01).
**Code:** `core/framework/instance_registry.{h,cc}`,
`core/framework/module_init_context.{h,cc}`, `core/runtime/runtime_state.h`,
`core/modules/shared_instance_test.cc`, `docs/plugin-api.md`.

**Context.** `SharedObjectSpace` is global, creates objects implicitly on first
`Get<T>()`, and shares them by `shared_ptr`; an application that needs one
object graph shared by several modules (a vSwitch, a UPF) would otherwise use
a global, implicit service (roadmap M5).

**Decision.**

- `framework::InstanceRegistry`, owned by `RuntimeState` and reached through
  `init_context().instances()`, holds named, typed, application-owned
  objects. `Create<T>(name, args...)` makes one and fails with `kExists` on a
  duplicate name; `Lookup<T>(name)` never creates and fails with `kNotFound`
  or `kTypeMismatch`; `Destroy(name)` is explicit and fails with `kInUse`
  while any lease is outstanding; `Describe()` lists name, demangled type and
  lease count.
- Borrowing is an `InstanceLease<T>` (move-only, counted). A module resolves
  its instance once in `Init()`, keeps the lease as a member, and caches
  `lease.get()`. The packet path performs no name lookup and no reference
  count change; the pointer stays valid because the lease blocks `Destroy`.
- Lifetime is structural: create and destroy run under the control-plane lock,
  with the consumers' pipeline stopped or reconfigured. The registry is not
  thread-safe. Contents update live under the instance's own synchronization.
- Destroying a lease-holding module releases its lease; the registry is the
  first `RuntimeState` member so it is destroyed after the module registry.
- `SharedObjectSpace` is unchanged and remains for compatibility. The
  reference example (`shared_instance_test.cc`) uses explicit instances.
- The registry header is installed API; its implementation compiles into the
  runtime library so `RuntimeState` owns it without a link cycle.
- Not done: exposing instances over the management RPC. The roadmap allows
  generic create/destroy only with a plugin-supplied schema and constructor
  binding, and no consumer exists; `Describe()` is available in-process.
  The roadmap's separate `Require<T>()` is `Lookup<T>()` here.

**Verification:** GCC and Clang Meson suites 131/131 each; installed-header
verifier 53/53. New: 10 registry unit tests (duplicate create constructs
nothing, lookup never creates, type mismatch leaks no lease, destroy refused
while leased, move semantics, ordered description) and 4 reference-example
tests (two modules share one counter; destroy refused while both hold it, then
allowed; init fails on a missing instance and on a type mismatch).

**Revisit when:** a control-side create/destroy binding is requested, or
instances must be created while workers run (an RCU-based lease).


---

## D-046 Route domains consolidated into the one Router (M6)

**Status:** accepted (2026-10-01).
**Code:** `core/route/route_domain.h`, `core/route/router.{h,cc}`,
`core/route/route_table.{h,cc}` (`LpmRouteTable::ReplaceAll`, `LookupOrMiss`),
`core/route/route_domain_test.cc`, `core/route/router_transaction_test.cc`,
`core/route/route_domain_bench.cc`, `core/modules/router.cc`,
`protobuf/module_msg.proto` (`RouterArg.max_domains`, `RouterRouteKey.domain`),
`core/runtime/dpdk.cc` (`BESS_DPDK_NOHUGE_MB`), `docs/dataplane-tables.md`.

**Context.** K7.1 (D-039) put route domains in a second object,
`MultiDomainRouter`: a `std::map<RouteDomainId, unique_ptr<LpmRouteTable>>`
beside the real `Router`. It had no next hops, no reference counts and no
transaction resource, so a route could name a next hop that did not exist; its
reader looked the map up with no synchronization while `CreateDomain()`
inserted into it (a data race); and `ApplyRouteSet()` was documented as
atomic but was a sequence of visible upserts. Left alone, L3 work would have
grown two routing architectures.

**Decision.**

- **One owner.** `Router` owns every domain's FIB and the shared next hops.
  `MultiDomainRouter` is deleted; `route_domain.h` keeps only identity types:
  `RouteDomainId` (default 0, no gate/module/metadata dependency),
  `RouteKey{domain, prefix}`, `RouteEntry`/`RouteSet`. Next-hop reference
  counts span all domains (a next hop cannot be removed while any domain
  routes to it).
- **Domain reader structure: a dense `SlotTable<DomainSlot, Domain>` of
  `max_domains` slots** (`Router::Create(..., max_domains)`, default 1 = the
  default domain only, the cost and behaviour of the pre-M6 router). The slot
  is `domain id + 1`. A reader does one bounds-checked acquire load; an
  unknown, removed or out-of-range id (including `UINT32_MAX`, which wraps to
  the invalid slot) is a miss, never undefined behaviour. `CreateDomain()`
  publishes a fully built FIB with one store; `RemoveDomain()` (empty,
  non-default domains only) unpublishes it and frees it after a grace period,
  so a reader that already loaded it keeps a consistent empty FIB. Domain 0 is
  permanent and the default-domain overloads read it through a cached pointer
  with no slot lookup. Domain ids are an index, so they are dense by contract
  (bound `kMaxRouteDomains = 2^24`, set by the resource key).
- **Domains are structural.** Once the router is enrolled in a
  `TransactionEngine`, `CreateDomain`/`RemoveDomain` return `kEnrolled`
  (enforced, not a comment): the engine's route resource holds raw FIB pointers
  and iterates the domain list without a lock.
- **Route identity and transactions.** One `"<router>/routes"` resource and one
  `"<router>/next_hops"` resource per router. The route key is
  `EncodeKey(domain << 40 | addr << 8 | len)`; domain 0 is byte-identical to the
  previous key. Placement/pending (D-023) is per `(domain, prefix)`, a domain's
  tbl8 pool is its own, an unknown domain is rejected at prepare, and one
  transaction can change several domains' routes with their next hops.
  The module RPC adds `RouterRouteKey.domain = 3` and `RouterArg.max_domains = 4`
  (new optional fields, wire compatible; unset means domain 0 / 1 domain).
  `max_domains` creates domains `0..n-1` at `Init()`, before enrollment.
- **Update modes, named for what they are.** `SetRoute`/`RemoveRoute`: ordinary
  live one-writer updates, applied in place, each atomic to readers, a sequence
  not atomic. `ReplaceRouteSetAtomic(domain, RouteSet)`: validates every
  route's next hop, builds the replacement FIB off to the side
  (`LpmRouteTable::ReplaceAll`: a fresh `rte_lpm`, every rule added, so rule and
  tbl8 exhaustion surface before anything is visible), publishes it with one
  `RcuPtr` store, retires the old FIB through the RCU domain, and only then
  moves the reference counts. Any failure (`kUnknownNextHop`, `kInvalidId`,
  `kTableFull`) leaves the old generation published and the counts unchanged.
  A prefix named twice takes its last entry. It refuses with `kEnrolled` on a
  router enrolled in a transaction engine, like the direct setters. It costs a
  table build, not an in-place update (D-003: 5.8-40 us/route at 64K-512K).
- **Direct specialized use.** `Resolve(domain, ipv4)`,
  `ResolveBatch(domain, dst, hops)` and `LookupRoute(domain, ipv4)` (id only)
  need no `Module`, metadata or gate; the single-domain overloads are the
  default domain's.
- **Error model.** New `RouteError`s: `kUnknownDomain` (ENOENT), `kDomainExists`
  (EEXIST), `kDomainInUse` (EBUSY).

**Evidence.** GCC (`build/gcc`): 98/98 non-benchmark tests and the two route
benchmarks pass; Clang runs in CI. The timings below were measured by the
implementing agent in Release (`build/perf-release`, x86-64-v3, GCC),
pinned to one P-core, `--benchmark_min_time=0.5s`.

- *Old `route_domain_bench` is not a valid multi-domain baseline.* Its
  fixture created 64 domains with `(void)CreateDomain(...)`, but each domain's
  `rte_lpm` is a fixed 64 MB tbl24 and tests/benchmarks run with a 512 MB EAL
  heap, so only the first few domains existed (4,408 "LPM memory allocation
  failed" lines per run). Its 16- and 64-domain rows (4.06 and 2.40 ns) largely
  measured misses on never-created domains; the "1.69-2.41 ns" figure is
  therefore not evidence for 16/64 domains. Only its 1- and 4-domain rows
  (1.66 and 2.46 ns, id-only `MultiDomainRouter::Lookup`) hit real FIBs.
- *New, real FIBs (`BM_DomainSweep`, rotating over N domains, hot key):*
  `LookupRoute` (same work as the old `Lookup`) 1.03 / 1.04 / 1.06 ns at
  1 / 4 / 16 domains, against 1.66 / 2.46 ns at 1 / 4 before (the `std::map`
  walk is gone). `Resolve` (route + acquire fence + next-hop object) 1.49 /
  1.50 / 1.92 ns. Hot single domain: 1.35 ns by id, 0.91 ns through the
  default-domain overload.
- *Batches inside one domain:* 4-wide `ResolveBatch` 4.67 / 6.09 / 5.65 ns at
  1 / 4 / 16 domains (this also resolves next hops; the old X4 was id-only
  3.19 / 3.92 / 3.97 ns, so it is not the same work); 32-wide 45.7 / 54.6 /
  53.4 ns (0.70 / 0.59 / 0.60 G keys/s). Small and large FIB, 32-wide batches
  rotating over 4 domains: 1K routes 46.5-52 ns, 64K routes 62.6 ns.
- *Lookup during updates (32-wide, reader CPU time):* 48-49 ns with a
  second thread doing in-place `SetRoute`/`RemoveRoute` in the same domain
  (~1.4M updates/s) and 48 ns with `ReplaceRouteSetAtomic` swapping another
  domain (~340 swaps/s), against 45.7 ns idle. (Wall time per batch is ~2x
  because `taskset` may place the writer on the reader's core; CPU time is the
  reader's.) Domain lifecycle: `CreateDomain`+`RemoveDomain` 1.56 ms (a 64 MB
  table).
- *Single-domain `Router` against `route_bench`:* `BM_LookupRouter`
  (32-wide `ResolveBatch`, ns/batch, median of 7, ABBA) HEAD 213 / 239 / 267
  at 1K / 16K / 64K routes, M6 as built 236-239 / 256-264 / 270-278 in
  the same session, i.e. +11% / +10% / +2%. `BM_LookupRouteTable` (the raw FIB,
  unchanged code) is flat (168 vs 174 ns at 1K). I treated this as a
  regression to be explained. The M6 and HEAD loops compile to the same
  instructions (matching instruction count, L1 and dTLB misses); the
  difference is branch mispredicts, concentrated on the data-dependent tbl8
  branch. The effect tracks heap layout, not the lookup code [INFERENCE from
  perturbation]: changing only what the benchmark mallocs before it builds the
  router (so the `NextHop` objects it dereferences land elsewhere) moved the
  M6 binary between 216 and 237 ns/batch and HEAD's own binary between 213 and
  223 ns/batch. Reordering `Router`'s members and forcing the batch helper
  inline changed nothing. I found no code change that removes the gap, so it
  is recorded as measured: a single-domain `Router` is 2-11% slower on this
  one benchmark, with identical instruction streams.
- A control build of HEAD (`git archive` into a separate tree, same options)
  reproduced the 213 / 239 / 267 figures, so the baseline binary was not stale.

**Not done.**

- **`NextHop::egress` is still a `gate_idx_t`.** A strongly typed
  `InterfaceId` would change `NextHop`, `session_pipeline_test.cc`
  (`HopWithEgress`, `->egress`), the Router module adapter and the RPC value;
  that is the identity work M7 owns, and I did not widen M6 into modules
  outside the route files.
- **64 real domains were not measured.** 64 x 64 MB tbl24 needs 4.2 GB; this
  host had ~3 GB free, so rows with 64 domains are skipped by the benchmark
  (`BESS_DOMAIN_BENCH_MAX=64` with `BESS_DPDK_NOHUGE_MB` above 4.6 GB runs
  them). The 16-domain row is the largest real sweep.
- **Alternatives to the dense `SlotTable` were not benchmarked** (an immutable
  flat-vector generation republished per domain change, and an RCU-published
  sparse representation). The first has the same one-load read and an O(domains)
  republish per change; the second only pays when ids are sparse, which the
  dense-id contract excludes. Neither was built.
- **Domains cannot be created or removed once the router is enrolled**, and
  the RPC creates `0..max_domains-1` once at `Init()`. A live
  create/remove command and a transactional domain lifecycle are not provided;
  `ReplaceRouteSetAtomic` is not exposed over the RPC (an enrolled router
  changes routes through transactions).
- Each domain costs a 64 MB `rte_lpm` tbl24 regardless of its size.
- `LpmRouteTable::Clear()` still keeps the default route; only
  `ReplaceAll` replaces it.

**Revisit when:** more than ~16 domains per router are needed (a smaller
per-domain FIB would be the lever, not the domain index), domains must be
created under transactions, sparse domain ids appear, or M7 introduces the
interface identity that replaces the gate in `NextHop`.

---

## D-047 Phase A closure: link-graph checker, plugin descriptor range, conformance plugins

**Status:** accepted (2026-10-01).
**Code:** `tools/check_link_graph.py`, `tools/layer_dag.json`,
`docs/baselines/dependency-graph.json`, `core/framework/plugin.h`,
`core/framework/plugin_check.{h,cc}`, `core/bessd.cc`,
`sample_plugin/modules/incompatible_probe.cc`,
`examples/standalone_plugin/`, `docs/architecture.md`,
`.github/workflows/ci.yml`.

**Context.** Auditing M0-M5 against the roadmap found Phase A incomplete in
three places. M1 had an include checker but no check of what actually links.
M2's descriptor carried only a name and a version, nothing consumed it, and
conformance covered a trivial plugin and a negative case but not packet or
classifier use. M0's architecture contract lacked the API classification, RCU
and handle lifetime rules, and battery admission criteria, and described
`VISIBILITY_ATOMIC` as enabled when only `ScopeCell`, unused by the engine,
exists.

**Decision.**

- `tools/check_link_graph.py` reads the built static archives, resolves each
  undefined symbol against the strong definitions of the other BESS libraries,
  and compares the resulting graph with the allowlist in `tools/layer_dag.json`.
  A grandfathered edge needs a reason, an owner milestone and a removal phase;
  the tool fails on any other edge or on a cycle outside the exceptions, and
  warns about stale exceptions. Weak symbols are ignored. It runs as
  `check_link_graph` (and a self-test) in the `architecture` suite and
  emits `docs/baselines/dependency-graph.json`. Measured: 17 libraries, 47
  edges, 7 grandfathered. The grandfathered edges are `utils`, `classifier`,
  `meter`, `route` to `runtime` (lazy EAL bring-up, M22), `framework` to
  `runtime` (an `execution` split), `framework` to `route` (until M7 removes
  the gate from `Router`) and `control` to `host` (plugin loading).
- The descriptor gains `api_min`, `api_max` and `required_capabilities`
  (`BESS_CAP_*`), and `BESS_PLUGIN_REQUIRES`. `BESS_PLUGIN_API_VERSION` is 1.
  `bessd` validates a plugin that exports a descriptor before keeping it:
  layout version, API range and capabilities. A refused plugin is `dlclose`d,
  which deregisters the modules its static constructors registered, and a
  refusal is permanent (no inheritance-retry pass). A plugin with no
  descriptor loads as before. `plugin_check.h` is internal and not installed.
- Conformance: `standalone_macswap` (packet mutation) and `standalone_range_gate`
  (public classifier headers) join `standalone_pass`, all built from the staged
  install only; CI asserts each exports its descriptor. The sample plugin
  declares `BESS_CAP_INIT_CONTEXT`, and `incompatible_probe` declares an
  unsupported API range so the daemon test proves a refused plugin leaves no
  module registered and logs the refusal.
- `docs/architecture.md` now states the API classification, RCU lifetime
  rules, handle lifetime rules and battery admission criteria, and says plainly
  that `VISIBILITY_ATOMIC` is not provided until M8.

**Not done (still open in Phase A).**

- M0's measured baselines: the structured `f4fdab03` benchmark set, memory
  footprints, assembly shape notes, a one-command rerun, and the sanitizer
  subset. They need Release builds and benchmark time and are not started.
- M1's binary code-size comparison and the `route/**` to `module.h` include
  rule for `router.h` (blocked on M7).
- `bess_execution` split out of `bess_framework`. **Closed by D-057** (the split needed definition
  relocations and an EAL library, not only file moves).

**Verification.** GCC: 98/98 non-benchmark tests plus both route benchmarks,
including the two new architecture tests (`check_link_graph` and its
self-test), `plugin_check_test` (6 cases) and the sample-plugin daemon test
with the refusal assertion. Installed-tree check: 54 headers present, private
headers absent, positive and negative compiles pass, and the three
conformance plugins build against the staged package and export
`bess_plugin_descriptor_v1`. A negative run of the link checker with a
trimmed allowlist reported both injected violations and the resulting cycle.

**Revisit when:** a plugin needs a capability or API break that the range
cannot express, or the `framework` and `runtime` edges are untangled.

---

## D-048 Fast build profile: normal test linking, mold, ccache, quiet EAL

**Status:** accepted (2026-10-02).
**Code:** `tools/profiles/fast.ini`, `core/meson.build`, `core/packet_pool.cc`,
`MODERNIZATION.md` ("Build profiles").

**Context.** The debugoptimized tree was 12 GB: 128 executables averaging 85
MB, 96% of it DWARF, each linking 11 BESS libraries whole-archive, relinked
by GNU ld at 3.1-3.5 s per 55 MB test. A full suite run also wrote 397 MB of
logs, and filled a 7.7 GB `/tmp` until a test that reads a daemon log from a
temporary file failed spuriously.

**Decision.**

- `tools/profiles/fast.ini`: `-O1 -g0`, mold, ccache, benchmarks off. Perf and
  release stay ordinary Meson option sets; no native file for them.
- Unit tests link the BESS libraries with `link_with` (the linker pulls only
  referenced objects). Tests that need static registration (ADD_MODULE,
  drivers, hooks, the daemon) keep `link_whole`, as before.
- `rte_dump_physmem_layout()` in `PacketPool::CreateDefaultPools` runs only at
  `--v=1`. In no-hugepage mode it printed one line per 4 KB page, about 131,000
  lines per daemon start, 125 MB over the Python suite.

**Measured** (this workstation, GCC 16, `-j8`): link of one 55 MB test: GNU ld
3.1-3.5 s, gold 0.78 s, mold 0.27 s. Fast tree: cold build without benchmarks
174 s; edit of a core `.cc` rebuilding every test binary and `bessd` 4 s; leaf
edit plus its test 5 s; `module.h` edit 14 s for one test. Tree 240 MB (205 MB
executables, 25 MB generated protobuf) against 12 GB. 98/98 tests pass, with
the benchmark targets not built in this profile.

**Not done.**

- The 100 MB target. Executables are 92 files totalling 205 MB; reaching 100 MB
  needs the tests merged into a dozen or so binaries. That trades per-test
  isolation and parallel scheduling for size, and was not attempted.
- Widely included headers: `module.h` is included by 147 translation units,
  `packet.h` by 162 and `utils/endian.h` by 113, so an edit to them rebuilds
  minutes of work whatever the linker.
- No DPDK trim yet (apps and unused drivers off, curated NIC set).

**Revisit when:** the test count or binary sizes grow enough to break the
one-minute loop, or a precompiled-header experiment shows a measured gain.

---

## D-049 Logical network identities: InterfaceId replaces the gate in the route library (M7)

**Status:** accepted (2026-10-02).
**Code:** `core/dataplane/interface_id.h`, `core/dataplane/generation_handle.h`,
`core/dataplane/identity_test.cc`, `core/route/router.{h,cc}`,
`core/modules/router.cc`, `core/meson.build`, `tools/check_includes.py`,
`tools/layer_dag.json`, `docs/performance-contract.md`.

**Context.** `route::NextHop::egress` was a `gate_idx_t`, so the reusable
routing library included `gate.h`, and `route/router.cc` was compiled into
`bess_framework` (the one remaining `framework` -> `route` link edge). D-046
deliberately left that to M7.

**Decision.**

- `dataplane::InterfaceId` (`StrongId<_, uint32_t>`, zero = no interface)
  names a logical forwarding endpoint. `NextHop::egress` is an `InterfaceId`
  defaulting to none. `route/` no longer includes `gate.h`; `router.cc` moved
  from `bess_framework` to `bess_route`, and the grandfathered link exception
  was deleted (the link checker reports 46 edges, 6 grandfathered).
- The graph adapter, `modules/router.cc`, owns the mapping: interface `n`
  leaves on gate `n - 1`; interface 0, or one beyond the gate space, drops.
  The wire is unchanged: `egress_gate` keeps meaning a gate, and the codec
  converts (`DROP_GATE` is no interface). One subtraction and one compare
  per packet; no table.
- `dataplane::GenerationHandle<Id>` (`{Id id; uint32_t generation;}`, 8 bytes,
  one 64-bit compare) is added for ids that outlive an RCU read: queued
  continuations, punts, hardware completions. No consumer exists yet; it is a
  roadmap M7 deliverable and its contract is in `docs/architecture.md`
  section 7. It compares as one word because the defaulted member-wise `==`
  compiled to a branch plus two compares.
- `tools/check_includes.py` now applies to all of `core/route/` (not only the
  table): no `module.h`, `gate.h`, runtime, control or protobuf includes.
- Only ids with a consumer were introduced. `FlowId`, `ContinuationId`,
  `BridgeDomainId`, `NeighborId` and `NextHopGroupId` arrive with M9, M11, M14
  and M15.
- `docs/performance-contract.md` required every `StrongId` to be 32 bits, but
  `WorkerId` is 16; it now states the rule for ids that reach packets or
  hardware and names the exceptions.

**Evidence.**

- Assembly (GCC 16, `-O2`, `.scratch/asm`): the interface-to-gate mapping on
  `InterfaceId` and on a raw `uint32_t` compile to the same five instructions
  (`lea`, `mov`, `cmp`, `cmova`, `ret`); `InterfaceId` equality differs from the
  raw compare only in operand order; the validity test is identical;
  `GenerationHandle` equality is a single `cmpq`, as a raw 64-bit compare.
- Tests: `identity_test` (distinct id types do not compare or convert, zero is
  invalid, a handle with the same id and a later generation is unequal, hashes
  separate generations) and the existing route, route-domain, transaction and
  session-pipeline tests on the new type.

**Not measured.** There is no Router-module microbenchmark, so the added
per-packet subtraction and compare were not timed; `route_bench` exercises the
library, which no longer touches the egress.

**Revisit when:** a second adapter (a fused appliance, hardware) needs a mapping
that is not a constant offset, which would call for a table the owner supplies.

## D-050 Explicit transaction consistency: referential vs scope-snapshot (M8)

**Status:** accepted (2026-10-02).
**Code:** `core/dataplane/scope.h` (new; replaces `scope_cell.h`),
`core/dataplane/{resource.h,slot_resource.h,transaction_engine.{h,cc}}`,
`core/dataplane/scope_snapshot_test.cc` (replaces `scope_cell_test.cc`),
`core/control/dataplane_transactions.{h,cc}`, `core/control/dataplane_transactions_test.cc`,
`protobuf/control_v2.proto`, `pybess/bess.py`, `core/meson.build`,
`tools/check_includes.py`, `tools/check_installed_headers.py`,
`docs/{architecture,dataplane-tables,performance-contract}.md`.

**Context.** "Atomic" was an overloaded promise. `docs/architecture.md` and
`docs/performance-contract.md` said `VISIBILITY_ATOMIC` was enabled by a
`ScopeCell` that packed `{MeterId, NextHopId}` into one 64-bit word. The code
disagreed: nothing but its own test used `ScopeCell`; the control path only ever
recorded `VISIBILITY_DEPENDENCY_ORDERED`; `ApplyTransactionRequest` could not
ask for a level, so "reject instead of downgrade" had no surface; and
`dataplane/` included `meter/meter.h` and `route/next_hop_id.h` for it, an
upward edge the include checker did not look for. The roadmap (M8) asks for two
named levels, snapshot scopes only for a real consumer, a refusal with a reason
instead of a silent downgrade, and adversarial reader tests. Where roadmap and
code differed, the code was trusted: there was no `ScopeCell` with
`Read() -> const ScopeVersion*`, only a packed word.

**Decision.**

1. **Two levels, named apart.** C++: `dataplane::Consistency { kReferential,
   kScopeSnapshot }`. Wire: `ApplyTransactionRequest.consistency` (new field 4;
   `CONSISTENCY_UNSPECIFIED` means referential, which is all a request could ask
   before) and `TransactionRecord.Visibility` gains `VISIBILITY_SCOPE_SNAPSHOT
   = 3`. `VISIBILITY_ATOMIC = 2` stays only because removing a value breaks
   `buf breaking`; it is `deprecated` and never sent (it promised "all at
   once", which nothing provides: the promise is per scope). The record's
   visibility reports the level the transaction ran under.
2. **A level is a capability of a resource, checked, not negotiated.**
   `Resource::ProvidedConsistency()` defaults to referential. `Apply(ops,
   expected_generation, consistency)` answers `Outcome::kUnsupported` for a
   scope-snapshot request that names any resource without the capability, in
   the structure phase before anything is reserved (the failing operation
   carries the reason; the rest are `kNotApplied`). Over gRPC that is
   `UNIMPLEMENTED` with error detail `UNSUPPORTED_TRANSACTION`, `field =
   "consistency"`, `object =` the resource, the reason as the message -- a
   typed error, not a record: retrying cannot change it, nothing was attempted,
   and nothing is stored under the `request_id`. A consistency value the build
   does not know is `INVALID_ARGUMENT` (reading it as referential would be the
   downgrade). `consistency` is part of the request digest, so reusing a
   `request_id` with the other level is the existing `CONFLICT`.
3. **The smallest scope primitive: a `SlotTable` keyed by `ScopeId`.**
   `ScopeTable<Version> = SlotTable<ScopeId, Version>` and `ScopeResource<Version>
   : SlotResource<ScopeId, Version>` reporting `kScopeSnapshot`. A `SlotTable`
   already is "stable id -> atomic pointer to an immutable object": one acquire
   load to read (the roadmap's `ScopeCell::Read()` is `ScopeTable::Lookup`),
   one exchange to publish, the old version retired through the transaction's
   grace period, id reuse quarantined until the removal cascade (the ScopeId
   reuse risk), and references from a version to separately owned state (a
   meter id) tracked by the engine's ledger. A second cell class would
   duplicate all of it and need its own resource adapter. `ScopeResource`
   inherits that; `SlotResource` lost `final` for it. The meter/route-specific
   `ScopePlan`/`ScopeCell`/`ScopeTable` were deleted (clean cutover, no
   alias); `Version` is whatever the application defines.
4. **Operational definition of "identifiable independently".** A scope is the
   key of a `ScopeResource`: the id a packet carries, chosen by the
   application and unrelated to the rules replaced. The engine already allows
   one operation per key per transaction, so each scope in a transaction
   switches by exactly one pointer store. What the engine cannot check -- that
   the lookups a version covers do not depend on rules outside it -- stays the
   application's obligation, written in `scope.h`.
5. **Mixing is refused, not approximated.** A scope-snapshot request that also
   names an ordinary resource (a new action, a next hop) is `kUnsupported`:
   serving it "snapshot where possible" is the downgrade. The pattern is
   two calls: create the referents (invisible until named) in a referential
   transaction, then switch the scope in a scope-snapshot one. Two scopes in
   one scope-snapshot transaction each switch whole; there is no order between
   them and no reader binds both.
6. **Layering.** `scope.h` includes only `dataplane/` headers. `check_includes.py`
   now forbids non-test `core/dataplane/` from including `meter/`, `route/`,
   `classifier/` and `stats/`, with four negative self-test cases (11 cases in
   all; it was 7). `scope.h` and `slot_resource.h` (which it includes) are
   installed (`core_public_subdir_headers`, `PUBLIC_REQUIRED`); `scope_cell.h`
   is gone from both.
7. **The "real consumer" is two test applications, decided here.** The
   application owns what a scope means, so the acceptance shapes are in-tree
   applications of the library, not changes to a shipped module: a per-session
   policy (meter, next hop, QoS class that must move together) and a VFP-like
   policy group (a classification layer and an action layer that must come
   from one group version). Both run through the production engine; the
   session shape also runs through the production RPC server and codec
   machinery. No shipped module reads a scope on its packet path: that would
   add a read to a hot path with no user to size the `Version` for.

**Evidence.** `dataplane_scope_snapshot_test` (18 tests) and the new
`ConsistencyRpcTest` cases of `control_dataplane_transactions_test` (4):

- *Schedule enumeration.* The writer's publication steps and a reader's loads
  are interleaved in every possible way (before the transaction, after each
  publication step, after the transaction returned with the reader still
  holding what it read), and the set of observations is compared with the
  contract exactly.
  - `SessionBoundOnceIsAlwaysOneWholeVersion` (21 schedules): a reader that
    binds the scope and reads four fields sees exactly `{old, new}`, never a mix.
  - `ReaderThatRebindsPerFieldSeesATear`: the same harness shows a tear for a
    reader that looks the scope up per field, so the harness is able to fail.
  - `TwoScopesEachSwitchWholeWithNoOrderBetweenThem`: each scope whole; some
    interleaving shows one switched and the other not.
  - `GroupBoundOnceResolvesThroughOneVersion` vs
    `GroupUpdatedReferentiallyCanBeObservedHalfApplied`: the group resolves to
    `{v1, v2}` as a scope and to `{v1, v2, miss}` when its two layers are
    replaced referentially.
  - `ReferentialModelTest.*`: a reader that follows a reference sees the new
    referent or a newer one, exactly `{(1,1),(1,2),(2,2)}`; the reverse reader
    sees all four combinations; a new referrer is never visible without its
    referent; an erased referent stays readable for a reader holding its
    referrer and goes after the reader's quiescent state.
- *Threads* (`ScopeSnapshotStressTest.*`, `ReferentialStressTest.*`): two real
  RCU readers against a writer running at least 2,500 transactions and 300 ms;
  0 torn policies, 0 scope going backwards, 0 misses, 0 dangling references.
  The last run read about 1.1 M session policies over 286 K epochs (scope 3
  erased and recreated every 100), 2.1 M group resolutions over 429 K
  versions, and 1.9 M referential reads over 277 K epochs.
- *Failure injection* (`ScopeSnapshotFailureTest.*`): an error or an exception
  at each of four reservation positions leaves the very same old version
  objects visible, with no publication step run and the generation unchanged,
  and the identical transaction then applies; a version naming a missing hop is
  refused with the old scope intact; a hop a live version names cannot be
  erased, and an erased hop stays readable to a reader holding the old version.
- *Refusal* (`ScopeSnapshotRefusalTest.*`, `ConsistencyRpcTest.*`): a scope
  request naming a plain resource, first, last or alone, is `kUnsupported`
  with the reason, and neither resource changes; the same operations apply
  as referential; over gRPC the refusal is `UNIMPLEMENTED` /
  `UNSUPPORTED_TRANSACTION` / `consistency` / the resource, unrecorded, and
  the corrected request succeeds under the same `request_id`; an unknown level
  is `INVALID_ARGUMENT`.
- *Wire compatibility.* `buf` is not installed here, so the old and new
  `control_v2.proto` were compiled with `protoc` to descriptors and compared
  field by field: nothing removed, renumbered or retyped; added: field 4, the
  nested `Consistency` enum and `VISIBILITY_SCOPE_SNAPSHOT = 3`.
- *Layering and packaging.* `check_includes.py` self-test 11/11 and clean run;
  `check_link_graph.py` 46 edges, 6 grandfathered (unchanged);
  `check_installed_headers.py` on a staged `meson install --tags devel`: 57
  curated headers present, all compile, private include rejected.

**Not done.**

- No shipped module consumes a scope (decision 7). `ScopeResource` is
  experimental API with two test applications.
- No cross-scope atomicity and no mixed referential + snapshot transaction;
  both are refused or documented, not approximated (decisions 4 and 5).
- No packet-path code changed: `SlotTable::Lookup` is untouched and `Resource`
  gained one control-side virtual, so no benchmark was run and the cost of the
  scope load is not measured; the contract line says so.
- The schedule enumeration has one reader against one writer. Several readers
  are covered only by the threaded tests, which are probabilistic. No
  ThreadSanitizer run (the build has no sanitizer option; M22).
- `buf lint`/`buf breaking` themselves were not run (the descriptor comparison
  above stands in). `pybess.apply_transaction` gained a `consistency`
  argument; no live-daemon Python test was added or run (bessd starts through
  `sudo` in `run_module_tests.py`).
- A `ScopeId` carried across a task boundary (a handoff, a completion) is not
  generation-checked: `SlotTable` has no generations and no such consumer
  exists. `GenerationHandle<ScopeId>` (D-049) is the tool when one does.

**Revisit when:** a shipped module or plugin (the OMEC UPF plugin, a policy-group
firewall) reads a scope on its packet path -- then size its `Version`, add its
codec and a live test, and benchmark the load; a client needs the referents and
the switch in one call (a prepare-then-switch request); a use needs two scopes
to switch together; or a worker must hold a scope across task invocations.

---

## D-051 DPDK build profiles: bess (software ports) and full (every NIC family)

**Status:** accepted (2026-10-02).
**Code:** `tools/bootstrap_dpdk.py`, `.github/workflows/ci.yml`,
`MODERNIZATION.md` ("Build profiles").

**Context.** The pinned DPDK built 2,246 compile units: 976 NIC drivers, 308 of
DPDK's own apps, and crypto, event, baseband, regex, ml, compress, vdpa, raw,
gpu and dma drivers BESS never touches. It cost a 545 MB CI cache entry and
the longest cold start in the pipeline, and a development machine carried
about 1.3 GB of install and build tree.

**Decision.**

- `bootstrap_dpdk.py --profile {bess,full}`. Apps are never built and every
  library stays enabled in both (64 libraries, identical sets), so a plugin
  that uses any DPDK library still compiles and links; only devices differ.
- `bess`: the PCI and vdev buses, the ring and stack mempools, and the
  `null`, `ring`, `af_xdp`, `af_packet` and `tap` ports. This is what the code
  and tests name (`net_null`, `net_ring`, `net_af_xdp`); the Intel driver
  names in `drivers/pmd_test.cc` are strings in fake `rte_eth_dev_info`
  structs, not devices.
- `full`: every NIC family; the unused device classes above stay off. It is the
  default, so a README build for real hardware behaves as before.
- CI builds and tests with `bess`; the release job builds `full`. The two have
  separate cache keys (they previously shared one).

**Measured** (cold builds from scratch, `-j8`, this workstation): `bess` 38 s,
21 MB installed, 67 static libraries, 372 compile units; `full` 141 s, 68 MB
installed, 148 static libraries, 1,624 compile units. The previous
configuration was 2,246 units and a 703 MB build directory. BESS built against
the `bess` install passes 99/99 tests (fast profile).

**Out-of-tree plugins.** A plugin compiles against `bess-dev`, whose
pkg-config file requires `libdpdk`; that now lists the libraries of the
installed profile. Libraries are the same in both profiles, so no plugin
loses an API. A plugin that needs a particular device (a crypto PMD, a NIC
driver) must run on a DPDK that has it: it gets the devices of the `bessd` it
loads into, which is the release (`full`) binary or whatever the deployer
built. Plugins do not link drivers themselves.

**Not done.**

- A curated NIC set for `full` (for example only Intel and Mellanox families).
  Which families are deployed is a product decision not made here.
- Removing the old install and build trees on this machine: `build/gcc`,
  `build/clang` and `build/perf-*` still load libraries from the previous
  install, so deleting it would break them.
- Re-running the full CI release build with `full` is left to CI; locally only
  `bess` was built and tested end to end, `full` was built cold and
  size-checked but BESS was not linked against it.

**Revisit when:** a NIC family is deployed that `full` omits, a DPDK library
is needed that has to be disabled to save time (none is expensive: libraries
are 335 of 2,246 units), or the release set is curated.

## D-052 Generic flow-state substrate: typed flow tables with generation-checked ids (M9)

**Status:** accepted (2026-10-02), with the exceptions under "Not done".
**Code:** `core/flow/{flow_types.h,flow_key.h,flow_index.h,flow_storage.h,flow_observer.h,owner.h,owner.cc,worker_flow_table.h,shared_flow_table.h}`,
tests `core/flow/{flow_index_test.cc,worker_flow_table_test.cc,shared_flow_table_test.cc,reference_apps_test.cc}`,
benchmark `core/flow/flow_bench.cc`, `core/meson.build` (`bess_flow`, install manifest,
test and benchmark registration), `tools/{check_includes.py,check_installed_headers.py,layer_dag.json}`,
`docs/{flow-state,architecture,dataplane-tables,performance-contract,benchmarking}.md`,
`docs/baselines/dependency-graph.json`.

**Context.** Firewalls, NATs, load balancers, session compilers and caches all
need the same primitive: typed state under an application-defined key that
outlives a lookup, with a handle that is safe to keep. What existed:
`ConcurrentExactTable` (rte_hash, shared writers by D-028) holds an 8-byte value
and is internal; `utils::CuckooMap` is single-writer but grows itself when an
insert fails (`DoEmplace`, "expand the table as the last resort"), which is a
resize on the path that creates flows, and its free-entry list is a
`std::stack` over a deque, which allocates as it grows; `GenerationHandle`
(D-049) had no consumer; `SlotTable` publishes immutable objects one at a time
from a control thread. The roadmap's text and the code agreed except where
noted below (it names `std::equal_to` and `DefaultHash`; the hash and
equality here must be `noexcept` and the defaults read the key's bytes, see
"Key").

**Decision.**

- **A new library, `bess_flow` (`core/flow/`)**, on `bess_dataplane_core`,
  `bess_rcu`, `bess_utils` and (for the shared table only) `bess_classifier`.
  `check_includes.py` forbids `module.h`, `gate.h`, `framework/`, `runtime/`,
  `control/`, protobuf, gRPC and `worker.h` there (13-case self-test; the
  `worker.h` rule also catches `stats/current_worker.h`). The link-graph
  checker lists `bess_flow` with no exception; it reports 18 libraries and 46
  edges (6 grandfathered, unchanged). The committed
  `docs/baselines/dependency-graph.json` was regenerated; it also drops two
  edges D-049 had already removed. Expiry stays in `dataplane/` for M10, so the
  flow library defines only the seam (below). All headers except
  `shared_flow_table.h` are installed as experimental API (it includes
  `classifier/concurrent_exact.h`, which is internal); the installed-header
  check compiles them standalone.
- **Ids.** `FlowId` (`StrongId<_, uint32_t>`, one-based, zero = none) and
  `FlowHandle = GenerationHandle<FlowId>`. A slot's generation is odd while it
  holds a flow and even while free and advances at creation and at erasure, so
  a handle matches only the lifetime it was issued for (a forged even
  generation matches nothing). A slot whose generation would wrap (2^31 reuses)
  is retired instead of reused: no ABA, ever, at the price of that slot.
  `SlotReuse::kLifo` (warm cache) or `kFifo` (slot freed longest ago; use when
  handles live long) is a template choice.
- **`WorkerFlowTable<Key, State, Hash, Equal, Traits>`** (worker-owned). One
  contiguous slot record per flow holds its keys, generation and the in-place
  State (State is never moved or copied, its address is stable, its
  constructor may throw without changing the table); a directory maps hashes
  to key ids. Everything is allocated in `Create()` (which reports
  `kInvalidCapacity`, `kTooLarge` or `kOutOfMemory` and leaves nothing
  allocated); after that no operation allocates, resizes or rehashes. A full
  table returns `EmplaceStatus::kFull`, builds no State, changes nothing and
  tells the observer; it never evicts. Nothing is atomic. `Traits` carries the
  alias count, reuse policy, observer, owner policy and allocator.
- **The directory is a new, small, fixed-size table (`FlowIndex`)**, a
  deliberate exception to "do not create a hash implementation by default":
  64-byte buckets of eight 16-bit tags and 32-bit key ids, overflow to the next
  bucket counted in the buckets passed (an erase takes the counts back, so
  there are no tombstones), user hash mixed by two multiplies, bucket chosen by
  multiply-shift so any bucket count works, tags compared with one SSE2 compare
  (a scalar definition is tested equal). Why not the existing backends,
  measured below: `CuckooMap` resizes and allocates (cannot satisfy the
  requirement), and rte_hash/`ConcurrentExactTable` need the EAL, compare keys
  through a function pointer, and store 8-byte values, so a
  typed State costs a second array and a second cache miss. Sized for 4 keys per
  bucket (50% load) because deletion never moves an entry back: simulated with
  random keys at steady churn, the average chain a *missing* lookup walks was 1.24
  buckets at 4 per bucket, 2.1 at 5, 3.4 at 5.5 and 7.2 at 6.
- **Key.** `FixedFlowKey` = trivially copyable. The default hash and equality
  read the key's bytes and exist only for canonical keys: no padding
  (`has_unique_object_representations`) or a `FlowKeyTraits<K>` specialisation
  in which the author promises zeroed padding; anything else is a compile error
  saying so. Keys with masks or don't-care fields pass their own `noexcept`
  `Hash` and `Equal`. The hidden-padding hazard is thereby a compile-time
  decision, not a runtime surprise.
- **Aliases** (`Traits::kAliases = N`): up to N extra keys per flow, each one more
  directory entry pointing at the same slot record; `FindRef` says whether the
  key was an alias (no direction semantics in the table); `EmplaceAliased` is
  all-or-nothing; erasing a flow removes every key. Chosen over a second table
  the application keeps in step by measurement (`BM_AliasLookup`,
  `BM_AliasChurn`, below): equal lookup speed, 16% fewer bytes per flow, equal
  create/erase rate, and consistency by construction. A secondary index inside
  the table was not built (it has the second table's second probe).
- **Expiry seam (for M10).** `Traits::Observer` (`OnCreate`, `OnErase`, `OnFull`;
  inline, `NoFlowObserver` is empty, `FlowCounters` is optional). The expiry
  engine keeps `FlowHandle`s, expires by `Erase(handle)`, and a record that
  outlived its flow fails closed. The table has no timer and no `Touch()`
  (refresh is a store in the application's State). Tested with a fake expiry
  engine, including the stale-record case M10's exit criteria name.
- **Ownership diagnostics.** `Traits::Owner` (`kChecked`, `Current()`): with a
  checked policy every call compares the caller with the owner, bound on the
  first call, and aborts on a mismatch; unchecked policies store and compile
  nothing. The library cannot include `worker.h`, so worker identity is
  injected (`TokenOf(WorkerId)`; the default checks the calling thread in
  builds without `NDEBUG`). `ReleaseOwner()` hands a table over.
- **`SharedFlowTable<Key, State, Traits>`** (shared lookup; not installed).
  The directory is a `ConcurrentExactTable` holding the 8-byte `FlowHandle`; the
  State lives in the same slot records as above. Readers take no lock and do no
  read-modify-write; writers from any thread serialize on one spinlock (D-028's
  model, whose numbers decide when to use it). An erased flow leaves the
  directory and its handle stops resolving at once; its State is destroyed and
  slot reused only after a grace period of the caller's `RcuDomain`, one grace
  period per `Reclaim()` batch, in a fixed 4-byte-per-flow ring (no allocation
  on erase; a bounded number of outstanding batches, overflow folds into the
  newest). Destruction runs on whoever calls `Reclaim()` or `Emplace` on a full
  table. `StateSharing::kSharedMutable` (State synchronises itself; `Find`
  returns `State *`) or `kOwnedByCreator` (others get `const State *` from
  `Peek`; `FindOwned` checks the creator under an owner policy) keeps the
  roadmap's two questions apart.

**Evidence.** Everything below was run; GCC 16.2.1 unless stated.

*Tests* (fast tree, `-O1`; 103/103 pass: the 99 before plus four new test
binaries holding 49 cases):

- `flow_index_test` (10): the SSE2 tag match equals its scalar definition for
  every pattern; the designed key count always fits, with bounded chains, for
  sequential, high-half and strided keys; hash flood (every key to one bucket,
  counters saturated) still finds, erases and reuses everything; after 400,000
  random churn operations each bucket's overflow count equals exactly what the
  live entries imply (no residue) and a missing lookup walks under 1.5 buckets;
  the default hash spreads a counter placed at *every* byte offset of a key.
  The last two caught two real defects while writing them: a first default
  hash (multiply then rotate) confined a counter in the high half of a word to
  8 buckets, and a multiplicative hash alone made lookups of a counter in the
  top bytes of a 32-byte key walk 1.3 buckets against 1.05 for a random hash;
  the index therefore always mixes, and the default hash folds high bits down.
- `flow_worker_flow_table_test` (21): create/find/erase/duplicate; capacity
  exhaustion (refusal builds no State, an existing key is `exists` not `full`,
  one erase frees exactly one slot); in-place State with stable address and
  destruction; a throwing constructor changes nothing; **a stale handle cannot
  reach the flow that reused its slot** (including forged handles and erase by
  stale handle); LIFO vs FIFO reuse; generation exhaustion retires the slot;
  alias lifetime (every key leaves with the flow, freed keys are reusable,
  `EmplaceAliased` all-or-nothing); any key type and a constant hash; batch
  equals scalar; `ForEach`; reported bytes per flow; failure injection in the
  allocator at each of the three allocations (nothing leaks) and impossible
  capacities; ownership (first caller owns, other callers and wrong
  `ReleaseOwner` abort with the operation named, hand-off, thread tokens); the
  fake expiry engine (hooks, cancel on erase, budgeted expiry by handle, stale
  record); and **a random differential test against `std::map` /
  `unordered_map` models** (six seeds x three table shapes plus a weak-hash
  table and a one-flow table, 40,000 operations each, comparing state, handles,
  `via_alias`, size, fullness, stale handles and batches after every step).
- `flow_shared_flow_table_test` (14): the same API checks on the shared table
  with State larger than rte_hash's value; **a stalled RCU reader keeps the
  erased State alive and intact and its slot taken until it passes a quiescent
  state**, then everything is released; more outstanding batches than the table
  tracks fold safely; stale-handle and generation-retirement tests; alias
  lifetime; `kOwnedByCreator` access and death on a stranger; creation-failure
  injection and a directory rte_hash refuses (no slot array allocated first);
  four racing creators make each of 3,000 flows exactly once;
  **concurrent lookup against create/erase** (3 registered readers using each
  State they find, 2 writers, a reclaiming control thread, 1.5 s; a State seen
  after its destructor or with another key's value fails); and a differential
  model test.
- `flow_reference_apps_test` (4): a NAT (reverse key as alias, idle expiry,
  full-table drop), an L2 forwarding table (learn, move, VLAN in the key, aging
  by scan) and a load balancer's connection table shared by four workers (each
  connection pinned to one backend as seen by all; backend removal erases its
  connections) are written against the public API only: three appliance states,
  no table of their own.
- Mutation checks (each fails the named tests, then reverted): reclaiming
  without the grace-period check; not bumping the generation on erase; not
  erasing alias keys (worker and shared); accepting an even generation; not
  decrementing overflow counts; not retiring an exhausted generation.
- Not run under ThreadSanitizer (no instrumented tree); the concurrent tests
  ran in the ordinary build. Compiled clean with Clang 22.1.8 `-Werror`
  (syntax only, tests and benchmark); GCC 14 and Clang 19 run in CI.

*Benchmarks.* `core/flow/flow_bench.cc`; Intel Core i9-13900H (hybrid:
6 P-cores with SMT, 8 E-cores; 1.25 MiB L2 per P-core, 24 MiB shared L3), GCC
16.2.1 `-O3` (meson `release`) `-march=x86-64-v3`, DPDK 25.11.3, Linux 7.2,
governor `powersave` (HWP, EPP `balance_performance`; not changed), no core
isolation; single-thread benchmarks pinned to P-core CPU 2 with `taskset`,
3 repetitions, medians (coefficient of variation of the single-thread lookup
benchmarks: median 0.2%, 90th percentile 1.3%, worst 5.4%). 16-byte five-tuple key
(13 bytes of fields, 3 explicit zero bytes), 32-byte State, unless a column
says otherwise; every hit reads `State::counter`. The TSC ran at 3.00 GHz
(1 ns = 3.0 ticks; the core ran faster, so ticks are not core cycles). Query
streams are 262,144 precomputed keys replayed. ns per lookup:

| table | access | batch 1 | batch 8 | batch 16 | batch 32 |
|---|---|---|---|---|---|
| 1K | one hot flow | 4.3 | 4.5 | 4.4 | 4.6 |
| 1K | Zipf(1) | 4.5 | 4.7 | 4.6 | 4.8 |
| 1K | uniform hit | 4.8 | 4.9 | 4.7 | 5.0 |
| 1K | miss | 3.2 | 3.4 | 3.4 | 3.8 |
| 1K | 50/50 hit/miss | 11.5 | 9.6 | 10.0 | 9.7 |
| 64K | one hot flow | 4.3 | 4.5 | 4.4 | 4.6 |
| 64K | Zipf(1) | 7.1 | 6.8 | 5.6 | 5.6 |
| 64K | uniform hit | 8.7 | 8.0 | 6.7 | 6.3 |
| 64K | miss | 4.5 | 4.0 | 3.7 | 4.0 |
| 64K | 50/50 hit/miss | 14.3 | 10.7 | 10.3 | 9.9 |
| 1M | one hot flow | 4.3 | 4.5 | 4.4 | 4.6 |
| 1M | Zipf(1) | 17.2 | 15.8 | 13.1 | 11.3 |
| 1M | uniform hit | 35.5 | 32.1 | 22.5 | 19.4 |
| 1M | miss | 6.1 | 6.3 | 5.4 | 4.6 |
| 1M | 50/50 hit/miss | 28.1 | 26.6 | 19.3 | 15.3 |

(Zipf(1) is the log-uniform approximation: rank *r* with probability ~1/*r*. The
50/50 case is slow even in a 1K table because a random hit/miss pattern
mispredicts the tag-match branch about half the time; a 1K batch of
misses or hits alone is 3-5 ns. At batch 1 and 1M, a uniform hit is one
dependent miss to the index and one to the slot record; a batch of 32 overlaps
them, 35.5 to 19.4 ns.)

Key width and State size, batch 32, with bytes per flow at full occupancy
(slot record + free list + directory):

| key | State | 64K hit | 64K miss | 1M hit | 1M miss | bytes/flow |
|---|---|---|---|---|---|---|
| 8 B | 32 B | 5.5 | 3.6 | 18.2 | 4.0 | 68 |
| 16 B | 32 B | 6.3 | 4.1 | 19.6 | 4.5 | 76 |
| 5-tuple (16 B) | 32 B | 6.3 | 4.0 | 19.4 | 4.6 | 76 |
| 32 B | 32 B | 8.4 | 5.0 | 23.5 | 6.0 | 92 |
| 48 B (tunnel + inner tuple) | 32 B | 10.8 | 6.0 | 27.8 | 7.4 | 108 |
| 5-tuple | 16 B | 6.0 | - | 19.0 | - | 60 |
| 5-tuple | 64 B | 6.7 | - | 21.2 | - | 108 |
| 5-tuple | 128 B | 6.9 | - | 20.6 | - | 172 |
| 5-tuple | 16 B + 256 B record in a separate array | 12.0 | - | 37.5 | - | 316 |

The backends the roadmap says to evaluate, on the same streams (ns per lookup;
`CuckooMap` with its own prefetch staging for batch 32, rte_hash in position
mode with `rte_hash_lookup_bulk` and the State in a slab, `ConcurrentExactTable`
with `LookupBatch` and a slab; "FlowIndex raw" is this directory with keys and
States in two flat arrays, a scalar loop, no generation: it isolates what the
slot record and batch stages add):

| backend | 64K hit b1 | 64K hit b32 | 64K miss b1 | 64K miss b32 | 1M hit b1 | 1M hit b32 | 1M miss b1 | 1M miss b32 | bytes/flow |
|---|---|---|---|---|---|---|---|---|---|
| `WorkerFlowTable` | 8.7 | 6.3 | 4.5 | 4.0 | 35.5 | 19.4 | 6.1 | 4.6 | 76 |
| FlowIndex raw | 8.4 | 7.2 | 4.7 | 3.9 | 39.8 | 34.8 | 7.3 | 6.2 | 64 |
| `CuckooMap<K, State>` | 8.1 | 16.4 | 5.0 | 6.1 | 34.2 | 27.6 | 8.7 | 10.2 | 64 |
| rte_hash + slab | 24.3 | 10.1 | 14.6 | 7.0 | 57.9 | 26.8 | 16.5 | 7.7 | 109 |
| `ConcurrentExactTable` + slab | 28.8 | 8.7 | 14.0 | 4.0 | 120.3 | 27.1 | 21.6 | 5.8 | 152 |
| `std::unordered_map` | 18.6 | - | 24.9 | - | 58.4 | - | 67.1 | - | not measured |

With an 8-byte key and 1M flows, uniform hit, batch 1 / 32: `WorkerFlowTable`
(batch 32 only) 18.2; `CuckooMap` 29.4 / 27.9; rte_hash 61.2 / 34.7;
`ConcurrentExactTable` 114.1 / 25.8; FlowIndex raw 34.4 / 29.1;
`std::unordered_map` 47.8. Scalar lookups: `CuckooMap`, the one backend that
resizes, is 4-7% faster on hits than `WorkerFlowTable` and slower on misses
(5.0 vs 4.5 ns at 64K, 8.7 vs 6.1 at 1M); the others are 1.6-3.4x slower on hits.
Batches of 32: `WorkerFlowTable` is 27-28% faster than the best other backend
on hits (6.3 vs 8.7 ns at 64K, 19.4 vs 26.8 at 1M), equal on 64K misses and 21%
faster on 1M misses. Bytes per flow of rte_hash and `ConcurrentExactTable` are
the EAL heap each used plus the slab.

Slot-record prefetch in `FindBatch` (`Traits::kPrefetchSlots`; batch
8 / 16 / 32, ns): 64K hit 8.0 / 6.7 / 6.3 without, 8.4 / 6.9 / 7.1 with; 64K
miss 4.0 / 3.7 / 4.0 vs 4.6 / 4.5 / 4.8; 1M hit 32.1 / 22.5 / 19.4 vs
30.7 / 20.7 / 18.5; 1M miss 6.3 / 5.4 / 4.6 vs 6.9 / 6.0 / 5.5. It helps only
an all-hit batch far beyond the caches (4-8%) and costs everywhere else (10-22%
on misses, up to 13% on cache-resident hits), so it is off by default.

Aliases (`BM_AliasLookup`, `BM_AliasChurn`; five-tuple key, 32-byte State,
batch 32, uniform hit, ns; one table with an alias key against two tables, the
second mapping the reverse key to a pointer): 64K via the forward key 6.8 vs
6.4, via the reverse key 6.9 vs 6.9; 1M 23.1 vs 19.9 and 22.6 vs 25.6. Bytes
per flow 108 vs 128. Creating and erasing a two-key flow flat out: 49.7 vs 55.2
ns at 64K, 154 vs 151 ns at 1M.

Churn, batch of 32 lookups with a fraction of operations replaced by a
create + erase pair of a flow in a sliding window (`BM_WorkerChurn`): 64K 1% 8.7
ns/op, 10% 9.5 ns/op, flat out 36.5 M create+erase pairs/s (13.7 ns/op);
1M 1% 29.3, 10% 31.3, flat out 21.4 M pairs/s. Latency of one create or erase
run alone (serialised timestamps, ticks / ns): 64K create p50 44 / 15, p99
96 / 32, p99.9 136 / 45; erase 58 / 19, 106 / 35, 134 / 45; 1M create p50
78 / 26, p99 524 / 174, p99.9 932 / 310; erase 100 / 33, 520 / 173, 936 / 312.
The maxima (60,000-123,000 ticks, 20-41 us) are interruptions, not the table:
nothing in create or erase loops or allocates.

Shared lookup (`BM_SharedReaders`; M lookups/s in total, batch 32, uniform
hit, each reader pinned to its own physical core; the partitioned row gives
each reader its own `WorkerFlowTable` of 1/N of the keys and only its own keys):

| cpus | table | readers: 1 | 2 | 4 | 8 |
|---|---|---|---|---|---|
| P-cores | 64K shared directory | 105 | 216 | 416 | - |
| P-cores | 64K partitioned | 160 | 343 | 741 | - |
| P-cores | 1M shared directory | 35 | 70 | 129 | - |
| P-cores | 1M partitioned | 51 | 98 | 161 | - |
| E-cores | 64K shared directory | 48 | 84 | 127 | 270 |
| E-cores | 64K partitioned | 67 | 128 | 216 | 541 |
| E-cores | 1M shared directory | 19 | 36 | 60 | 119 |
| E-cores | 1M partitioned | 23 | 48 | 82 | 145 |

Shared readers scale to the cores they were given (the shared directory runs
at 69-83% of a partitioned table's per-reader rate at 1M and 50-72% at 64K) and
nothing the readers share is written. One shared writer cycling 4,096 flows
(`BM_SharedReaderWriter`, as fast as it can) cost the readers 22-24% at 64K
on P-cores and 2-16% on E-cores, and 4-6% at 1M on P-cores and 3-6% on E-cores
(the 64K loss is probably the writer's stores invalidating directory lines the
readers share; not isolated); the writer managed 1.8-4.7 M
operations/s, its p50/p99 per operation was 424-1,662 / 794-3,184 ticks
(0.14-1.1 us), and the reclamation backlog peaked at 2,560-6,400 erased
flows. Bytes per flow of the shared table: 64 (slot, free list, ring) plus
about 120 for the rte_hash directory (measured as EAL heap growth) = 184.

10M flows (`FLOW_BENCH_LARGE=1`, 8-byte key and 16-byte State only, worker
table): 52 bytes per flow (about 0.5 GB), uniform hit 34.3 ns scalar and 19.5 ns
per lookup in batches of 32, miss 11.0 and 8.5 ns.

*Assembly.* GCC 16.2.1 `-O2 -march=x86-64-v3`, a 16-byte key and 32-byte State
(`.scratch/asm/find.cc`, not kept). `WorkerFlowTable::Find` compiles to the
same code as the raw directory loop: the inline key hash (two multiplies per
word, then the two-multiply mix and the multiply-shift bucket choice), a
broadcast of the tag and `vpcmpeqw` + `vpacksswb` + `vpmovmskb` against the
bucket's eight tags, `tzcnt` over the match mask, the id load, the slot
address (`imul $56`; the raw loop shifts), a 16-byte `vpxor` + `vptest` key
compare, the state address, and the overflow-byte test that continues or
ends. 81 instructions against the raw loop's 85, no call, no `lock`-prefixed
instruction, no allocation, no indirect jump; the expected shape for the
roadmap's list (hash, bucket loads, equality, direct state address). The one
instruction not strictly needed is a compare of the found id against the miss
marker.

**Not done.**

- **Timers and aging** are M10; only the seam exists, tested with a fake.
- **10M flows** were run only for the worker table with the smallest key and
  State; the shared table (about 1.8 GB of EAL heap at 10M) and larger keys or
  States above 1M were not run, nor were larger shared sizes: this machine has
  about 4 GB free.
- **Cache-miss counters** were not collected (`perf` was not used); "cycles" are
  TSC ticks. Frequency scaling and core isolation were not controlled (the
  governor is `powersave`; the repeat medians are tight, but absolute numbers
  would be a little higher or lower under `performance`). The single-thread
  matrix was run on one P-core only, not on an E-core; the shared benchmarks
  were run on both.
- **Shared readers on P-cores stop at 4** (five distinct physical cores with
  the benchmark thread aside); 8 readers ran on E-cores only. One writer only;
  multi-writer contention is D-028's measurement.
- **ThreadSanitizer** and GCC 14 / Clang 19 were not run locally.
- **A secondary-index alias representation** was not built; the choice rests on
  the comparison with a second table.
- **No module uses a flow table yet**, so there is no live-daemon test and no
  packet-loop throughput number; the three reference applications are tests.
- **`SharedFlowTable` needs the EAL** (the directory is rte_hash; the classifier
  already has the lazy bring-up exception) and **`Reclaim()` is driven by the
  caller**: nothing reclaims in the background, and destroying a table waits
  for the grace period.
- **No hugepage or NUMA-aware allocation**; `Traits::Allocator` is the seam.
- **`ForEach` is O(capacity)**, fine for scans and tests, not a packet-path
  operation.

**Revisit when:** a consumer needs more than 14 aliases or a different key set
per flow; a workload with mostly hit batches over very large tables makes the
slot prefetch default worth flipping; M10 finds the observer seam too narrow
(for example it needs the refresh to be a table operation); the shared table's
writer lock, measured above, is the bottleneck for a consumer that cannot
partition; or `ConcurrentExactTable` is promoted to public, which would let
`shared_flow_table.h` be installed.

## D-053 Expiry substrate: a worker-owned hierarchical timing wheel with budgeted polls (M10)

**Status:** accepted (2026-10-02), with the exceptions under "Not done".
**Code:** `core/dataplane/{expiry_wheel.h,tick_rate.h}`, tests
`core/dataplane/{expiry_wheel_test.cc,tick_rate_test.cc}` and
`core/flow/expiry_consumer_test.cc`, benchmarks `core/dataplane/expiry_bench.cc`
and `core/flow/flow_expiry_bench.cc`, `core/meson.build` (install manifest, test
and benchmark registration), `tools/{check_includes.py,check_installed_headers.py}`,
`core/flow/flow_observer.h` (comment only), `docs/{expiry,flow-state,architecture,performance-contract,benchmarking}.md`.

**Context.** No timer code existed. M9 left a seam: a flow table calls an
inline observer on create and erase, and hands out generation-checked
`FlowHandle`s. Idle expiry, FDB aging, NAT bindings and neighbour entries all need
the same thing: a deadline per entry, cheap refresh on the packet path, and
expiry work that cannot stall a poll. DPDK's `rte_timer` was the baseline to
beat. It is a per-lcore skip list with no per-poll budget.

**Decision.**

- `ExpiryWheel<Payload, Tick, LevelBits, Levels>`: a hierarchical timing wheel
  (6 levels of 64 slots by default) over a fixed array of 32-byte nodes (24 with
  32-bit ticks) linked by 32-bit indices. Everything is allocated in `Create`.
  After that nothing allocates, and a full wheel says so (`kNoExpiry`) and changes
  nothing.
- It lives in `dataplane/` and knows nothing about flows, modules, gates, workers
  or protobuf. It takes ticks as an unsigned integer type that may wrap, and
  compares them by serial-number arithmetic. It reads no clock; `TickRate`
  converts seconds to ticks at the control boundary only. `check_includes.py`
  forbids `flow/`, `gate.h` and `worker.h` under `dataplane/` (3 new self-test
  cases, 16 in all).
- `Schedule` returns an `ExpiryHandle` (node index plus generation, odd while
  armed). A fired or cancelled handle stays dead after the node is reused, so
  `Refresh` and `Cancel` on it return false and touch nothing. The payload
  (a `FlowHandle`) carries the owner's own generation check. A record that
  outlives its flow is therefore harmless in two independent ways.
- `Poll(now, budget, fn)` charges one unit per timer delivered and one per timer
  moved down a level. It stops at the budget, says `exhausted`, and the next call
  resumes with nothing lost or repeated. `Poll(now, 0, fn)` only reports whether
  work waits. Empty time is skipped with one occupancy word per level. Timers
  fire in non-decreasing deadline order, never early, and late by less than the
  granularity.
- Two ways to refresh a hot flow, both supported. (1) `Refresh(handle, later)`
  is a load, compare and store on the node; the wheel moves it once per timeout
  when it reaches the old position. (2) Owner-side: the flow's State keeps
  `last_seen`, the engine is not called per packet, and the callback returns the
  real deadline to re-arm the same timer. **Use (2) when the packet path must not
  touch a second array.**
- Single thread only: nothing is atomic and there is no owner-check policy.

**Evidence.**

*Tests* (fast tree, GCC 16, `-O1`): 36 wheel cases, 7 `TickRate` cases and 15
consumer cases. They include a random differential test against a trivially
correct model, exhaustive wraparound of an 8-bit clock (every start, timeout and
step), the same with refresh and cancel, 64-bit ticks near the wrap at every
granularity, budget and resume tests, forged and reused handles, generation
exhaustion, an allocation-counting `operator new` hook (nothing allocates after
`Create`), and a flow table wired to the wheel through the observer with both
refresh styles. Four deliberate defects each failed named tests: handles accepted
without the generation check; a budget off by one; a granularity that fired early;
a backwards `now` not clamped. All 58 cases pass 3/3 pinned to one CPU and to two
CPUs (`taskset -c 0`, `0,1`). The tests do not assert how much work fit in a time
window.

*Benchmarks* (`expiry_bench`, `flow_expiry_bench`; `build/perf-release`,
buildtype=release, GCC 16.2.1, i9-13900H, pinned to CPU 2, governor `powersave`
so frequency was not fixed, median of 3, one tick = 1 ns, wheel granularity
2^20 ticks ≈ 1 ms, 64 budget). Per-operation cost in ns and bytes per timer, the
timer's own storage only:

| N = 64K | B/timer | schedule | cancel | refresh (uniform) | refresh (hot 1%) | poll, nothing due | drain, per expiry |
|---|---|---|---|---|---|---|---|
| wheel (lazy refresh) | 32 | 5.8 | 3.2 | 3.3 | 1.1 | 5.0 | 23.7 |
| wheel, eager refresh | 32 | 5.9 | 3.2 | 12.2 | 4.9 | 5.0 | 23.9 |
| heap, lazy deletion | 93 | 21.4 | 0.1 | 1.0 | 0.5 | 0.5 | 66.6 |
| periodic scan, flat array | 8 | 0.7 | 0.0 | 1.1 | 0.4 | 40.0 | 0.8 |
| periodic scan, 64 B records | 64 | 3.8 | 1.7 | 2.3 | 0.5 | 71.2 | 1.9 |
| `rte_timer` | 120 | 243.6 | 237.0 | 428.8 | 212.5 | 7.0 | not measured |

At 1M timers the wheel costs 6.0 schedule, 3.7 cancel, 16.4 uniform refresh and
90 ns per expiry (about 26 us for the worst 64-unit poll). Each is slower than at
64K; the likely cause is cache misses over 32 MB of nodes, which I did not
confirm with a counter. Scan with 64 B records costs 5.9
ns per expiry. At 64K timers the wheel is **40 to 70 times cheaper than
`rte_timer` on schedule and cancel and uses a quarter of its memory**. Against
the heap it is cheaper on schedule, drain and the worst budgeted poll, and uses a
third of the memory; the heap is cheaper on cancel, refresh and an empty poll,
because lazy deletion leaves dead records in the heap until they are popped.

*Per packet, over a `WorkerFlowTable`* (`flow_expiry_bench`, hit loop over 1024
and 1M flows):

| Refresh style | 1K flows | 1M flows |
|---|---|---|
| none (lookup only) | 4.63 ns | 24.5 ns |
| owner-side store | 4.61 ns | 25.1 ns |
| engine, lazy | 6.06 ns | 41.8 ns |
| engine, eager | 10.13 ns | 76.8 ns |

The owner-side store is free within noise. The engine's lazy refresh costs
about 1.4 ns at 1K flows and 17 ns at 1M, because it touches a second array.
Scanning a 1M-flow table takes about 3.0 ms (3 ns per flow) in one pass.

*Layering and packaging:* include checker 16-case self-test and clean run; link
graph 18 libraries, 46 edges, 6 grandfathered, no violations (no new library, so
`docs/baselines/dependency-graph.json` is unchanged); staged install: 66 curated
headers present and compiling with `-Wall -Wextra -Werror`.

**What the measurements say, and what they do not.**

- A flat periodic scan is cheaper than the wheel on every raw cost at these
  sizes: no node, no bookkeeping, 8 bytes per timer. It loses on three other
  properties. Detection delay grows with the table (a budget of 64 entries over 1M
  needs about 16,000 polls to cover it). The cost of a poll with nothing due is
  40 ns and up, not 5. The work per poll is bounded only if the scan itself is
  budgeted. The wheel's detection delay and per-poll work do not depend on the
  table size. **If a consumer has one coarse timeout, high churn and a latency
  tolerance of one full pass, a budgeted scan of its own table is the cheaper
  choice; `docs/expiry.md` says so.**
- `rte_timer` has no budget, so its drain distribution is not comparable and is
  not reported.
- The hashed single-level wheel in the benchmark is a baseline only: it is not a
  complete implementation, and its 2 us poll at 1M timers is a revolution scan.
  It is not a recommendation.

**Not done.**

- **`rte_timer` at 1M timers was not measured.** The output file is
  truncated (invalid JSON) and I did not repeat the run. The 1K and 64K figures stand.
- **The 10M-timer benchmark was not re-run after the last header edit.** An
  earlier run (before the final change) showed 32 bytes per timer and 207 ns per
  expiry at 10M, but those numbers are not quoted above.
- **No ThreadSanitizer run.** The wheel is single-threaded by design and no
  concurrency test exists. GCC 14 and Clang 19 are left to CI.
- **No assembly inspection of the refresh path.** Its cost is measured above, not
  explained at instruction level.
- **The owner-side refresh is documented and tested but no module uses it.** No
  production consumer exists yet; the reference consumer is a test.
- **Frequency was not pinned** (governor `powersave`), so absolute nanoseconds
  carry that uncertainty. Ratios are more reliable than absolute values.
- **No timer-wheel statistics are exported.** Counters for late polls, moves
  per poll and retired nodes are not wired to `stats/`.

**Revisit when:** a consumer needs timers fired from a thread other than the
owner; a workload with over 10M timers or sub-microsecond granularity appears;
the lazy refresh's extra array touch shows up in a profile of a real module
(then use the owner-side style); M11 hand-off changes who may refresh a flow; or
a consumer with one coarse timeout prefers the cheaper budgeted scan.

## D-054 Handoff substrate: burst channels over `rte_ring_elem` with moved-from ownership, and generation-checked continuations (M11)

**Status:** accepted (2026-10-02), with the exceptions under "Not done".
**Code:** `core/dataplane/{handoff.h,handoff.cc,continuation.h}`, tests
`core/dataplane/{handoff_test.cc,handoff_threads_test.cc,continuation_test.cc}`,
benchmark `core/dataplane/handoff_bench.cc`, `core/meson.build` (source, install
manifest, tests, benchmark), `tools/{check_includes.py,check_installed_headers.py}`,
`docs/{handoff,architecture,performance-contract,benchmarking}.md`,
`docs/baselines/dependency-graph.json`.
`core/dataplane/continuation.h` gains a compile-time test seam (`ResolveHook`); tests
`core/dataplane/continuation_test.cc` (+5 cases, 18 in all) and `core/dataplane/handoff_test.cc` (allocation hook
replaces the nothrow `operator new` forms), `docs/handoff.md` ("Testing a race at an exact point").

**Context.** Nothing moved packets between threads with ownership: the Queue
module moves bare pointers between graph tasks and counts drops, and the flow and
expiry substrates (M9, M10) hand out generation handles that nothing could carry
across a queue. OVS/VFP misses, DPI, crypto, reassembly, neighbour resolution and
UPF buffering all need the same thing: hand a packet and a little context to
another thread, get it back (or not) with a token that is safe to resume from,
and shut the whole thing down with packets in flight.

**Decision.**

- **Mechanism: `rte_ring_elem`, no new queue.** `HandoffChannel<Context, Topology>`
  is one DPDK ring of `PuntItem<Context>` (the packet pointer and the context in
  one slot), with the explicit sync-mode calls. Topology (`kSpSc`, `kMpSc`,
  `kMpMc`) is a template argument, so nothing inspects it per call. One
  allocation from the config's allocator (DPDK's heap on the requested node by
  default) holds the object and the ring; `placement()` reports the node asked
  for and the node got.
- **Burst API; deviation from the roadmap sketch.** The sketch has
  `TryPunt(PacketHandle, Context)` returning `expected<void, HandoffError>` and
  `Dequeue(span<PuntItem>)`. A per-packet-only enqueue is the wrong shape: the
  gates, every baseline and the exit gate are burst-based, and measurement says a
  per-packet interface costs 3.6x at burst 32 (table). The sketch also leaves the
  caller holding a live-looking pointer after success (`PacketHandle` is a raw
  `rte_mbuf *`; no owning packet type exists, and `std::move` of a pointer
  copies it), which is the double-free hazard the roadmap text asks the API to
  avoid. The primary call is `TryPuntBurst(span<PuntItem>) -> PuntResult`: the
  first `accepted` items belong to the channel and **their `packet` is set to
  nullptr**; the rest are untouched, still the caller's, and the result says
  why (`kFull` or `kClosed`). `TryPunt(PacketHandle &, const Context &)` stays as
  the one-packet form with the same rule (null on success, untouched on failure).
  The roadmap's `expected<void, HandoffError>` is kept for it.
- **Context in the ring slot.** Trivially copyable, item at most 64 bytes, checked
  at compile time; `NoContext` makes the slot the pointer alone (8 bytes). Larger
  context travels as a generation-safe id.
- **Lifecycle.** `Close()` (producers get `kClosed` at once, queued items stay),
  stop the threads (a quiescent point; the channel cannot wait for threads inside
  a call), `Drain(fn)` (optional), destroy (what is left is freed and counted in
  `discarded`). `enqueued == dequeued + discarded + occupancy` when no call is in
  progress. Back-pressure is exactly two policies, drop or retry, and nothing
  blocks.
- **Continuations (`ContinuationId` is declared here, M7's rule).**
  `ContinuationHandle = GenerationHandle<ContinuationId>` and
  `ContinuationTable<Target>`: a fixed slot array (one allocation), generation odd
  while live, FIFO slot reuse (a handle parked in a queue sees the longest time
  before its slot holds anything else), a slot whose generation would wrap is
  quarantined. One thread at a time issues and retires; any number resolve
  concurrently, lock-free, without a read-modify-write (generation, copy,
  generation again, target words atomic). A stale, retired or forged handle
  resolves to nothing and retires nothing. The name `Continuation` avoids the
  worker "resume hook" (`resume_hook.h`, bessctl ResumeAll), which is unrelated.
- **A test seam in `Resolve`, free in production.** `ContinuationTable<Target, ResolveHook = NoResolveHook>`:
  `Resolve` calls `ResolveHook::AfterTargetWord(i)` after loading word `i` of the target and before reading the
  generation again; after the last word that is exactly "copied, not yet validated". `NoResolveHook` is an empty
  inline function. A test supplies a hook that retires and reuses the slot at that point, so the fail-closed check
  is exercised deterministically instead of by a race. It is a test facility, not an extension point.
- **Layering.** Header-only except `handoff.cc`; no new library, no new exception
  (`bess_dataplane_core` already may use `bess_utils`; the link graph went from
  46 to 47 edges, all allowed). `check_includes.py` gets a rule that keeps both
  headers off the packet view (`packet.h`, `pktbatch.h`, `packet_pool.h`) and six
  new negative cases (22 in all).
- **A DPDK 25.11 wart.** `rte_ring_elem.h` has no `extern "C"` of its own, so
  `rte_ring_get_memsize_elem` has C++ linkage and does not link; `handoff.cc`
  reaches the C symbol through an asm label. Everything else in the element API is
  inline.

**Evidence.**

*Tests* (fast tree, GCC 16, `-O1`, `-march=native`): 35 single-thread, 10
cross-thread and 13 continuation cases. Single thread: ownership (accepted items
moved-from, refused untouched, the retried tail), exact capacity for 16 sizes,
close/drain/accounting, an **exhaustive walk** of every enqueue/dequeue burst
sequence (bursts 1..capacity+1, capacity 1..5, depth 8..4, over a million nodes
per topology) from every ring alignment, three ring-size runs up to and over the
2^32 index wrap and the 2^31 boundary, checked step by step against a queue; a
random differential test over five context sizes (8 to 64 byte items) and all three
topologies with random Close/Drain/start index; allocator refusal; placement
(including a requested node served from another one, reported); an allocation-
counting `operator new` (nothing allocates after `Create`); real mbufs with the
pool's count as the ownership oracle (destroy with queued packets, drain exactly
once, refused tail freed, closed channel, punt and resume through two channels,
300 create/destroy rounds). Threads: SP/SC, MP/SC and MP/MC streams with exact
once-only accounting and per-producer order, a drop-policy stream, a consumer that
never drains (accepts exactly `capacity`, counts every refusal, memory unchanged,
teardown frees all), shutdown in order with live producers and consumer, a
consumer that stops mid-stream, 60 lifecycles under traffic, a single-producer
role handed between threads at a join, and punt-service-resume in which the worker
retires continuations while their packets are away and every one of them comes back
and fails closed (the architecture.md section 7 test). Continuation: stale handle
after slot reuse, forged handles, generation wrap, targets of 1 to 64 bytes
byte-for-byte, a model that predicts every handle in a grid after every operation,
no allocation after `Create`, allocator refusal at both allocations, and a
concurrent test (a resolved target is whole, is the one issued for that handle,
and a handle whose retirement completed never resolves). No test asserts how much
work fits in a time window; volume is the loop condition and a 25 s deadline only
turns a hang into a failure. Each binary passes 3/3 pinned to one CPU
(`taskset -c 0`) and to two (`0,1`), and standalone under `timeout 60 prlimit
--as=3000000000`. Clang 22 syntax check of both headers and all three tests with
the fast profile's flags (`-Wall -Wextra -Werror`) is clean; GCC 14 and Clang 19
are left to CI.

*The `Resolve` seam* (`ContinuationResolveSeamTest`, 5 cases, single thread, no timing): the hook runs once per
target word and an idle hook changes nothing; a retire between the copy and the check; a retire and reuse of the
only slot between them (the old handle must resolve to neither the old nor the new target, the new handle
must resolve); a retire and reuse after word 0 of two (the mix `{old, ~new}` must never be returned); a retire
before the `Resolve` never reaches the copy. Each asserts the hook was reached. **Zero cost in production, by
assembly:** `Resolve` for targets of 8, 24 and 13 bytes, GCC 16.2.1 `-O3 -march=x86-64-v3`, compiled from the
header before and after the change: the listing with directives, symbol names and labels normalised is identical
(499 lines each, a whole-file diff of 0 lines); a control instantiation with a hook that calls an external
function emits the call (1 `call`), so the seam is wired. (The first asm comparison of this step compared the
unchanged worktree with itself because the edit had landed in the wrong tree; it was redone after the edit was in
the right one.)

*Sanitizers on this branch.* **ASan + UBSan + LSan** (scratch tree, `-fsanitize=address,undefined -O1 -g1`,
no `prlimit`): `continuation_test` 18/18 and `handoff_test` 28/28 (every case that does not initialise the EAL)
with no report. 7 cases in `handoff_test` and all 10 in `handoff_threads_test` initialise the DPDK EAL, which fails
under ASan (`eal_legacy_hugepage_init(): couldn't allocate memory due to IOVA exceeding limits of current DMA mask`:
ASan's mappings sit above the mask, `--iova-mode va --no-huge`); they were not run under ASan. Running it
first exposed a defect of the allocation-counting hooks of both unit tests: they replaced `operator new` but not the
nothrow forms, so the library's own nothrow `new` bypassed the counting and the injected refusal and, under ASan,
produced memory the replaced `operator delete` freed (alloc-dealloc-mismatch). The hooks now replace the nothrow
forms too; that is also what made the "allocator-refusal test is skipped under TSan" artifact of the first text go
away (see below).

*ThreadSanitizer and the real (generic) ring variant.* The D-054 TSan run used `-DRTE_USE_C11_MEM_MODEL`. To
see what TSan says about the variant that ships, a second scratch tree was built without it and run
(`taskset -c 2,4,6,8`, 120 s cap per binary): `handoff_test` 35/35, 0 reports; `continuation_test` 17/18 and 0 reports (the 18th is the allocator-refusal test, the hook artifact; after the nothrow fix the same tree runs it: 18/18, 0 reports);
`handoff_threads_test` **630 reports** in the 120 s it was given (the run was cut by the cap, so the count is a
lower bound), of which 559 (89%) are inside the ring's own inline code (542 in `__rte_ring_enqueue_elems_32` /
`__rte_ring_dequeue_elems_32`, 17 in `__rte_ring_update_tail` / `__rte_ring_headtail_move_head`) and the other 71
are mbuf fields and payload reads in the producer and consumer, which race only because the ordering between them
travels through the ring. The generic variant orders with plain `volatile` accesses and `rte_smp_wmb()/rmb()`, which
are compiler barriers on x86 (TSO): TSan models atomics, locks and its own annotations, not volatile accesses or
barriers, so it reports every ring access as a race and cannot say whether the ordering is right. There is no flag
that makes it see them. The only ways to a clean run are (a) the C11 variant, which is what the first run
used, and (b) `__tsan_release`/`__tsan_acquire` annotations on the ring entry points, which *assert* the ordering
instead of checking it and would verify nothing about the ring. So the C11 build is the best mechanical evidence
available, and what it shows is that the ring's algorithm with DPDK's own acquire/release placement is race-free
under the channel's use; for the shipped x86 variant the argument is TSO plus compiler barriers, plus the normal
build's real-thread runs (every binary 3/3 on one and on two CPUs), not a tool. Defining `RTE_USE_C11_MEM_MODEL` for
`core/dataplane` would make the shipped ring the checked one (its cost is unmeasured: a one-hour experiment, H5 in
`.scratch/streaming-plan.md`).

*Benchmarks* (`handoff_bench`, `build/perf-release`, buildtype=release,
`-march=x86-64-v3`, GCC 16.2.1, i9-13900H, two P-cores on different physical cores
(`taskset -c 2,4`), governor `powersave` so frequency was not fixed, Chrome and
other desktop load present (`ab_bench --allow-busy`), median of 3 repetitions,
`--benchmark_min_time=0.3s`). Round trip: A enqueues a burst, B (pinned to the other
core) dequeues it and enqueues it back, A dequeues it; one burst in flight; ns per
burst (per packet is that divided by the burst):

| ring | B/item | b=1 | b=8 | b=16 | b=32 |
|---|---|---|---|---|---|
| ptr-ring SP/SC (Queue's exact calls) | 8 | 233 | 243 | 250 | 284 |
| elem8 | 8 | 234 | 239 | 249 | 315 |
| elem16 | 16 | 219 | 226 | 270 | 411 |
| elem24 | 24 | 307 | 309 | 418 | 662 |
| elem32 | 32 | 260 | 328 | 453 | 674 |
| elem64 | 64 | 242 | 551 | 728 | 1007 |
| elem16, flag-dispatching calls | 16 | 252 | 258 | 284 | 401 |
| elem16 MP/SC | 16 | 277 | 313 | 321 | 454 |
| elem16 MP/MC | 16 | 225 | 276 | 283 | 449 |
| cached-index SP/SC (not shipped) | 16 | 279 | 359 | 376 | 405 |
| **channel SP/SC, no context** | 8 | 234 | 230 | 245 | 316 |
| **channel SP/SC, 8-byte context** | 16 | 226 | 220 | 267 | 402 |
| channel SP/SC, 24-byte context | 32 | 198 | 306 | 459 | 611 |
| channel MP/SC, 8-byte context | 16 | 210 | 203 | 247 | 430 |
| channel MP/MC, 8-byte context | 16 | 212 | 214 | 256 | 423 |
| channel, one packet per call both sides | 16 | 200 | 208 | 709 | 1391 |

Run-to-run noise on this machine is 10-20% at burst 1 (single rows are not
ordered reliably); the decisions rest on paired ABBA runs (`tools/ab_bench.py`, 8
rounds, called only outside +-3% with 3/4 of pairs agreeing; B/A):

- **Exit criterion, round trip (SP/SC channel without context against the
  Queue's pointer ring, same 8-byte item):** -5.7% at burst 1, -9.7% at 8, no
  clear difference at 32. No regression.
- **Bookkeeping, round trip (channel against the bare `elem16` ring):** no clear
  difference at 1, 8 or 32. In a single thread (enqueue then dequeue one burst, no
  cross-core traffic) the channel costs +0.6, +2.0 and +5.4 ns per burst over the
  bare element ring (4.8/8.1/19.3 against 4.2/6.1/13.9 ns), the closed check, the
  counters and the nulling of the handed-over pointers.
- **A per-packet-only API:** x3.6 at burst 32 (1491 against 413 ns), no clear
  difference at 1 and 8 (noisy), hence the burst primary.
- **Where the context lives** (both sides touch the packet; a 128-byte header and a
  private line): in the ring slot is 31% and 21% faster than in the packet's
  private area at bursts 8 and 32 for an 8-byte context, 28% and 28% for 24
  bytes; at burst 1 the packet is 12% (8 bytes) and 20% (24 bytes) *faster*. The
  design point is the burst, and a context in the packet couples the channel to
  the packet layout, so: in the slot. At burst 1 alone the other choice wins.
- **Flag-dispatching calls against the explicit ones:** no clear difference in
  the round trip (the cross-core transfer dominates); in one thread it costs 0.3,
  1.4 and 0.9 ns per burst. Topology as a type costs nothing and removes the
  question.
- **A cached-index SP/SC ring** (a yardstick, not a candidate: the roadmap says
  no new queue algorithm): in the round trip, the handoff use, it is 21% and 40%
  *slower* than `rte_ring_elem` at bursts 1 and 8 and equal at 32. One way (a
  producer streaming into a consumer) it is 15%, 12% and 31% faster at bursts 1,
  8 and 32. That gain is real and is what the rule costs a streaming consumer.

Bytes per item scale the cost: at burst 32 the round trip is 284, 315, 411, 662
and 1007 ns for 8, 8 (element API), 16, 24-32 and 64 bytes; elements of 8 and 16
bytes have DPDK's dedicated copy paths. Keep contexts to a word where a
generation-safe id will do.

Software cost without coherence (one thread, enqueue and dequeue of one burst; ns
at burst 1/8/32): pointer ring 4.0/6.9/14.8, `elem16` 4.2/6.1/13.9, channel
SP/SC with no context 6.2/9.0/18.5 and with an 8-byte context 4.8/8.1/19.3,
channel MP/SC 13.4/19.5/25.3, MP/MC 22.9/28.5/34.4, channel with one call per
packet 8.6/60.1/239.2. A refused call on a full ring costs 1.4 ns (pointer
ring), 0.8 (`elem16`), 2.4 (channel SP/SC) and 6.6 ns (channel MP/SC, which
increments a shared counter); a producer that outruns its consumer pays 4.3, 8.6
and 17.6 ns per burst of 1, 8 and 32 offered to a full channel (`elem16`: 3.2,
7.0, 15.6). The assembly of SP/SC `TryPuntBurst` has no lock-prefixed
instruction and no call (154 static instructions, the copy loops included);
`Dequeue` 106; MP/SC has the compare-and-swap and three counter `lock xadd`s.
Bytes: 384 for the object (three 128-byte groups) plus the ring: for 1024 items of
16 bytes 17,152 bytes in all (about 16.75 per item at full occupancy).

*Mutants* (a defect put into the header, the three test binaries rebuilt and run, the header restored; nine, all
caught): accepted packets not nulled (10 tests: the ownership test, the exhaustive walk and the differential test
for each topology, and the freeing tests); closed check removed (13); `refused_full` not counted (11); destructor
not draining (3, the pool count); LIFO instead of FIFO slot reuse in the continuation table (3); **generation check
after the target copy removed in `Resolve` (3: `ContinuationResolveSeamTest.RetiredBetweenTheCopyAndTheCheckFailsClosed`,
`...RetiredAndReusedBetweenTheCopyAndTheCheckFailsClosed`, `...ReusedInTheMiddleOfTheCopyIsNeverReturnedTorn`)**;
the check before the copy removed (1: `...RetiredBeforeTheResolveNeverReachesTheCopy`, which asserts the early-out is
taken; the post-copy check alone is functionally sufficient, so this one is an optimisation, not a safety property);
the odd-generation check removed (2); the id bound removed (1: the forged-handle test crashes reading out of
bounds, rc 139). **Correction:** the first D-054 text said the post-copy check's removal was "not caught". That was
wrong in the other direction: the first mutation script's pattern matched the *pre-copy* check (the first
`if` before `LoadTarget`), whose removal changes no behaviour; the post-copy mutant had never been run. It was run
for the first time here and is caught.

**What the measurements say, and what they do not.**

- **One way, the SP/SC channel is slower than the bare pointer ring, and I did
  not find out why.** Balanced producer and consumer, no context (8-byte item,
  the same as the Queue's ring): +35% at burst 8 and +52% at 32 (paired, 0/8
  pairs favourable; burst 1 not clear). Against the bare 16-byte element ring,
  +14% and +45% (+8.7% and +67% in a rerun after the groups were moved apart).
  The software cost measured in one thread is 2 to 5 ns per burst, an eighth of the
  difference at burst 32, so the rest is coherence behaviour, and the L2 misses per
  burst (`perf stat`, user mode) were higher for the channel (6-7 against 2-4).
  Removing, one at a time and cumulatively, the counters, the nulling and the
  closed check left a +10% residual against a bare ring on the same DPDK heap
  (the ring on the DPDK heap was not slower than the `aligned_alloc` one). Moving
  the counter groups to separate 128-byte pairs did not remove it either; that
  layout is kept as ordinary practice, not as a measured win. The round trip, the
  handoff use, is at parity (above). A streaming consumer should measure its own
  case; see "Revisit when".
- The benchmark is noisy (powersave, a loaded desktop): a single run of a row
  can be 30% off, which is why the claims above are paired. The matrix was run
  with the counter groups 64 bytes apart; the A/B runs that follow the change to
  128 bytes agree with it.
- `perf stat` cache counters were used for the one-way investigation only; no
  systematic cache-line-traffic table exists.

**Not done.**

- **ThreadSanitizer: the real ring variant is not checkable by TSan** (reason above); the C11 variant is clean:
  35 + 10 + 12 cases, 0 reports. The thirteenth continuation case (allocator refusal) was skipped there; with the
  nothrow `operator new` forms replaced it runs under the sanitizers: 18/18 under TSan (generic-ring tree) and
  ASan, 0 reports.
- **ASan + UBSan + LSan: partial.** Clean on 18 + 28 cases; the 7 + 10 EAL-dependent cases cannot start under ASan
  (above). A fix would be an EAL option for a low `--base-virtaddr` in the test main, a change to
  `core/runtime/dpdk.cc` that was not made here.
- **Mutation testing: nine mutants, all caught**; none of them attacks the MP/SC and MP/MC enqueue paths specifically
  (all three topologies share the bookkeeping that was mutated; the ring calls themselves are DPDK's).
- **Still open, unchanged:** cross-NUMA handoff (one node); a graph adapter and a production consumer; an SP/SC
  role-misuse detector (a debug-only owner check is feature work, not a test, and was not started); GCC 14 and Clang 19
  (Clang 22 syntax check only); the one-way streaming slowdown (plan, hypotheses, experiments and time boxes in
  `.scratch/streaming-plan.md`; needs a quiet machine and the `performance` governor); the main matrix was
  measured with 64-byte counter groups, before the change to 128 (later A/B runs agree; re-running the matrix
  needs a quiet machine too); no systematic cache-line-traffic table.
- **Not run here:** the full `meson test` suite (only `continuation.h`'s default instantiation, two test files and
  `docs/handoff.md` changed: the three affected test binaries pass, standalone and 3/3 pinned to one and two CPUs).

**Revisit when:** a streaming consumer (reassembly into crypto, DPI) is
throughput-bound on the handoff and a profile shows the ring (then the one-way
gap above, and the cached-index ring's 12-31%, are the numbers to beat, and the
no-new-queue rule is the thing to reconsider); a consumer needs a graph adapter
(`Queue`-style, picking SP or MP at `PreResume` as D-036 does); cross-NUMA
placement matters (measure on a two-node machine); M20 hardware marks need a
continuation that survives a device reset; or a move-only owning packet type
arrives in `bess_packet` (then `PuntItem` should hold it and the nulling
convention becomes a type).

## D-055 One CI authority for gating and release lanes; static release links with -fno-lto

**Status:** accepted (2026-10-02), with the exceptions under "Not done".
**Code:** `tools/ci_profile.py`, `tools/ci_container.sh`, `env/ci.Dockerfile`, `tools/bootstrap_dpdk.py` (`--variant`), `.github/workflows/ci.yml`, `core/meson.build` (`bessd_link_args`), `docs/ci-parity.md`.

**Context.** Two CI-only failures in one day had the same shape: the configuration that failed was not
one a developer could run. A benchmark compiled with GCC 16 in the fast tree but failed under Clang 19,
because the fast tree builds no benchmarks. Then pinning the release job to GCC 14 (to build the shipped
binaries with a compiler that something else tests; before, it used the image's default g++ 13.3 and no
other job did) exposed a second problem: Ubuntu's static archives (`libunwind.a`, `libunwind-x86_64.a`)
are fat LTO objects built by GCC 13, and GCC 14's linker plugin reads their bytecode and stops with
`bytecode stream ... generated with LTO version 13.1 instead of the expected 14.0`. The release job went
red on that, and a review of the branch said so. The release job also had its own copy of the configure
and build commands, so it was outside the single authority that the gating lanes already shared.

**Decision.**

- `tools/ci_profile.py` defines every lane. The gating lanes (`gcc`, `clang`) and the release build
  (`release-bootstrap`, `release-configure`, `release-build`, `release-verify`) are steps of the same script
  with the same compiler and CPU authority; `ci.yml` calls it and `check-pins` fails when its compiler
  pins and the workflow matrix disagree. The release-specific options are listed in the script, not in the
  workflow.
- The static link passes `-fno-lto`. BESS is not built with LTO (`meson.build` asserts `b_lto` is off for
  static builds), so the only LTO input is a distribution archive, and its fat objects also carry machine
  code, which the link uses. The release compiler stays GCC 14; falling back to GCC 13 to match the archive
  was rejected, since nothing else tests that compiler.
- `tools/ci_container.sh` runs the same script in the CI image (Ubuntu 24.04, gcc-14, clang-19) for the
  exact compiler versions, including `release`; `CTR_CPUSET`, `CTR_MEMORY` and `CTR_JOBS` keep it from
  disturbing a benchmark.

**Evidence.** The failure was reproduced in an `ubuntu:24.04` container with g++-14 and the distribution's
`libunwind-dev`: the link of a small program against `libunwind-x86_64.a` and `libunwind.a` fails with the
LTO-version error as built by the old flags, and succeeds and runs with `-fno-use-linker-plugin` or with
`-fno-lto`. `ci_profile.py release --dry-run` prints the commands the old job ran, and `check-pins` passes.

**Not done.**

- The full `bessd` standalone link has not been run locally with GCC 14. It ran in CI: run 37001729024 (commit
  `627fa286`) built, verified (no dynamic DPDK library) and packaged it, with every gating and experimental
  lane green.
- Archives other than libunwind may also carry GCC 13 bytecode; `-fno-lto` covers them by construction, but
  none was individually inspected.
- The Ubuntu 26.04 lanes are not part of the pins (they are experiments).
- A system `libunwind` built by the release compiler (or a distribution without an LTO archive) would make
  the flag unnecessary; not pursued.

**Revisit when:** the release job moves to another compiler or distribution, BESS adopts LTO (the assertion
fails first), or a distribution ships static archives that are slim LTO only (then the link needs an
unwinder built without LTO).


## D-057 Execution layering: EAL extracted, framework no longer reaches runtime (M1)

**Status:** accepted (2026-10-02), with the exceptions under "Not done".
**Code:** `core/meson.build` (`eal_sources`, `execution_sources`, `framework_sources`, `bess_eal`, `bess_execution`; `bess_runtime` removed), `core/framework/plugin_loader.{h,cc}`, `core/runtime/thread_placement.{h,cc}`, `core/runtime/dpdk.cc`, `core/packet_pool.cc`, `core/module.cc`, `core/module_graph.cc`, `core/task.cc`, `core/worker.cc`, `core/framework/module_init_context.h`, `tools/layer_dag.json`, `tools/check_includes.py`, `tools/check_installed_headers.py`, `docs/baselines/dependency-graph.json`.

**Context.** D-047 left six grandfathered edges in `tools/layer_dag.json`: `bess_framework -> bess_runtime`,
`bess_utils/classifier/meter/route -> bess_runtime` (lazy EAL bring-up) and `bess_control -> bess_host`
(plugin loading). Its "Not done" item read as a file move: split a `bess_execution` out of `bess_framework`.
That premise was incomplete. A stand-alone execution archive cannot be made by moving files: `module.cc`
builds `Task` and `LeafTrafficClass` (`Module::RegisterTask`), `task.cc` calls `Module::ProcessBatch` and the
inline `ProcessOGates`, and `runtime_state.cc` calls the private `Module::Destroy`. The cycle had two halves:
the registries/state/`WorkerManager` (runtime) against Module/Worker/TrafficClass (framework), and the EAL
helpers (`dpdk.cc`, `opts.cc`) against `worker.cc`/`packet_pool.cc`. The second half is also why four
low-level libraries pointed at "runtime".

**Decision.** Cut at definitions, not at files, and add no virtual call on the packet path:

- `bess_eal` is a new bottom library (above `bess_utils` only): `runtime/{dpdk,memory,opts,path,startup,thread_placement}.cc`
  plus `utils/dpdk_memory.cc` and `utils/bpf_program.cc`, the two utils files that start the EAL lazily.
  `classifier`, `meter`, `route` and `dataplane` link it. `check_includes.py` forbids those files from
  including `worker.h`, `module.h`, `packet_pool.h`, `scheduler.h`, `traffic_class.h` and `runtime_state.h`
  (four negative self-test cases; 26 total).
- `bess_execution` sits below `bess_framework`: `worker.cc`, `task.cc`, `traffic_class.cc`, `event.cc`,
  `resume_hook.cc`, `packet_pool.cc`, `runtime/runtime_state.cc`, `runtime/worker_manager.cc`,
  `framework/instance_registry.cc`. `bess_framework` keeps Module, gates, ports, the module graph, the init
  context, the resource bindings and the plugin check/loader.
- `bess_runtime` is retired (no alias). Header paths and the `bess::runtime` namespace are unchanged, so
  installed headers and external plugins see no difference.
- Definition relocations that make the cut empty: `is_cpu_present` moved from `worker.cc` to
  `runtime/thread_placement.cc` (still declared in `worker.h`); `InitDpdk` no longer calls
  `current_worker.SetNonWorker()` (the call moved to `PacketPool::CreateDefaultPools` and the lazy path of the
  `PacketPool` constructor); `ModuleRegistry::Clear` moved into `module_graph.cc` (it calls the private
  `Module::Destroy`); `Task::GetSocketConstraints` moved into `module.cc`.
- The plugin loader moved out of `bessd.cc` into `framework/plugin_loader.{h,cc}` (namespace
  `bess::framework`), which removes `control -> host`.
- `tools/layer_dag.json` now has `"exceptions": []`.
- `ModuleInitContext::ProcessDefault()` is private with `friend class ::Module`. An author cannot get
  capabilities except through `Module::init_context()`. `ResourceBindings::ProcessDefault()` stays public:
  `framework/resource_bindings.h` is not installed (only `instance_registry.h`, `module_init_context.h`,
  `plugin.h` are). `check_installed_headers.py` compiles a plugin that calls `ProcessDefault()` and requires
  the failure to name it as private.

**Evidence.**
- Residual-symbol check (strong T/D/B/R/S/G symbols `bess_execution` needs from `bess_framework`, from `nm`
  on the built archives): empty. `bess_eal` needs nothing from `bess_execution` or `bess_framework`. So no
  new virtual call was needed and the fallback (stop at a merged framework+state library) was not taken.
- `check_link_graph.py`: 19 libraries, 51 edges, 0 grandfathered, no violations; the baseline JSON is
  regenerated.
- Hot-path assembly (the five touched translation units compiled with the perf-release flags, before vs
  after): see the report; no packet-path function body changed.
- fast tree, GCC: 109/109 tests. clang++ 22 `-fsyntax-only` on every changed file. Staged install:
  installed-header check passes including the new negative case, and the three standalone plugins build
  against it; `sample_plugin_load` passes.

**Not done.**
- No benchmark was run. Archive membership and object order changed for ~17 files, and D-046 measured 2-11%
  from layout alone; an A/B across all benchmarks is the open item.
- `SetNonWorker` behaviour: callers that only bring the EAL up lazily (classifier, meter, route, utils,
  benchmarks calling `InitDpdk` directly) no longer reset the main thread's worker TLS. Production paths and
  every test that goes through `PacketPool::CreateDefaultPools` are unchanged; the control plane still calls
  `SetNonWorker` per RPC.
- `ResourceBindings::ProcessDefault()` is still public (internal header); tests and `api_v2.cc` use it.

**Revisit when:** a second runtime instance (M5) needs a context that is not the process default, or a
benchmark shows the layout change cost more than noise.

## D-058 M1/M2 closure: enforceable include rules, classified installs

**Status:** accepted (2026-10-02), with the exceptions under "Not done".
**Code:** `tools/check_includes.py`, `tools/include_dotdot_baseline.txt`, `tools/api_classes.json`, `tools/check_installed_headers.py`, `tools/public_proto_closure.py`, `protobuf/meson.build` (`public_pb_headers`), `core/meson.build` (`check_layer_includes_self_test`), `tools/ci_profile.py` (`clean-tree`), `.github/workflows/ci.yml`, `docs/architecture.md` (sections 2, 5, 9), `docs/plugin-api.md`, `docs/ci-parity.md`.

**Context.** An outside review of `627fa286` found that several M1/M2 exit criteria were prose, not checks. (1) `check_includes.py` matched substrings of the *spelled* include, resolved nothing, and banned `..` nowhere; 359 `..` includes existed (all in `modules/`, `drivers/`, `gate_hooks/`, `resume_hooks/`, `framework/`, `utils/`, none in a layered library) and its self-test was only run by the CI `layers` step, and re-implemented the matching loop instead of calling it. (2) `protobuf/meson.build` installed the whole generated `pb/` directory: 9 protocols as `.pb.h`, `.pb.cc`, `.grpc.pb.h` and `.grpc.pb.cc`, though the public headers need five `.pb.h` (M2 exit: recursive installation removed). (3) The public/experimental split lived in `docs/architecture.md` and `docs/plugin-api.md` and in one flat `PUBLIC_REQUIRED` list that never checked the installed set was *only* that list. (4) `docs/plugin-api.md` still called the descriptor "metadata only" and never mentioned the API range or capabilities. (5) The scout found no `#include <bess/...>` and listed it as an M1 deliverable. (6) Nothing checked that a build leaves the source tree clean. (7) `docs/architecture.md` had no control-vs-dataplane section (the M0 text lists one).

**Decision.**

- **Include checker.** One function (`judge_include`) is used by the scanner and the self-test, so the test cannot pass while the scanner is wrong. A quoted include is resolved as the compiler does (next to the including file, then under the include root `core/`; a `<>` include only under the root; an include that resolves to nothing, like a generated `pb/` header, is judged as spelled under the root) and the rules see the resolved path: `"../runtime/x.h"` is `runtime/x.h`, `"runtime/../utils/x.h"` is `utils/x.h`. `..` as a path component is refused everywhere in `core/` (tests included), except for the pairs in `tools/include_dotdot_baseline.txt`; the baseline is exact `(file, include)` pairs, a stale entry fails, and `--emit-dotdot-baseline` regenerates it, so it can only shrink. Self-test: the 22 original synthetic edges unchanged, plus relative spellings, the `..` ban (also in a test file), baseline exactness, resolution order (sibling before root, `<>` never sibling) via a stand-in directory rule, and stale-entry reporting; it runs as the Meson test `check_layer_includes_self_test` in the `architecture` suite as well as in `layers`. Layering rules themselves are unchanged.
- **Installed generated headers.** `protobuf/meson.build` installs exactly `public_pb_headers` (`bess_msg`, `error`, `module_msg`, `port_msg`, `util_msg`; `util_msg` only transitively, through `module_msg`) with the same copy-target pattern `pybess` uses for generated Python, and nothing else of the generated directory. `tools/public_proto_closure.py` computes the closure from the real include graph (public headers' `"pb/x.pb.h"` includes, then the generated headers' own includes) and is both a script and a check; it runs as the Meson test `check_public_proto_closure` against the build's generated directory and again, on the staged install, inside `check_installed_headers.py`.
- **Public vs experimental.** `tools/api_classes.json` is the classification table: one line per installed header, `public` or `experimental`, plus the generated list. `check_installed_headers.py` fails if the installed file set differs from the table in either direction, if an internal file is installed (named internal headers, `*.grpc.pb.h`, `.pb.cc`, the control and test protocols, tests and benchmarks) or listed, if a **public header includes an experimental one** (an experimental type cannot leak into the public contract), if any quoted include of an installed header does not resolve inside the installed tree, or if the generated headers are not the closure. The install lists stay in `core/meson.build` (the table is verified against the installed result, so the two cannot drift silently); no header moved and the external contract is unchanged. The classes follow the existing prose: root headers, `framework/{module_init_context,instance_registry,plugin}.h` and `utils/` are public; `classifier/ dataplane/ flow/ meter/ rcu/ route/ stats/` are experimental, which is narrower than the roadmap's "likely public" list (typed classifier APIs, `StrongId`, `SlotTable`, meter and stats facades): promoting those is a per-header decision, not made here. `--self-test` (Meson test `check_installed_headers_self_test`) injects each defect class.
- **Descriptor documentation.** `docs/plugin-api.md` documents the descriptor, `api_min`/`api_max`, `required_capabilities`, `BESS_CAP_*`, the check order, and what a refused plugin is (logged with the reason, `dlclose`d, not listed; its static constructors already ran, and for `ADD_MODULE` the matching destructor deregisters the class; other static side effects are the plugin's to avoid; at daemon start the refusal is a warning, over `bessctl module load` the error is generic and the reason is in the daemon log).
- **`<bess/...>` canonical includes: explicit non-goal for now.** M1 asks to *begin* moving new public includes toward `#include <bess/dataplane/strong_id.h>` and not to mass-convert. The installed layout is `include/bess/core/<layer>/x.h` with plugins compiling at `-I include/bess -I include/bess/core`, so the spelling `<bess/dataplane/x.h>` resolves nowhere: a header would have to be installed at `include/bess/dataplane/x.h` (every public root header, `module.h`, `packet.h`, ..., at `include/bess/`). That changes the install layout for every external plugin (`"module.h"` through `-I include/bess/core` stops working unless both trees are installed, which duplicates headers and makes each reachable under two spellings), and in-tree there is no `bess/` directory (the source root is `core/`; a directory rename is excluded by M1 and a symlink or generated include tree adds a second way to reach every header). The benefit is spelling. Not done, deliberately. What was done toward the intent: the `..` ban (the part of M1 that prevents layering bypass), a checker that resolves include spellings (so adding a `bess/` prefix later needs one line in `resolve_include`, and `bess/runtime/x.h` is already caught by the substring rules), and new public includes stay root-relative.
- **Source-tree cleanliness.** `ci_profile.py clean-tree` (last step of `all`, a CI step of its own) fails if a modified or untracked non-ignored file differs from how the run found the tree. Alone it needs a clean tree; under `all` it compares content hashes against the state `all` started in, so uncommitted work of the developer is not blamed on the build.
- **Control-vs-dataplane responsibilities** is section 9 of `docs/architecture.md`, written from the code (the control lock, `WorkerPauser`, the transaction path, RCU reclamation on the control thread, resource bindings).

**Evidence.** See the report: mutations of each checker (judge the spelling instead of the resolution, drop the `..` ban, honour the baseline wholesale, apply layering to fixtures, drop stale detection, remove a generated header from the closure, install a `.grpc.pb.h`, a stray header, a control proto, make a public header include an experimental one) each fail the corresponding check; a staged install contains exactly the 5 generated headers (was 36 files) and `examples/standalone_plugin` (three plugins) builds against that stage alone and exports `bess_plugin_descriptor_v1`; `check_installed_headers.py` passes on the stage under GCC 16; the architecture suite and the full fast-tree suite pass; `clean-tree` passes after a real build and test.

**Not done.**

- The 359 grandfathered `..` includes are not rewritten. The rewrite is mechanical (`"../utils/x.h"` to `"utils/x.h"`, the same file) and no-codegen, but it touches 124 files in `core/modules`, `drivers`, `gate_hooks`, `resume_hooks`, `framework`, `utils` that other branches are editing; the baseline is the owner (M1 include hygiene) and shrinks with each file touched. Removal phase: a pass with no branches in flight, before the M2 API freeze.
- `<bess/...>` (above).
- `sample_plugin` is an in-tree build (it uses `core_include`), not an installed-artifacts consumer; only `examples/standalone_plugin` proves the installed contract. Converting `sample_plugin` was not asked and would lose its coverage of in-tree plugin loading.
- The install lists remain in two places (`core/meson.build` and `tools/api_classes.json`); the checker makes drift a failure on the staged install (`verify-install`), not at configure time.
- The classification table does not yet separate "public for plugin authors" from "public for application authors" (`instance_registry.h`, `module_init_context.h`).
- Only the GCC 16 compile of the conformance plugin was run here; the CI lanes (GCC 14, Clang 19) run it for real.

**Revisit when:** a header is promoted or demoted (edit the table in the same change); the plugin API version is bumped for a source break (the one moment an install-layout change to `include/bess/<layer>` is cheap, and `<bess/...>` could land with it, together with the baseline rewrite); or the install lists move into one generated manifest.
