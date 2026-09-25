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
| D-010 | Size concurrent tables for occupancy and grace-period headroom, not a fixed 75% | accepted in part |
| D-011 | Tune per table at build time from host facts discovered once per process | accepted |

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

**Status:** accepted in part (2026-09-26):

- **Accepted:** the 3/4 sizing, counting pending deletes against headroom,
  and growth (never failure) whenever headroom is short or an add returns
  `kFull`.
- **Provisional:** the headroom constant (`kMinHeadroom` = 256, or 5%). It
  assumes a 64 µs grace period, which has not been measured.
- **Next:** measure the grace-period distribution (p50/p95/p99/max) under
  representative workers, including slow modules and pauses, then set the
  constant from it.
- A review pointed out that a QSBR grace period is not a fixed constant. It
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

