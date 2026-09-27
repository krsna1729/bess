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
  reachable over the RPC carries a `ResourceCodec` (set by its owner): the
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
