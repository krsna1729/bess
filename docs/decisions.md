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
